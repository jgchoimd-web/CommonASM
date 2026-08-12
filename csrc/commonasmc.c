#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMMONASM_VERSION "0.1.0-dev"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

typedef struct {
    char base[64];
    char symbol[128];
    long long offset;
    bool has_base;
    bool has_symbol;
} Address;

/* rsp and rbp belong to the frame, and rax and r11 are the compiler's two
   scratch registers, so none of them appear here: a virtual register must
   never alias one. That leaves 12 machine registers for the 16 virtual ones,
   and virtual r12-r15 live in spill slots. See x86_vreg_operand().

   Two scratches are needed because one instruction can require both an
   address held in a spilled register and a value held in another. */
static const char *x86_regs[] = {
    "rbx", "rcx", "rdx", "rsi", "rdi", "r8",
    "r9", "r10", "r12", "r13", "r14", "r15"
};
static const char *x86_regs_d[] = {
    "ebx", "ecx", "edx", "esi", "edi", "r8d",
    "r9d", "r10d", "r12d", "r13d", "r14d", "r15d"
};
static const char *x86_regs_w[] = {
    "bx", "cx", "dx", "si", "di", "r8w",
    "r9w", "r10w", "r12w", "r13w", "r14w", "r15w"
};
static const char *x86_regs_b[] = {
    "bl", "cl", "dl", "sil", "dil", "r8b",
    "r9b", "r10b", "r12b", "r13b", "r14b", "r15b"
};

#define X86_MAPPED_COUNT ((int)(sizeof(x86_regs) / sizeof(x86_regs[0])))
#define X86_SPILL_COUNT (16 - X86_MAPPED_COUNT)
#define X86_SCRATCH "rax"
#define X86_ADDR_SCRATCH "r11"
#define X86_SPILL_SYMBOL "__cas_spill"

/* Bit per spill slot actually referenced, so the reservation is only emitted
   when the program really uses one of the spilled virtual registers. */
static unsigned x86_spill_used = 0;

/* Bit per virtual register the source mentions anywhere. A register that is
   never named cannot be holding anything, so the syscall lowering can skip
   saving the machine register it maps to. Defaults to "all of them" so that
   anything reaching the emitter without a scan stays conservative. */
static unsigned mentioned_vregs = 0xffffu;
static const char *rv_regs[] = {
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "s1",
    "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9"
};
static const char *mmix_regs[] = {
    "$32", "$33", "$34", "$35", "$36", "$37", "$38", "$39",
    "$40", "$41", "$42", "$43", "$44", "$45", "$46", "$47"
};
static const char *dcpu_regs[] = {
    "A", "B", "C", "X", "Y", "Z", "I", "J",
    "A", "B", "C", "X", "Y", "Z", "I", "J"
};
/* The old table listed eight machine registers twice, so virtual r0 and r8
   both became ebx and quietly overwrote each other, and it handed out esp and
   ebp as well. esp and ebp belong to the frame and eax and edx are the
   compiler's scratch pair, which leaves four machine registers; the other
   twelve virtual registers get spill slots. Reserving edx also means the
   division sequence has it free without saving anything. */
static const char *i386_regs[] = {
    "ebx", "ecx", "esi", "edi"
};
static const char *i386_regs_w[] = {
    "bx", "cx", "si", "di"
};
/* 32-bit x86 has no 8-bit view of esi or edi, so a byte store from one of
   those has to go through the scratch; ebx and ecx do not. */
static const char *i386_regs_b[] = {
    "bl", "cl", NULL, NULL
};

#define I386_MAPPED_COUNT ((int)(sizeof(i386_regs) / sizeof(i386_regs[0])))
#define I386_SPILL_COUNT (16 - I386_MAPPED_COUNT)
#define I386_SCRATCH "eax"
#define I386_ADDR_SCRATCH "edx"

static unsigned i386_spill_used = 0;
/* The old table ended in sp, lr and pc, so "mov r13, 0" wrote the stack
   pointer and "mov r15, 0" was a jump. r3 and r12 are the compiler's scratch
   pair, r7 carries the syscall number, and sp/lr/pc are the machine's, which
   leaves ten machine registers; virtual r10-r15 get spill slots. */
static const char *arm_regs[] = {
    "r0", "r1", "r2", "r4", "r5", "r6", "r8", "r9", "r10", "r11"
};

#define ARM_MAPPED_COUNT ((int)(sizeof(arm_regs) / sizeof(arm_regs[0])))
#define ARM_SPILL_COUNT (16 - ARM_MAPPED_COUNT)
#define ARM_SCRATCH "r3"
#define ARM_SCRATCH2 "r12"

static unsigned arm_spill_used = 0;
/* Set when something divided, so the divide routine is written out. */
static bool arm_divmod_used = false;

/* Signed division for a machine that has none: r0 divided by r1 leaves the
   quotient in r0 and the remainder in r1, and nothing else changes. It is a
   restoring division, one quotient bit per turn of the loop, and it takes the
   sign off both operands first so that the doubled remainder always stays
   inside the word. The quotient takes the two signs multiplied together and
   the remainder takes the dividend's, which is the rule the rest of the
   language follows. Dividing by zero answers zero rather than trapping,
   because there is no trap to take on a machine with no divide.

   The mnemonics are the ones ARM and Thumb-2 spell the same way, and the
   conditional part is written as branches, since Thumb would need an if-then
   block to run an instruction conditionally. */
static const char arm_divmod_routine[] =
    "\n__commonasm_divmod:\n"
    "  push {r4}\n"
    "  mov r4, #0\n"
    "  cmp r1, #0\n"
    "  beq 9f\n"
    "  bpl 1f\n"
    "  rsbs r1, r1, #0\n"
    "  mov r4, #1\n"
    "1:\n"
    "  cmp r0, #0\n"
    "  bpl 2f\n"
    "  rsbs r0, r0, #0\n"
    "  eor r4, r4, #1\n"
    "  orr r4, r4, #2\n"
    "2:\n"
    "  mov r2, #0\n"
    "  mov r3, #32\n"
    "3:\n"
    "  lsls r0, r0, #1\n"
    "  adcs r2, r2, r2\n"
    "  cmp r2, r1\n"
    "  bcc 4f\n"
    "  sub r2, r2, r1\n"
    "  orr r0, r0, #1\n"
    "4:\n"
    "  subs r3, r3, #1\n"
    "  bne 3b\n"
    "  mov r1, r2\n"
    "  tst r4, #1\n"
    "  beq 5f\n"
    "  rsbs r0, r0, #0\n"
    "5:\n"
    "  tst r4, #2\n"
    "  beq 6f\n"
    "  rsbs r1, r1, #0\n"
    "6:\n"
    "  pop {r4}\n"
    "  bx lr\n"
    "9:\n"
    "  mov r0, #0\n"
    "  mov r1, #0\n"
    "  pop {r4}\n"
    "  bx lr\n";
/* x29, x30 and sp are the frame, link and stack registers, x0-x8 carry the
   Linux syscall ABI, and x18 is the platform register, so none of them appear
   here. AArch64 has enough registers left that all sixteen virtual ones get a
   machine register and nothing has to spill. x16 and x17 are held back as the
   compiler's scratch pair: they are the architecture's designated
   intra-procedure scratch registers, which is exactly this use. */
static const char *aarch64_regs[] = {
    "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26",
    "x27", "x28", "x9", "x10", "x11", "x12", "x13", "x14"
};

#define A64_SCRATCH "x16"
#define A64_SCRATCH2 "x17"
static const char *ia64_regs[] = {
    "r32", "r33", "r34", "r35", "r36", "r37", "r38", "r39",
    "r40", "r41", "r42", "r43", "r44", "r45", "r46", "r47"
};
static const char *loong_regs[] = {
    "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
    "$t8", "$r21", "$a0", "$a1", "$a2", "$a3", "$a4", "$a5"
};
static const char *portable_regs[] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};
/* Which lowering path a target takes. */
typedef enum {
    CLASS_X86_64,
    CLASS_I386,
    CLASS_RISCV,
    CLASS_GENERIC,
    CLASS_LEGACY,
    CLASS_VM_IR,
    CLASS_TOY,
    CLASS_MMIX,
    CLASS_DCPU,
    CLASS_MIPS,
    CLASS_PPC,
    CLASS_SPARC,
    CLASS_M68K,
    CLASS_S390,
    CLASS_WASM,
    CLASS_ENCODING
} TargetClass;

/* How a target is filed under --list-targets, and the support level it
   advertises. Several classes share a group and vice versa. */
typedef enum {
    GROUP_PRIMARY,
    GROUP_I386,
    GROUP_GENERIC,
    GROUP_LEGACY,
    GROUP_VM_IR,
    GROUP_TOY,
    GROUP_SPECIAL,
    GROUP_ENCODING,
    GROUP_COUNT
} TargetGroup;

/* Sub-family bits, for the places where one class still needs to tell its
   members apart (register file, addressing syntax, branch mnemonics), plus
   the per-target instruction availability the extended operations below key
   off. ARM in particular gained clz, rev and rbit in different architecture
   revisions, so those cannot be decided by family. */
enum {
    TF_ARM32 = 1u << 0,
    TF_AARCH64 = 1u << 1,
    TF_RV_GENERIC = 1u << 2,
    TF_IA64 = 1u << 3,
    TF_LOONG = 1u << 4,
    TF_HAS_CLZ = 1u << 5,   /* ARMv5 and later */
    TF_HAS_REV = 1u << 6,   /* ARMv6 and later */
    TF_HAS_RBIT = 1u << 7,  /* ARMv6T2 and later */
    TF_RV_ZBB = 1u << 8,    /* RISC-V bit-manipulation extension */
    TF_64BIT = 1u << 9,     /* a 64-bit member of an otherwise 32-bit family */
    TF_RV32 = 1u << 10      /* the 32-bit member of the RISC-V family */
};

/* Operations the language offers on every target but that only some machines
   have an instruction for. Where the instruction exists it is emitted
   directly; where it does not, the operation is expanded into ordinary
   CommonASM and lowered like any other code. */
enum {
    CAP_POPCNT = 1u << 0,
    CAP_CLZ = 1u << 1,
    CAP_CTZ = 1u << 2,
    CAP_BSWAP = 1u << 3,
    CAP_ROT = 1u << 4,
    CAP_MINMAX = 1u << 5,
    CAP_ABS = 1u << 6,
    CAP_SEL = 1u << 7
};

typedef struct {
    const char *name;
    TargetClass cls;
    TargetGroup group;
    unsigned flags;
} TargetDesc;

/* The single source of truth for every target. Adding a backend is one row
   here plus whatever its class needs; nothing else scans for names. Rows are
   grouped in the order --list-targets prints them. */
static const TargetDesc target_table[] = {
    {"x86_64-nasm", CLASS_X86_64, GROUP_PRIMARY, 0},
    {"riscv64-gnu", CLASS_RISCV, GROUP_PRIMARY, 0},
    {"rv64i-gnu", CLASS_RISCV, GROUP_PRIMARY, 0},
    /* Same lowering as riscv64-gnu, but allowed to use the bit-manipulation
       instructions. Adding it costs one row, which is what the table is for. */
    {"riscv64-zbb", CLASS_RISCV, GROUP_PRIMARY, TF_RV_ZBB},

    {"i386-nasm", CLASS_I386, GROUP_I386, 0},
    {"ia32-nasm", CLASS_I386, GROUP_I386, 0},

    {"armv4-gnu", CLASS_GENERIC, GROUP_GENERIC, TF_ARM32},
    {"armv5-gnu", CLASS_GENERIC, GROUP_GENERIC, TF_ARM32 | TF_HAS_CLZ},
    {"armv7a-gnu", CLASS_GENERIC, GROUP_GENERIC, TF_ARM32 | TF_HAS_CLZ | TF_HAS_REV | TF_HAS_RBIT},
    {"aarch64-gnu", CLASS_GENERIC, GROUP_GENERIC, TF_AARCH64},
    {"thumb-gnu", CLASS_GENERIC, GROUP_GENERIC, TF_ARM32 | TF_HAS_CLZ},
    {"thumb2-gnu", CLASS_GENERIC, GROUP_GENERIC, TF_ARM32 | TF_HAS_CLZ | TF_HAS_REV | TF_HAS_RBIT},
    {"rv32i-gnu", CLASS_RISCV, GROUP_GENERIC, TF_RV32},
    {"rv128i-gnu", CLASS_GENERIC, GROUP_GENERIC, TF_RV_GENERIC},
    {"ia64-gnu", CLASS_GENERIC, GROUP_GENERIC, TF_IA64},
    {"loongarch64-gnu", CLASS_GENERIC, GROUP_GENERIC, TF_LOONG},

    {"mips1-gnu", CLASS_MIPS, GROUP_LEGACY, 0},
    {"mips32-gnu", CLASS_MIPS, GROUP_LEGACY, 0},
    {"mips64-gnu", CLASS_MIPS, GROUP_LEGACY, TF_64BIT},
    {"micromips-gnu", CLASS_MIPS, GROUP_LEGACY, 0},
    {"power1-gnu", CLASS_PPC, GROUP_LEGACY, 0},
    {"power2-gnu", CLASS_PPC, GROUP_LEGACY, 0},
    {"ppc603-gnu", CLASS_PPC, GROUP_LEGACY, 0},
    {"ppcg4-gnu", CLASS_PPC, GROUP_LEGACY, 0},
    {"ppcg5-gnu", CLASS_PPC, GROUP_LEGACY, TF_64BIT},
    {"power9-gnu", CLASS_PPC, GROUP_LEGACY, TF_64BIT},
    {"power10-gnu", CLASS_PPC, GROUP_LEGACY, TF_64BIT},
    {"sparcv8-gnu", CLASS_SPARC, GROUP_LEGACY, 0},
    {"sparcv9-gnu", CLASS_SPARC, GROUP_LEGACY, TF_64BIT},
    {"alpha-gnu", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"parisc-gnu", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"m88k-gnu", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"m68k", CLASS_M68K, GROUP_LEGACY, 0},
    {"coldfire", CLASS_M68K, GROUP_LEGACY, 0},
    {"avr", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"i8051", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"msp430", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"xtensa", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"superh", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"rx", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"nios2", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"microblaze", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"arc", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"ptx", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"amdgcn", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"rdna", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"intelgen", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"cell-spe", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"tms320", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"dsp56000", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"blackfin", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"hexagon", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"ebpf", CLASS_LEGACY, GROUP_LEGACY, 0},
    {"zarch", CLASS_S390, GROUP_LEGACY, 0},

    {"wasm", CLASS_WASM, GROUP_VM_IR, 0},
    {"llvm-ir", CLASS_VM_IR, GROUP_VM_IR, 0},
    {"gcc-gimple", CLASS_VM_IR, GROUP_VM_IR, 0},
    {"gcc-rtl", CLASS_VM_IR, GROUP_VM_IR, 0},
    {"jvm-bytecode", CLASS_VM_IR, GROUP_VM_IR, 0},
    {"cil", CLASS_VM_IR, GROUP_VM_IR, 0},
    {"dalvik", CLASS_VM_IR, GROUP_VM_IR, 0},
    {"lua-bytecode", CLASS_VM_IR, GROUP_VM_IR, 0},
    {"python-bytecode", CLASS_VM_IR, GROUP_VM_IR, 0},
    {"spirv", CLASS_VM_IR, GROUP_VM_IR, 0},
    {"evm", CLASS_VM_IR, GROUP_VM_IR, 0},

    {"mos6502", CLASS_TOY, GROUP_TOY, 0},
    {"wdc65c02", CLASS_TOY, GROUP_TOY, 0},
    {"wdc65816", CLASS_TOY, GROUP_TOY, 0},
    {"mos6510", CLASS_TOY, GROUP_TOY, 0},
    {"i8008", CLASS_TOY, GROUP_TOY, 0},
    {"i8080", CLASS_TOY, GROUP_TOY, 0},
    {"i8085", CLASS_TOY, GROUP_TOY, 0},
    {"z80", CLASS_TOY, GROUP_TOY, 0},
    {"ez80", CLASS_TOY, GROUP_TOY, 0},
    {"m6800", CLASS_TOY, GROUP_TOY, 0},
    {"m6809", CLASS_TOY, GROUP_TOY, 0},
    {"pic16", CLASS_TOY, GROUP_TOY, 0},
    {"pic32", CLASS_TOY, GROUP_TOY, 0},
    {"propeller", CLASS_TOY, GROUP_TOY, 0},
    {"pdp1", CLASS_TOY, GROUP_TOY, 0},
    {"pdp8", CLASS_TOY, GROUP_TOY, 0},
    {"pdp11", CLASS_TOY, GROUP_TOY, 0},
    {"vax", CLASS_TOY, GROUP_TOY, 0},
    {"system360", CLASS_TOY, GROUP_TOY, 0},
    {"system370", CLASS_TOY, GROUP_TOY, 0},
        {"cdc6600", CLASS_TOY, GROUP_TOY, 0},
    {"univac1", CLASS_TOY, GROUP_TOY, 0},
    {"cray1", CLASS_TOY, GROUP_TOY, 0},
    {"mix", CLASS_TOY, GROUP_TOY, 0},
    {"lc3", CLASS_TOY, GROUP_TOY, 0},
    {"lmc", CLASS_TOY, GROUP_TOY, 0},
    {"marie", CLASS_TOY, GROUP_TOY, 0},
    {"chip8", CLASS_TOY, GROUP_TOY, 0},
    {"schip8", CLASS_TOY, GROUP_TOY, 0},
    {"redcode", CLASS_TOY, GROUP_TOY, 0},
    {"subleq", CLASS_TOY, GROUP_TOY, 0},
    {"iota", CLASS_TOY, GROUP_TOY, 0},
    {"jot", CLASS_TOY, GROUP_TOY, 0},
    {"malbolge-asm", CLASS_TOY, GROUP_TOY, 0},
    {"brainfuck", CLASS_TOY, GROUP_TOY, 0},
    {"urisc", CLASS_TOY, GROUP_TOY, 0},
    {"tta", CLASS_TOY, GROUP_TOY, 0},
    {"secd", CLASS_TOY, GROUP_TOY, 0},
    {"pcode", CLASS_TOY, GROUP_TOY, 0},
    {"zmachine", CLASS_TOY, GROUP_TOY, 0},
    {"sweet16", CLASS_TOY, GROUP_TOY, 0},
    {"befunge", CLASS_TOY, GROUP_TOY, 0},
    {"bitblt-vm", CLASS_TOY, GROUP_TOY, 0},
    {"turing-machine", CLASS_TOY, GROUP_TOY, 0},
    {"unlambda", CLASS_TOY, GROUP_TOY, 0},

    {"mmixal", CLASS_MMIX, GROUP_SPECIAL, 0},
    {"dcpu16", CLASS_DCPU, GROUP_SPECIAL, 0},

    {"fractran", CLASS_ENCODING, GROUP_ENCODING, 0},
    {"cellular-automaton", CLASS_ENCODING, GROUP_ENCODING, 0}
};

#define TARGET_COUNT (sizeof(target_table) / sizeof(target_table[0]))
/* Open-addressing set of the constant names the source has defined. Names are
   heap copies, so a long long label is stored whole instead of being cut to fit a
   fixed cell, and the table grows instead of capping how many may exist. */
typedef struct {
    char **slots;
    long long *values;
    bool *known;
    size_t cap;
    size_t count;
} SymbolSet;

static SymbolSet known_constants;
static char **diagnostic_lines = NULL;
static int diagnostic_line_count = 0;
static const char *diagnostic_path = NULL;
static const char *usage_text =
    "usage: commonasmc input.cas|- --target TARGET [-o output|-] [-O0|-O1]\n"
    "                  [--emulate-extended] [--translate-asm]\n"
    "       commonasmc --list-targets\n"
    "       commonasmc --target-info TARGET\n"
    "       commonasmc --version\n"
    "       commonasmc --help";

static void die(const char *message) {
    fprintf(stderr, "commonasmc: error: %s\n", message);
    exit(1);
}

static char *diagnostic_copy_range(const char *start, size_t len) {
    char *copy = malloc(len + 1);
    if (!copy) {
        fprintf(stderr, "commonasmc: error: out of memory\n");
        exit(1);
    }
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

static void set_diagnostic_source(const char *path, const char *source) {
    int count = 1;
    const char *cursor = source;
    diagnostic_path = path;
    for (const char *p = source; *p; p++) {
        if (*p == '\n') count++;
    }
    diagnostic_lines = malloc(sizeof(char *) * (size_t)count);
    if (!diagnostic_lines) {
        fprintf(stderr, "commonasmc: error: out of memory\n");
        exit(1);
    }
    diagnostic_line_count = 0;
    while (*cursor) {
        const char *newline = strchr(cursor, '\n');
        size_t len = newline ? (size_t)(newline - cursor) : strlen(cursor);
        if (len > 0 && cursor[len - 1] == '\r') len--;
        diagnostic_lines[diagnostic_line_count++] = diagnostic_copy_range(cursor, len);
        if (!newline) break;
        cursor = newline + 1;
    }
    if (diagnostic_line_count == 0) {
        diagnostic_lines[diagnostic_line_count++] = diagnostic_copy_range("", 0);
    }
}

static int diagnostic_column_for_token(const char *line, const char *token) {
    const char *found;
    if (!line || !*line) return 1;
    if (token && *token) {
        found = strstr(line, token);
        if (found) return (int)(found - line) + 1;
    }
    for (int i = 0; line[i]; i++) {
        if (!isspace((unsigned char)line[i])) return i + 1;
    }
    return 1;
}

static int diagnostic_token_width(const char *line, const char *token, int column) {
    int width = 0;
    if (token && *token && line && strstr(line, token)) {
        return (int)strlen(token);
    }
    if (!line || column < 1) return 1;
    for (int i = column - 1; line[i] && !isspace((unsigned char)line[i]) && line[i] != ','; i++) {
        width++;
    }
    return width > 0 ? width : 1;
}

static void line_error_token(int line_no, const char *token, const char *label, const char *message) {
    const char *red = "\x1b[1;31m";
    const char *yellow = "\x1b[1;33m";
    const char *cyan = "\x1b[1;36m";
    const char *dim = "\x1b[2m";
    const char *reset = "\x1b[0m";
    const char *line = NULL;
    int column = 1;
    int width = 1;
    if (diagnostic_lines && line_no >= 1 && line_no <= diagnostic_line_count) {
        line = diagnostic_lines[line_no - 1];
        column = diagnostic_column_for_token(line, token);
        width = diagnostic_token_width(line, token, column);
    }
    fprintf(stderr, "%scommonasmc: error:%s %s%s%s\n", red, reset, yellow, message, reset);
    if (diagnostic_path) {
        fprintf(stderr, "%s  --> %s:%d:%d%s\n", cyan, diagnostic_path, line_no, column, reset);
    } else {
        fprintf(stderr, "%s  --> line %d:%d%s\n", cyan, line_no, column, reset);
    }
    fprintf(stderr, "%s   |\n%s", dim, reset);
    if (line) {
        fprintf(stderr, "%s%3d |%s %s\n", cyan, line_no, reset, line);
        fprintf(stderr, "%s   |%s ", dim, reset);
        for (int i = 1; i < column; i++) fputc(' ', stderr);
        fprintf(stderr, "%s", red);
        for (int i = 0; i < width; i++) fputc('^', stderr);
        fprintf(stderr, "%s %s%s%s\n", reset, yellow, label ? label : (token ? token : "token"), reset);
    }
    fprintf(stderr, "%s   |\n%s", dim, reset);
    exit(1);
}

static void line_error(int line_no, const char *op, const char *message) {
    line_error_token(line_no, op, op, message);
}

static void *xmalloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        die("out of memory");
    }
    return ptr;
}

static char *xstrdup(const char *text) {
    size_t len = strlen(text);
    char *copy = xmalloc(len + 1);
    memcpy(copy, text, len + 1);
    return copy;
}

static void buf_init(Buffer *buf) {
    buf->cap = 4096;
    buf->len = 0;
    buf->data = xmalloc(buf->cap);
    buf->data[0] = '\0';
}

static void buf_grow(Buffer *buf, size_t extra) {
    while (buf->len + extra + 1 > buf->cap) {
        buf->cap *= 2;
        buf->data = realloc(buf->data, buf->cap);
        if (!buf->data) {
            die("out of memory");
        }
    }
}

static void buf_append_char(Buffer *buf, char ch) {
    buf_grow(buf, 1);
    buf->data[buf->len++] = ch;
    buf->data[buf->len] = '\0';
}

static void buf_append(Buffer *buf, const char *text) {
    size_t extra = strlen(text);
    buf_grow(buf, extra);
    memcpy(buf->data + buf->len, text, extra + 1);
    buf->len += extra;
}

#if defined(__GNUC__)
#define CAS_PRINTF(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#define CAS_PRINTF(fmt_index, first_arg)
#endif

/* Formats straight into the buffer, growing it instead of capping the line
   length. Callers no longer have to pad unused arguments with NULL. */
static void buf_appendf(Buffer *buf, const char *fmt, ...) CAS_PRINTF(2, 3);

static void buf_appendf(Buffer *buf, const char *fmt, ...) {
    va_list args;
    va_list retry;
    int written;
    va_start(args, fmt);
    va_copy(retry, args);
    written = vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, args);
    va_end(args);
    if (written < 0) {
        va_end(retry);
        die("could not format generated line");
    }
    if ((size_t)written + 1 > buf->cap - buf->len) {
        buf_grow(buf, (size_t)written);
        written = vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, retry);
        if (written < 0) {
            va_end(retry);
            die("could not format generated line");
        }
    }
    va_end(retry);
    buf->len += (size_t)written;
}

static char *read_file(const char *path) {
    FILE *file = stdin;
    Buffer data;
    char chunk[4096];
    size_t read_count;
    bool should_close = false;
    if (strcmp(path, "-") != 0) {
        file = fopen(path, "rb");
        should_close = true;
    }
    if (!file) {
        die("could not open input file");
    }
    buf_init(&data);
    while ((read_count = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        buf_grow(&data, read_count);
        memcpy(data.data + data.len, chunk, read_count);
        data.len += read_count;
        data.data[data.len] = '\0';
    }
    if (ferror(file)) {
        die("could not read input file");
    }
    if (should_close && fclose(file) != 0) {
        die("could not close input file");
    }
    return data.data;
}

static void write_file_or_stdout(const char *path, const Buffer *out) {
    FILE *file = stdout;
    bool should_close = false;
    if (path && strcmp(path, "-") != 0) {
        file = fopen(path, "wb");
        should_close = true;
        if (!file) {
            die("could not open output file");
        }
    }
    if (out->len > 0 && fwrite(out->data, 1, out->len, file) != out->len) {
        die("could not write output");
    }
    if (should_close) {
        if (fclose(file) != 0) {
            die("could not close output file");
        }
    } else if (fflush(file) != 0) {
        die("could not flush output");
    }
}

static char *trim(char *text) {
    char *end;
    while (isspace((unsigned char)*text)) {
        text++;
    }
    if (*text == '\0') {
        return text;
    }
    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }
    return text;
}

static void strip_comment(char *line) {
    bool in_string = false;
    bool escaped = false;
    int bracket_depth = 0;
    for (size_t i = 0; line[i]; i++) {
        char ch = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\' && in_string) {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string && ch == '[') {
            bracket_depth++;
        } else if (!in_string && ch == ']' && bracket_depth > 0) {
            bracket_depth--;
        }
        if (!in_string && bracket_depth == 0 && (ch == ';' || ch == '#')) {
            line[i] = '\0';
            return;
        }
    }
}

static bool is_int(const char *text) {
    char *end = NULL;
    /* Called on every operand, most of which are not numbers at all. The
       leading character rules those out without entering strtoll; the set
       accepted here is exactly the set strtoll would start on. */
    if (!isdigit((unsigned char)text[0]) && text[0] != '-' && text[0] != '+' &&
        !isspace((unsigned char)text[0])) {
        return false;
    }
    errno = 0;
    strtoll(text, &end, 0);
    return errno == 0 && end && *end == '\0' && end != text;
}

/* Opcode dispatch is a chain of these, so the first character is compared
   inline and the call is only reached by a candidate that could still match. */
static bool op_is(const char *op, const char *name) {
    return op[0] == name[0] && strcmp(op + 1, name + 1) == 0;
}

static bool is_symbol_char(char ch) {
    return isalnum((unsigned char)ch) || ch == '_' || ch == '.';
}

static bool is_symbol(const char *text) {
    if (!text || (!isalpha((unsigned char)text[0]) && text[0] != '_' && text[0] != '.')) {
        return false;
    }
    for (size_t i = 1; text[i]; i++) {
        if (!is_symbol_char(text[i])) {
            return false;
        }
    }
    return true;
}

/* One run only ever compiles for one target, so the table scan is memoized and
   every is_*_target() predicate below settles into a field compare. */
static const TargetDesc *target_lookup(const char *name) {
    static const TargetDesc *cached = NULL;
    if (!name) return NULL;
    if (cached && strcmp(cached->name, name) == 0) {
        return cached;
    }
    for (size_t i = 0; i < TARGET_COUNT; i++) {
        if (strcmp(target_table[i].name, name) == 0) {
            cached = &target_table[i];
            return cached;
        }
    }
    return NULL;
}

static bool target_has_class(const char *target, TargetClass cls) {
    const TargetDesc *desc = target_lookup(target);
    return desc != NULL && desc->cls == cls;
}

static bool target_has_flag(const char *target, unsigned flag) {
    const TargetDesc *desc = target_lookup(target);
    return desc != NULL && (desc->flags & flag) != 0;
}

static bool is_i386_target(const char *target) {
    return target_has_class(target, CLASS_I386);
}

static bool is_riscv_target(const char *target) {
    return target_has_class(target, CLASS_RISCV);
}

static bool is_generic_arch_target(const char *target) {
    return target_has_class(target, CLASS_GENERIC);
}

static bool is_legacy_arch_target(const char *target) {
    return target_has_class(target, CLASS_LEGACY);
}

static bool is_mips_target(const char *target) {
    return target_has_class(target, CLASS_MIPS);
}

static bool is_ppc_target(const char *target) {
    return target_has_class(target, CLASS_PPC);
}

static bool is_sparc_target(const char *target) {
    return target_has_class(target, CLASS_SPARC);
}

static bool is_m68k_target(const char *target) {
    return target_has_class(target, CLASS_M68K);
}

static bool is_s390_target(const char *target) {
    return target_has_class(target, CLASS_S390);
}

static bool is_wasm_target(const char *target) {
    return target_has_class(target, CLASS_WASM);
}

static bool is_vm_ir_target(const char *target) {
    return target_has_class(target, CLASS_VM_IR);
}

static bool is_toy_target(const char *target) {
    return target_has_class(target, CLASS_TOY);
}

static bool is_arm32_target(const char *target) {
    return target_has_flag(target, TF_ARM32);
}

static bool is_aarch64_target(const char *target) {
    return target_has_flag(target, TF_AARCH64);
}

static bool is_rv_generic_target(const char *target) {
    return target_has_flag(target, TF_RV_GENERIC);
}

static bool is_ia64_target(const char *target) {
    return target_has_flag(target, TF_IA64);
}

static bool is_loong_target(const char *target) {
    return target_has_flag(target, TF_LOONG);
}

static bool is_supported_target(const char *target) {
    return target_lookup(target) != NULL;
}

/* Which extended operations this target has an instruction for. Everything
   else is expanded into ordinary CommonASM, so the operation still works, it
   just costs more. */
/* Set by --emulate-extended, which makes every extended operation take the
   expanded path. It exists for machines that lack the optional instructions
   their architecture defines - an x86-64 without POPCNT, say - and it lets the
   expansions be tested on a target that would otherwise never use them. */
static bool force_extended_fallback = false;

/* Set by --translate-asm. An inline block written for another machine is
   lifted back into CommonASM and lowered for this one, rather than being
   skipped for want of an arm somebody did not write. */
static bool translate_inline_asm = false;

static unsigned target_caps(const char *target) {
    const TargetDesc *desc = target_lookup(target);
    if (!desc || force_extended_fallback) return 0;
    switch (desc->cls) {
        case CLASS_X86_64:
            /* popcnt needs POPCNT and lzcnt/tzcnt need LZCNT/BMI1; both are
               reported by --target-info so the requirement is not hidden.
               min, max and sel are a compare and a cmov, which is 686. */
            return CAP_POPCNT | CAP_CLZ | CAP_CTZ | CAP_BSWAP | CAP_ROT |
                   CAP_MINMAX | CAP_SEL;
        case CLASS_I386:
            /* bswap is 486, and the rotates are 386. bsr and bsf could serve
               for clz and ctz but they leave the result undefined for a zero
               input, so those are expanded instead. */
            return CAP_BSWAP | CAP_ROT;
        case CLASS_RISCV:
            /* Zbb carries min and max themselves, as three-operand
               instructions, but nothing for abs or a select. */
            return (desc->flags & TF_RV_ZBB)
                 ? (CAP_POPCNT | CAP_CLZ | CAP_CTZ | CAP_BSWAP | CAP_ROT | CAP_MINMAX) : 0;
        case CLASS_WASM:
            /* wasm counts bits and rotates natively, but has nothing that
               reverses bytes. */
            return CAP_POPCNT | CAP_CLZ | CAP_CTZ | CAP_ROT;
        case CLASS_GENERIC:
            if (desc->flags & TF_AARCH64) {
                /* csel decides min, max and sel without a branch, and cneg
                   is abs once the sign has been tested. */
                return CAP_POPCNT | CAP_CLZ | CAP_CTZ | CAP_BSWAP | CAP_ROT |
                       CAP_MINMAX | CAP_ABS | CAP_SEL;
            }
            if (desc->flags & TF_ARM32) {
                unsigned caps = CAP_ROT; /* the barrel shifter is always there */
                if (desc->flags & TF_HAS_CLZ) caps |= CAP_CLZ;
                if (desc->flags & TF_HAS_REV) caps |= CAP_BSWAP;
                /* ctz is rbit followed by clz, so it needs both. */
                if ((desc->flags & TF_HAS_RBIT) && (desc->flags & TF_HAS_CLZ)) caps |= CAP_CTZ;
                return caps;
            }
            return 0;
        default:
            return 0;
    }
}

/* The natural register width, which the expansions need in order to build the
   right masks and shift distances. */
static int target_word_bits(const char *target) {
    const TargetDesc *desc = target_lookup(target);
    if (!desc) return 64;
    if (desc->cls == CLASS_DCPU) return 16;
    if (desc->cls == CLASS_I386) return 32;
    if (desc->cls == CLASS_MIPS || desc->cls == CLASS_PPC || desc->cls == CLASS_SPARC) {
        return (desc->flags & TF_64BIT) ? 64 : 32;
    }
    if (desc->cls == CLASS_M68K) return 32;
    if (desc->cls == CLASS_S390) return 64;
    if (desc->cls == CLASS_RISCV) return (desc->flags & TF_RV32) ? 32 : 64;
    if (desc->cls == CLASS_GENERIC && (desc->flags & TF_ARM32)) return 32;
    return 64;
}

/* The highest virtual register a target can actually name, which is what the
   expansions have to stay inside when they borrow one. */
static int target_max_vreg(const char *target) {
    return target_has_class(target, CLASS_DCPU) ? 7 : 15;
}

typedef struct {
    const char *name;
    unsigned cap;
    int argc;
} ExtendedOp;

static const ExtendedOp extended_ops[] = {
    {"popcnt", CAP_POPCNT, 1},
    {"clz", CAP_CLZ, 1},
    {"ctz", CAP_CTZ, 1},
    {"bswap", CAP_BSWAP, 1},
    {"rol", CAP_ROT, 2},
    {"ror", CAP_ROT, 2},
    {"min", CAP_MINMAX, 2},
    {"max", CAP_MINMAX, 2},
    {"abs", CAP_ABS, 1},
    {"sel", CAP_SEL, 3}
};

#define EXTENDED_OP_COUNT (sizeof(extended_ops) / sizeof(extended_ops[0]))

static const ExtendedOp *extended_op_lookup(const char *name) {
    for (size_t i = 0; i < EXTENDED_OP_COUNT; i++) {
        if (strcmp(extended_ops[i].name, name) == 0) return &extended_ops[i];
    }
    return NULL;
}

static const char *group_title(TargetGroup group) {
    switch (group) {
        case GROUP_PRIMARY: return "Primary";
        case GROUP_I386: return "i386 aliases";
        case GROUP_GENERIC: return "Mainstream/generic assembly";
        case GROUP_LEGACY: return "Experimental assembly/IR";
        case GROUP_VM_IR: return "VM/IR";
        case GROUP_TOY: return "Encoding/pseudo";
        case GROUP_SPECIAL: return "Special assembly";
        case GROUP_ENCODING: return "Source encoding";
        default: return "Other";
    }
}

static void print_target_list(void) {
    puts("CommonASM targets\n");
    for (int group = 0; group < GROUP_COUNT; group++) {
        printf("%s:\n", group_title((TargetGroup)group));
        for (size_t i = 0; i < TARGET_COUNT; i++) {
            if (target_table[i].group == (TargetGroup)group) {
                printf("  %s\n", target_table[i].name);
            }
        }
        printf("\n");
    }
}

static const char *target_support_level(const char *target) {
    const TargetDesc *desc = target_lookup(target);
    if (!desc) return "Unknown";
    switch (desc->group) {
        case GROUP_PRIMARY: return "Primary";
        case GROUP_I386:
        case GROUP_GENERIC:
        case GROUP_LEGACY:
        case GROUP_SPECIAL: return "Experimental assembly/IR";
        case GROUP_VM_IR: return "VM/IR";
        case GROUP_TOY:
        case GROUP_ENCODING: return "Encoding/pseudo";
        default: return "Unknown";
    }
}

/* True when the target has a backend of its own rather than sharing the
   assembly-shaped text emitters. AArch64 and ARM32 sit in the generic class
   for their register files but have had their own emitters for a while, so
   the class alone is not the answer. */
static bool target_emits_assembly(const TargetDesc *desc) {
    switch (desc->cls) {
        case CLASS_X86_64: case CLASS_I386: case CLASS_RISCV: case CLASS_MIPS:
        case CLASS_PPC: case CLASS_SPARC: case CLASS_M68K: case CLASS_S390:
        case CLASS_WASM:
            return true;
        case CLASS_GENERIC:
            return (desc->flags & (TF_AARCH64 | TF_ARM32)) != 0;
        default:
            return false;
    }
}

static const char *target_output_kind(const char *target) {
    const TargetDesc *desc = target_lookup(target);
    if (!desc) return "unknown output";
    if (desc->cls == CLASS_GENERIC && (desc->flags & TF_AARCH64)) return "GNU AArch64 assembly";
    if (desc->cls == CLASS_GENERIC && (desc->flags & TF_ARM32)) return "GNU ARM assembly";
    switch (desc->cls) {
        case CLASS_X86_64: return "NASM x86-64 assembly";
        case CLASS_RISCV: return (desc->flags & TF_RV32) ? "GNU RISC-V 32 assembly" : "GNU RISC-V 64 assembly";
        case CLASS_I386: return "NASM i386 assembly";
        case CLASS_MIPS: return "GNU MIPS assembly";
        case CLASS_PPC: return "GNU PowerPC assembly";
        case CLASS_SPARC: return "GNU SPARC assembly";
        case CLASS_M68K: return "GNU m68k assembly";
        case CLASS_S390: return "GNU z/Architecture assembly";
        case CLASS_WASM: return "WebAssembly text";
        case CLASS_GENERIC:
        case CLASS_LEGACY: return "assembly-style text output";
        case CLASS_VM_IR: return "VM or compiler IR-style text output";
        case CLASS_MMIX: return "MMIXAL assembly-style output";
        case CLASS_DCPU: return "DCPU-16 assembly-style output";
        case CLASS_TOY: return "pseudo assembly or toy-machine text output";
        case CLASS_ENCODING: return "source encoding output";
        default: return "unknown output";
    }
}

static const char *target_portability_note(const char *target) {
    const TargetDesc *desc = target_lookup(target);
    if (!desc) return "Unknown target.";
    if (target_emits_assembly(desc) && desc->group != GROUP_PRIMARY) {
        return "Assembly an assembler accepts; the portable subset, not a complete ABI-level port.";
    }
    switch (desc->group) {
        case GROUP_PRIMARY:
            return "Reference backend for CommonASM portable semantics.";
        case GROUP_I386:
        case GROUP_GENERIC:
        case GROUP_LEGACY:
        case GROUP_SPECIAL:
            return "Portable subset output; not a complete ABI-level port.";
        case GROUP_VM_IR:
            return "Portable subset represented as readable VM/IR text.";
        case GROUP_TOY:
        case GROUP_ENCODING:
            return "Experimental pseudo or encoding output for very different machine models.";
        default:
            return "Unknown target.";
    }
}

static void print_target_info(const char *target) {
    if (!is_supported_target(target)) {
        fprintf(stderr, "commonasmc: error: unknown target '%s'; run commonasmc --list-targets\n", target);
        exit(1);
    }
    printf("target: %s\n", target);
    printf("support: %s\n", target_support_level(target));
    printf("output: %s\n", target_output_kind(target));
    printf("note: %s\n", target_portability_note(target));
    printf("word: %d-bit\n", target_word_bits(target));
    {
        unsigned caps = target_caps(target);
        printf("extended native:");
        if (caps == 0) {
            printf(" none");
        } else {
            for (size_t i = 0; i < EXTENDED_OP_COUNT; i++) {
                if (caps & extended_ops[i].cap) printf(" %s", extended_ops[i].name);
            }
        }
        printf("\n");
        printf("extended expanded:");
        if (caps == (CAP_POPCNT | CAP_CLZ | CAP_CTZ | CAP_BSWAP | CAP_ROT)) {
            printf(" none");
        } else {
            for (size_t i = 0; i < EXTENDED_OP_COUNT; i++) {
                if (!(caps & extended_ops[i].cap)) printf(" %s", extended_ops[i].name);
            }
        }
        printf("\n");
        if (target_has_class(target, CLASS_X86_64) && caps != 0) {
            printf("requires: POPCNT for popcnt, LZCNT/BMI1 for clz and ctz;"
                   " use --emulate-extended for CPUs without them\n");
        }
    }
}

static size_t symbol_hash(const char *text) {
    size_t hash = 1469598103934665603u; /* FNV-1a */
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        hash ^= (size_t)*p;
        hash *= 1099511628211u;
    }
    return hash;
}

/* Returns the slot holding name, or the empty slot where it belongs. */
static size_t symbol_slot(char *const *slots, size_t cap, const char *name) {
    size_t mask = cap - 1;
    size_t index = symbol_hash(name) & mask;
    while (slots[index] && strcmp(slots[index], name) != 0) {
        index = (index + 1) & mask;
    }
    return index;
}

static void symbol_set_grow(SymbolSet *set) {
    size_t new_cap = set->cap ? set->cap * 2 : 64;
    char **new_slots = xmalloc(sizeof(char *) * new_cap);
    long long *new_values = xmalloc(sizeof(long long) * new_cap);
    bool *new_known = xmalloc(sizeof(bool) * new_cap);
    for (size_t i = 0; i < new_cap; i++) {
        new_slots[i] = NULL;
        new_values[i] = 0;
        new_known[i] = false;
    }
    for (size_t i = 0; i < set->cap; i++) {
        if (set->slots[i]) {
            size_t index = symbol_slot(new_slots, new_cap, set->slots[i]);
            new_slots[index] = set->slots[i];
            new_values[index] = set->values[i];
            new_known[index] = set->known[i];
        }
    }
    free(set->slots);
    free(set->values);
    free(set->known);
    set->slots = new_slots;
    set->values = new_values;
    set->known = new_known;
    set->cap = new_cap;
}

/* Constants carry their value where the source gave a plain number, because a
   backend that has to build the value cannot ask the assembler what it is:
   AArch64's mov reaches only a restricted set of immediates, so "mov x19,
   #VGA" is rejected for a constant like 0xb8000 that the compiler could have
   materialised had it known. */
static void symbol_set_add(SymbolSet *set, const char *name, const char *value_text) {
    size_t index;
    if ((set->count + 1) * 4 >= set->cap * 3) {
        symbol_set_grow(set);
    }
    index = symbol_slot(set->slots, set->cap, name);
    if (!set->slots[index]) {
        set->slots[index] = xstrdup(name);
        set->count++;
    }
    if (value_text && is_int(value_text)) {
        char *end = NULL;
        errno = 0;
        set->values[index] = strtoll(value_text, &end, 0);
        set->known[index] = errno == 0;
    }
}

static bool symbol_set_value(const SymbolSet *set, const char *name, long long *value) {
    size_t index;
    if (set->count == 0) return false;
    index = symbol_slot(set->slots, set->cap, name);
    if (!set->slots[index] || !set->known[index]) return false;
    *value = set->values[index];
    return true;
}

static bool symbol_set_contains(const SymbolSet *set, const char *name) {
    if (set->count == 0) {
        return false;
    }
    return set->slots[symbol_slot(set->slots, set->cap, name)] != NULL;
}

static void symbol_set_free(SymbolSet *set) {
    for (size_t i = 0; i < set->cap; i++) {
        free(set->slots[i]);
    }
    free(set->slots);
    free(set->values);
    free(set->known);
    set->values = NULL;
    set->known = NULL;
    set->slots = NULL;
    set->cap = 0;
    set->count = 0;
}

static void remember_constant(const char *name, const char *value_text) {
    symbol_set_add(&known_constants, name, value_text);
}

static bool constant_value(const char *name, long long *value) {
    return symbol_set_value(&known_constants, name, value);
}

static bool is_known_constant(const char *name) {
    return symbol_set_contains(&known_constants, name);
}

/* Runs on every operand of every instruction, so it reads the digits directly
   rather than going through strtoll. */
static int virtual_reg_index(const char *name) {
    const char *p = name + 1;
    int value = 0;
    if (name[0] != 'r' || !isdigit((unsigned char)*p)) {
        return -1;
    }
    while (*p == '0' && isdigit((unsigned char)p[1])) {
        p++;
    }
    for (; isdigit((unsigned char)*p); p++) {
        value = value * 10 + (*p - '0');
        if (value > 15) {
            return -1;
        }
    }
    return *p == '\0' ? value : -1;
}

/* The operand naming virtual register idx at the given size. A mapped
   register is a table entry and is returned as-is; only a spill slot has to
   be formatted, and those come out of a small rotation because no emitted
   instruction holds more than a handful of operands at once. x86 allows at
   most one memory operand per instruction, so callers that pair two operands
   route one through X86_SCRATCH when both are spilled. */
static const char *x86_operand_text(int idx, const char *size) {
    static char pool[8][48];
    static unsigned next = 0;
    char *slot;
    if (idx < X86_MAPPED_COUNT) {
        switch (size[0]) {
            case 'b': return x86_regs_b[idx];
            case 'w': return x86_regs_w[idx];
            case 'd': return x86_regs_d[idx];
            default: return x86_regs[idx];
        }
    }
    slot = pool[next];
    next = (next + 1) % 8;
    x86_spill_used |= 1u << (idx - X86_MAPPED_COUNT);
    snprintf(slot, sizeof(pool[0]), "[rel %s+%d]", X86_SPILL_SYMBOL, (idx - X86_MAPPED_COUNT) * 8);
    return slot;
}

static bool x86_reg_is_spilled(const char *value) {
    int reg = virtual_reg_index(value);
    return reg >= X86_MAPPED_COUNT;
}

static const char *x86_reg_sized(const char *value, const char *size, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        line_error_token(line_no, value, op, "expected virtual register r0-r15");
    }
    return x86_operand_text(reg, size);
}

static const char *x86_reg(const char *value, int line_no, const char *op) {
    return x86_reg_sized(value, "q", line_no, op);
}

static const char *x86_rax_sized(const char *size) {
    if (strcmp(size, "b") == 0) return "al";
    if (strcmp(size, "w") == 0) return "ax";
    if (strcmp(size, "d") == 0) return "eax";
    return "rax";
}

static const char *rv_reg(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        line_error_token(line_no, value, op, "expected virtual register r0-r15");
    }
    return rv_regs[reg];
}

static const char *mmix_reg(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        line_error_token(line_no, value, op, "expected virtual register r0-r15");
    }
    return mmix_regs[reg];
}

/* DCPU-16 has eight registers, so the other eight virtual ones live in a
   reserved block of memory. The machine addresses memory in most operand
   positions, so a slot reads and writes like a register does. */
#define DCPU_MAPPED_COUNT 8
#define DCPU_SPILL_COUNT (16 - DCPU_MAPPED_COUNT)

static unsigned dcpu_spill_used = 0;

static const char *dcpu_reg(const char *value, int line_no, const char *op) {
    static char pool[4][48];
    static unsigned next = 0;
    char *slot;
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        line_error_token(line_no, value, op, "expected virtual register r0-r15");
    }
    if (reg < DCPU_MAPPED_COUNT) return dcpu_regs[reg];
    dcpu_spill_used |= 1u << (reg - DCPU_MAPPED_COUNT);
    slot = pool[next];
    next = (next + 1) % 4;
    snprintf(slot, sizeof(pool[0]), "[%s+%d]", X86_SPILL_SYMBOL, reg - DCPU_MAPPED_COUNT);
    return slot;
}

static const char *generic_reg_for_target(const char *value, const char *target, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        line_error_token(line_no, value, op, "expected virtual register r0-r15");
    }
    /* i386 and AArch64 have their own emitters and their own register files;
       nothing routes them here. */
    if (is_arm32_target(target)) return arm_regs[reg];
    if (is_rv_generic_target(target)) return rv_regs[reg];
    if (is_ia64_target(target)) return ia64_regs[reg];
    if (is_loong_target(target)) return loong_regs[reg];
    if (is_legacy_arch_target(target) || is_vm_ir_target(target) || is_toy_target(target)) return portable_regs[reg];
    return rv_regs[reg];
}

/* Splits "load.q" into its opcode and size suffix. Runs once per line, so the
   opcode is copied rather than formatted. */
static const char *size_suffix_or_default(const char *op, char *base_op, size_t base_size) {
    const char *dot = strchr(op, '.');
    size_t len = dot ? (size_t)(dot - op) : strlen(op);
    if (len >= base_size) {
        len = base_size - 1;
    }
    memcpy(base_op, op, len);
    base_op[len] = '\0';
    if (!dot) {
        return "q";
    }
    if (dot[1] != '\0' && dot[2] == '\0') {
        switch (dot[1]) {
            case 'b': case 'w': case 'd': case 'q': return dot + 1;
            default: break;
        }
    }
    return NULL;
}

static const char *x86_size_word(const char *size) {
    if (strcmp(size, "b") == 0) return "byte";
    if (strcmp(size, "w") == 0) return "word";
    if (strcmp(size, "d") == 0) return "dword";
    return "qword";
}

/* On rv32 there is no eight-byte load, so an unsuffixed access is the
   four-byte one, which is that machine's word. */
static const char *rv_load_op(const char *size, bool wide) {
    if (strcmp(size, "b") == 0) return "lb";
    if (strcmp(size, "w") == 0) return "lh";
    if (strcmp(size, "d") == 0) return "lw";
    return wide ? "ld" : "lw";
}

static const char *rv_store_op(const char *size, bool wide) {
    if (strcmp(size, "b") == 0) return "sb";
    if (strcmp(size, "w") == 0) return "sh";
    if (strcmp(size, "d") == 0) return "sw";
    return wide ? "sd" : "sw";
}

/* Over-approximates which virtual registers the program mentions by scanning
   the raw source: a hit inside a string or a comment still counts. Erring that
   way only ever costs an extra save, never a lost value. */
static unsigned scan_mentioned_vregs(const char *source) {
    unsigned mask = 0;
    for (const char *p = source; *p; p++) {
        char *end = NULL;
        long long value;
        if (*p != 'r' || !isdigit((unsigned char)p[1])) continue;
        if (p != source && (isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '.')) continue;
        value = strtoll(p + 1, &end, 10);
        if (end && value >= 0 && value <= 15 && !is_symbol_char(*end)) {
            mask |= 1u << (unsigned)value;
        }
    }
    return mask;
}

/* Index of the virtual register a machine register carries, or -1. */
static int x86_vreg_of(const char *physical) {
    for (int i = 0; i < X86_MAPPED_COUNT; i++) {
        if (strcmp(x86_regs[i], physical) == 0) return i;
    }
    return -1;
}

static bool x86_must_preserve(const char *physical) {
    int vreg = x86_vreg_of(physical);
    return vreg >= 0 && (mentioned_vregs & (1u << (unsigned)vreg)) != 0;
}

static const char *x86_operand(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg >= 0) {
        return x86_operand_text(reg, "q");
    }
    if (is_int(value) || is_symbol(value) || is_known_constant(value)) {
        return value;
    }
    line_error_token(line_no, value, op, "expected register, integer, symbol, or constant");
    return value;
}

/* True when combining these two operands would put two memory references in
   one instruction, which x86 cannot encode. */
static bool x86_needs_scratch(const char *dst, const char *src) {
    return x86_reg_is_spilled(dst) && x86_reg_is_spilled(src);
}

/* Outside mov, an x86-64 instruction carries at most a sign-extended 32-bit
   immediate. A wider one assembles with a truncation warning rather than an
   error, so it has to be caught here and materialised instead. */
static bool x86_imm_too_wide(const char *value) {
    long long parsed;
    char *end = NULL;
    if (!is_int(value)) return false;
    errno = 0;
    parsed = strtoll(value, &end, 0);
    if (errno != 0) return true;
    return parsed < -2147483647LL - 1 || parsed > 2147483647LL;
}

static int split_args(char *arg_text, char **args, int max_args) {
    int count = 0;
    int bracket_depth = 0;
    bool in_string = false;
    char *cursor = arg_text;
    char *start = arg_text;
    while (*cursor && count < max_args) {
        if (*cursor == '"') {
            in_string = !in_string;
        } else if (!in_string && *cursor == '[') {
            bracket_depth++;
        } else if (!in_string && *cursor == ']' && bracket_depth > 0) {
            bracket_depth--;
        } else if (!in_string && bracket_depth == 0 && *cursor == ',') {
            *cursor = '\0';
            args[count++] = trim(start);
            start = cursor + 1;
        }
        cursor++;
    }
    if (*trim(start) && count < max_args) {
        args[count++] = trim(start);
    }
    return count;
}

static bool parse_address(const char *text, Address *addr) {
    char tmp[256];
    char *expr;
    char *sign = NULL;
    memset(addr, 0, sizeof(*addr));
    snprintf(tmp, sizeof(tmp), "%s", text);
    expr = trim(tmp);
    if (expr[0] == '[') {
        size_t len = strlen(expr);
        if (len < 3 || expr[len - 1] != ']') {
            return false;
        }
        expr[len - 1] = '\0';
        expr = trim(expr + 1);
    }
    for (char *p = expr + 1; *p; p++) {
        if (*p == '+' || *p == '-') {
            sign = p;
            break;
        }
    }
    if (sign) {
        char sign_char = *sign;
        *sign = '\0';
        addr->offset = strtoll(trim(sign + 1), NULL, 0);
        if (sign_char == '-') {
            addr->offset = -addr->offset;
        }
    }
    expr = trim(expr);
    if (virtual_reg_index(expr) >= 0) {
        snprintf(addr->base, sizeof(addr->base), "%s", expr);
        addr->has_base = true;
    } else if (is_symbol(expr)) {
        snprintf(addr->symbol, sizeof(addr->symbol), "%s", expr);
        addr->has_symbol = true;
    } else {
        return false;
    }
    return true;
}

/* Writes the x86 memory operand for an address expression. A spilled base
   register cannot be named inside an address, so it is first loaded into the
   address scratch, which is kept separate from the value scratch precisely so
   that one instruction can need both. */
static void x86_emit_address(Buffer *text, const char *addr_text, char *out, size_t out_size,
                             int line_no, const char *op) {
    Address addr;
    const char *base;
    if (!parse_address(addr_text, &addr)) {
        line_error_token(line_no, addr_text, op, "expected address like [r0 + 8] or [symbol + 8]");
    }
    if (!addr.has_base) {
        if (addr.offset == 0) snprintf(out, out_size, "[rel %s]", addr.symbol);
        else snprintf(out, out_size, "[rel %s%+lld]", addr.symbol, addr.offset);
        return;
    }
    if (x86_reg_is_spilled(addr.base)) {
        buf_appendf(text, "  mov %s, %s\n", X86_ADDR_SCRATCH, x86_reg(addr.base, line_no, op));
        base = X86_ADDR_SCRATCH;
    } else {
        base = x86_reg(addr.base, line_no, op);
    }
    if (addr.offset == 0) snprintf(out, out_size, "[%s]", base);
    else snprintf(out, out_size, "[%s%+lld]", base, addr.offset);
}

static void rv_emit_address_setup(Buffer *text, const char *addr_text, const char *scratch, int line_no, const char *op) {
    Address addr;
    char offset[64];
    if (!parse_address(addr_text, &addr)) {
        line_error_token(line_no, addr_text, op, "expected address like [r0 + 8] or [symbol + 8]");
    }
    snprintf(offset, sizeof(offset), "%lld", addr.offset);
    if (addr.has_base) {
        buf_appendf(text, "%s", "");
    } else {
        if (addr.offset == 0) {
            buf_appendf(text, "  la %s, %s\n", scratch, addr.symbol);
        } else {
            buf_appendf(text, "  la %s, %s\n", scratch, addr.symbol);
            buf_appendf(text, "  addi %s, %s, ", scratch, scratch);
            buf_append(text, offset);
            buf_append(text, "\n");
        }
    }
}

static const char *rv_address_base(const char *addr_text, const char *scratch, int line_no, const char *op, long long *offset) {
    Address addr;
    if (!parse_address(addr_text, &addr)) {
        line_error_token(line_no, addr_text, op, "expected address like [r0 + 8] or [symbol + 8]");
    }
    *offset = addr.has_base ? addr.offset : 0;
    return addr.has_base ? rv_reg(addr.base, line_no, op) : scratch;
}

static void emit_data_line(Buffer *out, Buffer *constants, char *line, int line_no, const char *target, const char *section) {
    char *colon = strchr(line, ':');
    char *name;
    char *kind;
    const bool x86 = strcmp(target, "x86_64-nasm") == 0;
    const bool rv = is_riscv_target(target);
    const bool mmix = strcmp(target, "mmixal") == 0;
    const bool dcpu = strcmp(target, "dcpu16") == 0;
    const bool generic = is_i386_target(target) || is_generic_arch_target(target) || is_legacy_arch_target(target) || is_mips_target(target) || is_ppc_target(target) || is_sparc_target(target) || is_m68k_target(target) || is_s390_target(target) || is_vm_ir_target(target) || is_toy_target(target);
    /* Both NASM targets take NASM directives. i386 used to fall through to the
       GNU spellings, which produced ".equ msg_len, 10" in a file NASM was
       about to read. */
    const bool nasm = x86 || is_i386_target(target);
    if (strncmp(line, "align ", 6) == 0) {
        const char *value = trim(line + 6);
        if (nasm) buf_appendf(out, "align %s\n", value);
        else if (rv) buf_appendf(out, ".balign %s\n", value);
        else if (mmix) buf_appendf(out, "        %% align %s\n", value);
        else if (dcpu) buf_appendf(out, "        ; align %s\n", value);
        else if (generic) buf_appendf(out, ".balign %s\n", value);
        return;
    }
    if (!colon) {
        line_error(line_no, "data", "expected name: directive");
    }
    *colon = '\0';
    name = trim(line);
    kind = trim(colon + 1);
    /* A label has to be aligned for the instructions that will reach it. Most
       of these machines fault on a word load from an odd address, and
       z/Architecture needs every label even whatever it holds: larl encodes
       half the distance to its target, so an odd address cannot be named at
       all, and asking for one gets the label before it instead. A one-byte
       newline sitting behind a one-byte space printed the space. */
    if (rv || (generic && !nasm && !is_toy_target(target) && !is_vm_ir_target(target))) {
        int align = 1;
        if (strncmp(kind, "word", 4) == 0 && isspace((unsigned char)kind[4])) align = 2;
        else if (strncmp(kind, "dword", 5) == 0 && isspace((unsigned char)kind[5])) align = 4;
        else if (strncmp(kind, "qword", 5) == 0 && isspace((unsigned char)kind[5])) align = 8;
        else if (strncmp(kind, "zero", 4) == 0 && isspace((unsigned char)kind[4])) {
            align = target_word_bits(target) / 8;
        }
        if (is_s390_target(target) && align < 2) align = 2;
        if (align > 1) buf_appendf(out, ".balign %d\n", align);
    }
    if (strncmp(kind, "string", 6) == 0 && isspace((unsigned char)kind[6])) {
        char *quote = trim(kind + 6);
        int byte_count = 0;
        if (*quote != '"') {
            line_error(line_no, "string", "expected string literal");
        }
        if (dcpu) buf_appendf(out, ":%s DAT ", name);
        else if (generic && is_toy_target(target)) buf_appendf(out, "%s: data ", name);
        else buf_appendf(out, "%s: %s ", name, nasm ? "db" : (mmix ? "BYTE" : ".byte"));
        quote++;
        for (size_t i = 0; quote[i] && quote[i] != '"'; i++) {
            unsigned char ch = (unsigned char)quote[i];
            char num[32];
            if (ch == '\\') {
                i++;
                if (quote[i] == 'n') ch = '\n';
                else if (quote[i] == 't') ch = '\t';
                else if (quote[i] == '0') ch = '\0';
                else ch = (unsigned char)quote[i];
            }
            snprintf(num, sizeof(num), "%s%u", byte_count == 0 ? "" : ", ", ch);
            buf_append(out, num);
            byte_count++;
        }
        buf_append(out, "\n");
        /* Sized from the label itself so that a long long name keeps its whole
           "<name>_len" spelling instead of being cut to fit a fixed buffer. */
        char *len_name = xmalloc(strlen(name) + sizeof("_len"));
        char len_text[32];
        sprintf(len_name, "%s_len", name);
        snprintf(len_text, sizeof(len_text), "%d", byte_count);
        remember_constant(len_name, len_text);
        free(len_name);
        if (nasm) buf_appendf(constants, "%s_len equ %d\n", name, byte_count);
        else if (rv) buf_appendf(constants, ".equ %s_len, %d\n", name, byte_count);
        else if (mmix) buf_appendf(constants, "%s_len IS %d\n", name, byte_count);
        else if (dcpu) buf_appendf(constants, "%s_len EQU %d\n", name, byte_count);
        else if (generic && !is_toy_target(target)) buf_appendf(constants, ".equ %s_len, %d\n", name, byte_count);
        else if (generic) buf_appendf(constants, "; const %s_len = %d\n", name, byte_count);
        return;
    }
    if (strncmp(kind, "zero", 4) == 0 && isspace((unsigned char)kind[4])) {
        const char *value = trim(kind + 4);
        if (dcpu) {
            buf_appendf(out, ":%s DAT ", name);
            buf_append(out, value);
            buf_append(out, " DUP(0)\n");
        } else if (mmix) {
            buf_appendf(out, "%s LOC @+%s\n", name, value);
        } else if (nasm && strcmp(section, "bss") == 0) {
            /* .bss holds no initialised bytes, so it has to be a reservation. */
            buf_appendf(out, "%s: resb %s\n", name, value);
        } else if (nasm) {
            buf_appendf(out, "%s: times %s db 0\n", name, value);
        } else if (generic && is_toy_target(target)) {
            buf_appendf(out, "%s: zero %s\n", name, value);
        } else {
            buf_appendf(out, "%s: .zero %s\n", name, value);
        }
        return;
    }
    const char *cas_dir = NULL;
    const char *x86_dir = NULL;
    const char *rv_dir = NULL;
    if (strncmp(kind, "bytes", 5) == 0 && isspace((unsigned char)kind[5])) {
        cas_dir = "bytes"; x86_dir = "db"; rv_dir = ".byte"; kind = trim(kind + 5);
    } else if (strncmp(kind, "byte", 4) == 0 && isspace((unsigned char)kind[4])) {
        cas_dir = "byte"; x86_dir = "db"; rv_dir = ".byte"; kind = trim(kind + 4);
    } else if (strncmp(kind, "word", 4) == 0 && isspace((unsigned char)kind[4])) {
        cas_dir = "word"; x86_dir = "dw"; rv_dir = ".word"; kind = trim(kind + 4);
    } else if (strncmp(kind, "dword", 5) == 0 && isspace((unsigned char)kind[5])) {
        cas_dir = "dword"; x86_dir = "dd"; rv_dir = ".long"; kind = trim(kind + 5);
    } else if (strncmp(kind, "qword", 5) == 0 && isspace((unsigned char)kind[5])) {
        cas_dir = "qword"; x86_dir = "dq"; rv_dir = ".quad"; kind = trim(kind + 5);
    }
    if (!cas_dir) {
        line_error(line_no, "data", "expected string, bytes, byte, word, dword, qword, zero, or align");
    }
    if (dcpu) {
        buf_appendf(out, ":%s DAT ", name);
    } else if (mmix) {
        const char *mmix_dir = strcmp(cas_dir, "word") == 0 ? "WYDE" : (strcmp(cas_dir, "dword") == 0 ? "TETRA" : (strcmp(cas_dir, "qword") == 0 ? "OCTA" : "BYTE"));
        buf_appendf(out, "%s: %s ", name, mmix_dir);
    } else if (generic && is_toy_target(target)) {
        buf_appendf(out, "%s: data ", name);
    } else {
        buf_appendf(out, "%s: %s ", name, nasm ? x86_dir : rv_dir);
    }
    buf_append(out, kind);
    buf_append(out, "\n");
}

/* A syscall may name a virtual register to receive its result: "syscall r0,
   read, ..." puts the count read into r0. Without it there is no way to tell
   a short read from a full one, or to notice an error. */
static const char *syscall_result_operand(char ***args, int *argc) {
    const char *result = NULL;
    if (*argc >= 2 && virtual_reg_index((*args)[0]) >= 0) {
        result = (*args)[0];
        (*args)++;
        (*argc)--;
    }
    return result;
}

static void emit_x86_syscall(Buffer *text, char **args, int argc, int line_no) {
    static const char *const arg_regs[] = {"rdi", "rsi", "rdx", "r10", "r8", "r9"};
    const char *result = syscall_result_operand(&args, &argc);
    int number = -1;
    int count;
    bool conflict = false;
    bool returns = true;
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    if (strcmp(args[0], "read") == 0) number = 0;
    else if (strcmp(args[0], "write") == 0) number = 1;
    else if (strcmp(args[0], "open") == 0) number = 2;
    else if (strcmp(args[0], "close") == 0) number = 3;
    else if (strcmp(args[0], "exit") == 0) { number = 60; returns = false; }
    else line_error(line_no, "syscall", "unknown syscall");
    count = argc - 1;
    if (count > 6) count = 6;

    /* Setting up the call writes the argument registers, and the syscall
       instruction itself destroys rcx and r11. rax and r11 are compiler
       scratch, but the argument registers and rcx carry virtual registers, so
       they are saved across the call instead of being silently destroyed.
       exit never comes back, so it skips all of that. */
    if (returns) {
        if (x86_must_preserve("rcx")) buf_append(text, "  push rcx\n");
        for (int i = 0; i < count; i++) {
            if (x86_must_preserve(arg_regs[i])) buf_appendf(text, "  push %s\n", arg_regs[i]);
        }
    }

    /* Loading one argument register can destroy the source of a later one, so
       check for that before choosing the cheap in-order form. */
    for (int i = 0; i < count && !conflict; i++) {
        const char *src = x86_operand(args[i + 1], line_no, "syscall");
        for (int j = 0; j < count; j++) {
            if (strcmp(src, arg_regs[j]) == 0) {
                conflict = true;
                break;
            }
        }
    }
    if (conflict) {
        /* Every source is still intact here, so stage the values on the stack
           and take them back in reverse. */
        for (int i = 0; i < count; i++) {
            buf_appendf(text, "  push %s%s\n", x86_reg_is_spilled(args[i + 1]) ? "qword " : "",
                        x86_operand(args[i + 1], line_no, "syscall"));
        }
        for (int i = count - 1; i >= 0; i--) {
            buf_appendf(text, "  pop %s\n", arg_regs[i]);
        }
    } else {
        for (int i = 0; i < count; i++) {
            buf_appendf(text, "  mov %s, %s\n", arg_regs[i], x86_operand(args[i + 1], line_no, "syscall"));
        }
    }
    buf_appendf(text, "  mov rax, %d\n", number);
    buf_append(text, "  syscall\n");
    if (returns) {
        for (int i = count - 1; i >= 0; i--) {
            if (x86_must_preserve(arg_regs[i])) buf_appendf(text, "  pop %s\n", arg_regs[i]);
        }
        if (x86_must_preserve("rcx")) buf_append(text, "  pop rcx\n");
    }
    /* rax is not one of the restored registers, so the result survives them. */
    if (result) {
        buf_appendf(text, "  mov %s%s, %s\n", x86_reg_is_spilled(result) ? "qword " : "",
                    x86_reg(result, line_no, "syscall"), X86_SCRATCH);
    }
}

static void emit_rv_syscall(Buffer *text, char **args, int argc, int line_no) {
    const char *arg_regs[] = {"a0", "a1", "a2", "a3", "a4", "a5"};
    const char *result = syscall_result_operand(&args, &argc);
    int number = -1;
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    if (strcmp(args[0], "read") == 0) number = 63;
    else if (strcmp(args[0], "write") == 0) number = 64;
    else if (strcmp(args[0], "open") == 0) number = 1024;
    else if (strcmp(args[0], "close") == 0) number = 57;
    else if (strcmp(args[0], "exit") == 0) number = 93;
    else line_error(line_no, "syscall", "unknown syscall");
    for (int i = 1; i < argc && i <= 6; i++) {
        int reg = virtual_reg_index(args[i]);
        if (reg >= 0) buf_appendf(text, "  mv %s, %s\n", arg_regs[i - 1], rv_regs[reg]);
        else if (is_int(args[i]) || is_known_constant(args[i])) buf_appendf(text, "  li %s, %s\n", arg_regs[i - 1], args[i]);
        else buf_appendf(text, "  la %s, %s\n", arg_regs[i - 1], args[i]);
    }
    char num[32];
    snprintf(num, sizeof(num), "%d", number);
    buf_appendf(text, "  li a7, %s\n", num);
    buf_append(text, "  ecall\n");
    if (result) buf_appendf(text, "  mv %s, a0\n", rv_reg(result, line_no, "syscall"));
}

static const char *mmix_operand(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg >= 0) return mmix_regs[reg];
    if (is_int(value) || is_symbol(value) || is_known_constant(value)) return value;
    line_error_token(line_no, value, op, "expected register, integer, symbol, or constant");
    return value;
}

static const char *dcpu_operand(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg >= 0) return dcpu_reg(value, line_no, op);
    if (is_int(value) || is_symbol(value) || is_known_constant(value)) return value;
    line_error_token(line_no, value, op, "expected register, integer, symbol, or constant");
    return value;
}

static const char *generic_operand(const char *value, const char *target, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg >= 0) return generic_reg_for_target(value, target, line_no, op);
    if (is_int(value) || is_symbol(value) || is_known_constant(value)) return value;
    line_error_token(line_no, value, op, "expected register, integer, symbol, or constant");
    return value;
}

static const char *generic_comment(const char *target) {
    return is_i386_target(target) ? ";" : "@";
}

static void generic_format_address(const char *text, const char *target, char *out, size_t out_size, int line_no, const char *op) {
    Address addr;
    if (!parse_address(text, &addr)) line_error_token(line_no, text, op, "expected address like [r0 + 8] or [symbol + 8]");
    if (is_i386_target(target)) {
        if (addr.has_base) snprintf(out, out_size, "[%s%s%lld]", generic_reg_for_target(addr.base, target, line_no, op), addr.offset < 0 ? "" : "+", addr.offset);
        else snprintf(out, out_size, "[%s%s%lld]", addr.symbol, addr.offset < 0 ? "" : "+", addr.offset);
        if (addr.offset == 0) {
            if (addr.has_base) snprintf(out, out_size, "[%s]", generic_reg_for_target(addr.base, target, line_no, op));
            else snprintf(out, out_size, "[%s]", addr.symbol);
        }
    } else if (is_arm32_target(target) || is_aarch64_target(target)) {
        if (addr.has_base) snprintf(out, out_size, "[%s, #%lld]", generic_reg_for_target(addr.base, target, line_no, op), addr.offset);
        else snprintf(out, out_size, "=%s%+lld", addr.symbol, addr.offset);
        if (addr.has_base && addr.offset == 0) snprintf(out, out_size, "[%s]", generic_reg_for_target(addr.base, target, line_no, op));
    } else if (is_rv_generic_target(target) || is_loong_target(target)) {
        if (addr.has_base) snprintf(out, out_size, "%lld(%s)", addr.offset, generic_reg_for_target(addr.base, target, line_no, op));
        else snprintf(out, out_size, "%s%+lld", addr.symbol, addr.offset);
        if (addr.has_symbol && addr.offset == 0) snprintf(out, out_size, "%s", addr.symbol);
    } else if (is_ia64_target(target)) {
        if (addr.has_base) snprintf(out, out_size, "[%s],%lld", generic_reg_for_target(addr.base, target, line_no, op), addr.offset);
        else snprintf(out, out_size, "%s%+lld", addr.symbol, addr.offset);
        if (addr.has_symbol && addr.offset == 0) snprintf(out, out_size, "%s", addr.symbol);
    } else {
        snprintf(out, out_size, "%s", text);
    }
}

static void mmix_format_address(const char *text, char *out, size_t out_size, int line_no, const char *op) {
    Address addr;
    if (!parse_address(text, &addr)) line_error_token(line_no, text, op, "expected address like [r0 + 8] or [symbol + 8]");
    if (addr.has_base) snprintf(out, out_size, "%lld,%s", addr.offset, mmix_reg(addr.base, line_no, op));
    else snprintf(out, out_size, "%s%+lld", addr.symbol, addr.offset);
}

static void dcpu_format_address(const char *text, char *out, size_t out_size, int line_no, const char *op) {
    Address addr;
    if (!parse_address(text, &addr)) line_error_token(line_no, text, op, "expected address like [r0 + 8] or [symbol + 8]");
    if (addr.has_base) {
        if (addr.offset == 0) snprintf(out, out_size, "[%s]", dcpu_reg(addr.base, line_no, op));
        else snprintf(out, out_size, "[%s%+lld]", dcpu_reg(addr.base, line_no, op), addr.offset);
    } else {
        if (addr.offset == 0) snprintf(out, out_size, "[%s]", addr.symbol);
        else snprintf(out, out_size, "[%s%+lld]", addr.symbol, addr.offset);
    }
}

static void emit_mmix_syscall(Buffer *text, char **args, int argc, int line_no) {
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    buf_appendf(text, "        %% syscall %s lowered as MMIX TRAP placeholder\n", args[0]);
    if (strcmp(args[0], "exit") == 0) buf_append(text, "        TRAP 0,Halt,0\n");
    else buf_append(text, "        TRAP 0,Fputs,StdOut\n");
}

static void emit_dcpu_syscall(Buffer *text, char **args, int argc, int line_no) {
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    buf_appendf(text, "        ; syscall %s lowered as DCPU software interrupt placeholder\n", args[0]);
    buf_append(text, "        INT 0\n");
}

static void emit_mmix_instruction(Buffer *text, const char *op, const char *size, char **args, int argc, int line_no) {
    (void)size;
    if (op_is(op, "func") && argc == 1) { buf_appendf(text, "%s:\n", args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) return;
    if (op_is(op, "enter") && argc == 1) { buf_appendf(text, "        SUBU $254,$254,%s\n", args[0]); return; }
    if (op_is(op, "leave") && argc == 0) return;
    if (op_is(op, "mov") && argc == 2) { buf_appendf(text, "        SET %s,%s\n", mmix_reg(args[0], line_no, op), mmix_operand(args[1], line_no, op)); return; }
    if (op_is(op, "load_addr") && argc == 2) { buf_appendf(text, "        LDA %s,%s\n", mmix_reg(args[0], line_no, op), args[1]); return; }
    if (op_is(op, "load") && argc == 2) {
        char addr[256]; mmix_format_address(args[1], addr, sizeof(addr), line_no, op);
        buf_appendf(text, "        LDO %s,%s\n", mmix_reg(args[0], line_no, op), addr); return;
    }
    if (op_is(op, "store") && argc == 2) {
        char addr[256]; mmix_format_address(args[0], addr, sizeof(addr), line_no, op);
        buf_appendf(text, "        STO %s,%s\n", mmix_operand(args[1], line_no, op), addr); return;
    }
    if ((op_is(op, "add") || op_is(op, "sub") || op_is(op, "mul") || op_is(op, "div") ||
         op_is(op, "and") || op_is(op, "or") || op_is(op, "xor")) && argc == 2) {
        const char *native = op_is(op, "add") ? "ADD" : op_is(op, "sub") ? "SUB" : op_is(op, "mul") ? "MUL" : op_is(op, "div") ? "DIV" : op_is(op, "and") ? "AND" : op_is(op, "or") ? "OR" : "XOR";
        buf_appendf(text, "        %s %s,%s,", native, mmix_reg(args[0], line_no, op), mmix_reg(args[0], line_no, op));
        buf_append(text, mmix_operand(args[1], line_no, op)); buf_append(text, "\n"); return;
    }
    if ((op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar")) && argc == 2) {
        const char *native = op_is(op, "shl") ? "SL" : "SR";
        buf_appendf(text, "        %s %s,%s,", native, mmix_reg(args[0], line_no, op), mmix_reg(args[0], line_no, op));
        buf_append(text, mmix_operand(args[1], line_no, op)); buf_append(text, "\n"); return;
    }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") || op_is(op, "dec")) && argc == 1) {
        if (op_is(op, "neg")) buf_appendf(text, "        NEG %s,0,%s\n", mmix_reg(args[0], line_no, op), mmix_reg(args[0], line_no, op));
        else if (op_is(op, "not")) buf_appendf(text, "        NOR %s,%s,%s\n", mmix_reg(args[0], line_no, op), mmix_reg(args[0], line_no, op), mmix_reg(args[0], line_no, op));
        else buf_appendf(text, op_is(op, "inc") ? "        ADD %s,%s,1\n" : "        SUB %s,%s,1\n", mmix_reg(args[0], line_no, op), mmix_reg(args[0], line_no, op));
        return;
    }
    if (op_is(op, "mod") && argc == 2) { buf_append(text, "        % mod is target-runtime dependent on MMIX; quotient/remainder omitted\n"); return; }
    if (op_is(op, "cmp") && argc == 2) { buf_appendf(text, "        CMP $48,%s,%s\n", mmix_reg(args[0], line_no, op), mmix_operand(args[1], line_no, op)); return; }
    if (op_is(op, "je") && argc == 1) { buf_appendf(text, "        BZ $48,%s\n", args[0]); return; }
    if (op_is(op, "jne") && argc == 1) { buf_appendf(text, "        BNZ $48,%s\n", args[0]); return; }
    if ((op_is(op, "jg") || op_is(op, "ja")) && argc == 1) { buf_appendf(text, "        BP $48,%s\n", args[0]); return; }
    if ((op_is(op, "jl") || op_is(op, "jb")) && argc == 1) { buf_appendf(text, "        BN $48,%s\n", args[0]); return; }
    if ((op_is(op, "jge") || op_is(op, "jae")) && argc == 1) { buf_appendf(text, "        BNN $48,%s\n", args[0]); return; }
    if ((op_is(op, "jle") || op_is(op, "jbe")) && argc == 1) { buf_appendf(text, "        BNP $48,%s\n", args[0]); return; }
    if (op_is(op, "push") && argc == 1) { buf_appendf(text, "        STO %s,0,$254\n        SUBU $254,$254,8\n", mmix_operand(args[0], line_no, op)); return; }
    if (op_is(op, "pop") && argc == 1) { buf_appendf(text, "        ADDU $254,$254,8\n        LDO %s,0,$254\n", mmix_reg(args[0], line_no, op)); return; }
    if (op_is(op, "jmp") && argc == 1) { buf_appendf(text, "        JMP %s\n", args[0]); return; }
    if (op_is(op, "call") && argc == 1) { buf_appendf(text, "        PUSHJ $15,%s\n", args[0]); return; }
    if (op_is(op, "ret") && argc == 0) { buf_append(text, "        POP 0,0\n"); return; }
    if (op_is(op, "syscall")) { emit_mmix_syscall(text, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count for MMIX");
}

static void emit_dcpu_instruction(Buffer *text, const char *op, const char *size, char **args, int argc, int line_no) {
    (void)size;
    if (op_is(op, "func") && argc == 1) { buf_appendf(text, ":%s\n", args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) return;
    if (op_is(op, "enter") && argc == 1) { buf_appendf(text, "        SUB SP, %s\n", args[0]); return; }
    if (op_is(op, "leave") && argc == 0) return;
    if (op_is(op, "mov") && argc == 2) { buf_appendf(text, "        SET %s, %s\n", dcpu_reg(args[0], line_no, op), dcpu_operand(args[1], line_no, op)); return; }
    if (op_is(op, "load_addr") && argc == 2) { buf_appendf(text, "        SET %s, %s\n", dcpu_reg(args[0], line_no, op), args[1]); return; }
    if (op_is(op, "load") && argc == 2) { char addr[256]; dcpu_format_address(args[1], addr, sizeof(addr), line_no, op); buf_appendf(text, "        SET %s, %s\n", dcpu_reg(args[0], line_no, op), addr); return; }
    if (op_is(op, "store") && argc == 2) { char addr[256]; dcpu_format_address(args[0], addr, sizeof(addr), line_no, op); buf_appendf(text, "        SET %s, %s\n", addr, dcpu_operand(args[1], line_no, op)); return; }
    if ((op_is(op, "add") || op_is(op, "sub") || op_is(op, "mul") || op_is(op, "div") ||
         op_is(op, "mod") || op_is(op, "and") || op_is(op, "xor") || op_is(op, "shl") ||
         op_is(op, "shr") || op_is(op, "sar")) && argc == 2) {
        /* MLI, DVI and MDI are the signed forms, which is what the language
           means by mul, div and mod; MUL, DIV and MOD are unsigned. ASR is
           the arithmetic shift the older lowering claimed did not exist. */
        const char *native = op_is(op, "or") ? "BOR" : op_is(op, "shl") ? "SHL" :
                             op_is(op, "shr") ? "SHR" : op_is(op, "sar") ? "ASR" :
                             op_is(op, "and") ? "AND" : op_is(op, "xor") ? "XOR" :
                             op_is(op, "mod") ? "MDI" : op_is(op, "mul") ? "MLI" :
                             op_is(op, "div") ? "DVI" : op_is(op, "sub") ? "SUB" : "ADD";
        buf_appendf(text, "        %s %s, %s\n", native, dcpu_reg(args[0], line_no, op), dcpu_operand(args[1], line_no, op)); return;
    }
    if (op_is(op, "or") && argc == 2) { buf_appendf(text, "        BOR %s, %s\n", dcpu_reg(args[0], line_no, op), dcpu_operand(args[1], line_no, op)); return; }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") || op_is(op, "dec")) && argc == 1) {
        if (op_is(op, "neg")) { buf_appendf(text, "        XOR %s, 0xffff\n        ADD %s, 1\n", dcpu_reg(args[0], line_no, op), dcpu_reg(args[0], line_no, op)); return; }
        if (op_is(op, "not")) { buf_appendf(text, "        XOR %s, 0xffff\n", dcpu_reg(args[0], line_no, op)); return; }
        if (op_is(op, "inc")) { buf_appendf(text, "        ADD %s, 1\n", dcpu_reg(args[0], line_no, op)); return; }
        buf_appendf(text, "        SUB %s, 1\n", dcpu_reg(args[0], line_no, op)); return;
    }
    if (op_is(op, "cmp") && argc == 2) { buf_appendf(text, "        SET EX, %s\n        SUB EX, %s\n", dcpu_reg(args[0], line_no, op), dcpu_operand(args[1], line_no, op)); return; }
    if (op_is(op, "je") && argc == 1) { buf_appendf(text, "        IFE EX, 0\n        SET PC, %s\n", args[0]); return; }
    if (op_is(op, "jne") && argc == 1) { buf_appendf(text, "        IFN EX, 0\n        SET PC, %s\n", args[0]); return; }
    if ((op_is(op, "jg") || op_is(op, "ja")) && argc == 1) { buf_appendf(text, "        IFG EX, 0\n        SET PC, %s\n", args[0]); return; }
    if ((op_is(op, "jl") || op_is(op, "jb")) && argc == 1) { buf_appendf(text, "        IFL EX, 0\n        SET PC, %s\n", args[0]); return; }
    if ((op_is(op, "jge") || op_is(op, "jae")) && argc == 1) { buf_appendf(text, "        IFG EX, 0xffff\n        SET PC, %s\n", args[0]); return; }
    if ((op_is(op, "jle") || op_is(op, "jbe")) && argc == 1) { buf_appendf(text, "        IFL EX, 1\n        SET PC, %s\n", args[0]); return; }
    if (op_is(op, "jmp") && argc == 1) { buf_appendf(text, "        SET PC, %s\n", args[0]); return; }
    if (op_is(op, "call") && argc == 1) { buf_appendf(text, "        JSR %s\n", args[0]); return; }
    if (op_is(op, "ret") && argc == 0) { buf_append(text, "        SET PC, POP\n"); return; }
    if (op_is(op, "push") && argc == 1) { buf_appendf(text, "        SET PUSH, %s\n", dcpu_operand(args[0], line_no, op)); return; }
    if (op_is(op, "pop") && argc == 1) { buf_appendf(text, "        SET %s, POP\n", dcpu_reg(args[0], line_no, op)); return; }
    if (op_is(op, "syscall")) { emit_dcpu_syscall(text, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count for DCPU-16");
}

static void emit_generic_syscall(Buffer *text, const char *target, char **args, int argc, int line_no) {
    /* These targets render the call rather than making one, but the operand
       still has to be stepped over so the name is found. */
    (void)syscall_result_operand(&args, &argc);
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    if (is_i386_target(target)) {
        int number = -1;
        if (strcmp(args[0], "exit") == 0) number = 1;
        else if (strcmp(args[0], "read") == 0) number = 3;
        else if (strcmp(args[0], "write") == 0) number = 4;
        else if (strcmp(args[0], "open") == 0) number = 5;
        else if (strcmp(args[0], "close") == 0) number = 6;
        else line_error(line_no, "syscall", "unknown syscall");
        char num[32]; snprintf(num, sizeof(num), "%d", number);
        buf_appendf(text, "  mov eax, %s\n", num);
        if (argc > 1) buf_appendf(text, "  mov ebx, %s\n", generic_operand(args[1], target, line_no, "syscall"));
        if (argc > 2) buf_appendf(text, "  mov ecx, %s\n", generic_operand(args[2], target, line_no, "syscall"));
        if (argc > 3) buf_appendf(text, "  mov edx, %s\n", generic_operand(args[3], target, line_no, "syscall"));
        buf_append(text, "  int 0x80\n");
        return;
    }
    buf_appendf(text, "  %s syscall ", generic_comment(target));
    buf_append(text, args[0]);
    buf_append(text, " lowered as target runtime call placeholder\n");
    if (is_arm32_target(target) || is_aarch64_target(target)) {
        buf_append(text, is_aarch64_target(target) ? "  svc #0\n" : "  svc #0\n");
    } else if (is_rv_generic_target(target)) {
        buf_append(text, "  ecall\n");
    } else if (is_ia64_target(target)) {
        buf_append(text, "  break 0x100000\n");
    } else if (is_loong_target(target)) {
        buf_append(text, "  syscall 0\n");
    }
}

static void emit_generic_instruction(Buffer *text, const char *target, const char *op, const char *size, char **args, int argc, int line_no) {
    (void)size;
    const char *c = generic_comment(target);
    if (op_is(op, "func") && argc == 1) { buf_appendf(text, "%s:\n", args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) return;
    if (op_is(op, "enter") && argc == 1) {
        if (is_i386_target(target)) buf_appendf(text, "  push ebp\n  mov ebp, esp\n  sub esp, %s\n", args[0]);
        else if (is_aarch64_target(target)) buf_appendf(text, "  stp x29, x30, [sp, #-16]!\n  mov x29, sp\n  sub sp, sp, #%s\n", args[0]);
        else if (is_arm32_target(target)) buf_appendf(text, "  push {fp, lr}\n  mov fp, sp\n  sub sp, sp, #%s\n", args[0]);
        else buf_appendf(text, "  %s enter %s\n", c, args[0]);
        return;
    }
    if (op_is(op, "leave") && argc == 0) {
        if (is_i386_target(target)) buf_append(text, "  mov esp, ebp\n  pop ebp\n");
        else if (is_aarch64_target(target)) buf_append(text, "  mov sp, x29\n  ldp x29, x30, [sp], #16\n");
        else if (is_arm32_target(target)) buf_append(text, "  mov sp, fp\n  pop {fp, lr}\n");
        else buf_appendf(text, "  %s leave\n", c);
        return;
    }
    if (op_is(op, "mov") && argc == 2) {
        if (is_i386_target(target)) buf_appendf(text, "  mov %s, %s\n", generic_reg_for_target(args[0], target, line_no, op), generic_operand(args[1], target, line_no, op));
        else if (is_arm32_target(target) || is_aarch64_target(target)) buf_appendf(text, "  mov %s, %s\n", generic_reg_for_target(args[0], target, line_no, op), generic_operand(args[1], target, line_no, op));
        else if (is_loong_target(target)) buf_appendf(text, "  ori %s, %s, 0\n", generic_reg_for_target(args[0], target, line_no, op), generic_operand(args[1], target, line_no, op));
        else buf_appendf(text, "  mov %s = %s\n", generic_reg_for_target(args[0], target, line_no, op), generic_operand(args[1], target, line_no, op));
        return;
    }
    if (op_is(op, "load_addr") && argc == 2) {
        if (is_i386_target(target)) buf_appendf(text, "  lea %s, [%s]\n", generic_reg_for_target(args[0], target, line_no, op), args[1]);
        else if (is_arm32_target(target) || is_aarch64_target(target)) buf_appendf(text, "  adr %s, %s\n", generic_reg_for_target(args[0], target, line_no, op), args[1]);
        else if (is_rv_generic_target(target) || is_loong_target(target)) buf_appendf(text, "  la %s, %s\n", generic_reg_for_target(args[0], target, line_no, op), args[1]);
        else buf_appendf(text, "  addl %s = @gprel(%s), gp\n", generic_reg_for_target(args[0], target, line_no, op), args[1]);
        return;
    }
    if ((op_is(op, "load") || op_is(op, "store")) && argc == 2) {
        char addr[256]; generic_format_address(op_is(op, "load") ? args[1] : args[0], target, addr, sizeof(addr), line_no, op);
        if (op_is(op, "load")) {
            if (is_i386_target(target)) buf_appendf(text, "  mov %s, %s\n", generic_reg_for_target(args[0], target, line_no, op), addr);
            else if (is_arm32_target(target) || is_aarch64_target(target)) buf_appendf(text, "  ldr %s, %s\n", generic_reg_for_target(args[0], target, line_no, op), addr);
            else if (is_rv_generic_target(target)) buf_appendf(text, "  lw %s, %s\n", generic_reg_for_target(args[0], target, line_no, op), addr);
            else if (is_loong_target(target)) buf_appendf(text, "  ld.d %s, %s\n", generic_reg_for_target(args[0], target, line_no, op), addr);
            else buf_appendf(text, "  ld8 %s = %s\n", generic_reg_for_target(args[0], target, line_no, op), addr);
        } else {
            if (is_i386_target(target)) buf_appendf(text, "  mov %s, %s\n", addr, generic_operand(args[1], target, line_no, op));
            else if (is_arm32_target(target) || is_aarch64_target(target)) buf_appendf(text, "  str %s, %s\n", generic_operand(args[1], target, line_no, op), addr);
            else if (is_rv_generic_target(target)) buf_appendf(text, "  sw %s, %s\n", generic_operand(args[1], target, line_no, op), addr);
            else if (is_loong_target(target)) buf_appendf(text, "  st.d %s, %s\n", generic_operand(args[1], target, line_no, op), addr);
            else buf_appendf(text, "  st8 %s = %s\n", addr, generic_operand(args[1], target, line_no, op));
        }
        return;
    }
    if ((op_is(op, "add") || op_is(op, "sub") || op_is(op, "mul") || op_is(op, "div") ||
         op_is(op, "mod") || op_is(op, "and") || op_is(op, "or") || op_is(op, "xor") ||
         op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar")) && argc == 2) {
        const char *native = op_is(op, "shl") ? (is_i386_target(target) ? "shl" : "lsl") : op_is(op, "shr") ? (is_i386_target(target) ? "shr" : "lsr") : op_is(op, "sar") ? (is_i386_target(target) ? "sar" : "asr") : op;
        if (is_i386_target(target)) {
            buf_appendf(text, "  %s %s, ", native, generic_reg_for_target(args[0], target, line_no, op));
            buf_append(text, generic_operand(args[1], target, line_no, op));
            buf_append(text, "\n");
        } else {
            buf_appendf(text, "  %s %s, %s, ", native, generic_reg_for_target(args[0], target, line_no, op), generic_reg_for_target(args[0], target, line_no, op));
            buf_append(text, generic_operand(args[1], target, line_no, op));
            buf_append(text, "\n");
        }
        return;
    }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") || op_is(op, "dec")) && argc == 1) {
        if (is_i386_target(target)) {
            if (op_is(op, "not") || op_is(op, "neg")) buf_appendf(text, "  %s %s\n", op, generic_reg_for_target(args[0], target, line_no, op));
            else buf_appendf(text, op_is(op, "inc") ? "  inc %s\n" : "  dec %s\n", generic_reg_for_target(args[0], target, line_no, op));
        } else {
            buf_appendf(text, "  %s %s\n", op, generic_reg_for_target(args[0], target, line_no, op));
        }
        return;
    }
    if (op_is(op, "cmp") && argc == 2) {
        if (is_i386_target(target)) buf_appendf(text, "  cmp %s, %s\n", generic_reg_for_target(args[0], target, line_no, op), generic_operand(args[1], target, line_no, op));
        else buf_appendf(text, "  cmp %s, %s\n", generic_reg_for_target(args[0], target, line_no, op), generic_operand(args[1], target, line_no, op));
        return;
    }
    if ((op_is(op, "je") || op_is(op, "jne") || op_is(op, "jg") || op_is(op, "jl") ||
         op_is(op, "jge") || op_is(op, "jle") || op_is(op, "ja") || op_is(op, "jb") ||
         op_is(op, "jae") || op_is(op, "jbe") || op_is(op, "jmp")) && argc == 1) {
        const char *branch = op_is(op, "jmp") ? (is_i386_target(target) ? "jmp" : "b") : op;
        buf_appendf(text, "  %s %s\n", branch, args[0]);
        return;
    }
    if (op_is(op, "call") && argc == 1) { buf_appendf(text, is_i386_target(target) ? "  call %s\n" : "  bl %s\n", args[0]); return; }
    if (op_is(op, "ret") && argc == 0) { buf_append(text, is_i386_target(target) ? "  ret\n" : "  ret\n"); return; }
    if (op_is(op, "push") && argc == 1) { buf_appendf(text, is_i386_target(target) ? "  push %s\n" : "  push {%s}\n", generic_operand(args[0], target, line_no, op)); return; }
    if (op_is(op, "pop") && argc == 1) { buf_appendf(text, is_i386_target(target) ? "  pop %s\n" : "  pop {%s}\n", generic_reg_for_target(args[0], target, line_no, op)); return; }
    if (op_is(op, "syscall")) { emit_generic_syscall(text, target, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count for generic architecture");
}

/* ------------------------------------------------------------------- i386 */

/* Same shape as the x86-64 operand model: a virtual register is either a
   machine register or a spill slot, and the two scratch registers exist so an
   instruction can need both an address and a value that live in memory. */
static bool i386_reg_is_spilled(const char *value) {
    return virtual_reg_index(value) >= I386_MAPPED_COUNT;
}

static const char *i386_operand_text(int idx, const char *size) {
    static char pool[8][48];
    static unsigned next = 0;
    char *slot;
    if (idx < I386_MAPPED_COUNT) {
        return size[0] == 'w' ? i386_regs_w[idx] : i386_regs[idx];
    }
    slot = pool[next];
    next = (next + 1) % 8;
    i386_spill_used |= 1u << (idx - I386_MAPPED_COUNT);
    snprintf(slot, sizeof(pool[0]), "[%s+%d]", X86_SPILL_SYMBOL, (idx - I386_MAPPED_COUNT) * 4);
    return slot;
}

static const char *i386_reg(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        line_error_token(line_no, value, op, "expected virtual register r0-r15");
    }
    return i386_operand_text(reg, "d");
}

static const char *i386_operand(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg >= 0) return i386_operand_text(reg, "d");
    if (is_int(value) || is_symbol(value) || is_known_constant(value)) return value;
    line_error_token(line_no, value, op, "expected register, integer, symbol, or constant");
    return value;
}

static int i386_vreg_of(const char *physical) {
    for (int i = 0; i < I386_MAPPED_COUNT; i++) {
        if (strcmp(i386_regs[i], physical) == 0) return i;
    }
    return -1;
}

static bool i386_must_preserve(const char *physical) {
    int vreg = i386_vreg_of(physical);
    return vreg >= 0 && (mentioned_vregs & (1u << (unsigned)vreg)) != 0;
}

static void i386_emit_address(Buffer *text, const char *addr_text, char *out, size_t out_size,
                              int line_no, const char *op) {
    Address addr;
    const char *base;
    if (!parse_address(addr_text, &addr)) {
        line_error_token(line_no, addr_text, op, "expected address like [r0 + 8] or [symbol + 8]");
    }
    if (!addr.has_base) {
        if (addr.offset == 0) snprintf(out, out_size, "[%s]", addr.symbol);
        else snprintf(out, out_size, "[%s%+lld]", addr.symbol, addr.offset);
        return;
    }
    if (i386_reg_is_spilled(addr.base)) {
        buf_appendf(text, "  mov %s, %s\n", I386_ADDR_SCRATCH, i386_reg(addr.base, line_no, op));
        base = I386_ADDR_SCRATCH;
    } else {
        base = i386_reg(addr.base, line_no, op);
    }
    if (addr.offset == 0) snprintf(out, out_size, "[%s]", base);
    else snprintf(out, out_size, "[%s%+lld]", base, addr.offset);
}

static const char *i386_size_word(const char *size) {
    if (strcmp(size, "b") == 0) return "byte";
    if (strcmp(size, "w") == 0) return "word";
    return "dword";
}

static void emit_i386_syscall(Buffer *text, char **args, int argc, int line_no) {
    static const char *const arg_regs[] = {"ebx", "ecx", "edx", "esi", "edi"};
    const char *result = syscall_result_operand(&args, &argc);
    int number = -1;
    int count;
    bool returns = true;
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    if (strcmp(args[0], "exit") == 0) { number = 1; returns = false; }
    else if (strcmp(args[0], "read") == 0) number = 3;
    else if (strcmp(args[0], "write") == 0) number = 4;
    else if (strcmp(args[0], "open") == 0) number = 5;
    else if (strcmp(args[0], "close") == 0) number = 6;
    else line_error(line_no, "syscall", "unknown syscall");
    count = argc - 1;
    if (count > 5) count = 5;
    /* ebx, ecx, esi and edi carry virtual registers, so the ones this call
       writes are saved unless the source never names them. */
    if (returns) {
        for (int i = 0; i < count; i++) {
            if (i386_must_preserve(arg_regs[i])) buf_appendf(text, "  push %s\n", arg_regs[i]);
        }
    }
    /* Values are staged on the stack first: loading one argument register can
       otherwise destroy the source of a later one. */
    for (int i = 0; i < count; i++) {
        buf_appendf(text, "  push %s%s\n", i386_reg_is_spilled(args[i + 1]) ? "dword " : "",
                    i386_operand(args[i + 1], line_no, "syscall"));
    }
    for (int i = count - 1; i >= 0; i--) {
        buf_appendf(text, "  pop %s\n", arg_regs[i]);
    }
    buf_appendf(text, "  mov eax, %d\n", number);
    buf_append(text, "  int 0x80\n");
    if (returns) {
        for (int i = count - 1; i >= 0; i--) {
            if (i386_must_preserve(arg_regs[i])) buf_appendf(text, "  pop %s\n", arg_regs[i]);
        }
    }
    /* eax is not one of the restored registers, so the result survives them. */
    if (result) {
        buf_appendf(text, "  mov %s%s, %s\n", i386_reg_is_spilled(result) ? "dword " : "",
                    i386_reg(result, line_no, "syscall"), I386_SCRATCH);
    }
}

static void emit_i386_instruction(Buffer *text, const char *op, const char *size, char **args, int argc, int line_no) {
    if (op_is(op, "func") && argc == 1) { buf_appendf(text, "%s:\n", args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) return;
    if (op_is(op, "enter") && argc == 1) {
        buf_append(text, "  push ebp\n  mov ebp, esp\n");
        if (strcmp(args[0], "0") != 0) buf_appendf(text, "  sub esp, %s\n", args[0]);
        return;
    }
    if (op_is(op, "leave") && argc == 0) { buf_append(text, "  mov esp, ebp\n  pop ebp\n"); return; }
    if (op_is(op, "mov") && argc == 2) {
        if (i386_reg_is_spilled(args[0]) && i386_reg_is_spilled(args[1])) {
            buf_appendf(text, "  mov %s, %s\n", I386_SCRATCH, i386_operand(args[1], line_no, op));
            buf_appendf(text, "  mov dword %s, %s\n", i386_reg(args[0], line_no, op), I386_SCRATCH);
        } else {
            buf_appendf(text, "  mov %s%s, %s\n", i386_reg_is_spilled(args[0]) ? "dword " : "",
                        i386_reg(args[0], line_no, op), i386_operand(args[1], line_no, op));
        }
        return;
    }
    if (op_is(op, "load_addr") && argc == 2) {
        if (i386_reg_is_spilled(args[0])) {
            buf_appendf(text, "  lea %s, [%s]\n", I386_SCRATCH, args[1]);
            buf_appendf(text, "  mov dword %s, %s\n", i386_reg(args[0], line_no, op), I386_SCRATCH);
        } else {
            buf_appendf(text, "  lea %s, [%s]\n", i386_reg(args[0], line_no, op), args[1]);
        }
        return;
    }
    if (op_is(op, "load") && argc == 2) {
        char addr[256];
        bool spilled = i386_reg_is_spilled(args[0]);
        i386_emit_address(text, args[1], addr, sizeof(addr), line_no, op);
        if (strcmp(size, "b") == 0 || strcmp(size, "w") == 0) {
            buf_appendf(text, "  movzx %s, %s %s\n", spilled ? I386_SCRATCH : i386_reg(args[0], line_no, op),
                        i386_size_word(size), addr);
        } else {
            buf_appendf(text, "  mov %s, dword %s\n", spilled ? I386_SCRATCH : i386_reg(args[0], line_no, op), addr);
        }
        if (spilled) buf_appendf(text, "  mov dword %s, %s\n", i386_reg(args[0], line_no, op), I386_SCRATCH);
        return;
    }
    if (op_is(op, "store") && argc == 2) {
        char addr[256];
        i386_emit_address(text, args[0], addr, sizeof(addr), line_no, op);
        /* A spilled source would make two memory operands, and esi and edi
           have no 8-bit form, so those go through the scratch. Everything
           else stores straight from its own register. */
        int src_reg = virtual_reg_index(args[1]);
        const char *direct = NULL;
        if (src_reg >= 0 && src_reg < I386_MAPPED_COUNT) {
            if (size[0] == 'b') direct = i386_regs_b[src_reg];
            else if (size[0] == 'w') direct = i386_regs_w[src_reg];
            else direct = i386_regs[src_reg];
        }
        if (direct) {
            buf_appendf(text, "  mov %s %s, %s\n", i386_size_word(size), addr, direct);
        } else if (is_int(args[1]) || is_known_constant(args[1])) {
            /* A literal, or a name the source gave a value, goes straight
               into the store rather than being staged in a register. */
            buf_appendf(text, "  mov %s %s, %s\n", i386_size_word(size), addr, args[1]);
        } else {
            buf_appendf(text, "  mov %s, %s\n", I386_SCRATCH, i386_operand(args[1], line_no, op));
            buf_appendf(text, "  mov %s %s, %s\n", i386_size_word(size), addr,
                        strcmp(size, "b") == 0 ? "al" : strcmp(size, "w") == 0 ? "ax" : "eax");
        }
        return;
    }
    if (op_is(op, "bswap") && argc == 1) {
        if (i386_reg_is_spilled(args[0])) {
            buf_appendf(text, "  mov %s, %s\n", I386_SCRATCH, i386_reg(args[0], line_no, op));
            buf_appendf(text, "  bswap %s\n", I386_SCRATCH);
            buf_appendf(text, "  mov dword %s, %s\n", i386_reg(args[0], line_no, op), I386_SCRATCH);
        } else {
            buf_appendf(text, "  bswap %s\n", i386_reg(args[0], line_no, op));
        }
        return;
    }
    if ((op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar") ||
         op_is(op, "rol") || op_is(op, "ror")) && argc == 2) {
        bool dst_mem = i386_reg_is_spilled(args[0]);
        if (virtual_reg_index(args[1]) < 0) {
            buf_appendf(text, "  %s %s%s, %s\n", op, dst_mem ? "dword " : "",
                        i386_reg(args[0], line_no, op), i386_operand(args[1], line_no, op));
            return;
        }
        {
            /* A count already in ecx needs no borrowing. */
            bool count_in_cl = strcmp(i386_operand(args[1], line_no, op), "ecx") == 0;
            buf_appendf(text, "  mov %s, %s\n", I386_SCRATCH, i386_reg(args[0], line_no, op));
            if (!count_in_cl) {
                buf_append(text, "  push ecx\n");
                buf_appendf(text, "  mov ecx, %s\n", i386_operand(args[1], line_no, op));
            }
            buf_appendf(text, "  %s %s, cl\n", op, I386_SCRATCH);
            if (!count_in_cl) buf_append(text, "  pop ecx\n");
            buf_appendf(text, "  mov %s%s, %s\n", dst_mem ? "dword " : "", i386_reg(args[0], line_no, op), I386_SCRATCH);
        }
        return;
    }
    if ((op_is(op, "add") || op_is(op, "sub") || op_is(op, "and") ||
         op_is(op, "or") || op_is(op, "xor") || op_is(op, "cmp")) && argc == 2) {
        if (i386_reg_is_spilled(args[0]) && i386_reg_is_spilled(args[1])) {
            buf_appendf(text, "  mov %s, %s\n", I386_SCRATCH, i386_operand(args[1], line_no, op));
            buf_appendf(text, "  %s dword %s, %s\n", op, i386_reg(args[0], line_no, op), I386_SCRATCH);
        } else {
            buf_appendf(text, "  %s %s%s, %s\n", op, i386_reg_is_spilled(args[0]) ? "dword " : "",
                        i386_reg(args[0], line_no, op), i386_operand(args[1], line_no, op));
        }
        return;
    }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") || op_is(op, "dec")) && argc == 1) {
        buf_appendf(text, "  %s %s%s\n", op, i386_reg_is_spilled(args[0]) ? "dword " : "",
                    i386_reg(args[0], line_no, op));
        return;
    }
    if (op_is(op, "mul") && argc == 2) {
        /* The one-operand "mul" multiplies edx:eax; the two-operand form the
           IR wants is imul, and it cannot write to memory. */
        if (i386_reg_is_spilled(args[0])) {
            buf_appendf(text, "  mov %s, %s\n", I386_SCRATCH, i386_reg(args[0], line_no, op));
            buf_appendf(text, "  imul %s, %s\n", I386_SCRATCH, i386_operand(args[1], line_no, op));
            buf_appendf(text, "  mov dword %s, %s\n", i386_reg(args[0], line_no, op), I386_SCRATCH);
        } else {
            buf_appendf(text, "  imul %s, %s\n", i386_reg(args[0], line_no, op), i386_operand(args[1], line_no, op));
        }
        return;
    }
    if ((op_is(op, "div") || op_is(op, "mod")) && argc == 2) {
        /* idiv works on edx:eax, and both are scratch here, so nothing has to
           be saved. The divisor is copied out before cdq overwrites edx. */
        buf_appendf(text, "  mov %s, %s\n", I386_SCRATCH, i386_reg(args[0], line_no, op));
        buf_append(text, "  push ecx\n");
        buf_appendf(text, "  mov ecx, %s\n", i386_operand(args[1], line_no, op));
        buf_append(text, "  cdq\n  idiv ecx\n");
        if (op_is(op, "mod")) buf_appendf(text, "  mov %s, edx\n", I386_SCRATCH);
        buf_append(text, "  pop ecx\n");
        buf_appendf(text, "  mov %s%s, %s\n", i386_reg_is_spilled(args[0]) ? "dword " : "",
                    i386_reg(args[0], line_no, op), I386_SCRATCH);
        return;
    }
    if (op_is(op, "push") && argc == 1) {
        buf_appendf(text, "  push %s%s\n", i386_reg_is_spilled(args[0]) ? "dword " : "",
                    i386_operand(args[0], line_no, op));
        return;
    }
    if (op_is(op, "pop") && argc == 1) {
        buf_appendf(text, "  pop %s%s\n", i386_reg_is_spilled(args[0]) ? "dword " : "",
                    i386_reg(args[0], line_no, op));
        return;
    }
    if ((op_is(op, "jmp") || op_is(op, "call") || op_is(op, "je") || op_is(op, "jne") ||
         op_is(op, "jg") || op_is(op, "jl") || op_is(op, "jge") || op_is(op, "jle") ||
         op_is(op, "ja") || op_is(op, "jb") || op_is(op, "jae") || op_is(op, "jbe")) && argc == 1) {
        buf_appendf(text, "  %s %s\n", op, args[0]); return;
    }
    if (op_is(op, "ret") && argc == 0) { buf_append(text, "  ret\n"); return; }
    if (op_is(op, "syscall")) { emit_i386_syscall(text, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count for i386");
}

/* ------------------------------------------------------------------ ARM32 */

static bool arm_reg_is_spilled(const char *value) {
    return virtual_reg_index(value) >= ARM_MAPPED_COUNT;
}

static int arm_spill_offset(const char *value) {
    int index = virtual_reg_index(value) - ARM_MAPPED_COUNT;
    arm_spill_used |= 1u << (unsigned)index;
    return index * 4;
}

/* ARM has no absolute addressing mode, so the spill area's address comes out
   of the literal pool. */
static void arm_load_spill_base(Buffer *text, const char *reg) {
    buf_appendf(text, "  ldr %s, =%s\n", reg, X86_SPILL_SYMBOL);
}

/* An immediate has to be an 8-bit value under an even rotation. Rather than
   model that, anything outside 0-255 goes through the literal pool. */
static bool arm_fits_imm(const char *value, long long *out) {
    char *end = NULL;
    long long parsed;
    if (!is_int(value)) return false;
    errno = 0;
    parsed = strtoll(value, &end, 0);
    if (parsed < 0 || parsed > 255) return false;
    *out = parsed;
    return true;
}

/* Returns a register holding the operand's value, materialising it into
   `into` when it is spilled or is not a register at all. */
static const char *arm_value_reg(Buffer *text, const char *value, const char *into, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg >= 0 && reg < ARM_MAPPED_COUNT) return arm_regs[reg];
    if (reg >= 0) {
        int offset = arm_spill_offset(value);
        arm_load_spill_base(text, into);
        buf_appendf(text, "  ldr %s, [%s, #%d]\n", into, into, offset);
        return into;
    }
    if (!is_int(value) && !is_symbol(value) && !is_known_constant(value)) {
        line_error_token(line_no, value, op, "expected register, integer, symbol, or constant");
    }
    buf_appendf(text, "  ldr %s, =%s\n", into, value);
    return into;
}

/* The register a result should be computed into: the machine register itself
   when the destination is mapped, otherwise the given scratch. */
static const char *arm_dst_reg(Buffer *text, const char *value, const char *scratch, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) line_error_token(line_no, value, op, "expected virtual register r0-r15");
    if (reg < ARM_MAPPED_COUNT) return arm_regs[reg];
    arm_load_spill_base(text, scratch);
    buf_appendf(text, "  ldr %s, [%s, #%d]\n", scratch, scratch, arm_spill_offset(value));
    return scratch;
}

/* Writes a computed value back. `via` must be a scratch register that is no
   longer holding anything live, since it is reused for the spill base. */
static void arm_store_vreg(Buffer *text, const char *value, const char *from, const char *via) {
    if (!arm_reg_is_spilled(value)) return;
    arm_load_spill_base(text, via);
    buf_appendf(text, "  str %s, [%s, #%d]\n", from, via, arm_spill_offset(value));
}

static void arm_emit_address(Buffer *text, const char *addr_text, char *out, size_t out_size,
                             const char *scratch, int line_no, const char *op) {
    Address addr;
    const char *base;
    if (!parse_address(addr_text, &addr)) {
        line_error_token(line_no, addr_text, op, "expected address like [r0 + 8] or [symbol + 8]");
    }
    if (addr.has_base) {
        base = arm_value_reg(text, addr.base, scratch, line_no, op);
    } else {
        buf_appendf(text, "  ldr %s, =%s\n", scratch, addr.symbol);
        base = scratch;
    }
    if (addr.offset == 0) snprintf(out, out_size, "[%s]", base);
    else snprintf(out, out_size, "[%s, #%lld]", base, addr.offset);
}

static int arm_vreg_of(const char *physical) {
    for (int i = 0; i < ARM_MAPPED_COUNT; i++) {
        if (strcmp(arm_regs[i], physical) == 0) return i;
    }
    return -1;
}

static bool arm_must_preserve(const char *physical) {
    int vreg = arm_vreg_of(physical);
    return vreg >= 0 && (mentioned_vregs & (1u << (unsigned)vreg)) != 0;
}

static void emit_arm_syscall(Buffer *text, char **args, int argc, int line_no) {
    static const char *const arg_regs[] = {"r0", "r1", "r2", "r3", "r4", "r5"};
    const char *result = syscall_result_operand(&args, &argc);
    int number = -1;
    int count;
    bool returns = true;
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    if (strcmp(args[0], "read") == 0) number = 3;
    else if (strcmp(args[0], "write") == 0) number = 4;
    else if (strcmp(args[0], "open") == 0) number = 5;
    else if (strcmp(args[0], "close") == 0) number = 6;
    else if (strcmp(args[0], "exit") == 0) { number = 1; returns = false; }
    else line_error(line_no, "syscall", "unknown syscall");
    count = argc - 1;
    if (count > 6) count = 6;
    if (returns) {
        for (int i = 0; i < count; i++) {
            if (arm_must_preserve(arg_regs[i])) buf_appendf(text, "  push {%s}\n", arg_regs[i]);
        }
    }
    /* Values are staged on the stack because loading one argument register can
       destroy the source of a later one. */
    for (int i = 0; i < count; i++) {
        const char *src = arm_value_reg(text, args[i + 1], ARM_SCRATCH2, line_no, "syscall");
        buf_appendf(text, "  push {%s}\n", src);
    }
    for (int i = count - 1; i >= 0; i--) {
        buf_appendf(text, "  pop {%s}\n", arg_regs[i]);
    }
    buf_appendf(text, "  ldr r7, =%d\n", number);
    buf_append(text, "  svc #0\n");
    /* r0 is both where the result arrives and a register the restores below
       are about to overwrite, so it is taken aside first. */
    if (result) buf_appendf(text, "  mov %s, r0\n", ARM_SCRATCH2);
    if (returns) {
        for (int i = count - 1; i >= 0; i--) {
            if (arm_must_preserve(arg_regs[i])) buf_appendf(text, "  pop {%s}\n", arg_regs[i]);
        }
    }
    if (result) {
        const char *dst = arm_reg_is_spilled(result) ? ARM_SCRATCH2 : arm_regs[virtual_reg_index(result)];
        if (strcmp(dst, ARM_SCRATCH2) != 0) buf_appendf(text, "  mov %s, %s\n", dst, ARM_SCRATCH2);
        arm_store_vreg(text, result, ARM_SCRATCH2, ARM_SCRATCH);
    }
}

static void emit_arm_instruction(Buffer *text, const char *op, const char *size, char **args, int argc, int line_no) {
    if (op_is(op, "func") && argc == 1) { buf_appendf(text, "%s:\n", args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) return;
    if (op_is(op, "enter") && argc == 1) {
        long long frame;
        buf_append(text, "  push {fp, lr}\n  mov fp, sp\n");
        if (strcmp(args[0], "0") == 0) return;
        if (arm_fits_imm(args[0], &frame)) buf_appendf(text, "  sub sp, sp, #%lld\n", frame);
        else {
            buf_appendf(text, "  ldr %s, =%s\n", ARM_SCRATCH, args[0]);
            buf_appendf(text, "  sub sp, sp, %s\n", ARM_SCRATCH);
        }
        return;
    }
    if (op_is(op, "leave") && argc == 0) { buf_append(text, "  mov sp, fp\n  pop {fp, lr}\n"); return; }
    if (op_is(op, "mov") && argc == 2) {
        const char *dst = arm_reg_is_spilled(args[0]) ? ARM_SCRATCH2 : arm_regs[virtual_reg_index(args[0])];
        long long imm;
        if (virtual_reg_index(args[0]) < 0) line_error_token(line_no, args[0], op, "expected virtual register r0-r15");
        if (arm_fits_imm(args[1], &imm)) buf_appendf(text, "  mov %s, #%lld\n", dst, imm);
        else {
            const char *src = arm_value_reg(text, args[1], dst, line_no, op);
            if (strcmp(src, dst) != 0) buf_appendf(text, "  mov %s, %s\n", dst, src);
        }
        arm_store_vreg(text, args[0], dst, ARM_SCRATCH);
        return;
    }
    if (op_is(op, "load_addr") && argc == 2) {
        const char *dst = arm_reg_is_spilled(args[0]) ? ARM_SCRATCH2 : arm_regs[virtual_reg_index(args[0])];
        buf_appendf(text, "  ldr %s, =%s\n", dst, args[1]);
        arm_store_vreg(text, args[0], dst, ARM_SCRATCH);
        return;
    }
    if (op_is(op, "load") && argc == 2) {
        char addr[256];
        const char *dst = arm_reg_is_spilled(args[0]) ? ARM_SCRATCH2 : arm_regs[virtual_reg_index(args[0])];
        const char *mnemonic = strcmp(size, "b") == 0 ? "ldrb" : strcmp(size, "w") == 0 ? "ldrh" : "ldr";
        if (virtual_reg_index(args[0]) < 0) line_error_token(line_no, args[0], op, "expected virtual register r0-r15");
        arm_emit_address(text, args[1], addr, sizeof(addr), ARM_SCRATCH, line_no, op);
        buf_appendf(text, "  %s %s, %s\n", mnemonic, dst, addr);
        arm_store_vreg(text, args[0], dst, ARM_SCRATCH);
        return;
    }
    if (op_is(op, "store") && argc == 2) {
        char addr[256];
        const char *mnemonic = strcmp(size, "b") == 0 ? "strb" : strcmp(size, "w") == 0 ? "strh" : "str";
        const char *src = arm_value_reg(text, args[1], ARM_SCRATCH2, line_no, op);
        arm_emit_address(text, args[0], addr, sizeof(addr), ARM_SCRATCH, line_no, op);
        buf_appendf(text, "  %s %s, %s\n", mnemonic, src, addr);
        return;
    }
    if ((op_is(op, "add") || op_is(op, "sub") || op_is(op, "and") ||
         op_is(op, "or") || op_is(op, "xor") || op_is(op, "mul") ||
         op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar")) && argc == 2) {
        const char *native = op_is(op, "or") ? "orr" : op_is(op, "xor") ? "eor" :
                             op_is(op, "shl") ? "lsl" : op_is(op, "shr") ? "lsr" :
                             op_is(op, "sar") ? "asr" : op;
        const char *dst = arm_dst_reg(text, args[0], ARM_SCRATCH2, line_no, op);
        long long imm;
        /* mul takes no immediate, and on ARMv4/v5 its destination must differ
           from its first source, so it always goes through registers. */
        if (strcmp(op, "mul") != 0 && arm_fits_imm(args[1], &imm)) {
            buf_appendf(text, "  %s %s, %s, #%lld\n", native, dst, dst, imm);
        } else {
            const char *src = arm_value_reg(text, args[1], ARM_SCRATCH, line_no, op);
            if (op_is(op, "mul")) buf_appendf(text, "  mul %s, %s, %s\n", dst, src, dst);
            else buf_appendf(text, "  %s %s, %s, %s\n", native, dst, dst, src);
        }
        arm_store_vreg(text, args[0], dst, ARM_SCRATCH);
        return;
    }
    if ((op_is(op, "div") || op_is(op, "mod")) && argc == 2) {
        /* ARM has no division instruction in the portable subset. Calling the
           EABI helper for it would make every program that divides need
           libgcc to link, which freestanding output cannot have, so the
           routine is written into the output instead. It takes r0 and r1 and
           clobbers r0-r3, so the virtual registers living there are saved
           around it, and so is the link register the call overwrites. */
        static const char *const clobbered[] = {"r0", "r1", "r2"};
        const char *lhs = arm_value_reg(text, args[0], ARM_SCRATCH2, line_no, op);
        const char *rhs;
        int saved = 0;
        arm_divmod_used = true;
        buf_appendf(text, "  push {%s}\n", lhs);
        rhs = arm_value_reg(text, args[1], ARM_SCRATCH2, line_no, op);
        buf_appendf(text, "  push {%s}\n", rhs);
        for (int i = 0; i < 3; i++) {
            if (arm_must_preserve(clobbered[i])) {
                buf_appendf(text, "  push {%s}\n", clobbered[i]);
                saved++;
            }
        }
        buf_append(text, "  push {lr}\n");
        saved++;
        /* The operands are under everything that was just saved. */
        buf_appendf(text, "  ldr r1, [sp, #%d]\n", saved * 4);
        buf_appendf(text, "  ldr r0, [sp, #%d]\n", saved * 4 + 4);
        buf_append(text, "  bl __commonasm_divmod\n");
        buf_appendf(text, "  mov %s, %s\n", ARM_SCRATCH2, op_is(op, "div") ? "r0" : "r1");
        buf_append(text, "  pop {lr}\n");
        for (int i = 2; i >= 0; i--) {
            if (arm_must_preserve(clobbered[i])) buf_appendf(text, "  pop {%s}\n", clobbered[i]);
        }
        buf_append(text, "  add sp, sp, #8\n");
        arm_store_vreg(text, args[0], ARM_SCRATCH2, ARM_SCRATCH);
        if (!arm_reg_is_spilled(args[0])) {
            buf_appendf(text, "  mov %s, %s\n", arm_regs[virtual_reg_index(args[0])], ARM_SCRATCH2);
        }
        return;
    }
    if ((op_is(op, "clz") || op_is(op, "ctz") || op_is(op, "bswap")) && argc == 1) {
        const char *dst = arm_dst_reg(text, args[0], ARM_SCRATCH2, line_no, op);
        if (op_is(op, "clz")) buf_appendf(text, "  clz %s, %s\n", dst, dst);
        else if (op_is(op, "bswap")) buf_appendf(text, "  rev %s, %s\n", dst, dst);
        else {
            buf_appendf(text, "  rbit %s, %s\n", dst, dst);
            buf_appendf(text, "  clz %s, %s\n", dst, dst);
        }
        arm_store_vreg(text, args[0], dst, ARM_SCRATCH);
        return;
    }
    if ((op_is(op, "rol") || op_is(op, "ror")) && argc == 2) {
        /* ARM rotates right only, so a left rotation goes the other way by
           the complement of the distance. */
        const char *dst = arm_dst_reg(text, args[0], ARM_SCRATCH2, line_no, op);
        if (is_int(args[1])) {
            long long amount = strtoll(args[1], NULL, 0) & 31;
            if (op_is(op, "rol")) amount = (32 - amount) & 31;
            buf_appendf(text, "  ror %s, %s, #%lld\n", dst, dst, amount);
        } else {
            const char *src = arm_value_reg(text, args[1], ARM_SCRATCH, line_no, op);
            if (op_is(op, "rol")) {
                buf_appendf(text, "  rsb %s, %s, #32\n", ARM_SCRATCH, src);
                src = ARM_SCRATCH;
            }
            buf_appendf(text, "  ror %s, %s, %s\n", dst, dst, src);
        }
        arm_store_vreg(text, args[0], dst, ARM_SCRATCH);
        return;
    }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") ||
         op_is(op, "dec")) && argc == 1) {
        const char *dst = arm_dst_reg(text, args[0], ARM_SCRATCH2, line_no, op);
        if (op_is(op, "neg")) buf_appendf(text, "  rsb %s, %s, #0\n", dst, dst);
        else if (op_is(op, "not")) buf_appendf(text, "  mvn %s, %s\n", dst, dst);
        else buf_appendf(text, "  %s %s, %s, #1\n", op_is(op, "inc") ? "add" : "sub", dst, dst);
        arm_store_vreg(text, args[0], dst, ARM_SCRATCH);
        return;
    }
    if (op_is(op, "cmp") && argc == 2) {
        const char *lhs = arm_value_reg(text, args[0], ARM_SCRATCH2, line_no, op);
        long long imm;
        if (arm_fits_imm(args[1], &imm)) buf_appendf(text, "  cmp %s, #%lld\n", lhs, imm);
        else buf_appendf(text, "  cmp %s, %s\n", lhs, arm_value_reg(text, args[1], ARM_SCRATCH, line_no, op));
        return;
    }
    if (op_is(op, "push") && argc == 1) {
        buf_appendf(text, "  push {%s}\n", arm_value_reg(text, args[0], ARM_SCRATCH2, line_no, op)); return;
    }
    if (op_is(op, "pop") && argc == 1) {
        const char *dst = arm_reg_is_spilled(args[0]) ? ARM_SCRATCH2 : arm_regs[virtual_reg_index(args[0])];
        if (virtual_reg_index(args[0]) < 0) line_error_token(line_no, args[0], op, "expected virtual register r0-r15");
        buf_appendf(text, "  pop {%s}\n", dst);
        arm_store_vreg(text, args[0], dst, ARM_SCRATCH);
        return;
    }
    if (op_is(op, "jmp") && argc == 1) { buf_appendf(text, "  b %s\n", args[0]); return; }
    if (op_is(op, "call") && argc == 1) { buf_appendf(text, "  bl %s\n", args[0]); return; }
    if (op_is(op, "ret") && argc == 0) { buf_append(text, "  bx lr\n"); return; }
    if (argc == 1) {
        const char *cond =
            op_is(op, "je") ? "eq" : op_is(op, "jne") ? "ne" :
            op_is(op, "jg") ? "gt" : op_is(op, "jl") ? "lt" :
            op_is(op, "jge") ? "ge" : op_is(op, "jle") ? "le" :
            op_is(op, "ja") ? "hi" : op_is(op, "jb") ? "lo" :
            op_is(op, "jae") ? "hs" : op_is(op, "jbe") ? "ls" : NULL;
        if (cond) { buf_appendf(text, "  b%s %s\n", cond, args[0]); return; }
    }
    if (op_is(op, "syscall")) { emit_arm_syscall(text, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count for ARM");
}

/* ---------------------------------------------------------------- AArch64 */

static const char *a64_reg(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        line_error_token(line_no, value, op, "expected virtual register r0-r15");
    }
    return aarch64_regs[reg];
}

/* The 32-bit view of a register, which is what the sub-word loads write and
   what zero-extends into the full register. */
static const char *a64_reg_w(const char *physical) {
    static char pool[4][8];
    static unsigned next = 0;
    char *slot = pool[next];
    next = (next + 1) % 4;
    snprintf(slot, sizeof(pool[0]), "w%s", physical + 1);
    return slot;
}

/* Materialises any non-register operand into reg. Numbers are built out of
   movz/movk so no literal pool is needed; a label becomes its address, and a
   name the source declared as a constant stays an immediate. */
static void a64_load_operand(Buffer *text, const char *reg, const char *value) {
    long long number;
    char resolved[32];
    /* A constant the source gave a plain number is materialised from that
       number: AArch64's mov reaches only a restricted set of immediates, and
       the assembler cannot rewrite one that does not fit. */
    if (!is_int(value) && constant_value(value, &number)) {
        snprintf(resolved, sizeof(resolved), "%lld", number);
        value = resolved;
    }
    if (is_int(value)) {
        unsigned long long bits;
        int shift;
        bool started = false;
        char *end = NULL;
        errno = 0;
        number = strtoll(value, &end, 0);
        bits = (unsigned long long)number;
        for (shift = 0; shift < 64; shift += 16) {
            unsigned long long part = (bits >> shift) & 0xffffu;
            if (part == 0 && started) continue;
            if (!started) {
                if (shift == 0) buf_appendf(text, "  movz %s, #%llu\n", reg, part);
                else buf_appendf(text, "  movz %s, #%llu, lsl #%d\n", reg, part, shift);
                started = true;
            } else {
                buf_appendf(text, "  movk %s, #%llu, lsl #%d\n", reg, part, shift);
            }
        }
        if (!started) buf_appendf(text, "  movz %s, #0\n", reg);
        return;
    }
    if (is_known_constant(value)) {
        buf_appendf(text, "  mov %s, #%s\n", reg, value);
        return;
    }
    buf_appendf(text, "  adrp %s, %s\n", reg, value);
    buf_appendf(text, "  add %s, %s, :lo12:%s\n", reg, reg, value);
}

/* Returns the register holding value, materialising it into fallback first if
   it is not already a virtual register. */
static const char *a64_operand_reg(Buffer *text, const char *value, const char *fallback, int line_no, const char *op) {
    if (virtual_reg_index(value) >= 0) {
        return a64_reg(value, line_no, op);
    }
    if (!is_int(value) && !is_symbol(value) && !is_known_constant(value)) {
        line_error_token(line_no, value, op, "expected register, integer, symbol, or constant");
    }
    a64_load_operand(text, fallback, value);
    return fallback;
}

/* add and sub take a 12-bit unsigned immediate; everything else has to go
   through a register. */
static bool a64_fits_add_imm(const char *value, long long *out) {
    char *end = NULL;
    long long parsed;
    if (!is_int(value)) return false;
    errno = 0;
    parsed = strtoll(value, &end, 0);
    if (parsed < 0 || parsed > 4095) return false;
    *out = parsed;
    return true;
}

static void a64_emit_address(Buffer *text, const char *addr_text, char *out, size_t out_size,
                             int line_no, const char *op) {
    Address addr;
    if (!parse_address(addr_text, &addr)) {
        line_error_token(line_no, addr_text, op, "expected address like [r0 + 8] or [symbol + 8]");
    }
    if (addr.has_base) {
        if (addr.offset == 0) snprintf(out, out_size, "[%s]", a64_reg(addr.base, line_no, op));
        else snprintf(out, out_size, "[%s, #%lld]", a64_reg(addr.base, line_no, op), addr.offset);
        return;
    }
    /* A bare symbol has no addressing mode of its own, so its address is
       computed into the scratch register first. */
    buf_appendf(text, "  adrp %s, %s\n", A64_SCRATCH2, addr.symbol);
    buf_appendf(text, "  add %s, %s, :lo12:%s\n", A64_SCRATCH2, A64_SCRATCH2, addr.symbol);
    if (addr.offset == 0) snprintf(out, out_size, "[%s]", A64_SCRATCH2);
    else snprintf(out, out_size, "[%s, #%lld]", A64_SCRATCH2, addr.offset);
}

static void emit_a64_syscall(Buffer *text, char **args, int argc, int line_no) {
    static const char *const arg_regs[] = {"x0", "x1", "x2", "x3", "x4", "x5"};
    const char *result = syscall_result_operand(&args, &argc);
    int number = -1;
    int count;
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    if (strcmp(args[0], "read") == 0) number = 63;
    else if (strcmp(args[0], "write") == 0) number = 64;
    else if (strcmp(args[0], "open") == 0) number = 1024;
    else if (strcmp(args[0], "close") == 0) number = 57;
    else if (strcmp(args[0], "exit") == 0) number = 93;
    else line_error(line_no, "syscall", "unknown syscall");
    count = argc - 1;
    if (count > 6) count = 6;
    /* No virtual register maps onto x0-x8, so nothing has to be saved here. */
    for (int i = 0; i < count; i++) {
        if (virtual_reg_index(args[i + 1]) >= 0) {
            buf_appendf(text, "  mov %s, %s\n", arg_regs[i], a64_reg(args[i + 1], line_no, "syscall"));
        } else {
            a64_load_operand(text, arg_regs[i], args[i + 1]);
        }
    }
    buf_appendf(text, "  mov x8, #%d\n", number);
    buf_append(text, "  svc #0\n");
    if (result) buf_appendf(text, "  mov %s, x0\n", a64_reg(result, line_no, "syscall"));
}

static void emit_a64_instruction(Buffer *text, const char *op, const char *size, char **args, int argc, int line_no) {
    if (op_is(op, "func") && argc == 1) { buf_appendf(text, "%s:\n", args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) return;
    if (op_is(op, "enter") && argc == 1) {
        long long frame;
        buf_append(text, "  stp x29, x30, [sp, #-16]!\n  mov x29, sp\n");
        if (strcmp(args[0], "0") == 0) return;
        if (a64_fits_add_imm(args[0], &frame)) {
            /* The stack pointer must stay 16-byte aligned. */
            buf_appendf(text, "  sub sp, sp, #%lld\n", (frame + 15) & ~15L);
        } else {
            a64_load_operand(text, A64_SCRATCH, args[0]);
            buf_appendf(text, "  sub sp, sp, %s\n", A64_SCRATCH);
        }
        return;
    }
    if (op_is(op, "leave") && argc == 0) {
        buf_append(text, "  mov sp, x29\n  ldp x29, x30, [sp], #16\n"); return;
    }
    if (op_is(op, "mov") && argc == 2) {
        if (virtual_reg_index(args[1]) >= 0) {
            buf_appendf(text, "  mov %s, %s\n", a64_reg(args[0], line_no, op), a64_reg(args[1], line_no, op));
        } else {
            a64_load_operand(text, a64_reg(args[0], line_no, op), args[1]);
        }
        return;
    }
    if (op_is(op, "load_addr") && argc == 2) {
        const char *dst = a64_reg(args[0], line_no, op);
        buf_appendf(text, "  adrp %s, %s\n", dst, args[1]);
        buf_appendf(text, "  add %s, %s, :lo12:%s\n", dst, dst, args[1]);
        return;
    }
    if (op_is(op, "load") && argc == 2) {
        char addr[256];
        const char *dst = a64_reg(args[0], line_no, op);
        a64_emit_address(text, args[1], addr, sizeof(addr), line_no, op);
        if (strcmp(size, "b") == 0) buf_appendf(text, "  ldrb %s, %s\n", a64_reg_w(dst), addr);
        else if (strcmp(size, "w") == 0) buf_appendf(text, "  ldrh %s, %s\n", a64_reg_w(dst), addr);
        else if (strcmp(size, "d") == 0) buf_appendf(text, "  ldr %s, %s\n", a64_reg_w(dst), addr);
        else buf_appendf(text, "  ldr %s, %s\n", dst, addr);
        return;
    }
    if (op_is(op, "store") && argc == 2) {
        char addr[256];
        const char *src;
        a64_emit_address(text, args[0], addr, sizeof(addr), line_no, op);
        src = a64_operand_reg(text, args[1], A64_SCRATCH, line_no, op);
        if (strcmp(size, "b") == 0) buf_appendf(text, "  strb %s, %s\n", a64_reg_w(src), addr);
        else if (strcmp(size, "w") == 0) buf_appendf(text, "  strh %s, %s\n", a64_reg_w(src), addr);
        else if (strcmp(size, "d") == 0) buf_appendf(text, "  str %s, %s\n", a64_reg_w(src), addr);
        else buf_appendf(text, "  str %s, %s\n", src, addr);
        return;
    }
    if ((op_is(op, "add") || op_is(op, "sub")) && argc == 2) {
        const char *dst = a64_reg(args[0], line_no, op);
        long long imm;
        if (a64_fits_add_imm(args[1], &imm)) {
            buf_appendf(text, "  %s %s, %s, #%lld\n", op, dst, dst, imm);
        } else {
            const char *src = a64_operand_reg(text, args[1], A64_SCRATCH, line_no, op);
            buf_appendf(text, "  %s %s, %s, %s\n", op, dst, dst, src);
        }
        return;
    }
    if ((op_is(op, "and") || op_is(op, "or") || op_is(op, "xor") ||
         op_is(op, "mul")) && argc == 2) {
        /* The logical immediate encoding is a bitmask pattern rather than a
           plain range, and mul has no immediate form at all, so every
           non-register operand is materialised. */
        const char *dst = a64_reg(args[0], line_no, op);
        const char *src = a64_operand_reg(text, args[1], A64_SCRATCH, line_no, op);
        const char *native = op_is(op, "and") ? "and" : op_is(op, "or") ? "orr" :
                             op_is(op, "xor") ? "eor" : "mul";
        buf_appendf(text, "  %s %s, %s, %s\n", native, dst, dst, src);
        return;
    }
    if ((op_is(op, "div") || op_is(op, "mod")) && argc == 2) {
        const char *dst = a64_reg(args[0], line_no, op);
        const char *src = a64_operand_reg(text, args[1], A64_SCRATCH, line_no, op);
        if (op_is(op, "div")) {
            buf_appendf(text, "  sdiv %s, %s, %s\n", dst, dst, src);
        } else {
            /* AArch64 has no remainder instruction: quotient, then subtract
               back the product. */
            buf_appendf(text, "  sdiv %s, %s, %s\n", A64_SCRATCH2, dst, src);
            buf_appendf(text, "  msub %s, %s, %s, %s\n", dst, A64_SCRATCH2, src, dst);
        }
        return;
    }
    if ((op_is(op, "min") || op_is(op, "max")) && argc == 2) {
        /* csel picks one of two registers on the flags a compare just set, so
           there is no branch here and nothing to save. */
        const char *dst = a64_reg(args[0], line_no, op);
        const char *src = a64_operand_reg(text, args[1], A64_SCRATCH, line_no, op);
        buf_appendf(text, "  cmp %s, %s\n", dst, src);
        buf_appendf(text, "  csel %s, %s, %s, %s\n", dst, dst, src, op_is(op, "min") ? "le" : "ge");
        return;
    }
    if (op_is(op, "abs") && argc == 1) {
        /* cneg negates when the condition holds and copies when it does not,
           so testing the sign is the whole of it. */
        const char *dst = a64_reg(args[0], line_no, op);
        buf_appendf(text, "  cmp %s, #0\n", dst);
        buf_appendf(text, "  cneg %s, %s, lt\n", dst, dst);
        return;
    }
    if (op_is(op, "sel") && argc == 3) {
        /* Both arms are brought in before the compare, since materialising an
           immediate leaves the flags alone but reading the condition out of
           the destination has to happen before the destination is written. */
        const char *dst = a64_reg(args[0], line_no, op);
        const char *yes = a64_operand_reg(text, args[1], A64_SCRATCH, line_no, op);
        const char *no = a64_operand_reg(text, args[2], A64_SCRATCH2, line_no, op);
        buf_appendf(text, "  cmp %s, #0\n", dst);
        buf_appendf(text, "  csel %s, %s, %s, ne\n", dst, yes, no);
        return;
    }
    if ((op_is(op, "clz") || op_is(op, "ctz") || op_is(op, "bswap")) && argc == 1) {
        const char *dst = a64_reg(args[0], line_no, op);
        if (op_is(op, "clz")) buf_appendf(text, "  clz %s, %s\n", dst, dst);
        else if (op_is(op, "bswap")) buf_appendf(text, "  rev %s, %s\n", dst, dst);
        else {
            /* Counting trailing zeros is counting leading zeros of the
               bit-reversed value. */
            buf_appendf(text, "  rbit %s, %s\n", dst, dst);
            buf_appendf(text, "  clz %s, %s\n", dst, dst);
        }
        return;
    }
    if (op_is(op, "popcnt") && argc == 1) {
        /* The population count lives on the vector side: move across, count
           bits per byte, sum the bytes, move back. */
        const char *dst = a64_reg(args[0], line_no, op);
        buf_appendf(text, "  fmov d0, %s\n", dst);
        buf_append(text, "  cnt v0.8b, v0.8b\n");
        buf_append(text, "  addv b0, v0.8b\n");
        buf_appendf(text, "  fmov %s, s0\n", a64_reg_w(dst));
        return;
    }
    if ((op_is(op, "rol") || op_is(op, "ror")) && argc == 2) {
        /* AArch64 only rotates right, so a left rotation is a right rotation
           by the complement of the distance. */
        const char *dst = a64_reg(args[0], line_no, op);
        int bits = 64;
        if (is_int(args[1])) {
            long long amount = strtoll(args[1], NULL, 0) & (bits - 1);
            if (op_is(op, "rol")) amount = (bits - amount) & (bits - 1);
            buf_appendf(text, "  ror %s, %s, #%lld\n", dst, dst, amount);
        } else {
            const char *src = a64_operand_reg(text, args[1], A64_SCRATCH, line_no, op);
            if (op_is(op, "rol")) {
                buf_appendf(text, "  neg %s, %s\n", A64_SCRATCH, src);
                src = A64_SCRATCH;
            }
            buf_appendf(text, "  ror %s, %s, %s\n", dst, dst, src);
        }
        return;
    }
    if ((op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar")) && argc == 2) {
        const char *dst = a64_reg(args[0], line_no, op);
        const char *native = op_is(op, "shl") ? "lsl" : op_is(op, "shr") ? "lsr" : "asr";
        if (is_int(args[1])) {
            buf_appendf(text, "  %s %s, %s, #%s\n", native, dst, dst, args[1]);
        } else {
            const char *src = a64_operand_reg(text, args[1], A64_SCRATCH, line_no, op);
            buf_appendf(text, "  %s %s, %s, %s\n", native, dst, dst, src);
        }
        return;
    }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") ||
         op_is(op, "dec")) && argc == 1) {
        const char *dst = a64_reg(args[0], line_no, op);
        if (op_is(op, "neg")) buf_appendf(text, "  neg %s, %s\n", dst, dst);
        else if (op_is(op, "not")) buf_appendf(text, "  mvn %s, %s\n", dst, dst);
        else buf_appendf(text, "  %s %s, %s, #1\n", op_is(op, "inc") ? "add" : "sub", dst, dst);
        return;
    }
    if (op_is(op, "cmp") && argc == 2) {
        const char *lhs = a64_reg(args[0], line_no, op);
        long long imm;
        if (a64_fits_add_imm(args[1], &imm)) buf_appendf(text, "  cmp %s, #%lld\n", lhs, imm);
        else buf_appendf(text, "  cmp %s, %s\n", lhs, a64_operand_reg(text, args[1], A64_SCRATCH, line_no, op));
        return;
    }
    if (op_is(op, "push") && argc == 1) {
        /* The stack pointer has to stay 16-byte aligned, so a single register
           still moves it by 16. */
        buf_appendf(text, "  str %s, [sp, #-16]!\n", a64_operand_reg(text, args[0], A64_SCRATCH, line_no, op));
        return;
    }
    if (op_is(op, "pop") && argc == 1) {
        buf_appendf(text, "  ldr %s, [sp], #16\n", a64_reg(args[0], line_no, op)); return;
    }
    if (op_is(op, "jmp") && argc == 1) { buf_appendf(text, "  b %s\n", args[0]); return; }
    if (op_is(op, "call") && argc == 1) { buf_appendf(text, "  bl %s\n", args[0]); return; }
    if (op_is(op, "ret") && argc == 0) { buf_append(text, "  ret\n"); return; }
    if (argc == 1) {
        const char *cond =
            op_is(op, "je") ? "eq" : op_is(op, "jne") ? "ne" :
            op_is(op, "jg") ? "gt" : op_is(op, "jl") ? "lt" :
            op_is(op, "jge") ? "ge" : op_is(op, "jle") ? "le" :
            op_is(op, "ja") ? "hi" : op_is(op, "jb") ? "lo" :
            op_is(op, "jae") ? "hs" : op_is(op, "jbe") ? "ls" : NULL;
        if (cond) { buf_appendf(text, "  b.%s %s\n", cond, args[0]); return; }
    }
    if (op_is(op, "syscall")) { emit_a64_syscall(text, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count for AArch64");
}

static void emit_arch_instruction(Buffer *text, const char *target, const char *op, const char *size, char **args, int argc, int line_no) {
    (void)size;
    const char *comment = is_legacy_arch_target(target) ? "#" : ";";
    if (op_is(op, "func") && argc == 1) { buf_appendf(text, "%s:\n", args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) return;
    if (op_is(op, "enter") && argc == 1) { buf_appendf(text, "  %s enter %s\n", comment, args[0]); return; }
    if (op_is(op, "leave") && argc == 0) { buf_appendf(text, "  %s leave\n", comment); return; }
    if (op_is(op, "mov") && argc == 2) { buf_appendf(text, "  mov %s, %s\n", generic_reg_for_target(args[0], target, line_no, op), generic_operand(args[1], target, line_no, op)); return; }
    if (op_is(op, "load_addr") && argc == 2) { buf_appendf(text, "  la %s, %s\n", generic_reg_for_target(args[0], target, line_no, op), args[1]); return; }
    if (op_is(op, "load") && argc == 2) {
        char addr[256]; generic_format_address(args[1], target, addr, sizeof(addr), line_no, op);
        buf_appendf(text, "  load %s, %s\n", generic_reg_for_target(args[0], target, line_no, op), addr); return;
    }
    if (op_is(op, "store") && argc == 2) {
        char addr[256]; generic_format_address(args[0], target, addr, sizeof(addr), line_no, op);
        buf_appendf(text, "  store %s, %s\n", generic_operand(args[1], target, line_no, op), addr); return;
    }
    if ((op_is(op, "add") || op_is(op, "sub") || op_is(op, "mul") || op_is(op, "div") ||
         op_is(op, "mod") || op_is(op, "and") || op_is(op, "or") || op_is(op, "xor") ||
         op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar")) && argc == 2) {
        buf_appendf(text, "  %s %s, %s, ", op, generic_reg_for_target(args[0], target, line_no, op), generic_reg_for_target(args[0], target, line_no, op));
        buf_append(text, generic_operand(args[1], target, line_no, op));
        buf_append(text, "\n");
        return;
    }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") || op_is(op, "dec")) && argc == 1) {
        buf_appendf(text, "  %s %s\n", op, generic_reg_for_target(args[0], target, line_no, op)); return;
    }
    if (op_is(op, "cmp") && argc == 2) { buf_appendf(text, "  cmp %s, %s\n", generic_reg_for_target(args[0], target, line_no, op), generic_operand(args[1], target, line_no, op)); return; }
    if ((op_is(op, "jmp") || op_is(op, "je") || op_is(op, "jne") || op_is(op, "jg") ||
         op_is(op, "jl") || op_is(op, "jge") || op_is(op, "jle") || op_is(op, "ja") ||
         op_is(op, "jb") || op_is(op, "jae") || op_is(op, "jbe")) && argc == 1) {
        buf_appendf(text, "  %s %s\n", op, args[0]); return;
    }
    if (op_is(op, "call") && argc == 1) { buf_appendf(text, "  call %s\n", args[0]); return; }
    if (op_is(op, "ret") && argc == 0) { buf_append(text, "  ret\n"); return; }
    if (op_is(op, "push") && argc == 1) { buf_appendf(text, "  push %s\n", generic_operand(args[0], target, line_no, op)); return; }
    if (op_is(op, "pop") && argc == 1) { buf_appendf(text, "  pop %s\n", generic_reg_for_target(args[0], target, line_no, op)); return; }
    if (op_is(op, "syscall") && argc >= 1) { buf_appendf(text, "  %s syscall %s runtime placeholder\n", comment, args[0]); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count for architecture-style target");
}

static void emit_vm_ir_instruction(Buffer *text, const char *target, const char *op, const char *size, char **args, int argc, int line_no) {
    (void)size;
    (void)line_no;
    if (op_is(op, "func") && argc == 1) { buf_appendf(text, "func %s\n", args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) { buf_append(text, "endfunc\n"); return; }
    if (op_is(op, "enter") && argc == 1) { buf_appendf(text, "  ; enter %s\n", args[0]); return; }
    if (op_is(op, "leave") && argc == 0) { buf_append(text, "  ; leave\n"); return; }
    if (op_is(op, "mov") && argc == 2) { buf_appendf(text, "  %s.mov %s, ", target, args[0]); buf_append(text, args[1]); buf_append(text, "\n"); return; }
    if (op_is(op, "load_addr") && argc == 2) { buf_appendf(text, "  %s.addr %s, %s\n", target, args[0], args[1]); return; }
    if ((op_is(op, "load") || op_is(op, "store")) && argc == 2) {
        buf_appendf(text, "  %s.%s %s, ", target, op, args[0]);
        buf_append(text, args[1]);
        buf_append(text, "\n");
        return;
    }
    if ((op_is(op, "add") || op_is(op, "sub") || op_is(op, "and") || op_is(op, "or") ||
         op_is(op, "xor") || op_is(op, "cmp") || op_is(op, "mul") || op_is(op, "div") ||
         op_is(op, "mod") || op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar")) && argc == 2) {
        buf_appendf(text, "  %s.%s %s, ", target, op, args[0]); buf_append(text, args[1]); buf_append(text, "\n"); return;
    }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") || op_is(op, "dec") || op_is(op, "push") || op_is(op, "pop")) && argc == 1) {
        buf_appendf(text, "  %s.%s %s\n", target, op, args[0]); return;
    }
    if ((op_is(op, "jmp") || op_is(op, "je") || op_is(op, "jne") || op_is(op, "jg") ||
         op_is(op, "jl") || op_is(op, "jge") || op_is(op, "jle") || op_is(op, "call")) && argc == 1) {
        buf_appendf(text, "  %s.%s %s\n", target, op, args[0]); return;
    }
    if (op_is(op, "ret") && argc == 0) { buf_appendf(text, "  %s.ret\n", target); return; }
    if (op_is(op, "syscall") && argc >= 1) { buf_appendf(text, "  %s.syscall %s\n", target, args[0]); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count for VM/IR target");
}

static void emit_encoded_or_toy_instruction(Buffer *text, const char *target, const char *op, const char *size, char **args, int argc, int line_no) {
    (void)size;
    (void)line_no;
    if (strcmp(target, "brainfuck") == 0) { buf_append(text, "+["); for (int i = 0; i < argc; i++) buf_append(text, "+"); buf_append(text, "-]\n"); return; }
    if (strcmp(target, "subleq") == 0 || strcmp(target, "urisc") == 0) { buf_appendf(text, "  subleq %s_tmp, %s_tmp, next\n", op, op); return; }
    if (strcmp(target, "redcode") == 0) { buf_appendf(text, "        MOV 0, 1        ; %s\n", op); return; }
    if (strcmp(target, "chip8") == 0 || strcmp(target, "schip8") == 0) { buf_appendf(text, "        ; %s CHIP-8 pseudo op\n", op); return; }
    if (strcmp(target, "befunge") == 0) { buf_appendf(text, ">:%s v\n", op); return; }
    if (strcmp(target, "turing-machine") == 0) { buf_appendf(text, "state_%s: read _ write _ move R goto next\n", op); return; }
    if (strcmp(target, "unlambda") == 0 || strcmp(target, "iota") == 0 || strcmp(target, "jot") == 0) { buf_appendf(text, "`k`k ; %s\n", op); return; }
    if (op_is(op, "func") && argc == 1) { buf_appendf(text, "%s:\n", args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) return;
    buf_appendf(text, "  %s.%s", target, op);
    for (int i = 0; i < argc; i++) { buf_append(text, i == 0 ? " " : ", "); buf_append(text, args[i]); }
    buf_append(text, "\n");
}

static void emit_x86_instruction(Buffer *text, const char *op, const char *size, char **args, int argc, int line_no) {
    if (op_is(op, "func") && argc == 1) {
        buf_appendf(text, "%s:\n", args[0]); return;
    }
    if (op_is(op, "endfunc") && argc == 0) return;
    if (op_is(op, "enter") && argc == 1) {
        buf_append(text, "  push rbp\n  mov rbp, rsp\n");
        if (strcmp(args[0], "0") != 0) buf_appendf(text, "  sub rsp, %s\n", args[0]);
        return;
    }
    if (op_is(op, "leave") && argc == 0) {
        buf_append(text, "  mov rsp, rbp\n  pop rbp\n"); return;
    }
    if (op_is(op, "mov") && argc == 2) {
        /* mov is the one instruction that can carry a full 64-bit immediate,
           but only into a register: a memory destination still takes imm32. */
        if (x86_needs_scratch(args[0], args[1]) ||
            (x86_reg_is_spilled(args[0]) && x86_imm_too_wide(args[1]))) {
            buf_appendf(text, "  mov %s, %s\n", X86_SCRATCH, x86_operand(args[1], line_no, op));
            buf_appendf(text, "  mov qword %s, %s\n", x86_reg(args[0], line_no, op), X86_SCRATCH);
        } else if (x86_reg_is_spilled(args[0])) {
            buf_appendf(text, "  mov qword %s, %s\n", x86_reg(args[0], line_no, op), x86_operand(args[1], line_no, op));
        } else {
            buf_appendf(text, "  mov %s, %s\n", x86_reg(args[0], line_no, op), x86_operand(args[1], line_no, op));
        }
        return;
    }
    if (op_is(op, "load_addr") && argc == 2) {
        /* lea cannot write to memory, so a spilled destination goes through
           the scratch register. */
        if (x86_reg_is_spilled(args[0])) {
            buf_appendf(text, "  lea %s, [rel %s]\n", X86_SCRATCH, args[1]);
            buf_appendf(text, "  mov qword %s, %s\n", x86_reg(args[0], line_no, op), X86_SCRATCH);
        } else {
            buf_appendf(text, "  lea %s, [rel %s]\n", x86_reg(args[0], line_no, op), args[1]);
        }
        return;
    }
    if (op_is(op, "load") && argc == 2) {
        char addr[256];
        bool spilled = x86_reg_is_spilled(args[0]);
        x86_emit_address(text, args[1], addr, sizeof(addr), line_no, op);
        if (strcmp(size, "q") == 0) {
            buf_appendf(text, "  mov %s, qword %s\n", spilled ? X86_SCRATCH : x86_reg(args[0], line_no, op), addr);
        } else if (strcmp(size, "d") == 0) {
            /* A 32-bit destination zero-extends, which is what the spilled
               path relies on before storing the full quadword back. */
            buf_appendf(text, "  mov %s, dword %s\n", spilled ? "eax" : x86_reg_sized(args[0], size, line_no, op), addr);
        } else {
            buf_appendf(text, "  movzx %s, %s %s\n", spilled ? X86_SCRATCH : x86_reg(args[0], line_no, op), x86_size_word(size), addr);
        }
        if (spilled) buf_appendf(text, "  mov qword %s, %s\n", x86_reg(args[0], line_no, op), X86_SCRATCH);
        return;
    }
    if (op_is(op, "store") && argc == 2) {
        char addr[256];
        x86_emit_address(text, args[0], addr, sizeof(addr), line_no, op);
        if (virtual_reg_index(args[1]) >= 0 && !x86_reg_is_spilled(args[1])) {
            buf_appendf(text, "  mov %s %s, %s\n", x86_size_word(size), addr, x86_reg_sized(args[1], size, line_no, op));
        } else if ((is_int(args[1]) || is_known_constant(args[1])) && !x86_imm_too_wide(args[1])) {
            /* A literal narrow enough to encode goes straight into the store
               rather than being staged in a register first. */
            buf_appendf(text, "  mov %s %s, %s\n", x86_size_word(size), addr, args[1]);
        } else {
            /* The destination is already a memory operand, so a spilled or
               symbolic source has to come through the scratch. */
            buf_appendf(text, "  mov %s, %s\n", X86_SCRATCH, x86_operand(args[1], line_no, op));
            buf_appendf(text, "  mov %s %s, %s\n", x86_size_word(size), addr, x86_rax_sized(size));
        }
        return;
    }
    if ((op_is(op, "popcnt") || op_is(op, "clz") || op_is(op, "ctz")) && argc == 1) {
        /* lzcnt and tzcnt need LZCNT/BMI1, and popcnt needs POPCNT;
           --target-info reports that so the requirement is not hidden. All
           three write a register, so a spilled destination round-trips. */
        const char *mnemonic = op_is(op, "popcnt") ? "popcnt" : op_is(op, "clz") ? "lzcnt" : "tzcnt";
        if (x86_reg_is_spilled(args[0])) {
            buf_appendf(text, "  %s %s, qword %s\n", mnemonic, X86_SCRATCH, x86_reg(args[0], line_no, op));
            buf_appendf(text, "  mov qword %s, %s\n", x86_reg(args[0], line_no, op), X86_SCRATCH);
        } else {
            buf_appendf(text, "  %s %s, %s\n", mnemonic, x86_reg(args[0], line_no, op), x86_reg(args[0], line_no, op));
        }
        return;
    }
    if ((op_is(op, "min") || op_is(op, "max")) && argc == 2) {
        /* cmov cannot take an immediate, so the other operand goes through a
           register whatever it started as. */
        const char *cc = op_is(op, "min") ? "cmovg" : "cmovl";
        buf_appendf(text, "  mov %s, %s\n", X86_SCRATCH, x86_reg(args[0], line_no, op));
        buf_appendf(text, "  mov %s, %s\n", X86_ADDR_SCRATCH, x86_operand(args[1], line_no, op));
        buf_appendf(text, "  cmp %s, %s\n", X86_SCRATCH, X86_ADDR_SCRATCH);
        buf_appendf(text, "  %s %s, %s\n", cc, X86_SCRATCH, X86_ADDR_SCRATCH);
        if (x86_reg_is_spilled(args[0])) {
            buf_appendf(text, "  mov qword %s, %s\n", x86_reg(args[0], line_no, op), X86_SCRATCH);
        } else {
            buf_appendf(text, "  mov %s, %s\n", x86_reg(args[0], line_no, op), X86_SCRATCH);
        }
        return;
    }
    if (op_is(op, "sel") && argc == 3) {
        /* The condition is read out of the destination, so it is tested
           before either arm is loaded. Moving a value does not touch the
           flags, so the compare survives until the cmov reads it. */
        if (x86_reg_is_spilled(args[0])) {
            buf_appendf(text, "  cmp qword %s, 0\n", x86_reg(args[0], line_no, op));
        } else {
            buf_appendf(text, "  cmp %s, 0\n", x86_reg(args[0], line_no, op));
        }
        buf_appendf(text, "  mov %s, %s\n", X86_SCRATCH, x86_operand(args[1], line_no, op));
        buf_appendf(text, "  mov %s, %s\n", X86_ADDR_SCRATCH, x86_operand(args[2], line_no, op));
        buf_appendf(text, "  cmovz %s, %s\n", X86_SCRATCH, X86_ADDR_SCRATCH);
        if (x86_reg_is_spilled(args[0])) {
            buf_appendf(text, "  mov qword %s, %s\n", x86_reg(args[0], line_no, op), X86_SCRATCH);
        } else {
            buf_appendf(text, "  mov %s, %s\n", x86_reg(args[0], line_no, op), X86_SCRATCH);
        }
        return;
    }
    if (op_is(op, "bswap") && argc == 1) {
        /* bswap takes a register only. */
        if (x86_reg_is_spilled(args[0])) {
            buf_appendf(text, "  mov %s, %s\n", X86_SCRATCH, x86_reg(args[0], line_no, op));
            buf_appendf(text, "  bswap %s\n", X86_SCRATCH);
            buf_appendf(text, "  mov qword %s, %s\n", x86_reg(args[0], line_no, op), X86_SCRATCH);
        } else {
            buf_appendf(text, "  bswap %s\n", x86_reg(args[0], line_no, op));
        }
        return;
    }
    if ((op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar") ||
         op_is(op, "rol") || op_is(op, "ror")) && argc == 2) {
        bool dst_mem = x86_reg_is_spilled(args[0]);
        if (virtual_reg_index(args[1]) < 0) {
            if (dst_mem) buf_appendf(text, "  %s qword %s, %s\n", op, x86_reg(args[0], line_no, op), x86_operand(args[1], line_no, op));
            else buf_appendf(text, "  %s %s, %s\n", op, x86_reg(args[0], line_no, op), x86_operand(args[1], line_no, op));
            return;
        }
        /* A variable shift count must sit in cl, but rcx carries a virtual
           register, so the value is shifted in the scratch while rcx is
           borrowed. A count that already lives in rcx needs no borrowing at
           all. This stays correct when the count and the destination are the
           same virtual register. */
        {
            bool count_in_cl = strcmp(x86_operand(args[1], line_no, op), "rcx") == 0;
            buf_appendf(text, "  mov %s, %s\n", X86_SCRATCH, x86_reg(args[0], line_no, op));
            if (!count_in_cl) {
                buf_append(text, "  push rcx\n");
                buf_appendf(text, "  mov rcx, %s\n", x86_operand(args[1], line_no, op));
            }
            buf_appendf(text, "  %s %s, cl\n", op, X86_SCRATCH);
            if (!count_in_cl) buf_append(text, "  pop rcx\n");
            buf_appendf(text, "  mov %s%s, %s\n", dst_mem ? "qword " : "", x86_reg(args[0], line_no, op), X86_SCRATCH);
        }
        return;
    }
    if ((op_is(op, "add") || op_is(op, "sub") || op_is(op, "and") ||
         op_is(op, "or") || op_is(op, "xor") || op_is(op, "cmp")) && argc == 2) {
        if (x86_needs_scratch(args[0], args[1]) || x86_imm_too_wide(args[1])) {
            buf_appendf(text, "  mov %s, %s\n", X86_SCRATCH, x86_operand(args[1], line_no, op));
            buf_appendf(text, "  %s %s%s, %s\n", op, x86_reg_is_spilled(args[0]) ? "qword " : "",
                        x86_reg(args[0], line_no, op), X86_SCRATCH);
        } else if (x86_reg_is_spilled(args[0])) {
            buf_appendf(text, "  %s qword %s, %s\n", op, x86_reg(args[0], line_no, op), x86_operand(args[1], line_no, op));
        } else {
            buf_appendf(text, "  %s %s, %s\n", op, x86_reg(args[0], line_no, op), x86_operand(args[1], line_no, op));
        }
        return;
    }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") || op_is(op, "dec")) && argc == 1) {
        buf_appendf(text, "  %s %s%s\n", op, x86_reg_is_spilled(args[0]) ? "qword " : "", x86_reg(args[0], line_no, op));
        return;
    }
    if (op_is(op, "mul") && argc == 2) {
        /* imul cannot write to memory, and takes no 64-bit immediate. */
        if (x86_imm_too_wide(args[1])) {
            buf_appendf(text, "  mov %s, %s\n", X86_ADDR_SCRATCH, x86_operand(args[1], line_no, op));
            if (x86_reg_is_spilled(args[0])) {
                buf_appendf(text, "  mov %s, %s\n", X86_SCRATCH, x86_reg(args[0], line_no, op));
                buf_appendf(text, "  imul %s, %s\n", X86_SCRATCH, X86_ADDR_SCRATCH);
                buf_appendf(text, "  mov qword %s, %s\n", x86_reg(args[0], line_no, op), X86_SCRATCH);
            } else {
                buf_appendf(text, "  imul %s, %s\n", x86_reg(args[0], line_no, op), X86_ADDR_SCRATCH);
            }
            return;
        }
        if (x86_reg_is_spilled(args[0])) {
            buf_appendf(text, "  mov %s, %s\n", X86_SCRATCH, x86_reg(args[0], line_no, op));
            buf_appendf(text, "  imul %s, %s\n", X86_SCRATCH, x86_operand(args[1], line_no, op));
            buf_appendf(text, "  mov qword %s, %s\n", x86_reg(args[0], line_no, op), X86_SCRATCH);
        } else {
            buf_appendf(text, "  imul %s, %s\n", x86_reg(args[0], line_no, op), x86_operand(args[1], line_no, op));
        }
        return;
    }
    if ((op_is(op, "div") || op_is(op, "mod")) && argc == 2) {
        /* idiv is hard-wired to rax:rdx. rdx carries a virtual register, so it
           is saved across the division; the divisor is copied out before cqo
           overwrites rdx, which keeps "div r0, r2" correct. */
        buf_append(text, "  push rdx\n");
        buf_appendf(text, "  mov %s, %s\n", X86_SCRATCH, x86_reg(args[0], line_no, op));
        buf_appendf(text, "  mov %s, %s\n", X86_ADDR_SCRATCH, x86_operand(args[1], line_no, op));
        buf_append(text, "  cqo\n");
        buf_appendf(text, "  idiv %s\n", X86_ADDR_SCRATCH);
        if (op_is(op, "mod")) {
            buf_appendf(text, "  mov %s, rdx\n", X86_SCRATCH);
        }
        buf_append(text, "  pop rdx\n");
        buf_appendf(text, "  mov %s%s, %s\n", x86_reg_is_spilled(args[0]) ? "qword " : "", x86_reg(args[0], line_no, op), X86_SCRATCH);
        return;
    }
    if (op_is(op, "push") && argc == 1) {
        /* push takes imm32 at most. */
        if (x86_imm_too_wide(args[0])) {
            buf_appendf(text, "  mov %s, %s\n", X86_SCRATCH, x86_operand(args[0], line_no, op));
            buf_appendf(text, "  push %s\n", X86_SCRATCH);
            return;
        }
        buf_appendf(text, "  push %s%s\n", x86_reg_is_spilled(args[0]) ? "qword " : "", x86_operand(args[0], line_no, op));
        return;
    }
    if (op_is(op, "pop") && argc == 1) {
        buf_appendf(text, "  pop %s%s\n", x86_reg_is_spilled(args[0]) ? "qword " : "", x86_reg(args[0], line_no, op));
        return;
    }
    if ((op_is(op, "jmp") || op_is(op, "call") || op_is(op, "je") || op_is(op, "jne") ||
         op_is(op, "jg") || op_is(op, "jl") || op_is(op, "jge") || op_is(op, "jle") ||
         op_is(op, "ja") || op_is(op, "jb") || op_is(op, "jae") || op_is(op, "jbe")) && argc == 1) {
        buf_appendf(text, "  %s %s\n", op, args[0]); return;
    }
    if (op_is(op, "ret") && argc == 0) { buf_append(text, "  ret\n"); return; }
    if (op_is(op, "syscall")) { emit_x86_syscall(text, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count");
}

/* A machine without condition flags has to get a compare to its branch some
   other way. The old RISC-V lowering copied both operands into a6/a7 and
   branched on those, but a6/a7 are also the scratch and syscall-number
   registers, so any syscall between the compare and the branch silently
   replaced the comparison. The operands are recorded instead and folded into
   the branch itself, which is both correct and two instructions shorter. When
   an unrelated instruction comes between the two, the comparison is first
   snapshotted into a register pair nothing else touches, so it still
   describes the values as of the compare.

   RISC-V and MIPS both work this way and spell the comparing branches the
   same, so they share this and differ only in the registers they snapshot
   into and how they spell a register-to-register move. */
typedef enum { CMP_NONE, CMP_PENDING, CMP_SNAPSHOT } CompareState;

typedef struct {
    CompareState state;
    char lhs[64];
    char rhs[64];
    bool rhs_is_reg;
    const char *lhs_snapshot;
    const char *rhs_snapshot;
    const char *move_op;
    const char *load_imm_op;
} DeferredCompare;

#define RV_SCRATCH "a6"

static DeferredCompare rv_cmp = {CMP_NONE, {0}, {0}, false, "s10", "s11", "mv", "li"};

static bool rv_parse_long(const char *text, long long *value) {
    char *end = NULL;
    errno = 0;
    *value = strtoll(text, &end, 0);
    return errno == 0 && end && *end == '\0' && end != text;
}

/* addi and friends take a signed 12-bit immediate; wider values must be
   materialised into a register first. */
static bool rv_fits_imm12(long long value) {
    return value >= -2048 && value <= 2047;
}

static void compare_flush(Buffer *text, DeferredCompare *cmp) {
    if (cmp->state != CMP_PENDING) return;
    buf_appendf(text, "  %s %s, %s\n", cmp->move_op, cmp->lhs_snapshot, cmp->lhs);
    if (cmp->rhs_is_reg) buf_appendf(text, "  %s %s, %s\n", cmp->move_op, cmp->rhs_snapshot, cmp->rhs);
    else buf_appendf(text, "  %s %s, %s\n", cmp->load_imm_op, cmp->rhs_snapshot, cmp->rhs);
    snprintf(cmp->lhs, sizeof(cmp->lhs), "%s", cmp->lhs_snapshot);
    snprintf(cmp->rhs, sizeof(cmp->rhs), "%s", cmp->rhs_snapshot);
    cmp->rhs_is_reg = true;
    cmp->state = CMP_SNAPSHOT;
}

/* Instructions whose first operand is the register they write. */
static bool writes_first_arg(const char *op) {
    return op_is(op, "mov") || op_is(op, "load_addr") || op_is(op, "load") ||
           op_is(op, "add") || op_is(op, "sub") || op_is(op, "and") ||
           op_is(op, "or") || op_is(op, "xor") || op_is(op, "shl") ||
           op_is(op, "shr") || op_is(op, "sar") || op_is(op, "neg") ||
           op_is(op, "not") || op_is(op, "inc") || op_is(op, "dec") ||
           op_is(op, "mul") || op_is(op, "div") || op_is(op, "mod") ||
           op_is(op, "pop") || op_is(op, "popcnt") || op_is(op, "clz") ||
           op_is(op, "ctz") || op_is(op, "bswap") || op_is(op, "rol") ||
           op_is(op, "ror");
}

static bool compare_reads(const DeferredCompare *cmp, const char *reg) {
    if (cmp->state != CMP_PENDING) return false;
    if (strcmp(cmp->lhs, reg) == 0) return true;
    return cmp->rhs_is_reg && strcmp(cmp->rhs, reg) == 0;
}

static void compare_record(DeferredCompare *cmp, const char *lhs, const char *rhs, bool rhs_is_reg) {
    snprintf(cmp->lhs, sizeof(cmp->lhs), "%s", lhs);
    snprintf(cmp->rhs, sizeof(cmp->rhs), "%s", rhs);
    cmp->rhs_is_reg = rhs_is_reg;
    cmp->state = CMP_PENDING;
}

/* A label starts a new basic block, so a comparison recorded before it no
   longer describes the state of anything that jumps here. It is dropped
   rather than snapshotted: writing the snapshot out would only leave dead
   copies behind the common case where a branch already consumed it. */
static void compare_discard(DeferredCompare *cmp) {
    cmp->state = CMP_NONE;
}

static bool is_conditional_branch(const char *op) {
    return op_is(op, "je") || op_is(op, "jne") || op_is(op, "jg") ||
           op_is(op, "jl") || op_is(op, "jge") || op_is(op, "jle") ||
           op_is(op, "ja") || op_is(op, "jb") || op_is(op, "jae") ||
           op_is(op, "jbe");
}

static void compare_emit_branch(Buffer *text, DeferredCompare *cmp, const char *op,
                                const char *label, int line_no) {
    const char *mnemonic =
        op_is(op, "je") ? "beq" : op_is(op, "jne") ? "bne" :
        op_is(op, "jg") ? "bgt" : op_is(op, "jl") ? "blt" :
        op_is(op, "jge") ? "bge" : op_is(op, "jle") ? "ble" :
        op_is(op, "ja") ? "bgtu" : op_is(op, "jb") ? "bltu" :
        op_is(op, "jae") ? "bgeu" : "bleu";
    const char *rhs;
    if (cmp->state == CMP_NONE) {
        line_error(line_no, op, "conditional jump with no preceding cmp");
    }
    if (cmp->rhs_is_reg) {
        rhs = cmp->rhs;
    } else {
        /* Branches compare registers only, so an immediate operand is
           materialised at the branch rather than kept live in a register. */
        buf_appendf(text, "  %s %s, %s\n", cmp->load_imm_op, cmp->rhs_snapshot, cmp->rhs);
        rhs = cmp->rhs_snapshot;
    }
    buf_appendf(text, "  %s %s, %s, %s\n", mnemonic, cmp->lhs, rhs, label);
}

static void emit_rv_instruction(Buffer *text, const char *target, const char *op, const char *size,
                                char **args, int argc, int line_no) {
    /* rv32 and rv64 differ only in how wide a word is: same registers, same
       mnemonics for everything narrower, and the same syscall numbers. */
    const bool wide = !target_has_flag(target, TF_RV32);
    const char *word_load = wide ? "ld" : "lw";
    const char *word_store = wide ? "sd" : "sw";
    const int slot = wide ? 8 : 4;
    /* A pending comparison only has to be pinned down when something is about
       to disturb the registers it names. Snapshotting on every intervening
       instruction instead would leave dead copies behind after the common
       "cmp, branch, carry on" sequence. A call is enough on its own: the
       operands live in caller-saved registers. */
    if (!is_conditional_branch(op) && strcmp(op, "cmp") != 0) {
        if (op_is(op, "call")) {
            compare_flush(text, &rv_cmp);
        } else if (argc >= 1 && writes_first_arg(op)) {
            int dst = virtual_reg_index(args[0]);
            if (dst >= 0 && compare_reads(&rv_cmp, rv_regs[dst])) {
                compare_flush(text, &rv_cmp);
            }
        }
    }
    if (op_is(op, "func") && argc == 1) { buf_appendf(text, "%s:\n", args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) return;
    if (op_is(op, "enter") && argc == 1) {
        long long frame;
        buf_appendf(text, "  addi sp, sp, -%d\n  %s ra, %d(sp)\n  %s s0, 0(sp)\n  mv s0, sp\n",
                    slot * 2, word_store, slot, word_store);
        if (strcmp(args[0], "0") == 0) {
            return;
        }
        /* A frame wider than a 12-bit immediate cannot ride along in addi. */
        if (rv_parse_long(args[0], &frame) && frame != LLONG_MIN && rv_fits_imm12(-frame)) {
            buf_appendf(text, "  addi sp, sp, %lld\n", -frame);
        } else {
            buf_appendf(text, "  li %s, %s\n", RV_SCRATCH, args[0]);
            buf_appendf(text, "  sub sp, sp, %s\n", RV_SCRATCH);
        }
        return;
    }
    if (op_is(op, "leave") && argc == 0) {
        buf_appendf(text, "  mv sp, s0\n  %s s0, 0(sp)\n  %s ra, %d(sp)\n  addi sp, sp, %d\n",
                    word_load, word_load, slot, slot * 2); return;
    }
    if (op_is(op, "mov") && argc == 2) {
        int src = virtual_reg_index(args[1]);
        if (src >= 0) buf_appendf(text, "  mv %s, %s\n", rv_reg(args[0], line_no, op), rv_regs[src]);
        else buf_appendf(text, "  li %s, %s\n", rv_reg(args[0], line_no, op), args[1]);
        return;
    }
    if (op_is(op, "load_addr") && argc == 2) { buf_appendf(text, "  la %s, %s\n", rv_reg(args[0], line_no, op), args[1]); return; }
    if ((op_is(op, "load") || op_is(op, "store")) && argc == 2) {
        long long off = 0;
        const char *base;
        if (op_is(op, "load")) {
            rv_emit_address_setup(text, args[1], "a6", line_no, op);
            base = rv_address_base(args[1], "a6", line_no, op, &off);
            char offstr[32]; snprintf(offstr, sizeof(offstr), "%lld", off);
            buf_appendf(text, "  %s %s, ", rv_load_op(size, wide), rv_reg(args[0], line_no, op));
            buf_append(text, offstr); buf_appendf(text, "(%s)\n", base);
        } else {
            int src = virtual_reg_index(args[1]);
            if (src < 0) buf_appendf(text, "  li a7, %s\n", args[1]);
            rv_emit_address_setup(text, args[0], "a6", line_no, op);
            base = rv_address_base(args[0], "a6", line_no, op, &off);
            char offstr[32]; snprintf(offstr, sizeof(offstr), "%lld", off);
            buf_appendf(text, "  %s %s, ", rv_store_op(size, wide), src >= 0 ? rv_regs[src] : "a7");
            buf_append(text, offstr); buf_appendf(text, "(%s)\n", base);
        }
        return;
    }
    if ((op_is(op, "add") || op_is(op, "sub") || op_is(op, "and") ||
         op_is(op, "or") || op_is(op, "xor")) && argc == 2) {
        int src = virtual_reg_index(args[1]);
        const char *dst = rv_reg(args[0], line_no, op);
        long long value;
        if (src >= 0) {
            buf_appendf(text, "  %s %s, %s, %s\n", op, dst, dst, rv_regs[src]);
            return;
        }
        /* "sub rX, imm" lowers to an addi of the negated immediate. The old
           lowering spelled that by writing a minus in front of the operand
           text, which turned "sub r0, -5" into "addi t0, t0, --5"; negating
           the parsed value instead keeps both signs working. */
        if (rv_parse_long(args[1], &value) && !(op_is(op, "sub") && value == LLONG_MIN)) {
            long long applied = op_is(op, "sub") ? -value : value;
            if (rv_fits_imm12(applied)) {
                const char *immop = op_is(op, "and") ? "andi" :
                                    op_is(op, "or") ? "ori" :
                                    op_is(op, "xor") ? "xori" : "addi";
                buf_appendf(text, "  %s %s, %s, %lld\n", immop, dst, dst, applied);
                return;
            }
        }
        /* Symbolic operands, and values too wide for a 12-bit field, have to
           be materialised into a register first. */
        buf_appendf(text, "  li %s, %s\n", RV_SCRATCH, args[1]);
        buf_appendf(text, "  %s %s, %s, %s\n", op, dst, dst, RV_SCRATCH);
        return;
    }
    if ((op_is(op, "popcnt") || op_is(op, "clz") || op_is(op, "ctz") || op_is(op, "bswap")) && argc == 1) {
        /* Only reached on a target whose capabilities include these, which
           today means the bit-manipulation extension. */
        const char *dst = rv_reg(args[0], line_no, op);
        const char *native = op_is(op, "popcnt") ? "cpop" : op_is(op, "clz") ? "clz" :
                             op_is(op, "ctz") ? "ctz" : "rev8";
        buf_appendf(text, "  %s %s, %s\n", native, dst, dst);
        return;
    }
    if ((op_is(op, "min") || op_is(op, "max")) && argc == 2) {
        /* The bit-manipulation extension carries these as ordinary
           three-operand instructions. They only take registers, so an
           immediate is loaded first. */
        const char *dst = rv_reg(args[0], line_no, op);
        int src = virtual_reg_index(args[1]);
        const char *rhs;
        if (src >= 0) {
            rhs = rv_regs[src];
        } else {
            buf_appendf(text, "  li %s, %s\n", RV_SCRATCH, args[1]);
            rhs = RV_SCRATCH;
        }
        buf_appendf(text, "  %s %s, %s, %s\n", op, dst, dst, rhs);
        return;
    }
    if ((op_is(op, "rol") || op_is(op, "ror")) && argc == 2) {
        const char *dst = rv_reg(args[0], line_no, op);
        int src = virtual_reg_index(args[1]);
        if (src >= 0) {
            buf_appendf(text, "  %s %s, %s, %s\n", op, dst, dst, rv_regs[src]);
        } else {
            /* There is only a rotate-right immediate, so a left rotation uses
               the complement of the distance. */
            long long amount = strtoll(args[1], NULL, 0) & 63;
            if (op_is(op, "rol")) amount = (64 - amount) & 63;
            buf_appendf(text, "  rori %s, %s, %lld\n", dst, dst, amount);
        }
        return;
    }
    if ((op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar")) && argc == 2) {
        int src = virtual_reg_index(args[1]);
        const char *native = op_is(op, "shl") ? "sll" : (op_is(op, "shr") ? "srl" : "sra");
        const char *nativei = op_is(op, "shl") ? "slli" : (op_is(op, "shr") ? "srli" : "srai");
        if (src >= 0) buf_appendf(text, "  %s %s, %s, ", native, rv_reg(args[0], line_no, op), rv_reg(args[0], line_no, op));
        else buf_appendf(text, "  %s %s, %s, ", nativei, rv_reg(args[0], line_no, op), rv_reg(args[0], line_no, op));
        buf_append(text, src >= 0 ? rv_regs[src] : args[1]); buf_append(text, "\n"); return;
    }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") || op_is(op, "dec")) && argc == 1) {
        const char *dst = rv_reg(args[0], line_no, op);
        if (op_is(op, "neg")) buf_appendf(text, "  neg %s, %s\n", dst, dst);
        else if (op_is(op, "not")) buf_appendf(text, "  not %s, %s\n", dst, dst);
        else buf_appendf(text, op_is(op, "inc") ? "  addi %s, %s, 1\n" : "  addi %s, %s, -1\n", dst, dst);
        return;
    }
    if ((op_is(op, "mul") || op_is(op, "div") || op_is(op, "mod")) && argc == 2) {
        int src = virtual_reg_index(args[1]);
        const char *native = op_is(op, "mul") ? "mul" : (op_is(op, "div") ? "div" : "rem");
        if (src < 0) buf_appendf(text, "  li a6, %s\n", args[1]);
        buf_appendf(text, "  %s %s, %s, ", native, rv_reg(args[0], line_no, op), rv_reg(args[0], line_no, op));
        buf_append(text, src >= 0 ? rv_regs[src] : "a6"); buf_append(text, "\n"); return;
    }
    if (op_is(op, "cmp") && argc == 2) {
        int src = virtual_reg_index(args[1]);
        compare_record(&rv_cmp, rv_reg(args[0], line_no, op),
                       src >= 0 ? rv_regs[src] : args[1], src >= 0);
        return;
    }
    if (is_conditional_branch(op) && argc == 1) {
        compare_emit_branch(text, &rv_cmp, op, args[0], line_no);
        return;
    }
    if (op_is(op, "push") && argc == 1) {
        int src = virtual_reg_index(args[0]);
        if (src < 0) buf_appendf(text, "  li a6, %s\n", args[0]);
        buf_appendf(text, "  addi sp, sp, -%d\n", slot);
        buf_appendf(text, "  %s %s, 0(sp)\n", word_store, src >= 0 ? rv_regs[src] : "a6"); return;
    }
    if (op_is(op, "pop") && argc == 1) { buf_appendf(text, "  %s %s, 0(sp)\n  addi sp, sp, %d\n", word_load, rv_reg(args[0], line_no, op), slot); return; }
    if (op_is(op, "jmp") && argc == 1) { buf_appendf(text, "  j %s\n", args[0]); return; }
    if (op_is(op, "call") && argc == 1) { buf_appendf(text, "  call %s\n", args[0]); return; }
    if (op_is(op, "ret") && argc == 0) { buf_append(text, "  ret\n"); return; }
    if (op_is(op, "syscall")) { emit_rv_syscall(text, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count");
}

/* --------------------------------------------- extended operation fallback */

static void emit_text_line(Buffer *text, char *line, int line_no, const char *target);

/* Emits one line of CommonASM through the normal path. The expansions below
   are written in the language itself, so a target without the instruction
   gets a working version of it without a hand-written sequence per backend. */
static void emit_cas(Buffer *text, const char *target, int line_no, const char *fmt, ...) CAS_PRINTF(4, 5);

/* When set, emit_cas() collects CommonASM text instead of compiling it. That
   is how a whole assembly file is read back into a source the ordinary
   pipeline can then compile for a different machine. */
static Buffer *cas_sink = NULL;

static void emit_cas(Buffer *text, const char *target, int line_no, const char *fmt, ...) {
    char line[256];
    va_list args;
    int written;
    va_start(args, fmt);
    written = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written >= sizeof(line)) {
        die("could not expand an extended instruction");
    }
    if (cas_sink) {
        buf_appendf(cas_sink, "  %s\n", line);
        return;
    }
    emit_text_line(text, line, line_no, target);
}

/* Picks two virtual registers the expansion can borrow. They are saved and
   restored around it, so an expansion leaves nothing behind but its result. */
static void ext_pick_scratch(const char *target, const char *dst, const char *other, int *s1, int *s2) {
    int avoid_a = virtual_reg_index(dst);
    int avoid_b = other ? virtual_reg_index(other) : -1;
    int picked = 0;
    *s1 = *s2 = -1;
    /* Counting down from the highest register the target can name, so the
       borrowed ones are the least likely to be in use. */
    for (int i = target_max_vreg(target); i >= 0 && picked < 2; i--) {
        if (i == avoid_a || i == avoid_b) continue;
        if (picked == 0) *s1 = i; else *s2 = i;
        picked++;
    }
}

/* The masks the population count folds with, cut down to the target word. */
static void ext_popcount_masks(int bits, char *m1, char *m2, char *m4, char *h01, size_t size) {
    unsigned long long width = bits >= 64 ? ~0ull : ((1ull << bits) - 1u);
    snprintf(m1, size, "0x%llx", 0x5555555555555555ull & width);
    snprintf(m2, size, "0x%llx", 0x3333333333333333ull & width);
    snprintf(m4, size, "0x%llx", 0x0f0f0f0f0f0f0f0full & width);
    snprintf(h01, size, "0x%llx", 0x0101010101010101ull & width);
}

static void ext_expand_popcnt(Buffer *text, const char *target, int line_no, const char *dst) {
    char m1[24], m2[24], m4[24], h01[24];
    int bits = target_word_bits(target);
    int s1, s2;
    ext_pick_scratch(target, dst, NULL, &s1, &s2);
    ext_popcount_masks(bits, m1, m2, m4, h01, sizeof(m1));
    /* The usual SWAR fold: pair counts, then nibble counts, then a multiply
       that sums every byte into the top one. */
    emit_cas(text, target, line_no, "push r%d", s1);
    emit_cas(text, target, line_no, "mov r%d, %s", s1, dst);
    emit_cas(text, target, line_no, "shr r%d, 1", s1);
    emit_cas(text, target, line_no, "and r%d, %s", s1, m1);
    emit_cas(text, target, line_no, "sub %s, r%d", dst, s1);
    emit_cas(text, target, line_no, "mov r%d, %s", s1, dst);
    emit_cas(text, target, line_no, "shr r%d, 2", s1);
    emit_cas(text, target, line_no, "and r%d, %s", s1, m2);
    emit_cas(text, target, line_no, "and %s, %s", dst, m2);
    emit_cas(text, target, line_no, "add %s, r%d", dst, s1);
    emit_cas(text, target, line_no, "mov r%d, %s", s1, dst);
    emit_cas(text, target, line_no, "shr r%d, 4", s1);
    emit_cas(text, target, line_no, "add %s, r%d", dst, s1);
    emit_cas(text, target, line_no, "and %s, %s", dst, m4);
    emit_cas(text, target, line_no, "mul %s, %s", dst, h01);
    emit_cas(text, target, line_no, "shr %s, %d", dst, bits - 8);
    emit_cas(text, target, line_no, "pop r%d", s1);
    (void)s2;
}

static void ext_expand_clz(Buffer *text, const char *target, int line_no, const char *dst) {
    int bits = target_word_bits(target);
    int s1, s2;
    ext_pick_scratch(target, dst, NULL, &s1, &s2);
    /* Smear the highest set bit down, then count the zeros that survived.
       An all-zero input smears to zero and correctly reports the full width. */
    emit_cas(text, target, line_no, "push r%d", s1);
    for (int shift = 1; shift < bits; shift *= 2) {
        emit_cas(text, target, line_no, "mov r%d, %s", s1, dst);
        emit_cas(text, target, line_no, "shr r%d, %d", s1, shift);
        emit_cas(text, target, line_no, "or %s, r%d", dst, s1);
    }
    emit_cas(text, target, line_no, "not %s", dst);
    emit_cas(text, target, line_no, "popcnt %s", dst);
    emit_cas(text, target, line_no, "pop r%d", s1);
    (void)s2;
}

static void ext_expand_ctz(Buffer *text, const char *target, int line_no, const char *dst) {
    int s1, s2;
    ext_pick_scratch(target, dst, NULL, &s1, &s2);
    /* Isolate the lowest set bit, then count the bits below it. Zero isolates
       to zero, and zero minus one is all ones, which reports the full width. */
    emit_cas(text, target, line_no, "push r%d", s1);
    emit_cas(text, target, line_no, "mov r%d, %s", s1, dst);
    emit_cas(text, target, line_no, "neg r%d", s1);
    emit_cas(text, target, line_no, "and %s, r%d", dst, s1);
    emit_cas(text, target, line_no, "sub %s, 1", dst);
    emit_cas(text, target, line_no, "popcnt %s", dst);
    emit_cas(text, target, line_no, "pop r%d", s1);
    (void)s2;
}

static void ext_expand_bswap(Buffer *text, const char *target, int line_no, const char *dst) {
    int bits = target_word_bits(target);
    int s1, s2;
    ext_pick_scratch(target, dst, NULL, &s1, &s2);
    emit_cas(text, target, line_no, "push r%d", s1);
    /* Swap neighbouring bytes, then neighbouring pairs, and so on up to the
       two halves of the word. */
    for (int width = 8; width < bits; width *= 2) {
        char mask[24];
        unsigned long long pattern = 0;
        unsigned long long field = width == 8 ? 0xffull : (width == 16 ? 0xffffull : 0xffffffffull);
        if (width * 2 < bits) {
            for (int shift = 0; shift < bits; shift += width * 2) {
                pattern |= field << shift;
            }
            snprintf(mask, sizeof(mask), "0x%llx", pattern);
            emit_cas(text, target, line_no, "mov r%d, %s", s1, dst);
            emit_cas(text, target, line_no, "shr r%d, %d", s1, width);
            emit_cas(text, target, line_no, "and r%d, %s", s1, mask);
            emit_cas(text, target, line_no, "and %s, %s", dst, mask);
            emit_cas(text, target, line_no, "shl %s, %d", dst, width);
            emit_cas(text, target, line_no, "or %s, r%d", dst, s1);
        } else {
            /* The final step swaps the two halves, where no mask is needed
               because the bits that would be masked shift out anyway. */
            emit_cas(text, target, line_no, "mov r%d, %s", s1, dst);
            emit_cas(text, target, line_no, "shr r%d, %d", s1, width);
            emit_cas(text, target, line_no, "shl %s, %d", dst, width);
            emit_cas(text, target, line_no, "or %s, r%d", dst, s1);
        }
    }
    emit_cas(text, target, line_no, "pop r%d", s1);
    (void)s2;
}

static void ext_expand_rotate(Buffer *text, const char *target, int line_no,
                              const char *dst, const char *amount, bool left) {
    int bits = target_word_bits(target);
    int s1, s2;
    ext_pick_scratch(target, dst, amount, &s1, &s2);
    /* One shift each way, with the distances reduced modulo the word size so
       that a rotation of zero leaves the value alone instead of shifting by a
       full word, which is undefined. */
    emit_cas(text, target, line_no, "push r%d", s1);
    emit_cas(text, target, line_no, "push r%d", s2);
    emit_cas(text, target, line_no, "mov r%d, %s", s2, amount);
    emit_cas(text, target, line_no, "and r%d, %d", s2, bits - 1);
    emit_cas(text, target, line_no, "mov r%d, %s", s1, dst);
    emit_cas(text, target, line_no, "%s %s, r%d", left ? "shl" : "shr", dst, s2);
    emit_cas(text, target, line_no, "neg r%d", s2);
    emit_cas(text, target, line_no, "and r%d, %d", s2, bits - 1);
    emit_cas(text, target, line_no, "%s r%d, r%d", left ? "shr" : "shl", s1, s2);
    emit_cas(text, target, line_no, "or %s, r%d", dst, s1);
    emit_cas(text, target, line_no, "pop r%d", s2);
    emit_cas(text, target, line_no, "pop r%d", s1);
}

/* Labels the expansions below branch over. Nothing in the language can
   produce this spelling, since a source label cannot start with a digit and
   these are only ever emitted here. */
static int ext_label_serial = 0;

static void ext_expand_abs(Buffer *text, const char *target, int line_no, const char *dst) {
    int bits = target_word_bits(target);
    int s1, s2;
    ext_pick_scratch(target, dst, NULL, &s1, &s2);
    /* The sign bit smeared across the word is zero for a positive value and
       all ones for a negative one, and x ^ m - m is x in the first case and
       -x in the second. No branch, and no compare to lose. */
    emit_cas(text, target, line_no, "push r%d", s1);
    emit_cas(text, target, line_no, "mov r%d, %s", s1, dst);
    emit_cas(text, target, line_no, "sar r%d, %d", s1, bits - 1);
    emit_cas(text, target, line_no, "xor %s, r%d", dst, s1);
    emit_cas(text, target, line_no, "sub %s, r%d", dst, s1);
    emit_cas(text, target, line_no, "pop r%d", s1);
    (void)s2;
}

static void ext_expand_minmax(Buffer *text, const char *target, int line_no,
                              const char *dst, const char *other, bool want_min) {
    int serial = ++ext_label_serial;
    /* Keeping what is already there when it is already the answer means the
       operand is only read once, which matters when it is a spill slot. */
    emit_cas(text, target, line_no, "cmp %s, %s", dst, other);
    emit_cas(text, target, line_no, "%s __cas_ext_%d", want_min ? "jle" : "jge", serial);
    emit_cas(text, target, line_no, "mov %s, %s", dst, other);
    emit_cas(text, target, line_no, "__cas_ext_%d:", serial);
}

static void ext_expand_sel(Buffer *text, const char *target, int line_no,
                           const char *dst, const char *when_true, const char *when_false) {
    int taken = ++ext_label_serial;
    int done = ++ext_label_serial;
    /* The destination carries the condition in, so both arms are chosen
       before either one writes it, and naming it as an arm is allowed. */
    emit_cas(text, target, line_no, "cmp %s, 0", dst);
    emit_cas(text, target, line_no, "jne __cas_ext_%d", taken);
    emit_cas(text, target, line_no, "mov %s, %s", dst, when_false);
    emit_cas(text, target, line_no, "jmp __cas_ext_%d", done);
    emit_cas(text, target, line_no, "__cas_ext_%d:", taken);
    emit_cas(text, target, line_no, "mov %s, %s", dst, when_true);
    emit_cas(text, target, line_no, "__cas_ext_%d:", done);
}

/* True when the operation was handled here, either because the target has no
   instruction for it or because it is not an extended operation at all. */
static bool emit_extended_fallback(Buffer *text, const char *target, const char *op,
                                   char **args, int argc, int line_no) {
    const ExtendedOp *ext = extended_op_lookup(op);
    if (!ext) return false;
    if (argc != ext->argc) {
        line_error(line_no, op, "wrong argument count for extended operation");
    }
    if (virtual_reg_index(args[0]) < 0) {
        line_error_token(line_no, args[0], op, "expected virtual register r0-r15");
    }
    if ((target_caps(target) & ext->cap) != 0) {
        return false; /* the backend has an instruction for it */
    }
    if (strcmp(op, "popcnt") == 0) ext_expand_popcnt(text, target, line_no, args[0]);
    else if (strcmp(op, "clz") == 0) ext_expand_clz(text, target, line_no, args[0]);
    else if (strcmp(op, "ctz") == 0) ext_expand_ctz(text, target, line_no, args[0]);
    else if (strcmp(op, "bswap") == 0) ext_expand_bswap(text, target, line_no, args[0]);
    else if (strcmp(op, "abs") == 0) ext_expand_abs(text, target, line_no, args[0]);
    else if (strcmp(op, "min") == 0 || strcmp(op, "max") == 0) {
        ext_expand_minmax(text, target, line_no, args[0], args[1], strcmp(op, "min") == 0);
    } else if (strcmp(op, "sel") == 0) {
        ext_expand_sel(text, target, line_no, args[0], args[1], args[2]);
    } else ext_expand_rotate(text, target, line_no, args[0], args[1], strcmp(op, "rol") == 0);
    return true;
}

/* ------------------------------------------------------------------- MIPS */

/* $zero, $at, $k0, $k1, $gp, $sp, $fp and $ra belong to the machine and the
   assembler, and $v0 and $a0-$a3 carry the syscall convention, so none of
   them appear here. That leaves the callee-saved set and eight temporaries
   for the sixteen virtual registers, with $t8 and $t9 held back for a
   deferred comparison and $v1 as the scratch. $at is left alone because the
   assembler's own macros - la, a wide li, the comparing branches - use it.

   The temporaries are spelled by number because o32 and n64 disagree about
   their names: $8-$11 are $t0-$t3 under o32 but $a4-$a7 under n64, so "$t7"
   is not even a register name on a 64-bit assembler. $s0-$s7, $v1, $a3 and
   $t8-$t9 mean the same thing under both. */
static const char *mips_regs[] = {
    "$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
    "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15"
};

/* $a3 doubles as the second scratch. It carries a syscall argument, but a
   scratch register is only ever live inside one instruction's expansion, and
   a syscall's expansion never overlaps another's. */
#define MIPS_SCRATCH "$v1"
#define MIPS_SCRATCH2 "$a3"

/* The li macro only reaches 32 bits, so the 64-bit member needs dli. Set once
   per instruction from the target, because the helpers below are shared. */
static const char *mips_li_op = "li";

static DeferredCompare mips_cmp = {CMP_NONE, {0}, {0}, false, "$t8", "$t9", "move", "li"};

static bool mips_is_64(const char *target) {
    return target_word_bits(target) == 64;
}

static const char *mips_reg(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        line_error_token(line_no, value, op, "expected virtual register r0-r15");
    }
    return mips_regs[reg];
}

/* addiu and the logical immediates take a 16-bit field. */
static bool mips_fits_imm16(const char *value, long long *out, bool signed_field) {
    char *end = NULL;
    long long parsed;
    if (!is_int(value)) return false;
    errno = 0;
    parsed = strtoll(value, &end, 0);
    if (errno != 0) return false;
    if (signed_field ? (parsed < -32768 || parsed > 32767) : (parsed < 0 || parsed > 65535)) {
        return false;
    }
    *out = parsed;
    return true;
}

/* Returns a register holding the operand, materialising it into the scratch
   when it is not already one. */
static const char *mips_operand_reg(Buffer *text, const char *value, const char *scratch,
                                    int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg >= 0) return mips_regs[reg];
    if (is_int(value) || is_known_constant(value)) {
        buf_appendf(text, "  %s %s, %s\n", mips_li_op, scratch, value);
    } else if (is_symbol(value)) {
        buf_appendf(text, "  la %s, %s\n", scratch, value);
    } else {
        line_error_token(line_no, value, op, "expected register, integer, symbol, or constant");
    }
    return scratch;
}

static void mips_emit_address(Buffer *text, const char *addr_text, char *out, size_t out_size,
                              int line_no, const char *op) {
    Address addr;
    if (!parse_address(addr_text, &addr)) {
        line_error_token(line_no, addr_text, op, "expected address like [r0 + 8] or [symbol + 8]");
    }
    if (addr.has_base) {
        snprintf(out, out_size, "%lld(%s)", addr.offset, mips_reg(addr.base, line_no, op));
        return;
    }
    /* A symbolic address has to be materialised; the assembler's own la macro
       would otherwise need a base register anyway. */
    buf_appendf(text, "  la %s, %s\n", MIPS_SCRATCH, addr.symbol);
    snprintf(out, out_size, "%lld(%s)", addr.offset, MIPS_SCRATCH);
}

static void emit_mips_syscall(Buffer *text, const char *target, char **args, int argc, int line_no) {
    static const char *const arg_regs[] = {"$a0", "$a1", "$a2", "$a3"};
    const char *result = syscall_result_operand(&args, &argc);
    int number = -1;
    int count;
    (void)target;
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    if (op_is(args[0], "exit")) number = 4001;
    else if (op_is(args[0], "read")) number = 4003;
    else if (op_is(args[0], "write")) number = 4004;
    else if (op_is(args[0], "open")) number = 4005;
    else if (op_is(args[0], "close")) number = 4006;
    else line_error(line_no, "syscall", "unknown syscall");
    count = argc - 1;
    if (count > 4) count = 4;
    /* No virtual register maps onto $v0 or $a0-$a3, so nothing needs saving. */
    for (int i = 0; i < count; i++) {
        int reg = virtual_reg_index(args[i + 1]);
        if (reg >= 0) buf_appendf(text, "  move %s, %s\n", arg_regs[i], mips_regs[reg]);
        else if (is_int(args[i + 1]) || is_known_constant(args[i + 1])) {
            buf_appendf(text, "  %s %s, %s\n", mips_li_op, arg_regs[i], args[i + 1]);
        } else {
            buf_appendf(text, "  la %s, %s\n", arg_regs[i], args[i + 1]);
        }
    }
    buf_appendf(text, "  li $v0, %d\n", number);
    buf_append(text, "  syscall\n");
    if (result) buf_appendf(text, "  move %s, $v0\n", mips_reg(result, line_no, "syscall"));
}

static void emit_mips_instruction(Buffer *text, const char *target, const char *op, const char *size,
                                  char **args, int argc, int line_no) {
    const bool wide = mips_is_64(target);
    const char *addiu = wide ? "daddiu" : "addiu";
    const char *addu = wide ? "daddu" : "addu";
    const char *subu = wide ? "dsubu" : "subu";
    const char *word_load = wide ? "ld" : "lw";
    const char *word_store = wide ? "sd" : "sw";
    const int slot = wide ? 8 : 4;
    mips_li_op = wide ? "dli" : "li";
    mips_cmp.load_imm_op = mips_li_op;

    /* A pending comparison is pinned down before anything can disturb the
       registers it names, and a call is enough on its own. */
    if (!is_conditional_branch(op) && !op_is(op, "cmp")) {
        if (op_is(op, "call")) {
            compare_flush(text, &mips_cmp);
        } else if (argc >= 1 && writes_first_arg(op)) {
            int dst = virtual_reg_index(args[0]);
            if (dst >= 0 && compare_reads(&mips_cmp, mips_regs[dst])) {
                compare_flush(text, &mips_cmp);
            }
        }
    }

    if (op_is(op, "func") && argc == 1) { buf_appendf(text, "%s:\n", args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) return;
    if (op_is(op, "enter") && argc == 1) {
        long long frame;
        buf_appendf(text, "  %s $sp, $sp, -%d\n", addiu, slot * 2);
        buf_appendf(text, "  %s $ra, %d($sp)\n", word_store, slot);
        buf_appendf(text, "  %s $fp, 0($sp)\n", word_store);
        buf_append(text, "  move $fp, $sp\n");
        if (strcmp(args[0], "0") == 0) return;
        if (mips_fits_imm16(args[0], &frame, true) && frame != -32768) {
            buf_appendf(text, "  %s $sp, $sp, %lld\n", addiu, -frame);
        } else {
            buf_appendf(text, "  %s %s, %s\n", mips_li_op, MIPS_SCRATCH, args[0]);
            buf_appendf(text, "  %s $sp, $sp, %s\n", subu, MIPS_SCRATCH);
        }
        return;
    }
    if (op_is(op, "leave") && argc == 0) {
        buf_append(text, "  move $sp, $fp\n");
        buf_appendf(text, "  %s $fp, 0($sp)\n", word_load);
        buf_appendf(text, "  %s $ra, %d($sp)\n", word_load, slot);
        buf_appendf(text, "  %s $sp, $sp, %d\n", addiu, slot * 2);
        return;
    }
    if (op_is(op, "mov") && argc == 2) {
        const char *dst = mips_reg(args[0], line_no, op);
        int src = virtual_reg_index(args[1]);
        if (src >= 0) buf_appendf(text, "  move %s, %s\n", dst, mips_regs[src]);
        else if (is_int(args[1]) || is_known_constant(args[1])) buf_appendf(text, "  %s %s, %s\n", mips_li_op, dst, args[1]);
        else buf_appendf(text, "  la %s, %s\n", dst, args[1]);
        return;
    }
    if (op_is(op, "load_addr") && argc == 2) {
        buf_appendf(text, "  la %s, %s\n", mips_reg(args[0], line_no, op), args[1]); return;
    }
    if (op_is(op, "load") && argc == 2) {
        char addr[256];
        const char *dst = mips_reg(args[0], line_no, op);
        const char *mnemonic = size[0] == 'b' ? "lbu" : size[0] == 'w' ? "lhu" :
                               size[0] == 'd' ? "lw" : word_load;
        mips_emit_address(text, args[1], addr, sizeof(addr), line_no, op);
        buf_appendf(text, "  %s %s, %s\n", mnemonic, dst, addr);
        return;
    }
    if (op_is(op, "store") && argc == 2) {
        char addr[256];
        const char *mnemonic = size[0] == 'b' ? "sb" : size[0] == 'w' ? "sh" :
                               size[0] == 'd' ? "sw" : word_store;
        /* Address and value can both need materialising, which is why there
           are two scratch registers. */
        const char *value;
        mips_emit_address(text, args[0], addr, sizeof(addr), line_no, op);
        value = mips_operand_reg(text, args[1], MIPS_SCRATCH2, line_no, op);
        buf_appendf(text, "  %s %s, %s\n", mnemonic, value, addr);
        return;
    }
    if ((op_is(op, "add") || op_is(op, "sub") || op_is(op, "and") ||
         op_is(op, "or") || op_is(op, "xor")) && argc == 2) {
        const char *dst = mips_reg(args[0], line_no, op);
        const bool logical = op_is(op, "and") || op_is(op, "or") || op_is(op, "xor");
        long long value;
        int src = virtual_reg_index(args[1]);
        if (src < 0 && mips_fits_imm16(args[1], &value, !logical) &&
            !(op_is(op, "sub") && value == -32768)) {
            const char *immop = op_is(op, "and") ? "andi" : op_is(op, "or") ? "ori" :
                                op_is(op, "xor") ? "xori" : addiu;
            buf_appendf(text, "  %s %s, %s, %lld\n", immop, dst, dst,
                        op_is(op, "sub") ? -value : value);
            return;
        }
        {
            const char *rhs = mips_operand_reg(text, args[1], MIPS_SCRATCH, line_no, op);
            const char *native = op_is(op, "add") ? addu : op_is(op, "sub") ? subu : op;
            buf_appendf(text, "  %s %s, %s, %s\n", native, dst, dst, rhs);
        }
        return;
    }
    if ((op_is(op, "mul") || op_is(op, "div") || op_is(op, "mod")) && argc == 2) {
        const char *dst = mips_reg(args[0], line_no, op);
        const char *rhs = mips_operand_reg(text, args[1], MIPS_SCRATCH, line_no, op);
        const char *native = op_is(op, "mul") ? (wide ? "dmul" : "mul") :
                             op_is(op, "div") ? (wide ? "ddiv" : "div") : (wide ? "drem" : "rem");
        buf_appendf(text, "  %s %s, %s, %s\n", native, dst, dst, rhs);
        return;
    }
    if ((op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar")) && argc == 2) {
        const char *dst = mips_reg(args[0], line_no, op);
        int src = virtual_reg_index(args[1]);
        const char *immop = op_is(op, "shl") ? (wide ? "dsll" : "sll") :
                            op_is(op, "shr") ? (wide ? "dsrl" : "srl") : (wide ? "dsra" : "sra");
        const char *regop = op_is(op, "shl") ? (wide ? "dsllv" : "sllv") :
                            op_is(op, "shr") ? (wide ? "dsrlv" : "srlv") : (wide ? "dsrav" : "srav");
        if (src >= 0) buf_appendf(text, "  %s %s, %s, %s\n", regop, dst, dst, mips_regs[src]);
        else buf_appendf(text, "  %s %s, %s, %s\n", immop, dst, dst, args[1]);
        return;
    }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") || op_is(op, "dec")) && argc == 1) {
        const char *dst = mips_reg(args[0], line_no, op);
        if (op_is(op, "neg")) buf_appendf(text, "  %s %s, %s\n", wide ? "dnegu" : "negu", dst, dst);
        else if (op_is(op, "not")) buf_appendf(text, "  nor %s, %s, $zero\n", dst, dst);
        else buf_appendf(text, "  %s %s, %s, %s\n", addiu, dst, dst, op_is(op, "inc") ? "1" : "-1");
        return;
    }
    if (op_is(op, "cmp") && argc == 2) {
        int src = virtual_reg_index(args[1]);
        compare_record(&mips_cmp, mips_reg(args[0], line_no, op),
                       src >= 0 ? mips_regs[src] : args[1], src >= 0);
        return;
    }
    if (is_conditional_branch(op) && argc == 1) {
        compare_emit_branch(text, &mips_cmp, op, args[0], line_no);
        return;
    }
    if (op_is(op, "push") && argc == 1) {
        const char *value = mips_operand_reg(text, args[0], MIPS_SCRATCH, line_no, op);
        buf_appendf(text, "  %s $sp, $sp, -%d\n", addiu, slot);
        buf_appendf(text, "  %s %s, 0($sp)\n", word_store, value);
        return;
    }
    if (op_is(op, "pop") && argc == 1) {
        buf_appendf(text, "  %s %s, 0($sp)\n", word_load, mips_reg(args[0], line_no, op));
        buf_appendf(text, "  %s $sp, $sp, %d\n", addiu, slot);
        return;
    }
    if (op_is(op, "jmp") && argc == 1) { buf_appendf(text, "  b %s\n", args[0]); return; }
    if (op_is(op, "call") && argc == 1) { buf_appendf(text, "  jal %s\n", args[0]); return; }
    if (op_is(op, "ret") && argc == 0) { buf_append(text, "  jr $ra\n"); return; }
    if (op_is(op, "syscall")) { emit_mips_syscall(text, target, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count for MIPS");
}

/* --------------------------------------------------------------- PowerPC */

/* r1 is the stack pointer, r2 is reserved, r0 reads as zero in some operand
   positions and is the only place the link register can be parked, r3-r8
   carry the syscall convention, and r31 is the frame pointer. That leaves the
   callee-saved block for the sixteen virtual registers, with r11 and r12 as
   the scratch pair, and nothing spills. */
static const char *ppc_regs[] = {
    "14", "15", "16", "17", "18", "19", "20", "21",
    "22", "23", "24", "25", "26", "27", "28", "29"
};

#define PPC_SCRATCH "11"
#define PPC_SCRATCH2 "12"

/* PowerPC decides signedness at the comparison rather than at the branch, so
   the compare is held until the branch says which it wanted. */
static DeferredCompare ppc_cmp = {CMP_NONE, {0}, {0}, false, "", "", "mr", "li"};

static bool ppc_is_64(const char *target) {
    return target_word_bits(target) == 64;
}

static const char *ppc_reg(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        line_error_token(line_no, value, op, "expected virtual register r0-r15");
    }
    return ppc_regs[reg];
}

static bool ppc_fits_imm16(const char *value, long long *out) {
    char *end = NULL;
    long long parsed;
    if (!is_int(value)) {
        if (!constant_value(value, &parsed)) return false;
        if (parsed < -32768 || parsed > 32767) return false;
        *out = parsed;
        return true;
    }
    errno = 0;
    parsed = strtoll(value, &end, 0);
    if (errno != 0 || parsed < -32768 || parsed > 32767) return false;
    *out = parsed;
    return true;
}

/* Builds any value in a register: a 16-bit literal in one instruction, a
   wider one in two, and an address through its high and low halves. */
static void ppc_load_operand(Buffer *text, const char *reg, const char *value) {
    long long number;
    if (ppc_fits_imm16(value, &number)) {
        buf_appendf(text, "  li %s, %lld\n", reg, number);
        return;
    }
    if (is_int(value) || constant_value(value, &number)) {
        unsigned long long bits;
        if (is_int(value)) number = strtoll(value, NULL, 0);
        bits = (unsigned long long)number;
        buf_appendf(text, "  lis %s, %llu\n", reg, (bits >> 16) & 0xffffu);
        buf_appendf(text, "  ori %s, %s, %llu\n", reg, reg, bits & 0xffffu);
        return;
    }
    buf_appendf(text, "  lis %s, %s@ha\n", reg, value);
    buf_appendf(text, "  addi %s, %s, %s@l\n", reg, reg, value);
}

static const char *ppc_operand_reg(Buffer *text, const char *value, const char *scratch,
                                   int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg >= 0) return ppc_regs[reg];
    if (!is_int(value) && !is_symbol(value) && !is_known_constant(value)) {
        line_error_token(line_no, value, op, "expected register, integer, symbol, or constant");
    }
    ppc_load_operand(text, scratch, value);
    return scratch;
}

static void ppc_emit_address(Buffer *text, const char *addr_text, char *out, size_t out_size,
                             const char *scratch, int line_no, const char *op) {
    Address addr;
    if (!parse_address(addr_text, &addr)) {
        line_error_token(line_no, addr_text, op, "expected address like [r0 + 8] or [symbol + 8]");
    }
    if (addr.has_base) {
        snprintf(out, out_size, "%lld(%s)", addr.offset, ppc_reg(addr.base, line_no, op));
        return;
    }
    ppc_load_operand(text, scratch, addr.symbol);
    snprintf(out, out_size, "%lld(%s)", addr.offset, scratch);
}

static void emit_ppc_syscall(Buffer *text, char **args, int argc, int line_no) {
    static const char *const arg_regs[] = {"3", "4", "5", "6", "7", "8"};
    const char *result = syscall_result_operand(&args, &argc);
    int number = -1;
    int count;
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    if (op_is(args[0], "exit")) number = 1;
    else if (op_is(args[0], "read")) number = 3;
    else if (op_is(args[0], "write")) number = 4;
    else if (op_is(args[0], "open")) number = 5;
    else if (op_is(args[0], "close")) number = 6;
    else line_error(line_no, "syscall", "unknown syscall");
    count = argc - 1;
    if (count > 6) count = 6;
    /* No virtual register maps onto r0 or r3-r8, so nothing needs saving. */
    for (int i = 0; i < count; i++) {
        int reg = virtual_reg_index(args[i + 1]);
        if (reg >= 0) buf_appendf(text, "  mr %s, %s\n", arg_regs[i], ppc_regs[reg]);
        else ppc_load_operand(text, arg_regs[i], args[i + 1]);
    }
    buf_appendf(text, "  li 0, %d\n", number);
    buf_append(text, "  sc\n");
    if (result) buf_appendf(text, "  mr %s, 3\n", ppc_reg(result, line_no, "syscall"));
}

static void emit_ppc_instruction(Buffer *text, const char *target, const char *op, const char *size,
                                 char **args, int argc, int line_no) {
    const bool wide = ppc_is_64(target);
    const char *word_load = wide ? "ld" : "lwz";
    const char *word_store = wide ? "std" : "stw";
    const int slot = wide ? 8 : 4;

    if (!is_conditional_branch(op) && !op_is(op, "cmp")) {
        if (op_is(op, "call")) {
            ppc_cmp.state = CMP_NONE;
        } else if (argc >= 1 && writes_first_arg(op)) {
            int dst = virtual_reg_index(args[0]);
            if (dst >= 0 && compare_reads(&ppc_cmp, ppc_regs[dst])) {
                /* The comparison is settled where it stood, using a spare
                   condition field so the branch below still finds it. */
                ppc_cmp.state = CMP_SNAPSHOT;
            }
        }
    }

    if (op_is(op, "func") && argc == 1) { buf_appendf(text, "%s:\n", args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) return;
    if (op_is(op, "enter") && argc == 1) {
        long long frame;
        buf_appendf(text, "  addi 1, 1, -%d\n", slot * 2);
        buf_append(text, "  mflr 0\n");
        buf_appendf(text, "  %s 0, %d(1)\n", word_store, slot);
        buf_appendf(text, "  %s 31, 0(1)\n", word_store);
        buf_append(text, "  mr 31, 1\n");
        if (strcmp(args[0], "0") == 0) return;
        if (ppc_fits_imm16(args[0], &frame) && frame != -32768) {
            buf_appendf(text, "  addi 1, 1, %lld\n", -frame);
        } else {
            ppc_load_operand(text, PPC_SCRATCH, args[0]);
            buf_appendf(text, "  subf 1, %s, 1\n", PPC_SCRATCH);
        }
        return;
    }
    if (op_is(op, "leave") && argc == 0) {
        buf_append(text, "  mr 1, 31\n");
        buf_appendf(text, "  %s 31, 0(1)\n", word_load);
        buf_appendf(text, "  %s 0, %d(1)\n", word_load, slot);
        buf_append(text, "  mtlr 0\n");
        buf_appendf(text, "  addi 1, 1, %d\n", slot * 2);
        return;
    }
    if (op_is(op, "mov") && argc == 2) {
        const char *dst = ppc_reg(args[0], line_no, op);
        int src = virtual_reg_index(args[1]);
        if (src >= 0) buf_appendf(text, "  mr %s, %s\n", dst, ppc_regs[src]);
        else ppc_load_operand(text, dst, args[1]);
        return;
    }
    if (op_is(op, "load_addr") && argc == 2) {
        ppc_load_operand(text, ppc_reg(args[0], line_no, op), args[1]);
        return;
    }
    if (op_is(op, "load") && argc == 2) {
        char addr[256];
        const char *dst = ppc_reg(args[0], line_no, op);
        const char *mnemonic = size[0] == 'b' ? "lbz" : size[0] == 'w' ? "lhz" :
                               size[0] == 'd' ? "lwz" : word_load;
        ppc_emit_address(text, args[1], addr, sizeof(addr), PPC_SCRATCH, line_no, op);
        buf_appendf(text, "  %s %s, %s\n", mnemonic, dst, addr);
        return;
    }
    if (op_is(op, "store") && argc == 2) {
        char addr[256];
        const char *mnemonic = size[0] == 'b' ? "stb" : size[0] == 'w' ? "sth" :
                               size[0] == 'd' ? "stw" : word_store;
        const char *value;
        ppc_emit_address(text, args[0], addr, sizeof(addr), PPC_SCRATCH, line_no, op);
        value = ppc_operand_reg(text, args[1], PPC_SCRATCH2, line_no, op);
        buf_appendf(text, "  %s %s, %s\n", mnemonic, value, addr);
        return;
    }
    if (op_is(op, "add") && argc == 2) {
        const char *dst = ppc_reg(args[0], line_no, op);
        long long value;
        if (ppc_fits_imm16(args[1], &value)) buf_appendf(text, "  addi %s, %s, %lld\n", dst, dst, value);
        else buf_appendf(text, "  add %s, %s, %s\n", dst, dst,
                         ppc_operand_reg(text, args[1], PPC_SCRATCH, line_no, op));
        return;
    }
    if (op_is(op, "sub") && argc == 2) {
        const char *dst = ppc_reg(args[0], line_no, op);
        long long value;
        if (ppc_fits_imm16(args[1], &value) && value != -32768) {
            buf_appendf(text, "  addi %s, %s, %lld\n", dst, dst, -value);
        } else {
            /* subf computes the second operand minus the first. */
            buf_appendf(text, "  subf %s, %s, %s\n", dst,
                        ppc_operand_reg(text, args[1], PPC_SCRATCH, line_no, op), dst);
        }
        return;
    }
    if ((op_is(op, "and") || op_is(op, "or") || op_is(op, "xor")) && argc == 2) {
        /* The immediate forms of and record a condition field, which would
           destroy a comparison waiting for its branch, so every operand goes
           through a register. */
        const char *dst = ppc_reg(args[0], line_no, op);
        const char *src = ppc_operand_reg(text, args[1], PPC_SCRATCH, line_no, op);
        buf_appendf(text, "  %s %s, %s, %s\n", op_is(op, "xor") ? "xor" : op, dst, dst, src);
        return;
    }
    if ((op_is(op, "mul") || op_is(op, "div") || op_is(op, "mod")) && argc == 2) {
        const char *dst = ppc_reg(args[0], line_no, op);
        const char *src = ppc_operand_reg(text, args[1], PPC_SCRATCH, line_no, op);
        const char *mul = wide ? "mulld" : "mullw";
        const char *div = wide ? "divd" : "divw";
        if (op_is(op, "mul")) buf_appendf(text, "  %s %s, %s, %s\n", mul, dst, dst, src);
        else if (op_is(op, "div")) buf_appendf(text, "  %s %s, %s, %s\n", div, dst, dst, src);
        else {
            /* No remainder instruction: divide, multiply back, subtract. */
            buf_appendf(text, "  %s %s, %s, %s\n", div, PPC_SCRATCH2, dst, src);
            buf_appendf(text, "  %s %s, %s, %s\n", mul, PPC_SCRATCH2, PPC_SCRATCH2, src);
            buf_appendf(text, "  subf %s, %s, %s\n", dst, PPC_SCRATCH2, dst);
        }
        return;
    }
    if ((op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar")) && argc == 2) {
        const char *dst = ppc_reg(args[0], line_no, op);
        int src = virtual_reg_index(args[1]);
        const char *regop = op_is(op, "shl") ? (wide ? "sld" : "slw") :
                            op_is(op, "shr") ? (wide ? "srd" : "srw") : (wide ? "srad" : "sraw");
        const char *immop = op_is(op, "shl") ? (wide ? "sldi" : "slwi") :
                            op_is(op, "shr") ? (wide ? "srdi" : "srwi") : (wide ? "sradi" : "srawi");
        if (src >= 0) buf_appendf(text, "  %s %s, %s, %s\n", regop, dst, dst, ppc_regs[src]);
        else buf_appendf(text, "  %s %s, %s, %s\n", immop, dst, dst, args[1]);
        return;
    }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") || op_is(op, "dec")) && argc == 1) {
        const char *dst = ppc_reg(args[0], line_no, op);
        if (op_is(op, "neg")) buf_appendf(text, "  neg %s, %s\n", dst, dst);
        else if (op_is(op, "not")) buf_appendf(text, "  nor %s, %s, %s\n", dst, dst, dst);
        else buf_appendf(text, "  addi %s, %s, %s\n", dst, dst, op_is(op, "inc") ? "1" : "-1");
        return;
    }
    if (op_is(op, "cmp") && argc == 2) {
        int src = virtual_reg_index(args[1]);
        compare_record(&ppc_cmp, ppc_reg(args[0], line_no, op),
                       src >= 0 ? ppc_regs[src] : args[1], src >= 0);
        return;
    }
    if (is_conditional_branch(op) && argc == 1) {
        /* The comparison is emitted here, because only the branch says
           whether it was meant to be signed. */
        const bool unsigned_test = op_is(op, "ja") || op_is(op, "jb") ||
                                   op_is(op, "jae") || op_is(op, "jbe");
        const char *form = wide ? (unsigned_test ? "cmpld" : "cmpd")
                                : (unsigned_test ? "cmplw" : "cmpw");
        const char *branch = op_is(op, "je") ? "beq" : op_is(op, "jne") ? "bne" :
                             op_is(op, "jg") || op_is(op, "ja") ? "bgt" :
                             op_is(op, "jl") || op_is(op, "jb") ? "blt" :
                             op_is(op, "jge") || op_is(op, "jae") ? "bge" : "ble";
        if (ppc_cmp.state == CMP_NONE) {
            line_error(line_no, op, "conditional jump with no preceding cmp");
        }
        if (ppc_cmp.rhs_is_reg) {
            buf_appendf(text, "  %s %s, %s\n", form, ppc_cmp.lhs, ppc_cmp.rhs);
        } else {
            long long value;
            if (ppc_fits_imm16(ppc_cmp.rhs, &value)) {
                buf_appendf(text, "  %si %s, %lld\n", form, ppc_cmp.lhs, value);
            } else {
                ppc_load_operand(text, PPC_SCRATCH, ppc_cmp.rhs);
                buf_appendf(text, "  %s %s, %s\n", form, ppc_cmp.lhs, PPC_SCRATCH);
            }
        }
        buf_appendf(text, "  %s %s\n", branch, args[0]);
        return;
    }
    if (op_is(op, "push") && argc == 1) {
        const char *value = ppc_operand_reg(text, args[0], PPC_SCRATCH, line_no, op);
        buf_appendf(text, "  addi 1, 1, -%d\n", slot);
        buf_appendf(text, "  %s %s, 0(1)\n", word_store, value);
        return;
    }
    if (op_is(op, "pop") && argc == 1) {
        buf_appendf(text, "  %s %s, 0(1)\n", word_load, ppc_reg(args[0], line_no, op));
        buf_appendf(text, "  addi 1, 1, %d\n", slot);
        return;
    }
    if (op_is(op, "jmp") && argc == 1) { buf_appendf(text, "  b %s\n", args[0]); return; }
    if (op_is(op, "call") && argc == 1) { buf_appendf(text, "  bl %s\n", args[0]); return; }
    if (op_is(op, "ret") && argc == 0) { buf_append(text, "  blr\n"); return; }
    if (op_is(op, "syscall")) { emit_ppc_syscall(text, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count for PowerPC");
}

/* ----------------------------------------------------------------- SPARC */

/* SPARC's register windows exist to make calls cheap, and cost a save and a
   restore to enter. This emits flat code instead - no window is ever shifted -
   so %l, %i and the application globals are all just registers here.
   %g0 reads as zero, %g5-%g7 belong to the system, %o0-%o5 and %g1 carry the
   syscall convention, %o6/%i6 are the stack and frame pointers and %o7/%i7
   hold return addresses, so none of those are handed out. */
static const char *sparc_regs[] = {
    "%l0", "%l1", "%l2", "%l3", "%l4", "%l5", "%l6", "%l7",
    "%i0", "%i1", "%i2", "%i3", "%i4", "%i5", "%g2", "%g3"
};

#define SPARC_SCRATCH "%g1"
#define SPARC_SCRATCH2 "%g4"

static DeferredCompare sparc_cmp = {CMP_NONE, {0}, {0}, false, "", "", "mov", "set"};

static bool sparc_is_64(const char *target) {
    return target_word_bits(target) == 64;
}

static const char *sparc_reg(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        line_error_token(line_no, value, op, "expected virtual register r0-r15");
    }
    return sparc_regs[reg];
}

/* The immediate field is 13 bits signed; wider values go through set, which
   the assembler expands into however many instructions it needs. */
static bool sparc_fits_imm13(const char *value, long long *out) {
    char *end = NULL;
    long long parsed;
    if (!is_int(value)) {
        if (!constant_value(value, &parsed)) return false;
        if (parsed < -4096 || parsed > 4095) return false;
        *out = parsed;
        return true;
    }
    errno = 0;
    parsed = strtoll(value, &end, 0);
    if (errno != 0 || parsed < -4096 || parsed > 4095) return false;
    *out = parsed;
    return true;
}

static const char *sparc_operand_reg(Buffer *text, const char *value, const char *scratch,
                                     int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg >= 0) return sparc_regs[reg];
    if (!is_int(value) && !is_symbol(value) && !is_known_constant(value)) {
        line_error_token(line_no, value, op, "expected register, integer, symbol, or constant");
    }
    buf_appendf(text, "  set %s, %s\n", value, scratch);
    return scratch;
}

/* Every control transfer has a delay slot, and SPARC's assembler does not
   fill them, so each one is followed by an explicit nop. */
static void sparc_delay_slot(Buffer *text) {
    buf_append(text, "  nop\n");
}

static void sparc_emit_address(Buffer *text, const char *addr_text, char *out, size_t out_size,
                               const char *scratch, int line_no, const char *op) {
    Address addr;
    if (!parse_address(addr_text, &addr)) {
        line_error_token(line_no, addr_text, op, "expected address like [r0 + 8] or [symbol + 8]");
    }
    if (addr.has_base) {
        snprintf(out, out_size, "[%s+%lld]", sparc_reg(addr.base, line_no, op), addr.offset);
        return;
    }
    buf_appendf(text, "  set %s, %s\n", addr.symbol, scratch);
    snprintf(out, out_size, "[%s+%lld]", scratch, addr.offset);
}

static void emit_sparc_syscall(Buffer *text, char **args, int argc, int line_no) {
    static const char *const arg_regs[] = {"%o0", "%o1", "%o2", "%o3", "%o4", "%o5"};
    const char *result = syscall_result_operand(&args, &argc);
    int number = -1;
    int count;
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    if (op_is(args[0], "exit")) number = 1;
    else if (op_is(args[0], "read")) number = 3;
    else if (op_is(args[0], "write")) number = 4;
    else if (op_is(args[0], "open")) number = 5;
    else if (op_is(args[0], "close")) number = 6;
    else line_error(line_no, "syscall", "unknown syscall");
    count = argc - 1;
    if (count > 6) count = 6;
    /* No virtual register maps onto %g1 or %o0-%o5, so nothing needs saving. */
    for (int i = 0; i < count; i++) {
        int reg = virtual_reg_index(args[i + 1]);
        if (reg >= 0) buf_appendf(text, "  mov %s, %s\n", sparc_regs[reg], arg_regs[i]);
        else buf_appendf(text, "  set %s, %s\n", args[i + 1], arg_regs[i]);
    }
    buf_appendf(text, "  mov %d, %s\n", number, SPARC_SCRATCH);
    buf_append(text, "  ta 0x10\n");
    if (result) buf_appendf(text, "  mov %%o0, %s\n", sparc_reg(result, line_no, "syscall"));
}

static void emit_sparc_instruction(Buffer *text, const char *target, const char *op, const char *size,
                                   char **args, int argc, int line_no) {
    const bool wide = sparc_is_64(target);
    const char *word_load = wide ? "ldx" : "ld";
    const char *word_store = wide ? "stx" : "st";
    const int slot = wide ? 8 : 4;

    if (op_is(op, "func") && argc == 1) { buf_appendf(text, "%s:\n", args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) return;
    if (op_is(op, "enter") && argc == 1) {
        long long frame;
        /* Flat code, so the return address has to be kept by hand rather than
           by a window. */
        buf_appendf(text, "  sub %%sp, %d, %%sp\n", slot * 2);
        buf_appendf(text, "  %s %%o7, [%%sp+%d]\n", word_store, slot);
        buf_appendf(text, "  %s %%fp, [%%sp+0]\n", word_store);
        buf_append(text, "  mov %sp, %fp\n");
        if (strcmp(args[0], "0") == 0) return;
        if (sparc_fits_imm13(args[0], &frame)) buf_appendf(text, "  sub %%sp, %lld, %%sp\n", frame);
        else {
            buf_appendf(text, "  set %s, %s\n", args[0], SPARC_SCRATCH);
            buf_appendf(text, "  sub %%sp, %s, %%sp\n", SPARC_SCRATCH);
        }
        return;
    }
    if (op_is(op, "leave") && argc == 0) {
        buf_append(text, "  mov %fp, %sp\n");
        buf_appendf(text, "  %s [%%sp+0], %%fp\n", word_load);
        buf_appendf(text, "  %s [%%sp+%d], %%o7\n", word_load, slot);
        buf_appendf(text, "  add %%sp, %d, %%sp\n", slot * 2);
        return;
    }
    if (op_is(op, "mov") && argc == 2) {
        const char *dst = sparc_reg(args[0], line_no, op);
        int src = virtual_reg_index(args[1]);
        if (src >= 0) buf_appendf(text, "  mov %s, %s\n", sparc_regs[src], dst);
        else buf_appendf(text, "  set %s, %s\n", args[1], dst);
        return;
    }
    if (op_is(op, "load_addr") && argc == 2) {
        buf_appendf(text, "  set %s, %s\n", args[1], sparc_reg(args[0], line_no, op));
        return;
    }
    if (op_is(op, "load") && argc == 2) {
        char addr[256];
        const char *dst = sparc_reg(args[0], line_no, op);
        const char *mnemonic = size[0] == 'b' ? "ldub" : size[0] == 'w' ? "lduh" :
                               size[0] == 'd' ? "ld" : word_load;
        sparc_emit_address(text, args[1], addr, sizeof(addr), SPARC_SCRATCH, line_no, op);
        buf_appendf(text, "  %s %s, %s\n", mnemonic, addr, dst);
        return;
    }
    if (op_is(op, "store") && argc == 2) {
        char addr[256];
        const char *mnemonic = size[0] == 'b' ? "stb" : size[0] == 'w' ? "sth" :
                               size[0] == 'd' ? "st" : word_store;
        const char *value;
        sparc_emit_address(text, args[0], addr, sizeof(addr), SPARC_SCRATCH, line_no, op);
        value = sparc_operand_reg(text, args[1], SPARC_SCRATCH2, line_no, op);
        buf_appendf(text, "  %s %s, %s\n", mnemonic, value, addr);
        return;
    }
    if ((op_is(op, "add") || op_is(op, "sub") || op_is(op, "and") ||
         op_is(op, "or") || op_is(op, "xor")) && argc == 2) {
        const char *dst = sparc_reg(args[0], line_no, op);
        const char *native = op_is(op, "xor") ? "xor" : op;
        long long value;
        if (sparc_fits_imm13(args[1], &value)) {
            buf_appendf(text, "  %s %s, %lld, %s\n", native, dst, value, dst);
        } else {
            const char *src = sparc_operand_reg(text, args[1], SPARC_SCRATCH, line_no, op);
            buf_appendf(text, "  %s %s, %s, %s\n", native, dst, src, dst);
        }
        return;
    }
    if ((op_is(op, "mul") || op_is(op, "div") || op_is(op, "mod")) && argc == 2) {
        const char *dst = sparc_reg(args[0], line_no, op);
        const char *src = sparc_operand_reg(text, args[1], SPARC_SCRATCH, line_no, op);
        if (op_is(op, "mul")) {
            buf_appendf(text, "  %s %s, %s, %s\n", wide ? "mulx" : "smul", dst, src, dst);
            return;
        }
        if (!wide) {
            /* The 32-bit divide reads the Y register as the high half of the
               dividend, so it has to be given the sign extension first. */
            buf_appendf(text, "  sra %s, 31, %s\n", dst, SPARC_SCRATCH2);
            buf_appendf(text, "  wr %s, %%g0, %%y\n", SPARC_SCRATCH2);
        }
        if (op_is(op, "div")) {
            buf_appendf(text, "  %s %s, %s, %s\n", wide ? "sdivx" : "sdiv", dst, src, dst);
        } else {
            /* No remainder instruction: divide, multiply back, subtract. */
            buf_appendf(text, "  %s %s, %s, %s\n", wide ? "sdivx" : "sdiv", dst, src, SPARC_SCRATCH2);
            buf_appendf(text, "  %s %s, %s, %s\n", wide ? "mulx" : "smul", SPARC_SCRATCH2, src, SPARC_SCRATCH2);
            buf_appendf(text, "  sub %s, %s, %s\n", dst, SPARC_SCRATCH2, dst);
        }
        return;
    }
    if ((op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar")) && argc == 2) {
        const char *dst = sparc_reg(args[0], line_no, op);
        const char *native = op_is(op, "shl") ? (wide ? "sllx" : "sll") :
                             op_is(op, "shr") ? (wide ? "srlx" : "srl") : (wide ? "srax" : "sra");
        int src = virtual_reg_index(args[1]);
        if (src >= 0) buf_appendf(text, "  %s %s, %s, %s\n", native, dst, sparc_regs[src], dst);
        else buf_appendf(text, "  %s %s, %s, %s\n", native, dst, args[1], dst);
        return;
    }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") || op_is(op, "dec")) && argc == 1) {
        const char *dst = sparc_reg(args[0], line_no, op);
        if (op_is(op, "neg")) buf_appendf(text, "  sub %%g0, %s, %s\n", dst, dst);
        else if (op_is(op, "not")) buf_appendf(text, "  xnor %s, %%g0, %s\n", dst, dst);
        else buf_appendf(text, "  add %s, %s, %s\n", dst, op_is(op, "inc") ? "1" : "-1", dst);
        return;
    }
    if (op_is(op, "cmp") && argc == 2) {
        int src = virtual_reg_index(args[1]);
        compare_record(&sparc_cmp, sparc_reg(args[0], line_no, op),
                       src >= 0 ? sparc_regs[src] : args[1], src >= 0);
        return;
    }
    if (is_conditional_branch(op) && argc == 1) {
        /* The comparison is emitted here so the branch can say whether it
           meant a signed or an unsigned one. */
        const char *branch = op_is(op, "je") ? "be" : op_is(op, "jne") ? "bne" :
                             op_is(op, "jg") ? "bg" : op_is(op, "jl") ? "bl" :
                             op_is(op, "jge") ? "bge" : op_is(op, "jle") ? "ble" :
                             op_is(op, "ja") ? "bgu" : op_is(op, "jb") ? "blu" :
                             op_is(op, "jae") ? "bgeu" : "bleu";
        long long value;
        if (sparc_cmp.state == CMP_NONE) {
            line_error(line_no, op, "conditional jump with no preceding cmp");
        }
        if (sparc_cmp.rhs_is_reg || sparc_fits_imm13(sparc_cmp.rhs, &value)) {
            buf_appendf(text, "  cmp %s, %s\n", sparc_cmp.lhs, sparc_cmp.rhs);
        } else {
            buf_appendf(text, "  set %s, %s\n", sparc_cmp.rhs, SPARC_SCRATCH);
            buf_appendf(text, "  cmp %s, %s\n", sparc_cmp.lhs, SPARC_SCRATCH);
        }
        if (wide) buf_appendf(text, "  %s %%xcc, %s\n", branch, args[0]);
        else buf_appendf(text, "  %s %s\n", branch, args[0]);
        sparc_delay_slot(text);
        return;
    }
    if (op_is(op, "push") && argc == 1) {
        const char *value = sparc_operand_reg(text, args[0], SPARC_SCRATCH, line_no, op);
        buf_appendf(text, "  sub %%sp, %d, %%sp\n", slot);
        buf_appendf(text, "  %s %s, [%%sp+0]\n", word_store, value);
        return;
    }
    if (op_is(op, "pop") && argc == 1) {
        buf_appendf(text, "  %s [%%sp+0], %s\n", word_load, sparc_reg(args[0], line_no, op));
        buf_appendf(text, "  add %%sp, %d, %%sp\n", slot);
        return;
    }
    if (op_is(op, "jmp") && argc == 1) {
        buf_appendf(text, "  ba %s\n", args[0]);
        sparc_delay_slot(text);
        return;
    }
    if (op_is(op, "call") && argc == 1) {
        buf_appendf(text, "  call %s\n", args[0]);
        sparc_delay_slot(text);
        return;
    }
    if (op_is(op, "ret") && argc == 0) {
        /* retl returns through %o7 without unwinding a window, which is what
           flat code wants. */
        buf_append(text, "  retl\n");
        sparc_delay_slot(text);
        return;
    }
    if (op_is(op, "syscall")) { emit_sparc_syscall(text, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count for SPARC");
}

/* ------------------------------------------------------------------ m68k */

/* Only the data registers can do arithmetic and logic, and d0 and d1 are the
   compiler's scratch while a7 and a6 are the stack and frame pointers, which
   leaves six machine registers. The other ten virtual registers live in spill
   slots, which m68k reaches directly: nearly every instruction here takes a
   memory operand on either side. */
static const char *m68k_regs[] = {
    "%d2", "%d3", "%d4", "%d5", "%d6", "%d7"
};

#define M68K_MAPPED_COUNT ((int)(sizeof(m68k_regs) / sizeof(m68k_regs[0])))
#define M68K_SPILL_COUNT (16 - M68K_MAPPED_COUNT)
#define M68K_SCRATCH "%d0"
#define M68K_SCRATCH2 "%d1"
#define M68K_ADDR_SCRATCH "%a0"

static unsigned m68k_spill_used = 0;

static bool m68k_reg_is_spilled(const char *value) {
    return virtual_reg_index(value) >= M68K_MAPPED_COUNT;
}

static const char *m68k_operand_text(int idx) {
    static char pool[8][48];
    static unsigned next = 0;
    char *slot;
    if (idx < M68K_MAPPED_COUNT) return m68k_regs[idx];
    slot = pool[next];
    next = (next + 1) % 8;
    m68k_spill_used |= 1u << (idx - M68K_MAPPED_COUNT);
    snprintf(slot, sizeof(pool[0]), "%s+%d", X86_SPILL_SYMBOL, (idx - M68K_MAPPED_COUNT) * 4);
    return slot;
}

static const char *m68k_reg(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        line_error_token(line_no, value, op, "expected virtual register r0-r15");
    }
    return m68k_operand_text(reg);
}

/* An operand as m68k spells it: a register, a spill slot, or an immediate,
   which needs the # that marks one. */
static const char *m68k_operand(const char *value, int line_no, const char *op) {
    static char pool[4][64];
    static unsigned next = 0;
    char *slot;
    int reg = virtual_reg_index(value);
    if (reg >= 0) return m68k_operand_text(reg);
    if (!is_int(value) && !is_symbol(value) && !is_known_constant(value)) {
        line_error_token(line_no, value, op, "expected register, integer, symbol, or constant");
    }
    slot = pool[next];
    next = (next + 1) % 4;
    snprintf(slot, sizeof(pool[0]), "#%s", value);
    return slot;
}

static int m68k_vreg_of(const char *physical) {
    for (int i = 0; i < M68K_MAPPED_COUNT; i++) {
        if (strcmp(m68k_regs[i], physical) == 0) return i;
    }
    return -1;
}

static bool m68k_must_preserve(const char *physical) {
    int vreg = m68k_vreg_of(physical);
    return vreg >= 0 && (mentioned_vregs & (1u << (unsigned)vreg)) != 0;
}

static void m68k_emit_address(Buffer *text, const char *addr_text, char *out, size_t out_size,
                              int line_no, const char *op) {
    Address addr;
    if (!parse_address(addr_text, &addr)) {
        line_error_token(line_no, addr_text, op, "expected address like [r0 + 8] or [symbol + 8]");
    }
    if (!addr.has_base) {
        if (addr.offset == 0) snprintf(out, out_size, "%s", addr.symbol);
        else snprintf(out, out_size, "%s%+lld", addr.symbol, addr.offset);
        return;
    }
    /* Only an address register can hold a pointer being dereferenced. */
    buf_appendf(text, "  move.l %s, %s\n", m68k_reg(addr.base, line_no, op), M68K_ADDR_SCRATCH);
    snprintf(out, out_size, "%lld(%s)", addr.offset, M68K_ADDR_SCRATCH);
}

static void emit_m68k_syscall(Buffer *text, char **args, int argc, int line_no) {
    static const char *const arg_regs[] = {"%d1", "%d2", "%d3", "%d4", "%d5"};
    const char *result = syscall_result_operand(&args, &argc);
    int number = -1;
    int count;
    bool returns = true;
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    if (op_is(args[0], "exit")) { number = 1; returns = false; }
    else if (op_is(args[0], "read")) number = 3;
    else if (op_is(args[0], "write")) number = 4;
    else if (op_is(args[0], "open")) number = 5;
    else if (op_is(args[0], "close")) number = 6;
    else line_error(line_no, "syscall", "unknown syscall");
    count = argc - 1;
    if (count > 5) count = 5;
    /* The argument registers past the first are also virtual registers, so
       the ones this call writes are saved unless the source never names them. */
    if (returns) {
        for (int i = 0; i < count; i++) {
            if (m68k_must_preserve(arg_regs[i])) buf_appendf(text, "  move.l %s, -(%%sp)\n", arg_regs[i]);
        }
    }
    /* Values are staged on the stack because loading one argument register can
       destroy the source of a later one. */
    for (int i = 0; i < count; i++) {
        buf_appendf(text, "  move.l %s, -(%%sp)\n", m68k_operand(args[i + 1], line_no, "syscall"));
    }
    for (int i = count - 1; i >= 0; i--) {
        buf_appendf(text, "  move.l (%%sp)+, %s\n", arg_regs[i]);
    }
    buf_appendf(text, "  move.l #%d, %%d0\n", number);
    buf_append(text, "  trap #0\n");
    if (result) {
        /* d0 carries the result and is not one of the restored registers. */
        buf_appendf(text, "  move.l %%d0, %s\n", m68k_reg(result, line_no, "syscall"));
    }
    if (returns) {
        for (int i = count - 1; i >= 0; i--) {
            if (m68k_must_preserve(arg_regs[i])) buf_appendf(text, "  move.l (%%sp)+, %s\n", arg_regs[i]);
        }
    }
}

static void emit_m68k_instruction(Buffer *text, const char *op, const char *size,
                                  char **args, int argc, int line_no) {
    /* m68k writes the source first, which is the opposite of the two-address
       form here, so every operand pair below reads right to left. */
    const char *suffix = size[0] == 'b' ? ".b" : size[0] == 'w' ? ".w" : ".l";

    if (op_is(op, "func") && argc == 1) { buf_appendf(text, "%s:\n", args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) return;
    if (op_is(op, "enter") && argc == 1) {
        buf_appendf(text, "  link %%a6, #-%s\n", strcmp(args[0], "0") == 0 ? "0" : args[0]);
        return;
    }
    if (op_is(op, "leave") && argc == 0) { buf_append(text, "  unlk %a6\n"); return; }
    if (op_is(op, "mov") && argc == 2) {
        if (m68k_reg_is_spilled(args[0]) && m68k_reg_is_spilled(args[1])) {
            buf_appendf(text, "  move.l %s, %s\n", m68k_operand(args[1], line_no, op), M68K_SCRATCH);
            buf_appendf(text, "  move.l %s, %s\n", M68K_SCRATCH, m68k_reg(args[0], line_no, op));
        } else {
            buf_appendf(text, "  move.l %s, %s\n", m68k_operand(args[1], line_no, op),
                        m68k_reg(args[0], line_no, op));
        }
        return;
    }
    if (op_is(op, "load_addr") && argc == 2) {
        buf_appendf(text, "  move.l #%s, %s\n", args[1], m68k_reg(args[0], line_no, op));
        return;
    }
    if (op_is(op, "load") && argc == 2) {
        char addr[256];
        bool spilled = m68k_reg_is_spilled(args[0]);
        m68k_emit_address(text, args[1], addr, sizeof(addr), line_no, op);
        if (size[0] == 'b' || size[0] == 'w') {
            /* Sub-word loads leave the rest of the register alone, so it is
               cleared first. */
            buf_appendf(text, "  moveq #0, %s\n", M68K_SCRATCH);
            buf_appendf(text, "  move%s %s, %s\n", suffix, addr, M68K_SCRATCH);
            buf_appendf(text, "  move.l %s, %s\n", M68K_SCRATCH, m68k_reg(args[0], line_no, op));
        } else {
            buf_appendf(text, "  move.l %s, %s\n", addr,
                        spilled ? M68K_SCRATCH : m68k_reg(args[0], line_no, op));
            if (spilled) buf_appendf(text, "  move.l %s, %s\n", M68K_SCRATCH, m68k_reg(args[0], line_no, op));
        }
        return;
    }
    if (op_is(op, "store") && argc == 2) {
        char addr[256];
        m68k_emit_address(text, args[0], addr, sizeof(addr), line_no, op);
        if (m68k_reg_is_spilled(args[1])) {
            buf_appendf(text, "  move.l %s, %s\n", m68k_operand(args[1], line_no, op), M68K_SCRATCH);
            buf_appendf(text, "  move%s %s, %s\n", suffix, M68K_SCRATCH, addr);
        } else {
            buf_appendf(text, "  move%s %s, %s\n", suffix, m68k_operand(args[1], line_no, op), addr);
        }
        return;
    }
    if ((op_is(op, "add") || op_is(op, "sub") || op_is(op, "and") ||
         op_is(op, "or") || op_is(op, "xor") || op_is(op, "cmp")) && argc == 2) {
        const char *native = op_is(op, "xor") ? "eor" : op;
        /* Only one side may be memory, and eor and cmp need their source in a
           register, so anything else is staged there first. */
        if ((m68k_reg_is_spilled(args[0]) && m68k_reg_is_spilled(args[1])) ||
            op_is(op, "xor") || op_is(op, "cmp") || virtual_reg_index(args[1]) < 0) {
            buf_appendf(text, "  move.l %s, %s\n", m68k_operand(args[1], line_no, op), M68K_SCRATCH);
            buf_appendf(text, "  %s.l %s, %s\n", native, M68K_SCRATCH, m68k_reg(args[0], line_no, op));
        } else {
            buf_appendf(text, "  %s.l %s, %s\n", native, m68k_operand(args[1], line_no, op),
                        m68k_reg(args[0], line_no, op));
        }
        return;
    }
    if ((op_is(op, "mul") || op_is(op, "div") || op_is(op, "mod")) && argc == 2) {
        /* These need their destination in a data register. */
        buf_appendf(text, "  move.l %s, %s\n", m68k_reg(args[0], line_no, op), M68K_SCRATCH);
        buf_appendf(text, "  move.l %s, %s\n", m68k_operand(args[1], line_no, op), M68K_SCRATCH2);
        if (op_is(op, "mul")) buf_appendf(text, "  muls.l %s, %s\n", M68K_SCRATCH2, M68K_SCRATCH);
        else if (op_is(op, "div")) buf_appendf(text, "  divs.l %s, %s\n", M68K_SCRATCH2, M68K_SCRATCH);
        else {
            /* divsl leaves the remainder in the first register it names and
               the quotient in the second, but naming one register twice is
               the encoding for "quotient only" and that is what it stores.
               The divisor's own register takes the remainder instead: the
               divisor is read before either result is written. */
            buf_appendf(text, "  divsl.l %s, %s:%s\n", M68K_SCRATCH2, M68K_SCRATCH2, M68K_SCRATCH);
            buf_appendf(text, "  move.l %s, %s\n", M68K_SCRATCH2, M68K_SCRATCH);
        }
        buf_appendf(text, "  move.l %s, %s\n", M68K_SCRATCH, m68k_reg(args[0], line_no, op));
        return;
    }
    if ((op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar")) && argc == 2) {
        const char *native = op_is(op, "shl") ? "lsl" : op_is(op, "shr") ? "lsr" : "asr";
        buf_appendf(text, "  move.l %s, %s\n", m68k_reg(args[0], line_no, op), M68K_SCRATCH);
        buf_appendf(text, "  move.l %s, %s\n", m68k_operand(args[1], line_no, op), M68K_SCRATCH2);
        buf_appendf(text, "  %s.l %s, %s\n", native, M68K_SCRATCH2, M68K_SCRATCH);
        buf_appendf(text, "  move.l %s, %s\n", M68K_SCRATCH, m68k_reg(args[0], line_no, op));
        return;
    }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") || op_is(op, "dec")) && argc == 1) {
        const char *dst = m68k_reg(args[0], line_no, op);
        if (op_is(op, "neg")) buf_appendf(text, "  neg.l %s\n", dst);
        else if (op_is(op, "not")) buf_appendf(text, "  not.l %s\n", dst);
        else buf_appendf(text, "  %s.l #1, %s\n", op_is(op, "inc") ? "add" : "sub", dst);
        return;
    }
    if (is_conditional_branch(op) && argc == 1) {
        const char *branch = op_is(op, "je") ? "beq" : op_is(op, "jne") ? "bne" :
                             op_is(op, "jg") ? "bgt" : op_is(op, "jl") ? "blt" :
                             op_is(op, "jge") ? "bge" : op_is(op, "jle") ? "ble" :
                             op_is(op, "ja") ? "bhi" : op_is(op, "jb") ? "bcs" :
                             op_is(op, "jae") ? "bcc" : "bls";
        buf_appendf(text, "  %s %s\n", branch, args[0]);
        return;
    }
    if (op_is(op, "push") && argc == 1) {
        buf_appendf(text, "  move.l %s, -(%%sp)\n", m68k_operand(args[0], line_no, op));
        return;
    }
    if (op_is(op, "pop") && argc == 1) {
        buf_appendf(text, "  move.l (%%sp)+, %s\n", m68k_reg(args[0], line_no, op));
        return;
    }
    if (op_is(op, "jmp") && argc == 1) { buf_appendf(text, "  jmp %s\n", args[0]); return; }
    if (op_is(op, "call") && argc == 1) { buf_appendf(text, "  jsr %s\n", args[0]); return; }
    if (op_is(op, "ret") && argc == 0) { buf_append(text, "  rts\n"); return; }
    if (op_is(op, "syscall")) { emit_m68k_syscall(text, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count for m68k");
}

/* ---------------------------------------------------------------- s390x */

/* z/Architecture has sixteen general registers and spends half of them on the
   machine: r15 is the stack pointer, r14 the return address, r13 the frame
   pointer here, r0 cannot serve as a base register and r0/r1 are the
   compiler's scratch pair - which the division also needs, since it works on
   an even/odd pair. That leaves eleven, and virtual r11-r15 spill. */
static const char *s390_regs[] = {
    "%r2", "%r3", "%r4", "%r5", "%r6", "%r7",
    "%r8", "%r9", "%r10", "%r11", "%r12"
};

#define S390_MAPPED_COUNT ((int)(sizeof(s390_regs) / sizeof(s390_regs[0])))
#define S390_SPILL_COUNT (16 - S390_MAPPED_COUNT)
#define S390_SCRATCH "%r1"
#define S390_SCRATCH2 "%r0"

static unsigned s390_spill_used = 0;
static DeferredCompare s390_cmp = {CMP_NONE, {0}, {0}, false, "", "", "lgr", "lgfi"};

static bool s390_reg_is_spilled(const char *value) {
    return virtual_reg_index(value) >= S390_MAPPED_COUNT;
}

static int s390_spill_offset(const char *value) {
    int index = virtual_reg_index(value) - S390_MAPPED_COUNT;
    s390_spill_used |= 1u << (unsigned)index;
    return index * 8;
}

/* lgfi carries a 32-bit immediate sign-extended to 64. A wider value has to
   be built from its halves, which is what the population count masks need. */
static void s390_load_imm(Buffer *text, const char *reg, const char *value) {
    long long number;
    if (is_int(value)) {
        number = strtoll(value, NULL, 0);
    } else if (!constant_value(value, &number)) {
        /* A name the source did not give a plain number to is left for the
           assembler, which knows what it stands for. */
        buf_appendf(text, "  lgfi %s, %s\n", reg, value);
        return;
    }
    if (number >= -2147483647LL - 1 && number <= 2147483647LL) {
        buf_appendf(text, "  lgfi %s, %lld\n", reg, number);
        return;
    }
    buf_appendf(text, "  llilf %s, %llu\n", reg, (unsigned long long)number & 0xffffffffu);
    buf_appendf(text, "  iihf %s, %llu\n", reg, ((unsigned long long)number >> 32) & 0xffffffffu);
}

/* Naming r0 where an instruction wants a base register does not mean register
   zero: it means there is no base register, and the displacement is used as an
   absolute address. So a spill slot is addressed through r1, even when the
   value being loaded is going to r0. */
static const char *s390_base_reg(const char *scratch) {
    return strcmp(scratch, "%r0") == 0 ? S390_SCRATCH : scratch;
}

/* Brings a virtual register into a real one, loading it from its slot when it
   has no register of its own. */
static const char *s390_value_reg(Buffer *text, const char *value, const char *scratch,
                                  int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg >= 0 && reg < S390_MAPPED_COUNT) return s390_regs[reg];
    if (reg >= 0) {
        int offset = s390_spill_offset(value);
        const char *base = s390_base_reg(scratch);
        buf_appendf(text, "  larl %s, %s\n", base, X86_SPILL_SYMBOL);
        buf_appendf(text, "  lg %s, %d(%s)\n", scratch, offset, base);
        return scratch;
    }
    if (!is_int(value) && !is_symbol(value) && !is_known_constant(value)) {
        line_error_token(line_no, value, op, "expected register, integer, symbol, or constant");
    }
    if (is_int(value) || is_known_constant(value)) s390_load_imm(text, scratch, value);
    else buf_appendf(text, "  larl %s, %s\n", scratch, value);
    return scratch;
}

/* Where a result should be computed, and where it has to be put afterwards. */
static const char *s390_dst_reg(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) line_error_token(line_no, value, op, "expected virtual register r0-r15");
    return reg < S390_MAPPED_COUNT ? s390_regs[reg] : S390_SCRATCH2;
}

static void s390_store_back(Buffer *text, const char *value, const char *from, const char *via) {
    const char *base;
    if (!s390_reg_is_spilled(value)) return;
    base = s390_base_reg(via);
    buf_appendf(text, "  larl %s, %s\n", base, X86_SPILL_SYMBOL);
    buf_appendf(text, "  stg %s, %d(%s)\n", from, s390_spill_offset(value), base);
}

static void s390_emit_address(Buffer *text, const char *addr_text, char *out, size_t out_size,
                              const char *scratch, int line_no, const char *op) {
    Address addr;
    /* Whatever comes back from here is used as a base register, so r0 is not
       an option for it either. */
    const char *held = s390_base_reg(scratch);
    if (!parse_address(addr_text, &addr)) {
        line_error_token(line_no, addr_text, op, "expected address like [r0 + 8] or [symbol + 8]");
    }
    if (addr.has_base) {
        const char *base = s390_value_reg(text, addr.base, held, line_no, op);
        snprintf(out, out_size, "%lld(%s)", addr.offset, base);
        return;
    }
    buf_appendf(text, "  larl %s, %s\n", held, addr.symbol);
    snprintf(out, out_size, "%lld(%s)", addr.offset, held);
}

static int s390_vreg_of(const char *physical) {
    for (int i = 0; i < S390_MAPPED_COUNT; i++) {
        if (strcmp(s390_regs[i], physical) == 0) return i;
    }
    return -1;
}

static bool s390_must_preserve(const char *physical) {
    int vreg = s390_vreg_of(physical);
    return vreg >= 0 && (mentioned_vregs & (1u << (unsigned)vreg)) != 0;
}

static void emit_s390_syscall(Buffer *text, char **args, int argc, int line_no) {
    static const char *const arg_regs[] = {"%r2", "%r3", "%r4", "%r5", "%r6"};
    const char *result = syscall_result_operand(&args, &argc);
    int number = -1;
    int count;
    bool returns = true;
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    if (op_is(args[0], "exit")) { number = 1; returns = false; }
    else if (op_is(args[0], "read")) number = 3;
    else if (op_is(args[0], "write")) number = 4;
    else if (op_is(args[0], "open")) number = 5;
    else if (op_is(args[0], "close")) number = 6;
    else line_error(line_no, "syscall", "unknown syscall");
    count = argc - 1;
    if (count > 5) count = 5;
    /* r2 through r6 also carry virtual registers, so the ones this call
       writes are saved unless the source never names them. */
    if (returns) {
        for (int i = 0; i < count; i++) {
            if (s390_must_preserve(arg_regs[i])) {
                buf_append(text, "  aghi %r15, -8\n");
                buf_appendf(text, "  stg %s, 0(%%r15)\n", arg_regs[i]);
            }
        }
    }
    /* Values are staged on the stack because loading one argument register
       can destroy the source of a later one. */
    for (int i = 0; i < count; i++) {
        const char *src = s390_value_reg(text, args[i + 1], S390_SCRATCH, line_no, "syscall");
        buf_append(text, "  aghi %r15, -8\n");
        buf_appendf(text, "  stg %s, 0(%%r15)\n", src);
    }
    for (int i = count - 1; i >= 0; i--) {
        buf_appendf(text, "  lg %s, 0(%%r15)\n", arg_regs[i]);
        buf_append(text, "  aghi %r15, 8\n");
    }
    buf_appendf(text, "  lghi %s, %d\n", S390_SCRATCH, number);
    buf_append(text, "  svc 0\n");
    /* The result arrives in r2, which the restores below are about to
       overwrite, so it is taken aside first. */
    if (result) buf_appendf(text, "  lgr %s, %%r2\n", S390_SCRATCH2);
    if (returns) {
        for (int i = count - 1; i >= 0; i--) {
            if (s390_must_preserve(arg_regs[i])) {
                buf_appendf(text, "  lg %s, 0(%%r15)\n", arg_regs[i]);
                buf_append(text, "  aghi %r15, 8\n");
            }
        }
    }
    if (result) {
        const char *dst = s390_dst_reg(result, line_no, "syscall");
        if (strcmp(dst, S390_SCRATCH2) != 0) buf_appendf(text, "  lgr %s, %s\n", dst, S390_SCRATCH2);
        s390_store_back(text, result, S390_SCRATCH2, S390_SCRATCH);
    }
}

static void emit_s390_instruction(Buffer *text, const char *op, const char *size,
                                  char **args, int argc, int line_no) {
    if (op_is(op, "func") && argc == 1) { buf_appendf(text, "%s:\n", args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) return;
    if (op_is(op, "enter") && argc == 1) {
        buf_append(text, "  aghi %r15, -16\n");
        buf_append(text, "  stg %r14, 8(%r15)\n");
        buf_append(text, "  stg %r13, 0(%r15)\n");
        buf_append(text, "  lgr %r13, %r15\n");
        if (strcmp(args[0], "0") != 0) buf_appendf(text, "  aghi %%r15, -%s\n", args[0]);
        return;
    }
    if (op_is(op, "leave") && argc == 0) {
        buf_append(text, "  lgr %r15, %r13\n");
        buf_append(text, "  lg %r13, 0(%r15)\n");
        buf_append(text, "  lg %r14, 8(%r15)\n");
        buf_append(text, "  aghi %r15, 16\n");
        return;
    }
    if (op_is(op, "mov") && argc == 2) {
        const char *dst = s390_dst_reg(args[0], line_no, op);
        int src = virtual_reg_index(args[1]);
        if (src >= 0) {
            const char *from = s390_value_reg(text, args[1], S390_SCRATCH, line_no, op);
            if (strcmp(from, dst) != 0) buf_appendf(text, "  lgr %s, %s\n", dst, from);
        } else if (is_int(args[1]) || is_known_constant(args[1])) {
            s390_load_imm(text, dst, args[1]);
        } else {
            buf_appendf(text, "  larl %s, %s\n", dst, args[1]);
        }
        s390_store_back(text, args[0], dst, S390_SCRATCH);
        return;
    }
    if (op_is(op, "load_addr") && argc == 2) {
        const char *dst = s390_dst_reg(args[0], line_no, op);
        buf_appendf(text, "  larl %s, %s\n", dst, args[1]);
        s390_store_back(text, args[0], dst, S390_SCRATCH);
        return;
    }
    if (op_is(op, "load") && argc == 2) {
        char addr[256];
        const char *dst = s390_dst_reg(args[0], line_no, op);
        const char *mnemonic = size[0] == 'b' ? "llgc" : size[0] == 'w' ? "llgh" :
                               size[0] == 'd' ? "llgf" : "lg";
        s390_emit_address(text, args[1], addr, sizeof(addr), S390_SCRATCH, line_no, op);
        buf_appendf(text, "  %s %s, %s\n", mnemonic, dst, addr);
        s390_store_back(text, args[0], dst, S390_SCRATCH);
        return;
    }
    if (op_is(op, "store") && argc == 2) {
        char addr[256];
        const char *mnemonic = size[0] == 'b' ? "stc" : size[0] == 'w' ? "sth" :
                               size[0] == 'd' ? "st" : "stg";
        const char *value = s390_value_reg(text, args[1], S390_SCRATCH2, line_no, op);
        s390_emit_address(text, args[0], addr, sizeof(addr), S390_SCRATCH, line_no, op);
        buf_appendf(text, "  %s %s, %s\n", mnemonic, value, addr);
        return;
    }
    if ((op_is(op, "add") || op_is(op, "sub") || op_is(op, "and") ||
         op_is(op, "or") || op_is(op, "xor") || op_is(op, "mul")) && argc == 2) {
        const char *dst = s390_dst_reg(args[0], line_no, op);
        const char *native = op_is(op, "add") ? "agr" : op_is(op, "sub") ? "sgr" :
                             op_is(op, "and") ? "ngr" : op_is(op, "or") ? "ogr" :
                             op_is(op, "xor") ? "xgr" : "msgr";
        const char *src;
        if (s390_reg_is_spilled(args[0])) {
            buf_appendf(text, "  larl %s, %s\n", S390_SCRATCH, X86_SPILL_SYMBOL);
            buf_appendf(text, "  lg %s, %d(%s)\n", dst, s390_spill_offset(args[0]), S390_SCRATCH);
        }
        src = s390_value_reg(text, args[1], S390_SCRATCH, line_no, op);
        buf_appendf(text, "  %s %s, %s\n", native, dst, src);
        s390_store_back(text, args[0], dst, S390_SCRATCH);
        return;
    }
    if ((op_is(op, "div") || op_is(op, "mod")) && argc == 2) {
        /* The divide works on the r0:r1 pair: the dividend goes in the odd
           register sign-extended into the even one, and it leaves the
           remainder in the even and the quotient in the odd. */
        const char *dividend = s390_value_reg(text, args[0], S390_SCRATCH, line_no, op);
        const char *divisor;
        const char *dst;
        if (strcmp(dividend, S390_SCRATCH) != 0) buf_appendf(text, "  lgr %s, %s\n", S390_SCRATCH, dividend);
        divisor = s390_value_reg(text, args[1], "%r12", line_no, op);
        buf_append(text, "  srag %r0, %r1, 63\n");
        buf_appendf(text, "  dsgr %%r0, %s\n", divisor);
        dst = s390_dst_reg(args[0], line_no, op);
        buf_appendf(text, "  lgr %s, %s\n", dst, op_is(op, "div") ? "%r1" : "%r0");
        s390_store_back(text, args[0], dst, S390_SCRATCH);
        return;
    }
    if ((op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar")) && argc == 2) {
        const char *dst = s390_dst_reg(args[0], line_no, op);
        const char *native = op_is(op, "shl") ? "sllg" : op_is(op, "shr") ? "srlg" : "srag";
        int src = virtual_reg_index(args[1]);
        const char *value = s390_value_reg(text, args[0], dst, line_no, op);
        if (strcmp(value, dst) != 0) buf_appendf(text, "  lgr %s, %s\n", dst, value);
        /* The shift distance is a displacement, optionally plus a register. */
        if (src >= 0) {
            const char *amount = s390_value_reg(text, args[1], S390_SCRATCH, line_no, op);
            buf_appendf(text, "  %s %s, %s, 0(%s)\n", native, dst, dst, amount);
        } else {
            buf_appendf(text, "  %s %s, %s, %s\n", native, dst, dst, args[1]);
        }
        s390_store_back(text, args[0], dst, S390_SCRATCH);
        return;
    }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") || op_is(op, "dec")) && argc == 1) {
        const char *dst = s390_dst_reg(args[0], line_no, op);
        const char *value = s390_value_reg(text, args[0], dst, line_no, op);
        if (strcmp(value, dst) != 0) buf_appendf(text, "  lgr %s, %s\n", dst, value);
        if (op_is(op, "neg")) buf_appendf(text, "  lcgr %s, %s\n", dst, dst);
        else if (op_is(op, "not")) {
            /* No complement instruction: exclusive-or against all ones. */
            buf_appendf(text, "  lghi %s, -1\n", S390_SCRATCH);
            buf_appendf(text, "  xgr %s, %s\n", dst, S390_SCRATCH);
        } else buf_appendf(text, "  aghi %s, %s\n", dst, op_is(op, "inc") ? "1" : "-1");
        s390_store_back(text, args[0], dst, S390_SCRATCH);
        return;
    }
    if (op_is(op, "cmp") && argc == 2) {
        int src = virtual_reg_index(args[1]);
        compare_record(&s390_cmp, args[0], src >= 0 ? args[1] : args[1], src >= 0);
        return;
    }
    if (is_conditional_branch(op) && argc == 1) {
        /* Only the branch says whether the comparison was meant to be signed,
           so it is emitted here. */
        const bool unsigned_test = op_is(op, "ja") || op_is(op, "jb") ||
                                   op_is(op, "jae") || op_is(op, "jbe");
        const char *branch = op_is(op, "je") ? "je" : op_is(op, "jne") ? "jne" :
                             op_is(op, "jg") || op_is(op, "ja") ? "jh" :
                             op_is(op, "jl") || op_is(op, "jb") ? "jl" :
                             op_is(op, "jge") || op_is(op, "jae") ? "jhe" : "jle";
        const char *lhs;
        if (s390_cmp.state == CMP_NONE) {
            line_error(line_no, op, "conditional jump with no preceding cmp");
        }
        lhs = s390_value_reg(text, s390_cmp.lhs, S390_SCRATCH2, line_no, op);
        if (s390_cmp.rhs_is_reg) {
            const char *rhs = s390_value_reg(text, s390_cmp.rhs, S390_SCRATCH, line_no, op);
            buf_appendf(text, "  %s %s, %s\n", unsigned_test ? "clgr" : "cgr", lhs, rhs);
        } else {
            s390_load_imm(text, S390_SCRATCH, s390_cmp.rhs);
            buf_appendf(text, "  %s %s, %s\n", unsigned_test ? "clgr" : "cgr", lhs, S390_SCRATCH);
        }
        buf_appendf(text, "  %s %s\n", branch, args[0]);
        return;
    }
    if (op_is(op, "push") && argc == 1) {
        const char *value = s390_value_reg(text, args[0], S390_SCRATCH2, line_no, op);
        buf_append(text, "  aghi %r15, -8\n");
        buf_appendf(text, "  stg %s, 0(%%r15)\n", value);
        return;
    }
    if (op_is(op, "pop") && argc == 1) {
        const char *dst = s390_dst_reg(args[0], line_no, op);
        buf_appendf(text, "  lg %s, 0(%%r15)\n", dst);
        buf_append(text, "  aghi %r15, 8\n");
        s390_store_back(text, args[0], dst, S390_SCRATCH);
        return;
    }
    if (op_is(op, "jmp") && argc == 1) { buf_appendf(text, "  j %s\n", args[0]); return; }
    if (op_is(op, "call") && argc == 1) { buf_appendf(text, "  brasl %%r14, %s\n", args[0]); return; }
    if (op_is(op, "ret") && argc == 0) { buf_append(text, "  br %r14\n"); return; }
    if (op_is(op, "syscall")) { emit_s390_syscall(text, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count for s390x");
}

/* ----------------------------------------------------------------- wasm */

/* WebAssembly has no jump to a label. It has structured control flow, so
   arbitrary jumps are expressed the way compilers normally express them: a
   loop whose body is a br_table over a block index. Each label becomes a
   case, a jump sets the index and re-enters the dispatch, and falling from
   one case into the next needs nothing at all, because that is what the
   nesting already does. Calls and returns ride a shadow stack in linear
   memory, since the dispatch has no call stack of its own.

   Registers become i64 locals, data becomes one linear memory segment laid
   out in declaration order, and the syscalls become WASI imports. */

#define WASM_MAX_BLOCKS 256
#define WASM_DATA_BASE 1024
#define WASM_STACK_TOP 65536

typedef struct {
    char label[96];
    Buffer body;
} WasmBlock;

typedef struct {
    char name[96];
    long long offset;
} WasmSymbol;

static WasmBlock wasm_blocks[WASM_MAX_BLOCKS];
static int wasm_block_count = 0;
static WasmSymbol wasm_symbols[512];
static int wasm_symbol_count = 0;
static Buffer wasm_data;
static bool wasm_active = false;
static long long wasm_data_size = 0;
static bool wasm_uses_read = false;
static bool wasm_uses_write = false;
static bool wasm_uses_close = false;
/* Names the blocks that a call returns into. */
static int wasm_continuations = 0;

static void wasm_begin(void) {
    wasm_block_count = 0;
    wasm_symbol_count = 0;
    wasm_continuations = 0;
    wasm_uses_read = false;
    wasm_uses_write = false;
    wasm_uses_close = false;
    buf_init(&wasm_data);
    wasm_data_size = 0;
    wasm_active = true;
    /* Everything before the first label belongs to an entry block. */
    snprintf(wasm_blocks[0].label, sizeof(wasm_blocks[0].label), "%s", "");
    buf_init(&wasm_blocks[0].body);
    wasm_block_count = 1;
}

static Buffer *wasm_current(void) {
    return &wasm_blocks[wasm_block_count - 1].body;
}

static void wasm_open_block(const char *label) {
    if (wasm_block_count >= WASM_MAX_BLOCKS) {
        die("too many labels for the wasm dispatch table");
    }
    snprintf(wasm_blocks[wasm_block_count].label, sizeof(wasm_blocks[0].label), "%s", label);
    buf_init(&wasm_blocks[wasm_block_count].body);
    wasm_block_count++;
}

static int wasm_block_index(const char *label) {
    for (int i = 0; i < wasm_block_count; i++) {
        if (strcmp(wasm_blocks[i].label, label) == 0) return i;
    }
    return -1;
}

static void wasm_record_symbol(const char *name, long long offset) {
    if (wasm_symbol_count >= (int)(sizeof(wasm_symbols) / sizeof(wasm_symbols[0]))) {
        die("too many data symbols for the wasm layout");
    }
    snprintf(wasm_symbols[wasm_symbol_count].name, sizeof(wasm_symbols[0].name), "%s", name);
    wasm_symbols[wasm_symbol_count].offset = offset;
    wasm_symbol_count++;
}

static bool wasm_symbol_offset(const char *name, long long *offset) {
    for (int i = 0; i < wasm_symbol_count; i++) {
        if (strcmp(wasm_symbols[i].name, name) == 0) {
            *offset = wasm_symbols[i].offset;
            return true;
        }
    }
    return false;
}

/* Data is laid out as bytes rather than emitted as directives, because a
   wasm data segment is a byte string and every reference to a label is the
   offset the label ended up at. */
static void wasm_emit_bytes(const char *values, int width) {
    char work[1024];
    char *cursor;
    snprintf(work, sizeof(work), "%s", values);
    cursor = work;
    while (*cursor) {
        char *comma = strchr(cursor, ',');
        long long value;
        if (comma) *comma = '\0';
        value = strtoll(trim(cursor), NULL, 0);
        for (int i = 0; i < width; i++) {
            buf_appendf(&wasm_data, "\\%02x", (unsigned)((unsigned long long)value >> (i * 8)) & 0xffu);
        }
        if (!comma) break;
        cursor = comma + 1;
    }
}

static void wasm_data_line(char *line, int line_no) {
    char *colon = strchr(line, ':');
    char *name;
    char *kind;
    if (strncmp(line, "align ", 6) == 0) return;
    if (!colon) line_error(line_no, "data", "expected name: directive");
    *colon = '\0';
    name = trim(line);
    kind = trim(colon + 1);
    wasm_record_symbol(name, WASM_DATA_BASE + wasm_data_size);
    if (strncmp(kind, "string", 6) == 0 && isspace((unsigned char)kind[6])) {
        char *quote = trim(kind + 6);
        int count = 0;
        char len_name[128];
        if (*quote != '"') line_error(line_no, "string", "expected string literal");
        quote++;
        for (size_t i = 0; quote[i] && quote[i] != '"'; i++) {
            unsigned char ch = (unsigned char)quote[i];
            if (ch == '\\') {
                i++;
                if (quote[i] == 'n') ch = '\n';
                else if (quote[i] == 't') ch = '\t';
                else if (quote[i] == '0') ch = '\0';
                else ch = (unsigned char)quote[i];
            }
            buf_appendf(&wasm_data, "\\%02x", ch);
            count++;
        }
        wasm_data_size += count;
        snprintf(len_name, sizeof(len_name), "%s_len", name);
        {
            char len_text[32];
            snprintf(len_text, sizeof(len_text), "%d", count);
            remember_constant(len_name, len_text);
        }
        return;
    }
    if (strncmp(kind, "zero", 4) == 0 && isspace((unsigned char)kind[4])) {
        long long count = strtoll(trim(kind + 4), NULL, 0);
        for (long long i = 0; i < count; i++) buf_append(&wasm_data, "\\00");
        wasm_data_size += count;
        return;
    }
    {
        static const struct { const char *name; int width; } kinds[] = {
            {"bytes", 1}, {"byte", 1}, {"word", 2}, {"dword", 4}, {"qword", 8}, {NULL, 0}
        };
        for (int i = 0; kinds[i].name; i++) {
            size_t len = strlen(kinds[i].name);
            if (strncmp(kind, kinds[i].name, len) == 0 && isspace((unsigned char)kind[len])) {
                const char *values = trim(kind + len);
                int count = 1;
                for (const char *p = values; *p; p++) if (*p == ',') count++;
                wasm_emit_bytes(values, kinds[i].width);
                wasm_data_size += (long long)count * kinds[i].width;
                return;
            }
        }
    }
    line_error(line_no, "data", "expected string, bytes, byte, word, dword, qword, or zero");
}

/* Pushes an operand onto the wasm stack as an i64. */
static void wasm_push_operand(Buffer *out, const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    long long number;
    if (reg >= 0) {
        buf_appendf(out, "    local.get $r%d\n", reg);
        return;
    }
    if (is_int(value)) {
        buf_appendf(out, "    i64.const %lld\n", strtoll(value, NULL, 0));
        return;
    }
    if (constant_value(value, &number)) {
        buf_appendf(out, "    i64.const %lld\n", number);
        return;
    }
    if (wasm_symbol_offset(value, &number)) {
        buf_appendf(out, "    i64.const %lld\n", number);
        return;
    }
    line_error_token(line_no, value, op, "expected register, integer, symbol, or constant");
}

static void wasm_store_reg(Buffer *out, const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) line_error_token(line_no, value, op, "expected virtual register r0-r15");
    buf_appendf(out, "    local.set $r%d\n", reg);
}

/* Puts a byte address on the stack as an i32, which is what wasm memory
   instructions take. */
static void wasm_push_address(Buffer *out, const char *addr_text, int line_no, const char *op) {
    Address addr;
    long long base;
    if (!parse_address(addr_text, &addr)) {
        line_error_token(line_no, addr_text, op, "expected address like [r0 + 8] or [symbol + 8]");
    }
    if (addr.has_base) {
        buf_appendf(out, "    local.get $r%d\n", virtual_reg_index(addr.base));
        buf_append(out, "    i32.wrap_i64\n");
        if (addr.offset != 0) {
            buf_appendf(out, "    i32.const %lld\n    i32.add\n", addr.offset);
        }
        return;
    }
    if (!wasm_symbol_offset(addr.symbol, &base)) {
        line_error_token(line_no, addr.symbol, op, "no data with that name to address");
    }
    buf_appendf(out, "    i32.const %lld\n", base + addr.offset);
}

static void emit_wasm_instruction(const char *op, const char *size, char **args, int argc, int line_no) {
    Buffer *out = wasm_current();
    if (op_is(op, "func") && argc == 1) { wasm_open_block(args[0]); return; }
    if (op_is(op, "endfunc") && argc == 0) return;
    if (op_is(op, "enter") || op_is(op, "leave")) return;
    if (op_is(op, "mov") && argc == 2) {
        wasm_push_operand(out, args[1], line_no, op);
        wasm_store_reg(out, args[0], line_no, op);
        return;
    }
    if (op_is(op, "load_addr") && argc == 2) {
        wasm_push_operand(out, args[1], line_no, op);
        wasm_store_reg(out, args[0], line_no, op);
        return;
    }
    if (op_is(op, "load") && argc == 2) {
        const char *kind = size[0] == 'b' ? "i64.load8_u" : size[0] == 'w' ? "i64.load16_u" :
                           size[0] == 'd' ? "i64.load32_u" : "i64.load";
        wasm_push_address(out, args[1], line_no, op);
        buf_appendf(out, "    %s\n", kind);
        wasm_store_reg(out, args[0], line_no, op);
        return;
    }
    if (op_is(op, "store") && argc == 2) {
        const char *kind = size[0] == 'b' ? "i64.store8" : size[0] == 'w' ? "i64.store16" :
                           size[0] == 'd' ? "i64.store32" : "i64.store";
        wasm_push_address(out, args[0], line_no, op);
        wasm_push_operand(out, args[1], line_no, op);
        buf_appendf(out, "    %s\n", kind);
        return;
    }
    if ((op_is(op, "add") || op_is(op, "sub") || op_is(op, "mul") || op_is(op, "div") ||
         op_is(op, "mod") || op_is(op, "and") || op_is(op, "or") || op_is(op, "xor") ||
         op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar") ||
         op_is(op, "rol") || op_is(op, "ror")) && argc == 2) {
        const char *native = op_is(op, "add") ? "i64.add" : op_is(op, "sub") ? "i64.sub" :
                             op_is(op, "mul") ? "i64.mul" : op_is(op, "div") ? "i64.div_s" :
                             op_is(op, "mod") ? "i64.rem_s" : op_is(op, "and") ? "i64.and" :
                             op_is(op, "or") ? "i64.or" : op_is(op, "xor") ? "i64.xor" :
                             op_is(op, "shl") ? "i64.shl" : op_is(op, "shr") ? "i64.shr_u" :
                             op_is(op, "sar") ? "i64.shr_s" : op_is(op, "rol") ? "i64.rotl" : "i64.rotr";
        wasm_push_operand(out, args[0], line_no, op);
        wasm_push_operand(out, args[1], line_no, op);
        buf_appendf(out, "    %s\n", native);
        wasm_store_reg(out, args[0], line_no, op);
        return;
    }
    if ((op_is(op, "popcnt") || op_is(op, "clz") || op_is(op, "ctz")) && argc == 1) {
        const char *native = op_is(op, "popcnt") ? "i64.popcnt" : op_is(op, "clz") ? "i64.clz" : "i64.ctz";
        wasm_push_operand(out, args[0], line_no, op);
        buf_appendf(out, "    %s\n", native);
        wasm_store_reg(out, args[0], line_no, op);
        return;
    }
    if ((op_is(op, "neg") || op_is(op, "not") || op_is(op, "inc") || op_is(op, "dec")) && argc == 1) {
        if (op_is(op, "neg")) {
            buf_append(out, "    i64.const 0\n");
            wasm_push_operand(out, args[0], line_no, op);
            buf_append(out, "    i64.sub\n");
        } else if (op_is(op, "not")) {
            wasm_push_operand(out, args[0], line_no, op);
            buf_append(out, "    i64.const -1\n    i64.xor\n");
        } else {
            wasm_push_operand(out, args[0], line_no, op);
            buf_appendf(out, "    i64.const 1\n    %s\n", op_is(op, "inc") ? "i64.add" : "i64.sub");
        }
        wasm_store_reg(out, args[0], line_no, op);
        return;
    }
    if (op_is(op, "cmp") && argc == 2) {
        /* wasm has no condition register, so the two values are kept for the
           branch to test. */
        wasm_push_operand(out, args[0], line_no, op);
        buf_append(out, "    local.set $cmp_a\n");
        wasm_push_operand(out, args[1], line_no, op);
        buf_append(out, "    local.set $cmp_b\n");
        return;
    }
    if (is_conditional_branch(op) && argc == 1) {
        const char *test = op_is(op, "je") ? "i64.eq" : op_is(op, "jne") ? "i64.ne" :
                           op_is(op, "jg") ? "i64.gt_s" : op_is(op, "jl") ? "i64.lt_s" :
                           op_is(op, "jge") ? "i64.ge_s" : op_is(op, "jle") ? "i64.le_s" :
                           op_is(op, "ja") ? "i64.gt_u" : op_is(op, "jb") ? "i64.lt_u" :
                           op_is(op, "jae") ? "i64.ge_u" : "i64.le_u";
        buf_append(out, "    local.get $cmp_a\n    local.get $cmp_b\n");
        buf_appendf(out, "    %s\n", test);
        buf_appendf(out, "    if\n      i32.const @%s@\n      local.set $pc\n      br $dispatch\n    end\n", args[0]);
        return;
    }
    if (op_is(op, "jmp") && argc == 1) {
        buf_appendf(out, "    i32.const @%s@\n    local.set $pc\n    br $dispatch\n", args[0]);
        return;
    }
    if (op_is(op, "call") && argc == 1) {
        /* The return index goes on a shadow stack, because the dispatch loop
           has no call stack of its own. The index is the block after this
           one, so the instruction after the call has to start a block of its
           own: returning otherwise lands at the next label in the program,
           and the two are only the same thing when a call happens to be the
           last instruction before one. The name cannot collide with a label
           from the source, which is a single token and so has no space. */
        char label[96];
        buf_appendf(out, "    local.get $sp\n    i32.const 4\n    i32.sub\n    local.set $sp\n");
        buf_appendf(out, "    local.get $sp\n    i32.const @@next@@\n    i32.store\n");
        buf_appendf(out, "    i32.const @%s@\n    local.set $pc\n    br $dispatch\n", args[0]);
        snprintf(label, sizeof(label), " after call %d", ++wasm_continuations);
        wasm_open_block(label);
        return;
    }
    if (op_is(op, "ret") && argc == 0) {
        buf_append(out, "    local.get $sp\n    i32.load\n    local.set $pc\n");
        buf_append(out, "    local.get $sp\n    i32.const 4\n    i32.add\n    local.set $sp\n");
        buf_append(out, "    br $dispatch\n");
        return;
    }
    if (op_is(op, "push") && argc == 1) {
        buf_append(out, "    local.get $sp\n    i32.const 8\n    i32.sub\n    local.set $sp\n");
        buf_append(out, "    local.get $sp\n");
        wasm_push_operand(out, args[0], line_no, op);
        buf_append(out, "    i64.store\n");
        return;
    }
    if (op_is(op, "pop") && argc == 1) {
        buf_append(out, "    local.get $sp\n    i64.load\n");
        wasm_store_reg(out, args[0], line_no, op);
        buf_append(out, "    local.get $sp\n    i32.const 8\n    i32.add\n    local.set $sp\n");
        return;
    }
    if (op_is(op, "syscall")) {
        const char *result = syscall_result_operand(&args, &argc);
        if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
        if (op_is(args[0], "exit")) {
            wasm_push_operand(out, argc > 1 ? args[1] : "0", line_no, op);
            buf_append(out, "    i32.wrap_i64\n    call $proc_exit\n    unreachable\n");
            return;
        }
        if (op_is(args[0], "close")) {
            wasm_uses_close = true;
            if (argc < 2) line_error(line_no, "syscall", "close needs a descriptor");
            wasm_push_operand(out, args[1], line_no, op);
            buf_append(out, "    i32.wrap_i64\n    call $fd_close\n");
            if (result) {
                buf_append(out, "    i64.extend_i32_s\n");
                wasm_store_reg(out, result, line_no, op);
            } else {
                buf_append(out, "    drop\n");
            }
            return;
        }
        if (op_is(args[0], "write") || op_is(args[0], "read")) {
            const bool writing = op_is(args[0], "write");
            if (argc < 4) line_error(line_no, "syscall", "needs a descriptor, a buffer and a length");
            if (writing) wasm_uses_write = true; else wasm_uses_read = true;
            /* WASI takes a vector of buffers, so one is built in the scratch
               area below the stack. */
            buf_appendf(out, "    i32.const %d\n", WASM_STACK_TOP);
            wasm_push_operand(out, args[2], line_no, op);
            buf_append(out, "    i32.wrap_i64\n    i32.store\n");
            buf_appendf(out, "    i32.const %d\n", WASM_STACK_TOP + 4);
            wasm_push_operand(out, args[3], line_no, op);
            buf_append(out, "    i32.wrap_i64\n    i32.store\n");
            wasm_push_operand(out, args[1], line_no, op);
            buf_append(out, "    i32.wrap_i64\n");
            buf_appendf(out, "    i32.const %d\n    i32.const 1\n    i32.const %d\n",
                        WASM_STACK_TOP, WASM_STACK_TOP + 8);
            buf_appendf(out, "    call $%s\n    drop\n", writing ? "fd_write" : "fd_read");
            if (result) {
                buf_appendf(out, "    i32.const %d\n    i32.load\n    i64.extend_i32_u\n", WASM_STACK_TOP + 8);
                wasm_store_reg(out, result, line_no, op);
            }
            return;
        }
        line_error(line_no, "syscall",
                   "wasm reaches the host through WASI, which offers read, write, close and exit;"
                   " open needs a directory descriptor WASI does not hand a program by name");
    }
    line_error(line_no, op, "unsupported instruction or wrong argument count for wasm");
}

/* Replaces the @label@ placeholders with the block index each label ended up
   at, now that every label is known. */
static void wasm_resolve_labels(Buffer *out, const char *body, int next_index, int line_no) {
    const char *cursor = body;
    while (*cursor) {
        const char *at = strchr(cursor, '@');
        char name[96];
        const char *close;
        int index;
        if (!at) { buf_append(out, cursor); return; }
        buf_grow(out, (size_t)(at - cursor));
        memcpy(out->data + out->len, cursor, (size_t)(at - cursor));
        out->len += (size_t)(at - cursor);
        out->data[out->len] = '\0';
        if (at[1] == '@') {
            /* @@next@@ is the block after this one, which is where a call
               comes back to. */
            buf_appendf(out, "%d", next_index);
            cursor = strstr(at + 2, "@@") + 2;
            continue;
        }
        close = strchr(at + 1, '@');
        if (!close || (size_t)(close - at - 1) >= sizeof(name)) die("malformed wasm label reference");
        memcpy(name, at + 1, (size_t)(close - at - 1));
        name[close - at - 1] = '\0';
        index = wasm_block_index(name);
        if (index < 0) line_error(line_no, name, "no label with that name to jump to");
        buf_appendf(out, "%d", index);
        cursor = close + 1;
    }
}

static Buffer wasm_finish(void) {
    Buffer out;
    buf_init(&out);
    buf_append(&out, "(module\n");
    if (wasm_uses_write) {
        buf_append(&out, "  (import \"wasi_snapshot_preview1\" \"fd_write\"\n"
                         "    (func $fd_write (param i32 i32 i32 i32) (result i32)))\n");
    }
    if (wasm_uses_read) {
        buf_append(&out, "  (import \"wasi_snapshot_preview1\" \"fd_read\"\n"
                         "    (func $fd_read (param i32 i32 i32 i32) (result i32)))\n");
    }
    if (wasm_uses_close) {
        buf_append(&out, "  (import \"wasi_snapshot_preview1\" \"fd_close\"\n"
                         "    (func $fd_close (param i32) (result i32)))\n");
    }
    buf_append(&out, "  (import \"wasi_snapshot_preview1\" \"proc_exit\"\n"
                     "    (func $proc_exit (param i32)))\n");
    buf_append(&out, "  (memory (export \"memory\") 2)\n");
    if (wasm_data.len) {
        buf_appendf(&out, "  (data (i32.const %d) \"%s\")\n", WASM_DATA_BASE, wasm_data.data);
    }
    buf_append(&out, "  (func (export \"_start\")\n");
    for (int i = 0; i < 16; i++) buf_appendf(&out, "    (local $r%d i64)\n", i);
    buf_append(&out, "    (local $cmp_a i64) (local $cmp_b i64)\n");
    buf_append(&out, "    (local $pc i32) (local $sp i32)\n");
    buf_appendf(&out, "    i32.const %d\n    local.set $sp\n", WASM_STACK_TOP - 256);
    buf_append(&out, "    (block $exit\n      (loop $dispatch\n");
    /* The blocks nest so that a branch out of one lands at the start of the
       next case, which is what makes falling through cost nothing. */
    for (int i = wasm_block_count - 1; i >= 0; i--) {
        buf_appendf(&out, "%*s(block $case%d\n", 8 + (wasm_block_count - 1 - i) * 2, "", i);
    }
    buf_appendf(&out, "%*slocal.get $pc\n", 8 + wasm_block_count * 2, "");
    buf_appendf(&out, "%*sbr_table", 8 + wasm_block_count * 2, "");
    for (int i = 0; i < wasm_block_count; i++) buf_appendf(&out, " $case%d", i);
    buf_append(&out, " $exit\n");
    for (int i = 0; i < wasm_block_count; i++) {
        buf_appendf(&out, "%*s)\n", 8 + (wasm_block_count - 1 - i) * 2, "");
        wasm_resolve_labels(&out, wasm_blocks[i].body.data, i + 1, 0);
    }
    buf_append(&out, "        br $exit\n      )\n    )\n  )\n)\n");
    for (int i = 0; i < wasm_block_count; i++) free(wasm_blocks[i].body.data);
    free(wasm_data.data);
    wasm_active = false;
    return out;
}

/* ---------------------------------------------------------- inline assembly */

/* The operand text a backend uses for a virtual register, which is what an
   inline block interpolates so its assembly can reach the surrounding code.
   NULL when the register has no name on this target - a spilled ARM register,
   for instance, lives in memory the block would have to load from itself. */
static const char *target_register_name(const char *target, int index) {
    const TargetDesc *desc = target_lookup(target);
    if (!desc) return NULL;
    switch (desc->cls) {
        case CLASS_X86_64: return x86_operand_text(index, "q");
        case CLASS_I386: return i386_operand_text(index, "d");
        case CLASS_RISCV: return rv_regs[index];
        case CLASS_MMIX: return mmix_regs[index];
        case CLASS_DCPU: return index < 8 ? dcpu_regs[index] : NULL;
        case CLASS_GENERIC:
            if (desc->flags & TF_AARCH64) return aarch64_regs[index];
            if (desc->flags & TF_ARM32) return index < ARM_MAPPED_COUNT ? arm_regs[index] : NULL;
            if (desc->flags & TF_RV_GENERIC) return rv_regs[index];
            if (desc->flags & TF_IA64) return ia64_regs[index];
            if (desc->flags & TF_LOONG) return loong_regs[index];
            return portable_regs[index];
        default:
            return portable_regs[index];
    }
}

/* Selectors name a target exactly, a family, or every target. "portable" also
   matches every target, but its body is CommonASM rather than assembly, which
   is what makes it usable as the last arm: the fast path is written in the
   machine's own instructions, and everything else falls through to code that
   compiles anywhere. */
static bool asm_selector_is_known(const char *selector) {
    return strcmp(selector, "portable") == 0 ||
           strcmp(selector, "any") == 0 || strcmp(selector, "x86_64") == 0 ||
           strcmp(selector, "i386") == 0 || strcmp(selector, "riscv64") == 0 ||
           strcmp(selector, "aarch64") == 0 || strcmp(selector, "arm32") == 0 ||
           strcmp(selector, "mmix") == 0 || strcmp(selector, "dcpu16") == 0 ||
           is_supported_target(selector);
}

static bool asm_selector_matches(const char *selector, const char *target) {
    if (strcmp(selector, "any") == 0 || strcmp(selector, "portable") == 0) return true;
    if (strcmp(selector, target) == 0) return true;
    if (strcmp(selector, "x86_64") == 0) return target_has_class(target, CLASS_X86_64);
    if (strcmp(selector, "i386") == 0) return target_has_class(target, CLASS_I386);
    if (strcmp(selector, "riscv64") == 0) return target_has_class(target, CLASS_RISCV);
    if (strcmp(selector, "mmix") == 0) return target_has_class(target, CLASS_MMIX);
    if (strcmp(selector, "dcpu16") == 0) return target_has_class(target, CLASS_DCPU);
    if (strcmp(selector, "aarch64") == 0) return target_has_flag(target, TF_AARCH64);
    if (strcmp(selector, "arm32") == 0) return target_has_flag(target, TF_ARM32);
    return false;
}

/* Copies one line of a block through untouched, except that {rN} becomes
   whatever the backend put virtual register N in. Nothing else is parsed:
   that is the point of the construct. */
static void emit_inline_asm_line(Buffer *out, const char *line, const char *target, int line_no) {
    for (const char *p = line; *p; ) {
        const char *close;
        char name[16];
        size_t len;
        int index;
        const char *replacement;
        if (*p != '{') {
            buf_append_char(out, *p++);
            continue;
        }
        close = strchr(p, '}');
        if (!close) {
            line_error(line_no, "asm", "unterminated { in an inline assembly operand");
        }
        len = (size_t)(close - p) - 1;
        if (len == 0 || len >= sizeof(name)) {
            line_error(line_no, "asm", "expected an operand like {r0} in inline assembly");
        }
        memcpy(name, p + 1, len);
        name[len] = '\0';
        index = virtual_reg_index(name);
        if (index < 0) {
            line_error_token(line_no, name, "asm", "expected a virtual register r0-r15 in {}");
        }
        replacement = target_register_name(target, index);
        if (!replacement) {
            line_error_token(line_no, name, "asm",
                             "this target keeps that virtual register in memory, so inline assembly cannot name it");
        }
        buf_append(out, replacement);
        p = close + 1;
    }
    buf_append(out, "\n");
}

/* ------------------------------------------------- inline assembly lifting */

/* With --translate-asm, a block written for a machine other than the one being
   compiled for is lifted back into CommonASM instead of being skipped. That is
   only possible because the block names its registers as {rN}: the operands
   are already the compiler's, so a recognised instruction can be mapped
   straight back to the operation it came from. Anything outside the
   recognised set is reported rather than guessed at. */
typedef enum {
    LIFT_DST_SRC,   /* op d, s        -> cas d, s   */
    LIFT_DST,       /* op d           -> cas d      */
    LIFT_DST_A_B,   /* op d, a, b     -> cas d, b, moving a into d first */
    LIFT_UNARY_2    /* op d, s        -> mov d, s then cas d */
} LiftShape;

typedef struct {
    const char *mnemonic;
    const char *cas_op;
    LiftShape shape;
} LiftRule;

static const LiftRule lift_x86[] = {
    {"mov", "mov", LIFT_DST_SRC}, {"add", "add", LIFT_DST_SRC},
    {"sub", "sub", LIFT_DST_SRC}, {"and", "and", LIFT_DST_SRC},
    {"or", "or", LIFT_DST_SRC}, {"xor", "xor", LIFT_DST_SRC},
    {"cmp", "cmp", LIFT_DST_SRC}, {"imul", "mul", LIFT_DST_SRC},
    {"shl", "shl", LIFT_DST_SRC}, {"shr", "shr", LIFT_DST_SRC},
    {"sar", "sar", LIFT_DST_SRC}, {"rol", "rol", LIFT_DST_SRC},
    {"ror", "ror", LIFT_DST_SRC}, {"neg", "neg", LIFT_DST},
    {"not", "not", LIFT_DST}, {"inc", "inc", LIFT_DST},
    {"dec", "dec", LIFT_DST}, {"bswap", "bswap", LIFT_DST},
    {"push", "push", LIFT_DST}, {"pop", "pop", LIFT_DST},
    {"popcnt", "popcnt", LIFT_UNARY_2}, {"lzcnt", "clz", LIFT_UNARY_2},
    {"tzcnt", "ctz", LIFT_UNARY_2}, {NULL, NULL, LIFT_DST}
};

static const LiftRule lift_a64[] = {
    {"mov", "mov", LIFT_DST_SRC}, {"movz", "mov", LIFT_DST_SRC},{"add", "add", LIFT_DST_A_B},
    {"sub", "sub", LIFT_DST_A_B}, {"and", "and", LIFT_DST_A_B},
    {"orr", "or", LIFT_DST_A_B}, {"eor", "xor", LIFT_DST_A_B},
    {"mul", "mul", LIFT_DST_A_B}, {"sdiv", "div", LIFT_DST_A_B},
    {"lsl", "shl", LIFT_DST_A_B}, {"lsr", "shr", LIFT_DST_A_B},
    {"asr", "sar", LIFT_DST_A_B}, {"ror", "ror", LIFT_DST_A_B},
    {"cmp", "cmp", LIFT_DST_SRC}, {"clz", "clz", LIFT_UNARY_2},
    {"rev", "bswap", LIFT_UNARY_2}, {"neg", "neg", LIFT_UNARY_2},
    {"mvn", "not", LIFT_UNARY_2}, {NULL, NULL, LIFT_DST}
};

static const LiftRule lift_rv[] = {
    {"mv", "mov", LIFT_DST_SRC}, {"li", "mov", LIFT_DST_SRC},
    {"add", "add", LIFT_DST_A_B}, {"addi", "add", LIFT_DST_A_B},
    {"sub", "sub", LIFT_DST_A_B}, {"and", "and", LIFT_DST_A_B},
    {"andi", "and", LIFT_DST_A_B}, {"or", "or", LIFT_DST_A_B},
    {"ori", "or", LIFT_DST_A_B}, {"xor", "xor", LIFT_DST_A_B},
    {"xori", "xor", LIFT_DST_A_B}, {"mul", "mul", LIFT_DST_A_B},
    {"div", "div", LIFT_DST_A_B}, {"rem", "mod", LIFT_DST_A_B},
    {"sll", "shl", LIFT_DST_A_B}, {"slli", "shl", LIFT_DST_A_B},
    {"srl", "shr", LIFT_DST_A_B}, {"srli", "shr", LIFT_DST_A_B},
    {"sra", "sar", LIFT_DST_A_B}, {"srai", "sar", LIFT_DST_A_B},
    {"rol", "rol", LIFT_DST_A_B}, {"ror", "ror", LIFT_DST_A_B},
    {"rori", "ror", LIFT_DST_A_B}, {"cpop", "popcnt", LIFT_UNARY_2},
    {"clz", "clz", LIFT_UNARY_2}, {"ctz", "ctz", LIFT_UNARY_2},
    {"rev8", "bswap", LIFT_UNARY_2}, {"neg", "neg", LIFT_UNARY_2},
    {"not", "not", LIFT_UNARY_2}, {NULL, NULL, LIFT_DST}
};

static const LiftRule lift_mips[] = {
    {"move", "mov", LIFT_DST_SRC}, {"li", "mov", LIFT_DST_SRC},
    {"dli", "mov", LIFT_DST_SRC},
    {"addu", "add", LIFT_DST_A_B}, {"daddu", "add", LIFT_DST_A_B},
    {"addiu", "add", LIFT_DST_A_B}, {"daddiu", "add", LIFT_DST_A_B},
    {"subu", "sub", LIFT_DST_A_B}, {"dsubu", "sub", LIFT_DST_A_B},
    {"and", "and", LIFT_DST_A_B}, {"andi", "and", LIFT_DST_A_B},
    {"or", "or", LIFT_DST_A_B}, {"ori", "or", LIFT_DST_A_B},
    {"xor", "xor", LIFT_DST_A_B}, {"xori", "xor", LIFT_DST_A_B},
    {"mul", "mul", LIFT_DST_A_B}, {"div", "div", LIFT_DST_A_B},
    {"rem", "mod", LIFT_DST_A_B}, {"sll", "shl", LIFT_DST_A_B},
    {"srl", "shr", LIFT_DST_A_B}, {"sra", "sar", LIFT_DST_A_B},
    {"sllv", "shl", LIFT_DST_A_B}, {"srlv", "shr", LIFT_DST_A_B},
    {"srav", "sar", LIFT_DST_A_B}, {"negu", "neg", LIFT_UNARY_2},
    {NULL, NULL, LIFT_DST}
};

static const LiftRule *lift_rules_for(const char *selector, char *comment_out, size_t comment_size) {
    if (strcmp(selector, "x86_64") == 0 || strcmp(selector, "i386") == 0 ||
        target_has_class(selector, CLASS_X86_64) || target_has_class(selector, CLASS_I386)) {
        snprintf(comment_out, comment_size, ";");
        return lift_x86;
    }
    if (strcmp(selector, "aarch64") == 0 || target_has_flag(selector, TF_AARCH64)) {
        snprintf(comment_out, comment_size, "//");
        return lift_a64;
    }
    if (strcmp(selector, "arm32") == 0 || target_has_flag(selector, TF_ARM32)) {
        snprintf(comment_out, comment_size, "@");
        return lift_a64; /* ARM and AArch64 share this instruction shape */
    }
    if (strcmp(selector, "riscv64") == 0 || target_has_class(selector, CLASS_RISCV)) {
        snprintf(comment_out, comment_size, "#");
        return lift_rv;
    }
    if (strcmp(selector, "mips") == 0 || target_has_class(selector, CLASS_MIPS)) {
        snprintf(comment_out, comment_size, "#");
        return lift_mips;
    }
    return NULL;
}

/* The machine registers a family hands to virtual registers, which is what
   lets a plain assembly file be read back: a name in this table is virtual
   register N, and a name outside it is one the compiler never allocates and
   so cannot be reconstructed. */
static const char *const *lift_register_table(const char *family, int *count) {
    if (strcmp(family, "x86_64") == 0 || target_has_class(family, CLASS_X86_64)) {
        *count = X86_MAPPED_COUNT; return x86_regs;
    }
    if (strcmp(family, "i386") == 0 || target_has_class(family, CLASS_I386)) {
        *count = I386_MAPPED_COUNT; return i386_regs;
    }
    if (strcmp(family, "aarch64") == 0 || target_has_flag(family, TF_AARCH64)) {
        *count = 16; return aarch64_regs;
    }
    if (strcmp(family, "arm32") == 0 || target_has_flag(family, TF_ARM32)) {
        *count = ARM_MAPPED_COUNT; return arm_regs;
    }
    if (strcmp(family, "riscv64") == 0 || target_has_class(family, CLASS_RISCV)) {
        *count = 16; return rv_regs;
    }
    if (strcmp(family, "mips") == 0 || target_has_class(family, CLASS_MIPS)) {
        *count = 16; return mips_regs;
    }
    *count = 0;
    return NULL;
}

/* Registers each family uses for its own purposes - frame, scratch, the
   syscall convention. They appear in compiler output but are not virtual
   registers, so an operand naming one cannot be read back and is reported
   rather than mistaken for a symbol. */
static const char *const lift_reserved_x86[] = {
    "rax", "rsp", "rbp", "r11", "eax", "esp", "ebp", "edx", "al", "ax", "cl", NULL
};
static const char *const lift_reserved_a64[] = {
    "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8",
    "x16", "x17", "x18", "x29", "x30", "sp", NULL
};
static const char *const lift_reserved_arm[] = {
    "r3", "r7", "r12", "sp", "lr", "pc", "fp", "ip", NULL
};
static const char *const lift_reserved_rv[] = {
    "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
    "s0", "s10", "s11", "sp", "ra", "zero", NULL
};
static const char *const lift_reserved_mips[] = {
    "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
    "$t8", "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra", NULL
};

/* Set while a whole assembly file is being read back, so the lifting rules
   also accept real register names rather than only {rN} placeholders. */
static const char *const *lift_regs = NULL;
static int lift_reg_count = 0;
static const char *const *lift_reserved = NULL;

/* Every backend materialises a literal it cannot fold into an instruction by
   loading it into a scratch register first. Reading that back one line at a
   time would fail on the scratch, so the value is remembered against the
   register and substituted where the next instruction uses it - which is what
   turns "movz x16, #3" and "mul x19, x19, x16" back into "mul r0, 3". */
#define LIFT_MEMO_MAX 8
static char lift_memo_reg[LIFT_MEMO_MAX][16];
static char lift_memo_val[LIFT_MEMO_MAX][64];
static int lift_memo_count = 0;

static const char *lift_memo_lookup(const char *reg) {
    for (int i = 0; i < lift_memo_count; i++) {
        if (strcmp(lift_memo_reg[i], reg) == 0) return lift_memo_val[i];
    }
    return NULL;
}

static void lift_memo_set(const char *reg, const char *value) {
    for (int i = 0; i < lift_memo_count; i++) {
        if (strcmp(lift_memo_reg[i], reg) == 0) {
            snprintf(lift_memo_val[i], sizeof(lift_memo_val[i]), "%s", value);
            return;
        }
    }
    if (lift_memo_count == LIFT_MEMO_MAX) return;
    snprintf(lift_memo_reg[lift_memo_count], sizeof(lift_memo_reg[0]), "%s", reg);
    snprintf(lift_memo_val[lift_memo_count], sizeof(lift_memo_val[0]), "%s", value);
    lift_memo_count++;
}

static bool lift_is_reserved(const char *name) {
    for (int i = 0; lift_reserved && lift_reserved[i]; i++) {
        if (strcmp(lift_reserved[i], name) == 0) return true;
    }
    return false;
}

/* An operand a lifted instruction can use: a {rN} placeholder, a machine
   register the compiler allocates, or a literal in any of the spellings these
   assemblers accept. */
static bool lift_operand(const char *text, char *out, size_t out_size) {
    size_t len = strlen(text);
    if (len >= 4 && text[0] == '{' && text[len - 1] == '}') {
        char name[16];
        if (len - 2 >= sizeof(name)) return false;
        memcpy(name, text + 1, len - 2);
        name[len - 2] = '\0';
        if (virtual_reg_index(name) < 0) return false;
        snprintf(out, out_size, "%s", name);
        return true;
    }
    for (int i = 0; i < lift_reg_count; i++) {
        if (strcmp(lift_regs[i], text) == 0) {
            snprintf(out, out_size, "r%d", i);
            return true;
        }
    }
    {
        const char *remembered = lift_memo_lookup(text);
        if (remembered) {
            snprintf(out, out_size, "%s", remembered);
            return true;
        }
    }
    if (lift_is_reserved(text)) return false;
    if (text[0] == '#' || text[0] == '$') text++;
    if (!is_int(text) && !(lift_regs && is_symbol(text))) return false;
    snprintf(out, out_size, "%s", text);
    return true;
}

typedef struct {
    const char *mnemonic;
    const char *cas_op;
} BranchRule;

static const BranchRule branch_x86[] = {
    {"je", "je"}, {"jne", "jne"}, {"jg", "jg"}, {"jl", "jl"}, {"jge", "jge"},
    {"jle", "jle"}, {"ja", "ja"}, {"jb", "jb"}, {"jae", "jae"}, {"jbe", "jbe"},
    {"jmp", "jmp"}, {"call", "call"}, {NULL, NULL}
};
static const BranchRule branch_a64[] = {
    {"b.eq", "je"}, {"b.ne", "jne"}, {"b.gt", "jg"}, {"b.lt", "jl"},
    {"b.ge", "jge"}, {"b.le", "jle"}, {"b.hi", "ja"}, {"b.lo", "jb"},
    {"b.hs", "jae"}, {"b.ls", "jbe"}, {"b", "jmp"}, {"bl", "call"}, {NULL, NULL}
};
static const BranchRule branch_arm[] = {
    {"beq", "je"}, {"bne", "jne"}, {"bgt", "jg"}, {"blt", "jl"},
    {"bge", "jge"}, {"ble", "jle"}, {"bhi", "ja"}, {"blo", "jb"},
    {"bhs", "jae"}, {"bls", "jbe"}, {"b", "jmp"}, {"bl", "call"}, {NULL, NULL}
};
/* RISC-V and MIPS fuse the comparison into the branch, so reading one back
   yields the compare the source had before the backend folded it. */
static const BranchRule branch_fused[] = {
    {"beq", "je"}, {"bne", "jne"}, {"bgt", "jg"}, {"blt", "jl"},
    {"bge", "jge"}, {"ble", "jle"}, {"bgtu", "ja"}, {"bltu", "jb"},
    {"bgeu", "jae"}, {"bleu", "jbe"}, {NULL, NULL}
};

static const BranchRule *lift_branch_table(const char *family, bool *fused) {
    *fused = false;
    if (strcmp(family, "x86_64") == 0 || strcmp(family, "i386") == 0 ||
        target_has_class(family, CLASS_X86_64) || target_has_class(family, CLASS_I386)) {
        return branch_x86;
    }
    if (strcmp(family, "aarch64") == 0 || target_has_flag(family, TF_AARCH64)) return branch_a64;
    if (strcmp(family, "arm32") == 0 || target_has_flag(family, TF_ARM32)) return branch_arm;
    *fused = true;
    return branch_fused;
}

/* A virtual register the lifted sequence can borrow, avoiding up to three
   that are already in play. */
static int lift_pick_scratch(const char *target, const char *a, const char *b, const char *c) {
    for (int i = target_max_vreg(target); i >= 0; i--) {
        char name[8];
        snprintf(name, sizeof(name), "r%d", i);
        if (a && strcmp(name, a) == 0) continue;
        if (b && strcmp(name, b) == 0) continue;
        if (c && strcmp(name, c) == 0) continue;
        return i;
    }
    return -1;
}

/* lea computes an address instead of loading from one, which makes it the x86
   way to spell a small arithmetic expression, so it is worth carrying over.
   The forms it takes are a base, an optional scaled index and a constant. */
static bool lift_lea(Buffer *text, char **args, int argc, const char *target, int line_no) {
    char expr[256];
    char dst[32];
    char base[32] = {0};
    char index[32] = {0};
    long long scale = 1;
    long long disp = 0;
    bool have_base = false;
    bool have_index = false;
    char *cursor;
    size_t len;
    int scratch;
    if (argc != 2 || !lift_operand(args[0], dst, sizeof(dst))) return false;
    len = strlen(args[1]);
    if (len < 3 || args[1][0] != '[' || args[1][len - 1] != ']' || len - 2 >= sizeof(expr)) return false;
    memcpy(expr, args[1] + 1, len - 2);
    expr[len - 2] = '\0';
    cursor = expr;
    while (*cursor) {
        char term[64];
        char *plus = strchr(cursor, '+');
        char *star;
        char *piece;
        size_t term_len = plus ? (size_t)(plus - cursor) : strlen(cursor);
        if (term_len >= sizeof(term)) return false;
        memcpy(term, cursor, term_len);
        term[term_len] = '\0';
        cursor = plus ? plus + 1 : cursor + strlen(cursor);
        piece = trim(term);
        if (!*piece) continue;
        star = strchr(piece, '*');
        if (star) {
            char reg[16];
            char *amount;
            *star = '\0';
            amount = trim(star + 1);
            if (!lift_operand(trim(piece), reg, sizeof(reg)) || virtual_reg_index(reg) < 0) return false;
            if (!is_int(amount)) return false;
            scale = strtoll(amount, NULL, 0);
            snprintf(index, sizeof(index), "%s", reg);
            have_index = true;
        } else {
            char value[32];
            if (!lift_operand(piece, value, sizeof(value))) return false;
            if (virtual_reg_index(value) >= 0) {
                if (!have_base) { snprintf(base, sizeof(base), "%s", value); have_base = true; }
                else if (!have_index) { snprintf(index, sizeof(index), "%s", value); have_index = true; }
                else return false;
            } else {
                disp = strtoll(value, NULL, 0);
            }
        }
    }
    if (!have_base && !have_index) return false;
    scratch = lift_pick_scratch(target, dst, base, index);
    if (scratch < 0) return false;
    /* Built in a borrowed register so that the destination can also be one of
       the inputs, which is the usual case. */
    emit_cas(text, target, line_no, "push r%d", scratch);
    if (have_index) {
        emit_cas(text, target, line_no, "mov r%d, %s", scratch, index);
        if (scale != 1) emit_cas(text, target, line_no, "mul r%d, %lld", scratch, scale);
        if (have_base) emit_cas(text, target, line_no, "add r%d, %s", scratch, base);
    } else {
        emit_cas(text, target, line_no, "mov r%d, %s", scratch, base);
    }
    if (disp != 0) emit_cas(text, target, line_no, "add r%d, %lld", scratch, disp);
    emit_cas(text, target, line_no, "mov %s, r%d", dst, scratch);
    emit_cas(text, target, line_no, "pop r%d", scratch);
    return true;
}

static void lift_line(Buffer *text, const LiftRule *rules, const char *selector,
                      char *line, const char *target, int line_no) {
    char *space;
    char *args[8];
    char ops[4][64];
    int argc = 0;
    size_t len = strlen(line);
    if (len == 0) return;
    if (line[len - 1] == ':') {
        lift_memo_count = 0;
        emit_cas(text, target, line_no, "%s", line);
        return;
    }
    space = line;
    while (*space && !isspace((unsigned char)*space)) space++;
    if (*space) {
        *space++ = '\0';
        argc = split_args(space, args, 8);
    }
    /* A literal being parked in a scratch register is remembered rather than
       emitted; the instruction that reads it back gets the literal instead.
       This has to come first, because the scratch is not a virtual register
       and every other rule would refuse it. */
    if (lift_regs && argc >= 2 && lift_is_reserved(args[0])) {
        const char *literal = args[1];
        bool is_move = strcmp(line, "mov") == 0 || strcmp(line, "movz") == 0 ||
                       strcmp(line, "li") == 0 || strcmp(line, "dli") == 0 ||
                       strcmp(line, "move") == 0 || strcmp(line, "la") == 0;
        if (strcmp(line, "ldr") == 0 && literal[0] == '=') {
            is_move = true;
            literal++;
        }
        if (literal[0] == '#' || literal[0] == '$') literal++;
        if (is_move && argc == 2 && (is_int(literal) || is_symbol(literal))) {
            lift_memo_set(args[0], literal);
            return;
        }
        /* movk fills in a further halfword of a value movz started. */
        if (strcmp(line, "movk") == 0 && argc == 3) {
            const char *known = lift_memo_lookup(args[0]);
            const char *part = args[1];
            const char *shift = strchr(args[2], '#');
            if (known && shift && is_int(known)) {
                char combined[64];
                unsigned long long base = strtoull(known, NULL, 0);
                if (part[0] == '#') part++;
                base |= strtoull(part, NULL, 0) << strtoull(shift + 1, NULL, 0);
                snprintf(combined, sizeof(combined), "%llu", base);
                lift_memo_set(args[0], combined);
                return;
            }
        }
    }
    /* Shapes that stand for an operation rather than being one: ARM negates
       by reverse-subtracting from zero and loads a wide literal through the
       pool, and MIPS inverts by NOR-ing against the zero register. */
    if (strcmp(line, "rsb") == 0 && argc == 3 &&
        lift_operand(args[0], ops[0], sizeof(ops[0])) &&
        lift_operand(args[1], ops[1], sizeof(ops[1]))) {
        const char *zero = args[2];
        if (zero[0] == '#') zero++;
        if (strcmp(zero, "0") == 0) {
            if (strcmp(ops[0], ops[1]) != 0) emit_cas(text, target, line_no, "mov %s, %s", ops[0], ops[1]);
            emit_cas(text, target, line_no, "neg %s", ops[0]);
            return;
        }
    }
    if (strcmp(line, "nor") == 0 && argc == 3 && strcmp(args[2], "$zero") == 0 &&
        lift_operand(args[0], ops[0], sizeof(ops[0])) &&
        lift_operand(args[1], ops[1], sizeof(ops[1]))) {
        if (strcmp(ops[0], ops[1]) != 0) emit_cas(text, target, line_no, "mov %s, %s", ops[0], ops[1]);
        emit_cas(text, target, line_no, "not %s", ops[0]);
        return;
    }
    if (strcmp(line, "ldr") == 0 && argc == 2 && args[1][0] == '=') {
        char value[64];
        if (lift_operand(args[1] + 1, value, sizeof(value)) &&
            lift_operand(args[0], ops[0], sizeof(ops[0]))) {
            emit_cas(text, target, line_no, "mov %s, %s", ops[0], value);
            return;
        }
    }
    /* Returns, unconditional jumps and calls, spelled differently everywhere. */
    if (strcmp(line, "ret") == 0 || strcmp(line, "blr") == 0 ||
        (strcmp(line, "bx") == 0 && argc == 1 && strcmp(args[0], "lr") == 0) ||
        (strcmp(line, "jr") == 0 && argc == 1 && strcmp(args[0], "$ra") == 0)) {
        emit_cas(text, target, line_no, "ret");
        return;
    }
    if ((strcmp(line, "j") == 0 || strcmp(line, "jal") == 0) && argc == 1) {
        emit_cas(text, target, line_no, "%s %s", strcmp(line, "j") == 0 ? "jmp" : "call", args[0]);
        return;
    }
    {
        bool fused = false;
        const BranchRule *branches = lift_branch_table(selector, &fused);
        for (const BranchRule *b = branches; b->mnemonic; b++) {
            if (strcmp(b->mnemonic, line) != 0) continue;
            if (fused && argc == 3) {
                /* The compare the backend folded in comes back out. */
                if (lift_operand(args[0], ops[0], sizeof(ops[0])) &&
                    lift_operand(args[1], ops[1], sizeof(ops[1]))) {
                    emit_cas(text, target, line_no, "cmp %s, %s", ops[0], ops[1]);
                    emit_cas(text, target, line_no, "%s %s", b->cas_op, args[2]);
                    return;
                }
                break;
            }
            if (!fused && argc == 1) {
                emit_cas(text, target, line_no, "%s %s", b->cas_op, args[0]);
                return;
            }
            break;
        }
    }
    if (rules == lift_x86 && strcmp(line, "lea") == 0 &&
        lift_lea(text, args, argc, target, line_no)) {
        return;
    }
    for (const LiftRule *rule = rules; rule->mnemonic; rule++) {
        if (strcmp(rule->mnemonic, line) != 0) continue;
        if (rule->shape == LIFT_DST && argc == 1 && lift_operand(args[0], ops[0], sizeof(ops[0]))) {
            emit_cas(text, target, line_no, "%s %s", rule->cas_op, ops[0]);
            return;
        }
        if ((rule->shape == LIFT_DST_SRC || rule->shape == LIFT_UNARY_2) && argc == 2 &&
            lift_operand(args[0], ops[0], sizeof(ops[0])) &&
            lift_operand(args[1], ops[1], sizeof(ops[1]))) {
            if (rule->shape == LIFT_UNARY_2) {
                if (strcmp(ops[0], ops[1]) != 0) {
                    emit_cas(text, target, line_no, "mov %s, %s", ops[0], ops[1]);
                }
                emit_cas(text, target, line_no, "%s %s", rule->cas_op, ops[0]);
            } else {
                emit_cas(text, target, line_no, "%s %s, %s", rule->cas_op, ops[0], ops[1]);
            }
            return;
        }
        if (rule->shape == LIFT_DST_A_B && argc >= 3 && argc <= 4 &&
            lift_operand(args[0], ops[0], sizeof(ops[0])) &&
            lift_operand(args[1], ops[1], sizeof(ops[1])) &&
            lift_operand(args[2], ops[2], sizeof(ops[2]))) {
            /* A trailing "lsl #n" turns the third operand into a shifted one,
               which is common enough on ARM to be worth carrying over. */
            const char *shift_op = NULL;
            char shift_amount[32] = {0};
            int scratch_a, scratch_b;
            if (argc == 4) {
                char kind[16];
                const char *rest = args[3];
                size_t i = 0;
                while (rest[i] && !isspace((unsigned char)rest[i]) && i + 1 < sizeof(kind)) {
                    kind[i] = rest[i];
                    i++;
                }
                kind[i] = '\0';
                while (rest[i] && isspace((unsigned char)rest[i])) i++;
                if (!lift_operand(rest + i, shift_amount, sizeof(shift_amount))) break;
                if (strcmp(kind, "lsl") == 0) shift_op = "shl";
                else if (strcmp(kind, "lsr") == 0) shift_op = "shr";
                else if (strcmp(kind, "asr") == 0) shift_op = "sar";
                else break;
            }
            /* Some backends write the operands the other way round - ARM's
               multiply requires it before ARMv6 - so for an operation where
               that makes no difference, the shape is normalised first. */
            if (!shift_op && strcmp(ops[0], ops[2]) == 0 && strcmp(ops[0], ops[1]) != 0 &&
                (strcmp(rule->cas_op, "add") == 0 || strcmp(rule->cas_op, "and") == 0 ||
                 strcmp(rule->cas_op, "or") == 0 || strcmp(rule->cas_op, "xor") == 0 ||
                 strcmp(rule->cas_op, "mul") == 0)) {
                char swap[64];
                snprintf(swap, sizeof(swap), "%s", ops[1]);
                snprintf(ops[1], sizeof(ops[1]), "%s", ops[2]);
                snprintf(ops[2], sizeof(ops[2]), "%s", swap);
            }
            /* d = a op b has to become a two-address sequence. When d is
               already a, or when writing a into d cannot disturb b, no
               register has to be borrowed - which is the usual case. */
            if (!shift_op && strcmp(ops[0], ops[1]) == 0) {
                emit_cas(text, target, line_no, "%s %s, %s", rule->cas_op, ops[0], ops[2]);
                return;
            }
            if (!shift_op && strcmp(ops[0], ops[2]) != 0) {
                emit_cas(text, target, line_no, "mov %s, %s", ops[0], ops[1]);
                emit_cas(text, target, line_no, "%s %s, %s", rule->cas_op, ops[0], ops[2]);
                return;
            }
            ext_pick_scratch(target, ops[0], ops[1], &scratch_a, &scratch_b);
            emit_cas(text, target, line_no, "push r%d", scratch_a);
            emit_cas(text, target, line_no, "mov r%d, %s", scratch_a, ops[2]);
            if (shift_op) {
                emit_cas(text, target, line_no, "%s r%d, %s", shift_op, scratch_a, shift_amount);
            }
            emit_cas(text, target, line_no, "mov %s, %s", ops[0], ops[1]);
            emit_cas(text, target, line_no, "%s %s, r%d", rule->cas_op, ops[0], scratch_a);
            emit_cas(text, target, line_no, "pop r%d", scratch_a);
            return;
        }
        break;
    }
    line_error_token(line_no, line, "asm",
                     "--translate-asm does not recognise this instruction; give the run a portable arm instead");
    (void)selector;
}

/* Lifts a whole block. Returns false when the block's own family has no
   lifting rules at all. */
static bool translate_asm_block(Buffer *text, const char *selector, const char *body,
                                const char *target, int line_no) {
    char comment[8];
    const LiftRule *rules = lift_rules_for(selector, comment, sizeof(comment));
    const char *cursor = body;
    if (!rules) return false;
    while (*cursor) {
        char work[512];
        const char *newline = strchr(cursor, '\n');
        size_t len = newline ? (size_t)(newline - cursor) : strlen(cursor);
        char *trimmed;
        char *mark;
        if (len >= sizeof(work)) len = sizeof(work) - 1;
        memcpy(work, cursor, len);
        work[len] = '\0';
        cursor = newline ? newline + 1 : cursor + strlen(cursor);
        /* The source family decides what a comment looks like; '#' is an
           immediate in ARM assembly, not the start of one. */
        mark = strstr(work, comment);
        if (mark) *mark = '\0';
        trimmed = trim(work);
        if (*trimmed) lift_line(text, rules, selector, trimmed, target, line_no);
    }
    return true;
}

/* ------------------------------------------- reading a whole assembly file */

/* Maps one directive of a compiler-emitted assembly file back to the
   CommonASM it came from. Returns false when the line is not a directive. */
static bool cas_from_directive(Buffer *out, char *line) {
    char *colon;
    char *rest;
    /* Things that describe the output format rather than the program. */
    if (strncmp(line, "default rel", 11) == 0 || strncmp(line, ".option", 7) == 0 ||
        strncmp(line, ".syntax", 7) == 0 || strcmp(line, ".arm") == 0 ||
        strcmp(line, ".thumb") == 0 || strncmp(line, "alignb", 6) == 0) {
        return true;
    }
    if (strncmp(line, "section ", 8) == 0 || strncmp(line, ".section ", 9) == 0) {
        const char *name = strchr(line, '.');
        if (name && strchr(name + 1, '.')) name = strchr(name + 1, '.');
        buf_appendf(out, "%s\n", name ? name : ".text");
        return true;
    }
    if (strcmp(line, ".text") == 0 || strcmp(line, ".data") == 0 ||
        strcmp(line, ".rodata") == 0 || strcmp(line, ".bss") == 0) {
        buf_appendf(out, "%s\n", line);
        return true;
    }
    if (strncmp(line, "global ", 7) == 0 || strncmp(line, ".globl ", 7) == 0) {
        buf_appendf(out, "global %s\n", trim(line + 7));
        return true;
    }
    if (strncmp(line, "extern ", 7) == 0 || strncmp(line, ".extern ", 8) == 0) {
        buf_appendf(out, "extern %s\n", trim(line + (line[0] == '.' ? 8 : 7)));
        return true;
    }
    if (strncmp(line, ".equ ", 5) == 0) {
        char *comma = strchr(line, ',');
        if (!comma) return false;
        *comma = '\0';
        buf_appendf(out, "const %s = %s\n", trim(line + 5), trim(comma + 1));
        return true;
    }
    if (strncmp(line, "align ", 6) == 0 || strncmp(line, ".balign ", 8) == 0) {
        buf_appendf(out, "align %s\n", trim(strchr(line, ' ')));
        return true;
    }
    /* "NAME equ VALUE" has no leading marker, so it is checked by shape. */
    rest = strstr(line, " equ ");
    if (rest && !strchr(line, ':')) {
        *rest = '\0';
        buf_appendf(out, "const %s = %s\n", trim(line), trim(rest + 5));
        return true;
    }
    colon = strchr(line, ':');
    if (colon) {
        char *body;
        *colon = '\0';
        body = trim(colon + 1);
        if (*body == '\0') {
            buf_appendf(out, "%s:\n", trim(line));
            return true;
        }
        {
            static const struct { const char *asm_dir; const char *cas_dir; } data_dirs[] = {
                {"db", "bytes"}, {"dw", "word"}, {"dd", "dword"}, {"dq", "qword"},
                {".byte", "bytes"}, {".word", "word"}, {".long", "dword"},
                {".quad", "qword"}, {".zero", "zero"}, {"resb", "zero"}, {NULL, NULL}
            };
            for (int i = 0; data_dirs[i].asm_dir; i++) {
                size_t len = strlen(data_dirs[i].asm_dir);
                if (strncmp(body, data_dirs[i].asm_dir, len) == 0 && isspace((unsigned char)body[len])) {
                    buf_appendf(out, "%s: %s %s\n", trim(line), data_dirs[i].cas_dir, trim(body + len));
                    return true;
                }
            }
            /* A reservation counted in words becomes the same number of bytes. */
            if ((strncmp(body, "resq", 4) == 0 || strncmp(body, "resd", 4) == 0 ||
                 strncmp(body, "resw", 4) == 0) && isspace((unsigned char)body[4])) {
                long long units = strtoll(trim(body + 4), NULL, 0);
                int scale = body[3] == 'q' ? 8 : body[3] == 'd' ? 4 : 2;
                buf_appendf(out, "%s: zero %lld\n", trim(line), units * scale);
                return true;
            }
            if (strncmp(body, "times ", 6) == 0) {
                buf_appendf(out, "%s: zero %s\n", trim(line), trim(strtok(trim(body + 6), " \t")));
                return true;
            }
        }
        return false;
    }
    return false;
}

/* Reads a whole assembly file back into the CommonASM it came from, so it can
   be compiled for a different machine. This works on assembly written in the
   registers the compiler allocates; a sequence that stands for a CommonASM
   construct rather than an instruction - a syscall, a prologue, spill traffic
   - is reported rather than guessed at. */
static Buffer reconstruct_cas(const char *source, const char *family) {
    Buffer out;
    char comment[8];
    const LiftRule *rules = lift_rules_for(family, comment, sizeof(comment));
    const char *cursor = source;
    int line_no = 0;
    if (!rules) {
        die("--from needs one of x86_64, i386, aarch64, arm32, riscv64, mips");
    }
    lift_regs = lift_register_table(family, &lift_reg_count);
    lift_reserved = rules == lift_x86 ? lift_reserved_x86 :
                    rules == lift_a64 ? (strcmp(comment, "@") == 0 ? lift_reserved_arm : lift_reserved_a64) :
                    rules == lift_rv ? lift_reserved_rv : lift_reserved_mips;
    buf_init(&out);
    cas_sink = &out;
    while (*cursor) {
        char work[1024];
        const char *newline = strchr(cursor, '\n');
        size_t len = newline ? (size_t)(newline - cursor) : strlen(cursor);
        char *trimmed;
        char *mark;
        line_no++;
        if (len >= sizeof(work)) len = sizeof(work) - 1;
        memcpy(work, cursor, len);
        work[len] = '\0';
        cursor = newline ? newline + 1 : cursor + strlen(cursor);
        mark = strstr(work, comment);
        if (mark) *mark = '\0';
        trimmed = trim(work);
        if (!*trimmed) continue;
        if (cas_from_directive(&out, trimmed)) continue;
        lift_line(NULL, rules, family, trimmed, family, line_no);
    }
    cas_sink = NULL;
    lift_regs = NULL;
    lift_reg_count = 0;
    lift_reserved = NULL;
    return out;
}

static void emit_text_line(Buffer *text, char *line, int line_no, const char *target) {
    char *space;
    char *args[16];
    int argc = 0;
    char base_op[64];
    const char *size;
    size_t len = strlen(line);
    /* A label or a symbol directive ends the straight-line run a pending
       RISC-V comparison was counting on. */
    if (is_riscv_target(target) &&
        ((len > 0 && line[len - 1] == ':') ||
         strncmp(line, "global ", 7) == 0 || strncmp(line, "extern ", 7) == 0)) {
        compare_discard(&rv_cmp);
    }
    if (len > 0 && line[len - 1] == ':') {
        line[len - 1] = '\0';
        if (is_wasm_target(target)) { wasm_open_block(line); return; }
        if (strcmp(target, "mmixal") == 0) buf_appendf(text, "%s\n", line);
        else if (strcmp(target, "dcpu16") == 0) buf_appendf(text, ":%s\n", line);
        else buf_appendf(text, "%s:\n", line);
        return;
    }
    if (strncmp(line, "global ", 7) == 0) {
        const char *name = trim(line + 7);
        if (is_wasm_target(target)) return;
        if (strcmp(target, "x86_64-nasm") == 0) buf_appendf(text, "global %s\n", name);
        else if (is_riscv_target(target) || is_generic_arch_target(target) || is_legacy_arch_target(target) || is_mips_target(target) || is_ppc_target(target) || is_sparc_target(target) || is_m68k_target(target) || is_s390_target(target) || is_vm_ir_target(target)) buf_appendf(text, ".globl %s\n", name);
        else if (is_i386_target(target)) buf_appendf(text, "global %s\n", name);
        else if (strcmp(target, "mmixal") == 0) buf_appendf(text, "%s IS @\n", name);
        else if (strcmp(target, "dcpu16") == 0) buf_appendf(text, "; global %s\n", name);
        else if (is_toy_target(target)) buf_appendf(text, "; global %s\n", name);
        return;
    }
    if (strncmp(line, "extern ", 7) == 0) {
        const char *name = trim(line + 7);
        if (is_wasm_target(target)) return;
        if (strcmp(target, "x86_64-nasm") == 0) buf_appendf(text, "extern %s\n", name);
        else if (is_riscv_target(target) || is_generic_arch_target(target) || is_legacy_arch_target(target) || is_mips_target(target) || is_ppc_target(target) || is_sparc_target(target) || is_m68k_target(target) || is_s390_target(target) || is_vm_ir_target(target)) buf_appendf(text, ".extern %s\n", name);
        else if (is_i386_target(target)) buf_appendf(text, "extern %s\n", name);
        else if (strcmp(target, "mmixal") == 0) buf_appendf(text, "        %% extern %s\n", name);
        else if (strcmp(target, "dcpu16") == 0) buf_appendf(text, "        ; extern %s\n", name);
        else if (is_toy_target(target)) buf_appendf(text, "        ; extern %s\n", name);
        return;
    }
    space = line;
    while (*space && !isspace((unsigned char)*space)) space++;
    if (*space) {
        *space++ = '\0';
        argc = split_args(space, args, 16);
    }
    size = size_suffix_or_default(line, base_op, sizeof(base_op));
    if (!size) {
        line_error(line_no, line, "unknown size suffix");
    }
    /* A 32-bit machine cannot hold a wider immediate. Catching it here gives
       a diagnostic pointing at the source line instead of an assembler error
       about a file the user did not write. */
    if (target_word_bits(target) < 64) {
        for (int i = 0; i < argc; i++) {
            char *end = NULL;
            long long parsed;
            if (!is_int(args[i])) continue;
            errno = 0;
            parsed = strtoll(args[i], &end, 0);
            if (errno != 0 || parsed > 4294967295LL || parsed < -2147483647LL - 1) {
                line_error_token(line_no, args[i], base_op,
                                 "immediate does not fit this target's 32-bit word");
            }
        }
    }
    /* An extended operation the target has no instruction for is expanded
       into ordinary CommonASM here, before any backend sees it. */
    if (emit_extended_fallback(text, target, base_op, args, argc, line_no)) return;
    if (strcmp(target, "x86_64-nasm") == 0) emit_x86_instruction(text, base_op, size, args, argc, line_no);
    else if (is_riscv_target(target)) emit_rv_instruction(text, target, base_op, size, args, argc, line_no);
    else if (strcmp(target, "mmixal") == 0) emit_mmix_instruction(text, base_op, size, args, argc, line_no);
    else if (strcmp(target, "dcpu16") == 0) emit_dcpu_instruction(text, base_op, size, args, argc, line_no);
    else if (is_aarch64_target(target)) emit_a64_instruction(text, base_op, size, args, argc, line_no);
    else if (is_i386_target(target)) emit_i386_instruction(text, base_op, size, args, argc, line_no);
    else if (is_wasm_target(target)) emit_wasm_instruction(base_op, size, args, argc, line_no);
    else if (is_s390_target(target)) emit_s390_instruction(text, base_op, size, args, argc, line_no);
    else if (is_sparc_target(target)) emit_sparc_instruction(text, target, base_op, size, args, argc, line_no);
    else if (is_m68k_target(target)) emit_m68k_instruction(text, base_op, size, args, argc, line_no);
    else if (is_ppc_target(target)) emit_ppc_instruction(text, target, base_op, size, args, argc, line_no);
    else if (is_mips_target(target)) emit_mips_instruction(text, target, base_op, size, args, argc, line_no);
    else if (is_arm32_target(target)) emit_arm_instruction(text, base_op, size, args, argc, line_no);
    else if (is_generic_arch_target(target)) emit_generic_instruction(text, target, base_op, size, args, argc, line_no);
    else if (is_legacy_arch_target(target)) emit_arch_instruction(text, target, base_op, size, args, argc, line_no);
    else if (is_vm_ir_target(target)) emit_vm_ir_instruction(text, target, base_op, size, args, argc, line_no);
    else if (is_toy_target(target)) emit_encoded_or_toy_instruction(text, target, base_op, size, args, argc, line_no);
    else line_error(line_no, base_op, "unknown instruction target");
}

typedef struct {
    char base_op[64];
    char args[3][128];
    int argc;
    bool label;
    bool directive;
} OptInstruction;

typedef struct {
    bool enabled;
    bool has_pending;
    char pending[512];
    int pending_line_no;
    /* A pending line is always a "mov <vreg>, <immediate>", so its parsed form
       travels with the text. Recovering it by re-parsing on every following
       instruction was the single largest cost of -O1. */
    char pending_reg[128];
    long long pending_value;
} Optimizer;

/* emit_text_line() rewrites the line it is given in place, so it needs a
   mutable copy. The copy lives in a buffer reused across lines rather than
   being allocated and freed once per line. */
static char *line_scratch = NULL;
static size_t line_scratch_cap = 0;

static void emit_text_line_copy(Buffer *text, const char *line, int line_no, const char *target) {
    size_t len = strlen(line);
    if (len + 1 > line_scratch_cap) {
        size_t cap = line_scratch_cap ? line_scratch_cap : 256;
        while (cap < len + 1) cap *= 2;
        free(line_scratch);
        line_scratch = xmalloc(cap);
        line_scratch_cap = cap;
    }
    memcpy(line_scratch, line, len + 1);
    emit_text_line(text, line_scratch, line_no, target);
}

static bool opt_parse_long(const char *text, long long *value) {
    char *end = NULL;
    errno = 0;
    *value = strtoll(text, &end, 0);
    return errno == 0 && end && *end == '\0' && end != text;
}

static bool opt_parse_instruction(const char *line, OptInstruction *out) {
    char tmp[1024];
    char *space;
    char *args[16];
    char *work;
    size_t len = strlen(line);
    memset(out, 0, sizeof(*out));
    if (len >= sizeof(tmp)) return false;
    memcpy(tmp, line, len + 1);
    work = trim(tmp);
    len = strlen(work);
    if (len == 0) return false;
    if (work[len - 1] == ':') {
        out->label = true;
        return true;
    }
    if (strncmp(work, "global ", 7) == 0 || strncmp(work, "extern ", 7) == 0) {
        out->directive = true;
        return true;
    }
    space = work;
    while (*space && !isspace((unsigned char)*space)) space++;
    if (*space) {
        *space++ = '\0';
        out->argc = split_args(space, args, 16);
    }
    if (!size_suffix_or_default(work, out->base_op, sizeof(out->base_op))) {
        return false;
    }
    if (out->argc > 3) return false;
    for (int i = 0; i < out->argc; i++) {
        size_t arg_len = strlen(args[i]);
        if (arg_len >= sizeof(out->args[i])) return false;
        memcpy(out->args[i], args[i], arg_len + 1);
    }
    return true;
}

static bool opt_is_mov_imm(const OptInstruction *ins, char *reg, long long *value) {
    long long parsed;
    if (ins->label || ins->directive || strcmp(ins->base_op, "mov") != 0 || ins->argc != 2) return false;
    if (virtual_reg_index(ins->args[0]) < 0 || !opt_parse_long(ins->args[1], &parsed)) return false;
    if (reg) memcpy(reg, ins->args[0], strlen(ins->args[0]) + 1);
    if (value) *value = parsed;
    return true;
}

static bool opt_set_pending(Optimizer *opt, const char *line, int line_no, const char *reg, long long value) {
    if (strlen(line) >= sizeof(opt->pending)) return false;
    snprintf(opt->pending, sizeof(opt->pending), "%s", line);
    snprintf(opt->pending_reg, sizeof(opt->pending_reg), "%s", reg);
    opt->pending_value = value;
    opt->pending_line_no = line_no;
    opt->has_pending = true;
    return true;
}

static void opt_flush(Buffer *text, const char *target, Optimizer *opt) {
    if (!opt->has_pending) return;
    emit_text_line_copy(text, opt->pending, opt->pending_line_no, target);
    opt->has_pending = false;
}

static bool opt_safe_add(long long a, long long b, long long *out) {
    if ((b > 0 && a > LLONG_MAX - b) || (b < 0 && a < LLONG_MIN - b)) return false;
    *out = a + b;
    return true;
}

static bool opt_safe_sub(long long a, long long b, long long *out) {
    if ((b < 0 && a > LLONG_MAX + b) || (b > 0 && a < LLONG_MIN + b)) return false;
    *out = a - b;
    return true;
}

static bool opt_safe_mul(long long a, long long b, long long *out) {
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    if (a == -1 && b == LLONG_MIN) return false;
    if (b == -1 && a == LLONG_MIN) return false;
    if (a > 0) {
        if (b > 0 && a > LLONG_MAX / b) return false;
        if (b < 0 && b < LLONG_MIN / a) return false;
    } else {
        if (b > 0 && a < LLONG_MIN / b) return false;
        if (b < 0 && a < LLONG_MAX / b) return false;
    }
    *out = a * b;
    return true;
}

static bool opt_compute_binary(const char *op, long long lhs, long long rhs, long long *result) {
    const long long bits = (long long)(sizeof(long long) * CHAR_BIT);
    if (op_is(op, "add")) return opt_safe_add(lhs, rhs, result);
    if (op_is(op, "sub")) return opt_safe_sub(lhs, rhs, result);
    if (op_is(op, "mul")) return opt_safe_mul(lhs, rhs, result);
    if (op_is(op, "div")) {
        if (rhs == 0 || (lhs == LLONG_MIN && rhs == -1)) return false;
        *result = lhs / rhs;
        return true;
    }
    if (op_is(op, "mod")) {
        if (rhs == 0 || (lhs == LLONG_MIN && rhs == -1)) return false;
        *result = lhs % rhs;
        return true;
    }
    if (op_is(op, "and") || op_is(op, "or") || op_is(op, "xor")) {
        if (lhs < 0 || rhs < 0) return false;
        if (op_is(op, "and")) *result = lhs & rhs;
        else if (op_is(op, "or")) *result = lhs | rhs;
        else *result = lhs ^ rhs;
        return true;
    }
    if (op_is(op, "shl") || op_is(op, "shr") || op_is(op, "sar")) {
        if (lhs < 0 || rhs < 0 || rhs >= bits - 1) return false;
        if (op_is(op, "shl")) {
            if (lhs > (LLONG_MAX >> rhs)) return false;
            *result = lhs << rhs;
        } else {
            *result = lhs >> rhs;
        }
        return true;
    }
    return false;
}

static bool opt_rewrite_current(const OptInstruction *ins, char *line_out, size_t line_out_size, bool *skip) {
    long long value;
    int dst;
    int src;
    *skip = false;
    if (ins->label || ins->directive || ins->argc < 1) return false;
    dst = virtual_reg_index(ins->args[0]);
    if (dst < 0) return false;
    if (op_is(ins->base_op, "mov") && ins->argc == 2) {
        src = virtual_reg_index(ins->args[1]);
        if (src == dst) {
            *skip = true;
            return true;
        }
        return false;
    }
    if (ins->argc != 2) return false;
    src = virtual_reg_index(ins->args[1]);
    if (src == dst) {
        if (op_is(ins->base_op, "or") || op_is(ins->base_op, "and")) {
            *skip = true;
            return true;
        }
        if (op_is(ins->base_op, "sub") || op_is(ins->base_op, "xor")) {
            snprintf(line_out, line_out_size, "mov %s, 0", ins->args[0]);
            return true;
        }
    }
    if (!opt_parse_long(ins->args[1], &value)) return false;
    if ((op_is(ins->base_op, "add") || op_is(ins->base_op, "sub") ||
         op_is(ins->base_op, "or") || op_is(ins->base_op, "xor") ||
         op_is(ins->base_op, "shl") || op_is(ins->base_op, "shr") ||
         op_is(ins->base_op, "sar")) && value == 0) {
        *skip = true;
        return true;
    }
    if ((op_is(ins->base_op, "mul") || op_is(ins->base_op, "div")) && value == 1) {
        *skip = true;
        return true;
    }
    if (op_is(ins->base_op, "and") && value == -1) {
        *skip = true;
        return true;
    }
    if ((op_is(ins->base_op, "and") || op_is(ins->base_op, "mul")) && value == 0) {
        snprintf(line_out, line_out_size, "mov %s, 0", ins->args[0]);
        return true;
    }
    if (op_is(ins->base_op, "mod") && value == 1) {
        snprintf(line_out, line_out_size, "mov %s, 0", ins->args[0]);
        return true;
    }
    return false;
}

static bool opt_try_absorb_pending(Optimizer *opt, const OptInstruction *current, const char *current_line, int line_no) {
    char current_reg[128];
    long long current_value;
    long long rhs;
    long long result;
    if (!opt->has_pending) return false;
    if (opt_is_mov_imm(current, current_reg, &current_value) && strcmp(opt->pending_reg, current_reg) == 0) {
        return opt_set_pending(opt, current_line, line_no, current_reg, current_value);
    }
    if (current->label || current->directive || current->argc != 2) return false;
    if (strcmp(current->args[0], opt->pending_reg) != 0 || !opt_parse_long(current->args[1], &rhs)) return false;
    if (!opt_compute_binary(current->base_op, opt->pending_value, rhs, &result)) return false;
    snprintf(opt->pending, sizeof(opt->pending), "mov %s, %lld", opt->pending_reg, result);
    opt->pending_value = result;
    opt->pending_line_no = line_no;
    return true;
}

static void emit_text_line_optimized(Buffer *text, const char *line, int line_no, const char *target, Optimizer *opt) {
    OptInstruction parsed;
    OptInstruction current;
    char rewritten[512];
    char current_reg[128];
    long long current_value;
    const char *candidate = line;
    bool skip = false;
    if (!opt->enabled) {
        emit_text_line_copy(text, line, line_no, target);
        return;
    }
    if (!opt_parse_instruction(line, &parsed)) {
        opt_flush(text, target, opt);
        emit_text_line_copy(text, line, line_no, target);
        return;
    }
    if (parsed.label || parsed.directive) {
        opt_flush(text, target, opt);
        emit_text_line_copy(text, line, line_no, target);
        return;
    }
    if (opt_rewrite_current(&parsed, rewritten, sizeof(rewritten), &skip)) {
        if (skip) return;
        candidate = rewritten;
        if (!opt_parse_instruction(candidate, &current)) {
            opt_flush(text, target, opt);
            emit_text_line_copy(text, candidate, line_no, target);
            return;
        }
    } else {
        current = parsed;
    }
    if (opt_try_absorb_pending(opt, &current, candidate, line_no)) {
        return;
    }
    if (opt_is_mov_imm(&current, current_reg, &current_value)) {
        opt_flush(text, target, opt);
        if (!opt_set_pending(opt, candidate, line_no, current_reg, current_value)) {
            emit_text_line_copy(text, candidate, line_no, target);
        }
        return;
    }
    opt_flush(text, target, opt);
    emit_text_line_copy(text, candidate, line_no, target);
}

static Buffer compile_encoded_esolang(const char *source, const char *target) {
    Buffer out;
    buf_init(&out);
    if (strcmp(target, "fractran") == 0) {
        buf_append(&out, "2/3");
        for (size_t i = 0; source[i]; i++) {
            char frac[64];
            snprintf(frac, sizeof(frac), " %u/1", (unsigned char)source[i] + 2u);
            buf_append(&out, frac);
        }
        buf_append(&out, "\n");
    } else {
        unsigned int state = 0;
        buf_append(&out, "# CommonASM cellular automaton seed\n");
        buf_append(&out, "rule 110\n");
        buf_append(&out, "seed ");
        for (size_t i = 0; source[i]; i++) {
            unsigned char ch = (unsigned char)source[i];
            for (int bit = 7; bit >= 0; bit--) {
                char cell = ((ch >> bit) & 1) ? '1' : '0';
                buf_appendf(&out, "%s", cell == '1' ? "1" : "0");
                state = ((state << 1) ^ ch ^ (unsigned int)bit) & 0xffffu;
            }
        }
        char trailer[128];
        snprintf(trailer, sizeof(trailer), "\nchecksum %u\n", state);
        buf_append(&out, trailer);
    }
    return out;
}

static Buffer compile_source(char *source, const char *target, int opt_level) {
    Buffer constants, data, rodata, bss, text, out;
    Optimizer optimizer;
    char *cursor = source;
    const char *section = NULL;
    enum { SECTION_NONE, SECTION_DATA, SECTION_RODATA, SECTION_BSS, SECTION_TEXT } section_kind = SECTION_NONE;
    int line_no = 0;
    bool in_asm_block = false;
    bool asm_emitting = false;
    bool asm_verbatim = true;
    Buffer *asm_target_buffer = NULL;
    /* Consecutive asm blocks are alternatives for one spot: the first whose
       selector matches is emitted and the rest are skipped. A run where none
       matches is an error rather than a silently missing instruction. */
    bool asm_run_open = false;
    bool asm_run_matched = false;
    int asm_run_line = 0;
    /* The first arm of a run is kept so that --translate-asm has something to
       lift when no arm turns out to match. */
    Buffer asm_first_body;
    char asm_first_selector[64] = {0};
    bool asm_capturing = false;
    const bool x86 = strcmp(target, "x86_64-nasm") == 0;
    const bool i386 = is_i386_target(target);
    const bool rv = is_riscv_target(target);
    const bool mmix = strcmp(target, "mmixal") == 0;
    const bool dcpu = strcmp(target, "dcpu16") == 0;
    const bool generic = is_generic_arch_target(target) || is_legacy_arch_target(target) || is_mips_target(target) || is_ppc_target(target) || is_sparc_target(target) || is_m68k_target(target) || is_s390_target(target) || is_vm_ir_target(target) || is_toy_target(target);
    if (target_has_class(target, CLASS_ENCODING)) {
        return compile_encoded_esolang(source, target);
    }
    /* Scanned before the loop starts chopping the source into lines. */
    mentioned_vregs = scan_mentioned_vregs(source);
    if (is_wasm_target(target)) wasm_begin();
    optimizer.enabled = opt_level > 0;
    optimizer.has_pending = false;
    optimizer.pending[0] = '\0';
    optimizer.pending_reg[0] = '\0';
    optimizer.pending_value = 0;
    optimizer.pending_line_no = 0;
    buf_init(&constants); buf_init(&data); buf_init(&rodata); buf_init(&bss); buf_init(&text);
    buf_init(&asm_first_body);
    while (*cursor) {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        line_no++;
        if (newline) { *newline = '\0'; cursor = newline + 1; }
        else cursor += strlen(cursor);

        /* Inline assembly is collected before comments are stripped, because
           the body is not CommonASM: a '#' in it is an ARM immediate, not the
           start of a comment. */
        if (in_asm_block) {
            char *body = line;
            while (*body && isspace((unsigned char)*body)) body++;
            {
                char *end = body + strlen(body);
                while (end > body && isspace((unsigned char)end[-1])) *--end = '\0';
            }
            if (strcmp(body, "}") == 0) {
                in_asm_block = false;
                continue;
            }
            if (asm_capturing) {
                buf_append(&asm_first_body, line);
                buf_append(&asm_first_body, "\n");
            }
            if (asm_emitting) {
                if (asm_verbatim) {
                    emit_inline_asm_line(asm_target_buffer, line, target, line_no);
                } else {
                    /* The portable arm is ordinary CommonASM, so it goes
                       through the normal path, comments and all. */
                    char *portable;
                    strip_comment(line);
                    portable = trim(line);
                    if (*portable) {
                        emit_text_line_optimized(&text, portable, line_no, target, &optimizer);
                    }
                }
            }
            continue;
        }

        strip_comment(line);
        line = trim(line);
        if (*line == '\0') continue;
        if (line[0] == 'a' && strncmp(line, "asm ", 4) == 0) {
            char *selector = line + 4;
            char *brace = strchr(selector, '{');
            if (!brace || *trim(brace + 1) != '\0') {
                line_error(line_no, "asm", "expected asm SELECTOR { on its own line");
            }
            *brace = '\0';
            selector = trim(selector);
            if (!asm_selector_is_known(selector)) {
                line_error_token(line_no, selector, "asm", "unknown target or family in an asm selector");
            }
            if (section_kind == SECTION_NONE) {
                line_error(line_no, "asm", "expected .data, .rodata, .bss, or .text");
            }
            if (!asm_run_open) {
                asm_run_open = true;
                asm_run_matched = false;
                asm_run_line = line_no;
                asm_first_body.len = 0;
                asm_first_body.data[0] = '\0';
                snprintf(asm_first_selector, sizeof(asm_first_selector), "%s", selector);
                asm_capturing = true;
            } else {
                asm_capturing = false;
            }
            asm_verbatim = strcmp(selector, "portable") != 0;
            if (!asm_verbatim && section_kind != SECTION_TEXT) {
                line_error(line_no, "asm", "a portable asm arm holds instructions, so it belongs in .text");
            }
            in_asm_block = true;
            asm_emitting = !asm_run_matched && asm_selector_matches(selector, target);
            if (asm_emitting) asm_run_matched = true;
            /* Verbatim text can do anything, so nothing the compiler was
               holding back may outlive it. */
            opt_flush(&text, target, &optimizer);
            if (is_riscv_target(target)) compare_discard(&rv_cmp);
            asm_target_buffer = section_kind == SECTION_DATA ? &data :
                                section_kind == SECTION_RODATA ? &rodata :
                                section_kind == SECTION_BSS ? &bss : &text;
            continue;
        }
        if (asm_run_open) {
            if (!asm_run_matched) {
                if (!translate_inline_asm ||
                    !translate_asm_block(&text, asm_first_selector, asm_first_body.data, target, asm_run_line)) {
                    line_error(asm_run_line, "asm",
                               translate_inline_asm
                                   ? "no asm block matches this target, and the first one is not from a family that can be lifted"
                                   : "no asm block here matches the target being compiled");
                }
            }
            asm_run_open = false;
        }

        /* Classifying a line used to cost about ten string comparisons, on
           every line. Gating each family on its first character skips nearly
           all of them, and the section is remembered as a code so the text
           path stops re-deriving it from a name. */
        if (line[0] == '.') {
            if (strcmp(line, ".data") == 0 || strcmp(line, ".rodata") == 0 ||
                strcmp(line, ".bss") == 0 || strcmp(line, ".text") == 0) {
                opt_flush(&text, target, &optimizer);
                section = line + 1;
                section_kind = strcmp(section, "data") == 0 ? SECTION_DATA :
                               strcmp(section, "rodata") == 0 ? SECTION_RODATA :
                               strcmp(section, "bss") == 0 ? SECTION_BSS : SECTION_TEXT;
                continue;
            }
        }
        if (line[0] == 'c' && strncmp(line, "const ", 6) == 0) {
            char *name = trim(line + 6);
            char *eq = strchr(name, '=');
            if (!eq) line_error(line_no, "const", "expected const NAME = VALUE");
            *eq = '\0';
            name = trim(name);
            char *value = trim(eq + 1);
            remember_constant(name, value);
            if (x86) buf_appendf(&constants, "%s equ %s\n", name, value);
            else if (i386) buf_appendf(&constants, "%s equ %s\n", name, value);
            else if (rv) buf_appendf(&constants, ".equ %s, %s\n", name, value);
            else if (mmix) buf_appendf(&constants, "%s IS %s\n", name, value);
            else if (dcpu) buf_appendf(&constants, "%s EQU %s\n", name, value);
            else if (generic && !is_toy_target(target)) buf_appendf(&constants, ".equ %s, %s\n", name, value);
            else if (generic) buf_appendf(&constants, "; const %s = %s\n", name, value);
            continue;
        }
        if ((line[0] == 'g' && strncmp(line, "global ", 7) == 0) ||
            (line[0] == 'e' && strncmp(line, "extern ", 7) == 0)) {
            emit_text_line_optimized(&text, line, line_no, target, &optimizer);
            continue;
        }
        if (section_kind == SECTION_NONE) line_error(line_no, "section", "expected .data, .rodata, .bss, or .text");
        /* wasm has one linear memory rather than sections, so every kind of
           data lands in the same layout in the order it was declared. */
        if (is_wasm_target(target) && section_kind != SECTION_TEXT) {
            wasm_data_line(line, line_no);
            continue;
        }
        if (section_kind == SECTION_TEXT) emit_text_line_optimized(&text, line, line_no, target, &optimizer);
        else if (section_kind == SECTION_DATA) emit_data_line(&data, &constants, line, line_no, target, section);
        else if (section_kind == SECTION_RODATA) emit_data_line(&rodata, &constants, line, line_no, target, section);
        else emit_data_line(&bss, &constants, line, line_no, target, section);
    }
    if (in_asm_block) {
        line_error(asm_run_line, "asm", "unterminated asm block; expected a closing }");
    }
    if (asm_run_open && !asm_run_matched) {
        if (!translate_inline_asm ||
            !translate_asm_block(&text, asm_first_selector, asm_first_body.data, target, asm_run_line)) {
            line_error(asm_run_line, "asm", "no asm block here matches the target being compiled");
        }
    }
    free(asm_first_body.data);
    if (is_wasm_target(target)) {
        free(constants.data); free(data.data); free(rodata.data); free(bss.data); free(text.data);
        return wasm_finish();
    }
    opt_flush(&text, target, &optimizer);
    /* Only reserve backing store for the spilled virtual registers if the
       program actually referenced one. */
    if (x86 && x86_spill_used) {
        buf_append(&bss, "alignb 8\n");
        buf_appendf(&bss, "%s: resq %d\n", X86_SPILL_SYMBOL, X86_SPILL_COUNT);
    }
    if (i386 && i386_spill_used) {
        buf_append(&bss, "alignb 4\n");
        buf_appendf(&bss, "%s: resd %d\n", X86_SPILL_SYMBOL, I386_SPILL_COUNT);
    }
    if (dcpu && dcpu_spill_used) {
        buf_appendf(&bss, ":%s DAT %d DUP(0)\n", X86_SPILL_SYMBOL, DCPU_SPILL_COUNT);
    }
    if (is_m68k_target(target) && m68k_spill_used) {
        buf_appendf(&bss, "%s: .zero %d\n", X86_SPILL_SYMBOL, M68K_SPILL_COUNT * 4);
    }
    if (is_s390_target(target) && s390_spill_used) {
        buf_append(&bss, ".balign 8\n");
        buf_appendf(&bss, "%s: .zero %d\n", X86_SPILL_SYMBOL, S390_SPILL_COUNT * 8);
    }
    buf_init(&out);
    if (x86 || i386) {
        if (x86) buf_append(&out, "default rel\n");
        buf_append(&out, constants.data);
        if (rodata.len) { buf_append(&out, "\nsection .rodata\n"); buf_append(&out, rodata.data); }
        if (data.len) { buf_append(&out, "\nsection .data\n"); buf_append(&out, data.data); }
        if (bss.len) { buf_append(&out, "\nsection .bss\n"); buf_append(&out, bss.data); }
        buf_append(&out, "\nsection .text\n");
    } else if (rv) {
        /* A target that advertises the bit-manipulation instructions has to
           tell the assembler it may use them. */
        if (target_has_flag(target, TF_RV_ZBB)) buf_append(&out, ".option arch, +zbb\n");
        buf_append(&out, constants.data);
        if (rodata.len) { buf_append(&out, ".section .rodata\n"); buf_append(&out, rodata.data); }
        if (data.len) { buf_append(&out, ".section .data\n"); buf_append(&out, data.data); }
        if (bss.len) { buf_append(&out, ".section .bss\n"); buf_append(&out, bss.data); }
        buf_append(&out, "\n.section .text\n");
    } else if (mmix) {
        buf_append(&out, constants.data);
        if (rodata.len) { buf_append(&out, "\n        LOC #1000\n"); buf_append(&out, rodata.data); }
        if (data.len) { buf_append(&out, "\n        LOC #2000\n"); buf_append(&out, data.data); }
        if (bss.len) { buf_append(&out, "\n        LOC #3000\n"); buf_append(&out, bss.data); }
        buf_append(&out, "\n        LOC #4000\n");
    } else if (dcpu) {
        buf_append(&out, constants.data);
        if (rodata.len) { buf_append(&out, "\n; rodata\n"); buf_append(&out, rodata.data); }
        if (data.len) { buf_append(&out, "\n; data\n"); buf_append(&out, data.data); }
        if (bss.len) { buf_append(&out, "\n; bss\n"); buf_append(&out, bss.data); }
        buf_append(&out, "\n; text\n");
    } else if (generic) {
        buf_append(&out, constants.data);
        if (is_toy_target(target)) {
            if (rodata.len) { buf_append(&out, "; rodata\n"); buf_append(&out, rodata.data); }
            if (data.len) { buf_append(&out, "; data\n"); buf_append(&out, data.data); }
            if (bss.len) { buf_append(&out, "; bss\n"); buf_append(&out, bss.data); }
            buf_append(&out, "\n; text\n");
        } else {
            if (rodata.len) { buf_append(&out, ".section .rodata\n"); buf_append(&out, rodata.data); }
            if (data.len) { buf_append(&out, ".section .data\n"); buf_append(&out, data.data); }
            if (bss.len) { buf_append(&out, ".section .bss\n"); buf_append(&out, bss.data); }
            /* The ARM emitter writes unified-syntax mnemonics, so every ARM
               flavour has to announce that, Thumb included. */
            if (strcmp(target, "thumb-gnu") == 0 || strcmp(target, "thumb2-gnu") == 0) {
                buf_append(&out, ".syntax unified\n.thumb\n");
            } else if (is_arm32_target(target)) {
                buf_append(&out, ".syntax unified\n.arm\n");
            }
            /* A 64-bit SPARC assembler wants to be told before an application
               global is used, since the ABI leaves their role to the program. */
            if (is_sparc_target(target) && sparc_is_64(target)) {
                buf_append(&out, ".register %g2, #scratch\n.register %g3, #scratch\n");
            }
            if (is_arm32_target(target) && arm_spill_used) {
                buf_appendf(&out, ".lcomm %s, %d\n", X86_SPILL_SYMBOL, ARM_SPILL_COUNT * 4);
            }
            buf_append(&out, "\n.section .text\n");
        }
    } else {
        die("unknown target");
    }
    buf_append(&out, text.data);
    if (is_arm32_target(target) && arm_divmod_used) buf_append(&out, arm_divmod_routine);
    return out;
}

int main(int argc, char **argv) {
    const char *input = NULL;
    const char *target = NULL;
    const char *output = NULL;
    const char *from_family = NULL;
    bool emit_cas_only = false;
    int opt_level = 0;
    char *source;
    Buffer compiled;
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        puts(usage_text);
        puts("\nUse --list-targets to print every supported target.");
        puts("Use --target-info TARGET to inspect one target.");
        puts("Use --version to print the compiler version.");
        puts("Use -O1, -O, or --optimize to enable peephole code optimization.");
        puts("Use --emulate-extended to expand popcnt, clz, ctz, bswap, rol and ror");
        puts("instead of using the target's instructions for them.");
        puts("Use --translate-asm to lift an inline asm block written for another");
        puts("machine back into CommonASM, instead of failing for want of an arm.");
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        puts("commonasmc " COMMONASM_VERSION);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--list-targets") == 0) {
        print_target_list();
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--target-info") == 0) {
        print_target_info(argv[2]);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) target = argv[++i];
        else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) output = argv[++i];
        else if (strcmp(argv[i], "-O0") == 0 || strcmp(argv[i], "--no-optimize") == 0) opt_level = 0;
        else if (strcmp(argv[i], "-O") == 0 || strcmp(argv[i], "-O1") == 0 || strcmp(argv[i], "--optimize") == 0 || strcmp(argv[i], "--optimize=1") == 0) opt_level = 1;
        else if (strcmp(argv[i], "--emulate-extended") == 0) force_extended_fallback = true;
        else if (strcmp(argv[i], "--translate-asm") == 0) translate_inline_asm = true;
        else if (strcmp(argv[i], "--from") == 0 && i + 1 < argc) from_family = argv[++i];
        else if (strcmp(argv[i], "--emit-cas") == 0) emit_cas_only = true;
        else if (strcmp(argv[i], "--optimize=0") == 0) opt_level = 0;
        else if (strncmp(argv[i], "-O", 2) == 0 || strncmp(argv[i], "--optimize=", 11) == 0) die("unsupported optimization level; use -O0 or -O1");
        else if (!input) input = argv[i];
        else die(usage_text);
    }
    if (!input) die(usage_text);
    if (!target && !(from_family && emit_cas_only)) die(usage_text);
    if (target && !is_supported_target(target)) die("unknown target; run commonasmc --list-targets");
    source = read_file(input);
    if (from_family) {
        /* The input is assembly rather than CommonASM, so it is read back
           into CommonASM first and then compiled like any other source. */
        Buffer recovered = reconstruct_cas(source, from_family);
        free(source);
        source = recovered.data;
    }
    set_diagnostic_source(strcmp(input, "-") == 0 ? "<stdin>" : input, source);
    if (emit_cas_only) {
        Buffer shown;
        shown.data = source;
        shown.len = strlen(source);
        shown.cap = shown.len + 1;
        write_file_or_stdout(output, &shown);
        free(source);
        symbol_set_free(&known_constants);
        return 0;
    }
    compiled = compile_source(source, target, opt_level);
    write_file_or_stdout(output, &compiled);
    free(source);
    free(compiled.data);
    symbol_set_free(&known_constants);
    return 0;
}


























#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

typedef struct {
    char base[64];
    char symbol[128];
    long offset;
    bool has_base;
    bool has_symbol;
} Address;

static const char *x86_regs[] = {
    "rbx", "r12", "r13", "r14", "r15", "r8", "r9", "r10",
    "rcx", "rdx", "rsi", "rdi", "rbp", "rax", "r11", "rsp"
};
static const char *x86_regs_d[] = {
    "ebx", "r12d", "r13d", "r14d", "r15d", "r8d", "r9d", "r10d",
    "ecx", "edx", "esi", "edi", "ebp", "eax", "r11d", "esp"
};
static const char *x86_regs_w[] = {
    "bx", "r12w", "r13w", "r14w", "r15w", "r8w", "r9w", "r10w",
    "cx", "dx", "si", "di", "bp", "ax", "r11w", "sp"
};
static const char *x86_regs_b[] = {
    "bl", "r12b", "r13b", "r14b", "r15b", "r8b", "r9b", "r10b",
    "cl", "dl", "sil", "dil", "bpl", "al", "r11b", "spl"
};
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
static char known_constants[256][64];
static int known_constant_count = 0;

static void die(const char *message) {
    fprintf(stderr, "commonasmc: error: %s\n", message);
    exit(1);
}

static void line_error(int line_no, const char *op, const char *message) {
    fprintf(stderr, "commonasmc: error: line %d: %s: %s\n", line_no, op, message);
    exit(1);
}

static void *xmalloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        die("out of memory");
    }
    return ptr;
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

static void buf_append(Buffer *buf, const char *text) {
    size_t extra = strlen(text);
    buf_grow(buf, extra);
    memcpy(buf->data + buf->len, text, extra + 1);
    buf->len += extra;
}

static void buf_appendf(Buffer *buf, const char *fmt, const char *a, const char *b, const char *c) {
    char tmp[2048];
    int written = snprintf(tmp, sizeof(tmp), fmt, a ? a : "", b ? b : "", c ? c : "");
    if (written < 0 || (size_t)written >= sizeof(tmp)) {
        die("generated line is too long");
    }
    buf_append(buf, tmp);
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    long size;
    char *data;
    if (!file) {
        die("could not open input file");
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        die("could not seek input file");
    }
    size = ftell(file);
    if (size < 0) {
        die("could not read input size");
    }
    rewind(file);
    data = xmalloc((size_t)size + 1);
    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        die("could not read input file");
    }
    data[size] = '\0';
    fclose(file);
    return data;
}

static void write_file_or_stdout(const char *path, const Buffer *out) {
    FILE *file = stdout;
    if (path) {
        file = fopen(path, "wb");
        if (!file) {
            die("could not open output file");
        }
    }
    fwrite(out->data, 1, out->len, file);
    if (path) {
        fclose(file);
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
    errno = 0;
    strtol(text, &end, 0);
    return errno == 0 && end && *end == '\0' && end != text;
}

static long parse_int_or_die(const char *text, int line_no, const char *op) {
    char *end = NULL;
    long value;
    errno = 0;
    value = strtol(text, &end, 0);
    if (errno != 0 || !end || *end != '\0' || end == text) {
        line_error(line_no, op, "expected integer");
    }
    return value;
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

static void remember_constant(const char *name) {
    if (known_constant_count >= 256) {
        die("too many constants");
    }
    snprintf(known_constants[known_constant_count], sizeof(known_constants[known_constant_count]), "%s", name);
    known_constant_count++;
}

static bool is_known_constant(const char *name) {
    for (int i = 0; i < known_constant_count; i++) {
        if (strcmp(known_constants[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static int virtual_reg_index(const char *name) {
    if (name[0] == 'r' && isdigit((unsigned char)name[1])) {
        char *end = NULL;
        long value = strtol(name + 1, &end, 10);
        if (end && *end == '\0' && value >= 0 && value <= 15) {
            return (int)value;
        }
    }
    return -1;
}

static const char *x86_reg(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        line_error(line_no, op, "expected virtual register r0-r15");
    }
    return x86_regs[reg];
}

static const char *x86_reg_sized(const char *value, const char *size, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        line_error(line_no, op, "expected virtual register r0-r15");
    }
    if (strcmp(size, "b") == 0) return x86_regs_b[reg];
    if (strcmp(size, "w") == 0) return x86_regs_w[reg];
    if (strcmp(size, "d") == 0) return x86_regs_d[reg];
    return x86_regs[reg];
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
        line_error(line_no, op, "expected virtual register r0-r15");
    }
    return rv_regs[reg];
}

static const char *mmix_reg(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        line_error(line_no, op, "expected virtual register r0-r15");
    }
    return mmix_regs[reg];
}

static const char *dcpu_reg(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        line_error(line_no, op, "expected virtual register r0-r15");
    }
    if (reg >= 8) {
        line_error(line_no, op, "DCPU-16 maps only r0-r7 directly");
    }
    return dcpu_regs[reg];
}

static const char *size_suffix_or_default(const char *op, char *base_op, size_t base_size) {
    const char *dot = strchr(op, '.');
    if (!dot) {
        snprintf(base_op, base_size, "%s", op);
        return "q";
    }
    snprintf(base_op, base_size, "%.*s", (int)(dot - op), op);
    if (strcmp(dot + 1, "b") == 0 || strcmp(dot + 1, "w") == 0 ||
        strcmp(dot + 1, "d") == 0 || strcmp(dot + 1, "q") == 0) {
        return dot + 1;
    }
    return NULL;
}

static const char *x86_size_word(const char *size) {
    if (strcmp(size, "b") == 0) return "byte";
    if (strcmp(size, "w") == 0) return "word";
    if (strcmp(size, "d") == 0) return "dword";
    return "qword";
}

static const char *rv_load_op(const char *size) {
    if (strcmp(size, "b") == 0) return "lb";
    if (strcmp(size, "w") == 0) return "lh";
    if (strcmp(size, "d") == 0) return "lw";
    return "ld";
}

static const char *rv_store_op(const char *size) {
    if (strcmp(size, "b") == 0) return "sb";
    if (strcmp(size, "w") == 0) return "sh";
    if (strcmp(size, "d") == 0) return "sw";
    return "sd";
}

static const char *x86_operand(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg >= 0) {
        return x86_regs[reg];
    }
    if (is_int(value) || is_symbol(value) || is_known_constant(value)) {
        return value;
    }
    line_error(line_no, op, "expected register, integer, symbol, or constant");
    return value;
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
        addr->offset = strtol(trim(sign + 1), NULL, 0);
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

static void x86_format_address(const char *text, char *out, size_t out_size, int line_no, const char *op) {
    Address addr;
    if (!parse_address(text, &addr)) {
        line_error(line_no, op, "expected address like [r0 + 8] or [symbol + 8]");
    }
    if (addr.has_base) {
        snprintf(out, out_size, "[%s%s%ld]", x86_reg(addr.base, line_no, op), addr.offset < 0 ? "" : "+", addr.offset);
    } else {
        snprintf(out, out_size, "[rel %s%s%ld]", addr.symbol, addr.offset < 0 ? "" : "+", addr.offset);
    }
    if (addr.offset == 0) {
        if (addr.has_base) snprintf(out, out_size, "[%s]", x86_reg(addr.base, line_no, op));
        else snprintf(out, out_size, "[rel %s]", addr.symbol);
    }
}

static void rv_emit_address_setup(Buffer *text, const char *addr_text, const char *scratch, int line_no, const char *op) {
    Address addr;
    char offset[64];
    if (!parse_address(addr_text, &addr)) {
        line_error(line_no, op, "expected address like [r0 + 8] or [symbol + 8]");
    }
    snprintf(offset, sizeof(offset), "%ld", addr.offset);
    if (addr.has_base) {
        buf_appendf(text, "%s", "", NULL, NULL);
    } else {
        if (addr.offset == 0) {
            buf_appendf(text, "  la %s, %s\n", scratch, addr.symbol, NULL);
        } else {
            buf_appendf(text, "  la %s, %s\n", scratch, addr.symbol, NULL);
            buf_appendf(text, "  addi %s, %s, ", scratch, scratch, NULL);
            buf_append(text, offset);
            buf_append(text, "\n");
        }
    }
}

static const char *rv_address_base(const char *addr_text, const char *scratch, int line_no, const char *op, long *offset) {
    Address addr;
    if (!parse_address(addr_text, &addr)) {
        line_error(line_no, op, "expected address like [r0 + 8] or [symbol + 8]");
    }
    *offset = addr.has_base ? addr.offset : 0;
    return addr.has_base ? rv_reg(addr.base, line_no, op) : scratch;
}

static void emit_data_line(Buffer *out, Buffer *constants, char *line, int line_no, const char *target, const char *section) {
    char *colon = strchr(line, ':');
    char *name;
    char *kind;
    const bool x86 = strcmp(target, "x86_64-nasm") == 0;
    const bool rv = strcmp(target, "riscv64-gnu") == 0;
    const bool mmix = strcmp(target, "mmixal") == 0;
    const bool dcpu = strcmp(target, "dcpu16") == 0;
    if (strncmp(line, "align ", 6) == 0) {
        const char *value = trim(line + 6);
        if (x86) buf_appendf(out, "align %s\n", value, NULL, NULL);
        else if (rv) buf_appendf(out, ".balign %s\n", value, NULL, NULL);
        else if (mmix) buf_appendf(out, "        %% align %s\n", value, NULL, NULL);
        else if (dcpu) buf_appendf(out, "        ; align %s\n", value, NULL, NULL);
        return;
    }
    if (!colon) {
        line_error(line_no, "data", "expected name: directive");
    }
    *colon = '\0';
    name = trim(line);
    kind = trim(colon + 1);
    if (strncmp(kind, "string", 6) == 0 && isspace((unsigned char)kind[6])) {
        char *quote = trim(kind + 6);
        int byte_count = 0;
        if (*quote != '"') {
            line_error(line_no, "string", "expected string literal");
        }
        if (dcpu) buf_appendf(out, ":%s DAT ", name, NULL, NULL);
        else buf_appendf(out, "%s: %s ", name, x86 ? "db" : (mmix ? "BYTE" : ".byte"), NULL);
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
        char len_value[32];
        char len_name[128];
        snprintf(len_value, sizeof(len_value), "%d", byte_count);
        snprintf(len_name, sizeof(len_name), "%s_len", name);
        remember_constant(len_name);
        if (x86) buf_appendf(constants, "%s_len equ %s\n", name, len_value, NULL);
        else if (rv) buf_appendf(constants, ".equ %s_len, %s\n", name, len_value, NULL);
        else if (mmix) buf_appendf(constants, "%s_len IS %s\n", name, len_value, NULL);
        else if (dcpu) buf_appendf(constants, "%s_len EQU %s\n", name, len_value, NULL);
        return;
    }
    if (strncmp(kind, "zero", 4) == 0 && isspace((unsigned char)kind[4])) {
        const char *value = trim(kind + 4);
        if (dcpu) {
            buf_appendf(out, ":%s DAT ", name, NULL, NULL);
            buf_append(out, value);
            buf_append(out, " DUP(0)\n");
        } else if (mmix) {
            buf_appendf(out, "%s LOC @+%s\n", name, value, NULL);
        } else if (x86 && strcmp(section, "bss") == 0) {
            buf_appendf(out, "%s: resb %s\n", name, value, NULL);
        } else if (x86) {
            buf_appendf(out, "%s: times %s db 0\n", name, value, NULL);
        } else {
            buf_appendf(out, "%s: .zero %s\n", name, value, NULL);
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
        buf_appendf(out, ":%s DAT ", name, NULL, NULL);
    } else if (mmix) {
        const char *mmix_dir = strcmp(cas_dir, "word") == 0 ? "WYDE" : (strcmp(cas_dir, "dword") == 0 ? "TETRA" : (strcmp(cas_dir, "qword") == 0 ? "OCTA" : "BYTE"));
        buf_appendf(out, "%s: %s ", name, mmix_dir, NULL);
    } else {
        buf_appendf(out, "%s: %s ", name, x86 ? x86_dir : rv_dir, NULL);
    }
    buf_append(out, kind);
    buf_append(out, "\n");
}

static void emit_x86_syscall(Buffer *text, char **args, int argc, int line_no) {
    const char *arg_regs[] = {"rdi", "rsi", "rdx", "r10", "r8", "r9"};
    int number = -1;
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    if (strcmp(args[0], "read") == 0) number = 0;
    else if (strcmp(args[0], "write") == 0) number = 1;
    else if (strcmp(args[0], "open") == 0) number = 2;
    else if (strcmp(args[0], "close") == 0) number = 3;
    else if (strcmp(args[0], "exit") == 0) number = 60;
    else line_error(line_no, "syscall", "unknown syscall");
    char num[32];
    snprintf(num, sizeof(num), "%d", number);
    buf_appendf(text, "  mov rax, %s\n", num, NULL, NULL);
    for (int i = 1; i < argc && i <= 6; i++) {
        buf_appendf(text, "  mov %s, %s\n", arg_regs[i - 1], x86_operand(args[i], line_no, "syscall"), NULL);
    }
    buf_append(text, "  syscall\n");
}

static void emit_rv_syscall(Buffer *text, char **args, int argc, int line_no) {
    const char *arg_regs[] = {"a0", "a1", "a2", "a3", "a4", "a5"};
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
        if (reg >= 0) buf_appendf(text, "  mv %s, %s\n", arg_regs[i - 1], rv_regs[reg], NULL);
        else if (is_int(args[i]) || is_known_constant(args[i])) buf_appendf(text, "  li %s, %s\n", arg_regs[i - 1], args[i], NULL);
        else buf_appendf(text, "  la %s, %s\n", arg_regs[i - 1], args[i], NULL);
    }
    char num[32];
    snprintf(num, sizeof(num), "%d", number);
    buf_appendf(text, "  li a7, %s\n", num, NULL, NULL);
    buf_append(text, "  ecall\n");
}

static const char *mmix_operand(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg >= 0) return mmix_regs[reg];
    if (is_int(value) || is_symbol(value) || is_known_constant(value)) return value;
    line_error(line_no, op, "expected register, integer, symbol, or constant");
    return value;
}

static const char *dcpu_operand(const char *value, int line_no, const char *op) {
    int reg = virtual_reg_index(value);
    if (reg >= 0) return dcpu_reg(value, line_no, op);
    if (is_int(value) || is_symbol(value) || is_known_constant(value)) return value;
    line_error(line_no, op, "expected register, integer, symbol, or constant");
    return value;
}

static void mmix_format_address(const char *text, char *out, size_t out_size, int line_no, const char *op) {
    Address addr;
    if (!parse_address(text, &addr)) line_error(line_no, op, "expected address like [r0 + 8] or [symbol + 8]");
    if (addr.has_base) snprintf(out, out_size, "%ld,%s", addr.offset, mmix_reg(addr.base, line_no, op));
    else snprintf(out, out_size, "%s%+ld", addr.symbol, addr.offset);
}

static void dcpu_format_address(const char *text, char *out, size_t out_size, int line_no, const char *op) {
    Address addr;
    if (!parse_address(text, &addr)) line_error(line_no, op, "expected address like [r0 + 8] or [symbol + 8]");
    if (addr.has_base) {
        if (addr.offset == 0) snprintf(out, out_size, "[%s]", dcpu_reg(addr.base, line_no, op));
        else snprintf(out, out_size, "[%s%+ld]", dcpu_reg(addr.base, line_no, op), addr.offset);
    } else {
        if (addr.offset == 0) snprintf(out, out_size, "[%s]", addr.symbol);
        else snprintf(out, out_size, "[%s%+ld]", addr.symbol, addr.offset);
    }
}

static void emit_mmix_syscall(Buffer *text, char **args, int argc, int line_no) {
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    buf_appendf(text, "        %% syscall %s lowered as MMIX TRAP placeholder\n", args[0], NULL, NULL);
    if (strcmp(args[0], "exit") == 0) buf_append(text, "        TRAP 0,Halt,0\n");
    else buf_append(text, "        TRAP 0,Fputs,StdOut\n");
}

static void emit_dcpu_syscall(Buffer *text, char **args, int argc, int line_no) {
    if (argc < 1) line_error(line_no, "syscall", "needs a syscall name");
    buf_appendf(text, "        ; syscall %s lowered as DCPU software interrupt placeholder\n", args[0], NULL, NULL);
    buf_append(text, "        INT 0\n");
}

static void emit_mmix_instruction(Buffer *text, const char *op, const char *size, char **args, int argc, int line_no) {
    (void)size;
    if (strcmp(op, "func") == 0 && argc == 1) { buf_appendf(text, "%s:\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "endfunc") == 0 && argc == 0) return;
    if (strcmp(op, "enter") == 0 && argc == 1) { buf_appendf(text, "        SUBU $254,$254,%s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "leave") == 0 && argc == 0) return;
    if (strcmp(op, "mov") == 0 && argc == 2) { buf_appendf(text, "        SET %s,%s\n", mmix_reg(args[0], line_no, op), mmix_operand(args[1], line_no, op), NULL); return; }
    if (strcmp(op, "load_addr") == 0 && argc == 2) { buf_appendf(text, "        LDA %s,%s\n", mmix_reg(args[0], line_no, op), args[1], NULL); return; }
    if (strcmp(op, "load") == 0 && argc == 2) {
        char addr[256]; mmix_format_address(args[1], addr, sizeof(addr), line_no, op);
        buf_appendf(text, "        LDO %s,%s\n", mmix_reg(args[0], line_no, op), addr, NULL); return;
    }
    if (strcmp(op, "store") == 0 && argc == 2) {
        char addr[256]; mmix_format_address(args[0], addr, sizeof(addr), line_no, op);
        buf_appendf(text, "        STO %s,%s\n", mmix_operand(args[1], line_no, op), addr, NULL); return;
    }
    if ((strcmp(op, "add") == 0 || strcmp(op, "sub") == 0 || strcmp(op, "mul") == 0 || strcmp(op, "div") == 0 ||
         strcmp(op, "and") == 0 || strcmp(op, "or") == 0 || strcmp(op, "xor") == 0) && argc == 2) {
        const char *native = strcmp(op, "add") == 0 ? "ADD" : strcmp(op, "sub") == 0 ? "SUB" : strcmp(op, "mul") == 0 ? "MUL" : strcmp(op, "div") == 0 ? "DIV" : strcmp(op, "and") == 0 ? "AND" : strcmp(op, "or") == 0 ? "OR" : "XOR";
        buf_appendf(text, "        %s %s,%s,", native, mmix_reg(args[0], line_no, op), mmix_reg(args[0], line_no, op));
        buf_append(text, mmix_operand(args[1], line_no, op)); buf_append(text, "\n"); return;
    }
    if ((strcmp(op, "shl") == 0 || strcmp(op, "shr") == 0 || strcmp(op, "sar") == 0) && argc == 2) {
        const char *native = strcmp(op, "shl") == 0 ? "SL" : "SR";
        buf_appendf(text, "        %s %s,%s,", native, mmix_reg(args[0], line_no, op), mmix_reg(args[0], line_no, op));
        buf_append(text, mmix_operand(args[1], line_no, op)); buf_append(text, "\n"); return;
    }
    if ((strcmp(op, "neg") == 0 || strcmp(op, "not") == 0 || strcmp(op, "inc") == 0 || strcmp(op, "dec") == 0) && argc == 1) {
        if (strcmp(op, "neg") == 0) buf_appendf(text, "        NEG %s,0,%s\n", mmix_reg(args[0], line_no, op), mmix_reg(args[0], line_no, op), NULL);
        else if (strcmp(op, "not") == 0) buf_appendf(text, "        NOR %s,%s,%s\n", mmix_reg(args[0], line_no, op), mmix_reg(args[0], line_no, op), mmix_reg(args[0], line_no, op));
        else buf_appendf(text, strcmp(op, "inc") == 0 ? "        ADD %s,%s,1\n" : "        SUB %s,%s,1\n", mmix_reg(args[0], line_no, op), mmix_reg(args[0], line_no, op), NULL);
        return;
    }
    if (strcmp(op, "mod") == 0 && argc == 2) { buf_append(text, "        % mod is target-runtime dependent on MMIX; quotient/remainder omitted\n"); return; }
    if (strcmp(op, "cmp") == 0 && argc == 2) { buf_appendf(text, "        CMP $48,%s,%s\n", mmix_reg(args[0], line_no, op), mmix_operand(args[1], line_no, op), NULL); return; }
    if (strcmp(op, "je") == 0 && argc == 1) { buf_appendf(text, "        BZ $48,%s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "jne") == 0 && argc == 1) { buf_appendf(text, "        BNZ $48,%s\n", args[0], NULL, NULL); return; }
    if ((strcmp(op, "jg") == 0 || strcmp(op, "ja") == 0) && argc == 1) { buf_appendf(text, "        BP $48,%s\n", args[0], NULL, NULL); return; }
    if ((strcmp(op, "jl") == 0 || strcmp(op, "jb") == 0) && argc == 1) { buf_appendf(text, "        BN $48,%s\n", args[0], NULL, NULL); return; }
    if ((strcmp(op, "jge") == 0 || strcmp(op, "jae") == 0) && argc == 1) { buf_appendf(text, "        BNN $48,%s\n", args[0], NULL, NULL); return; }
    if ((strcmp(op, "jle") == 0 || strcmp(op, "jbe") == 0) && argc == 1) { buf_appendf(text, "        BNP $48,%s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "push") == 0 && argc == 1) { buf_appendf(text, "        STO %s,0,$254\n        SUBU $254,$254,8\n", mmix_operand(args[0], line_no, op), NULL, NULL); return; }
    if (strcmp(op, "pop") == 0 && argc == 1) { buf_appendf(text, "        ADDU $254,$254,8\n        LDO %s,0,$254\n", mmix_reg(args[0], line_no, op), NULL, NULL); return; }
    if (strcmp(op, "jmp") == 0 && argc == 1) { buf_appendf(text, "        JMP %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "call") == 0 && argc == 1) { buf_appendf(text, "        PUSHJ $15,%s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "ret") == 0 && argc == 0) { buf_append(text, "        POP 0,0\n"); return; }
    if (strcmp(op, "syscall") == 0) { emit_mmix_syscall(text, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count for MMIX");
}

static void emit_dcpu_instruction(Buffer *text, const char *op, const char *size, char **args, int argc, int line_no) {
    (void)size;
    if (strcmp(op, "func") == 0 && argc == 1) { buf_appendf(text, ":%s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "endfunc") == 0 && argc == 0) return;
    if (strcmp(op, "enter") == 0 && argc == 1) { buf_appendf(text, "        SUB SP, %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "leave") == 0 && argc == 0) return;
    if (strcmp(op, "mov") == 0 && argc == 2) { buf_appendf(text, "        SET %s, %s\n", dcpu_reg(args[0], line_no, op), dcpu_operand(args[1], line_no, op), NULL); return; }
    if (strcmp(op, "load_addr") == 0 && argc == 2) { buf_appendf(text, "        SET %s, %s\n", dcpu_reg(args[0], line_no, op), args[1], NULL); return; }
    if (strcmp(op, "load") == 0 && argc == 2) { char addr[256]; dcpu_format_address(args[1], addr, sizeof(addr), line_no, op); buf_appendf(text, "        SET %s, %s\n", dcpu_reg(args[0], line_no, op), addr, NULL); return; }
    if (strcmp(op, "store") == 0 && argc == 2) { char addr[256]; dcpu_format_address(args[0], addr, sizeof(addr), line_no, op); buf_appendf(text, "        SET %s, %s\n", addr, dcpu_operand(args[1], line_no, op), NULL); return; }
    if ((strcmp(op, "add") == 0 || strcmp(op, "sub") == 0 || strcmp(op, "mul") == 0 || strcmp(op, "div") == 0 ||
         strcmp(op, "mod") == 0 || strcmp(op, "and") == 0 || strcmp(op, "xor") == 0 || strcmp(op, "shl") == 0 || strcmp(op, "shr") == 0) && argc == 2) {
        const char *native = strcmp(op, "or") == 0 ? "BOR" : strcmp(op, "shl") == 0 ? "SHL" : strcmp(op, "shr") == 0 ? "SHR" : strcmp(op, "and") == 0 ? "AND" : strcmp(op, "xor") == 0 ? "XOR" : strcmp(op, "mod") == 0 ? "MOD" : strcmp(op, "mul") == 0 ? "MUL" : strcmp(op, "div") == 0 ? "DIV" : strcmp(op, "sub") == 0 ? "SUB" : "ADD";
        buf_appendf(text, "        %s %s, %s\n", native, dcpu_reg(args[0], line_no, op), dcpu_operand(args[1], line_no, op)); return;
    }
    if (strcmp(op, "or") == 0 && argc == 2) { buf_appendf(text, "        BOR %s, %s\n", dcpu_reg(args[0], line_no, op), dcpu_operand(args[1], line_no, op), NULL); return; }
    if ((strcmp(op, "neg") == 0 || strcmp(op, "not") == 0 || strcmp(op, "inc") == 0 || strcmp(op, "dec") == 0 || strcmp(op, "sar") == 0) && argc >= 1) {
        if (strcmp(op, "neg") == 0) { buf_appendf(text, "        XOR %s, 0xffff\n        ADD %s, 1\n", dcpu_reg(args[0], line_no, op), dcpu_reg(args[0], line_no, op), NULL); return; }
        if (strcmp(op, "not") == 0) { buf_appendf(text, "        XOR %s, 0xffff\n", dcpu_reg(args[0], line_no, op), NULL, NULL); return; }
        if (strcmp(op, "inc") == 0) { buf_appendf(text, "        ADD %s, 1\n", dcpu_reg(args[0], line_no, op), NULL, NULL); return; }
        if (strcmp(op, "dec") == 0) { buf_appendf(text, "        SUB %s, 1\n", dcpu_reg(args[0], line_no, op), NULL, NULL); return; }
        line_error(line_no, op, "DCPU-16 has no portable arithmetic right shift");
    }
    if (strcmp(op, "cmp") == 0 && argc == 2) { buf_appendf(text, "        SET EX, %s\n        SUB EX, %s\n", dcpu_reg(args[0], line_no, op), dcpu_operand(args[1], line_no, op), NULL); return; }
    if (strcmp(op, "je") == 0 && argc == 1) { buf_appendf(text, "        IFE EX, 0\n        SET PC, %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "jne") == 0 && argc == 1) { buf_appendf(text, "        IFN EX, 0\n        SET PC, %s\n", args[0], NULL, NULL); return; }
    if ((strcmp(op, "jg") == 0 || strcmp(op, "ja") == 0) && argc == 1) { buf_appendf(text, "        IFG EX, 0\n        SET PC, %s\n", args[0], NULL, NULL); return; }
    if ((strcmp(op, "jl") == 0 || strcmp(op, "jb") == 0) && argc == 1) { buf_appendf(text, "        IFL EX, 0\n        SET PC, %s\n", args[0], NULL, NULL); return; }
    if ((strcmp(op, "jge") == 0 || strcmp(op, "jae") == 0) && argc == 1) { buf_appendf(text, "        IFG EX, 0xffff\n        SET PC, %s\n", args[0], NULL, NULL); return; }
    if ((strcmp(op, "jle") == 0 || strcmp(op, "jbe") == 0) && argc == 1) { buf_appendf(text, "        IFL EX, 1\n        SET PC, %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "jmp") == 0 && argc == 1) { buf_appendf(text, "        SET PC, %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "call") == 0 && argc == 1) { buf_appendf(text, "        JSR %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "ret") == 0 && argc == 0) { buf_append(text, "        SET PC, POP\n"); return; }
    if (strcmp(op, "push") == 0 && argc == 1) { buf_appendf(text, "        SET PUSH, %s\n", dcpu_operand(args[0], line_no, op), NULL, NULL); return; }
    if (strcmp(op, "pop") == 0 && argc == 1) { buf_appendf(text, "        SET %s, POP\n", dcpu_reg(args[0], line_no, op), NULL, NULL); return; }
    if (strcmp(op, "syscall") == 0) { emit_dcpu_syscall(text, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count for DCPU-16");
}

static void emit_x86_instruction(Buffer *text, const char *op, const char *size, char **args, int argc, int line_no) {
    if (strcmp(op, "func") == 0 && argc == 1) {
        buf_appendf(text, "%s:\n", args[0], NULL, NULL); return;
    }
    if (strcmp(op, "endfunc") == 0 && argc == 0) return;
    if (strcmp(op, "enter") == 0 && argc == 1) {
        buf_append(text, "  push rbp\n  mov rbp, rsp\n");
        if (strcmp(args[0], "0") != 0) buf_appendf(text, "  sub rsp, %s\n", args[0], NULL, NULL);
        return;
    }
    if (strcmp(op, "leave") == 0 && argc == 0) {
        buf_append(text, "  mov rsp, rbp\n  pop rbp\n"); return;
    }
    if (strcmp(op, "mov") == 0 && argc == 2) {
        buf_appendf(text, "  mov %s, %s\n", x86_reg(args[0], line_no, op), x86_operand(args[1], line_no, op), NULL); return;
    }
    if (strcmp(op, "load_addr") == 0 && argc == 2) {
        buf_appendf(text, "  lea %s, [rel %s]\n", x86_reg(args[0], line_no, op), args[1], NULL); return;
    }
    if (strcmp(op, "load") == 0 && argc == 2) {
        char addr[256];
        x86_format_address(args[1], addr, sizeof(addr), line_no, op);
        if (strcmp(size, "q") == 0) {
            buf_appendf(text, "  mov %s, qword ", x86_reg(args[0], line_no, op), NULL, NULL);
        } else if (strcmp(size, "d") == 0) {
            buf_appendf(text, "  mov %s, dword ", x86_reg_sized(args[0], size, line_no, op), NULL, NULL);
        } else {
            buf_appendf(text, "  movzx %s, %s ", x86_reg(args[0], line_no, op), x86_size_word(size), NULL);
        }
        buf_append(text, addr); buf_append(text, "\n"); return;
    }
    if (strcmp(op, "store") == 0 && argc == 2) {
        char addr[256];
        x86_format_address(args[0], addr, sizeof(addr), line_no, op);
        if (virtual_reg_index(args[1]) >= 0) {
            buf_appendf(text, "  mov %s ", x86_size_word(size), NULL, NULL);
            buf_append(text, addr);
            buf_appendf(text, ", %s\n", x86_reg_sized(args[1], size, line_no, op), NULL, NULL);
        } else {
            buf_appendf(text, "  mov rax, %s\n", x86_operand(args[1], line_no, op), NULL, NULL);
            buf_appendf(text, "  mov %s ", x86_size_word(size), NULL, NULL);
            buf_append(text, addr);
            buf_appendf(text, ", %s\n", x86_rax_sized(size), NULL, NULL);
        }
        return;
    }
    if ((strcmp(op, "add") == 0 || strcmp(op, "sub") == 0 || strcmp(op, "and") == 0 ||
         strcmp(op, "or") == 0 || strcmp(op, "xor") == 0 || strcmp(op, "shl") == 0 ||
         strcmp(op, "shr") == 0 || strcmp(op, "sar") == 0) && argc == 2) {
        buf_appendf(text, "  %s %s, ", op, x86_reg(args[0], line_no, op), NULL);
        buf_append(text, x86_operand(args[1], line_no, op)); buf_append(text, "\n"); return;
    }
    if ((strcmp(op, "neg") == 0 || strcmp(op, "not") == 0 || strcmp(op, "inc") == 0 || strcmp(op, "dec") == 0) && argc == 1) {
        buf_appendf(text, "  %s %s\n", op, x86_reg(args[0], line_no, op), NULL); return;
    }
    if (strcmp(op, "mul") == 0 && argc == 2) {
        buf_appendf(text, "  imul %s, ", x86_reg(args[0], line_no, op), NULL, NULL);
        buf_append(text, x86_operand(args[1], line_no, op)); buf_append(text, "\n"); return;
    }
    if ((strcmp(op, "div") == 0 || strcmp(op, "mod") == 0) && argc == 2) {
        buf_appendf(text, "  mov rax, %s\n  cqo\n", x86_reg(args[0], line_no, op), NULL, NULL);
        if (virtual_reg_index(args[1]) >= 0) buf_appendf(text, "  idiv %s\n", x86_operand(args[1], line_no, op), NULL, NULL);
        else {
            buf_appendf(text, "  mov r11, %s\n", x86_operand(args[1], line_no, op), NULL, NULL);
            buf_append(text, "  idiv r11\n");
        }
        buf_appendf(text, strcmp(op, "mod") == 0 ? "  mov %s, rdx\n" : "  mov %s, rax\n", x86_reg(args[0], line_no, op), NULL, NULL);
        return;
    }
    if (strcmp(op, "cmp") == 0 && argc == 2) {
        buf_appendf(text, "  cmp %s, ", x86_reg(args[0], line_no, op), NULL, NULL);
        buf_append(text, x86_operand(args[1], line_no, op)); buf_append(text, "\n"); return;
    }
    if ((strcmp(op, "push") == 0 || strcmp(op, "pop") == 0) && argc == 1) {
        buf_appendf(text, "  %s %s\n", op, strcmp(op, "pop") == 0 ? x86_reg(args[0], line_no, op) : x86_operand(args[0], line_no, op), NULL); return;
    }
    if ((strcmp(op, "jmp") == 0 || strcmp(op, "call") == 0 || strcmp(op, "je") == 0 || strcmp(op, "jne") == 0 ||
         strcmp(op, "jg") == 0 || strcmp(op, "jl") == 0 || strcmp(op, "jge") == 0 || strcmp(op, "jle") == 0 ||
         strcmp(op, "ja") == 0 || strcmp(op, "jb") == 0 || strcmp(op, "jae") == 0 || strcmp(op, "jbe") == 0) && argc == 1) {
        buf_appendf(text, "  %s %s\n", op, args[0], NULL); return;
    }
    if (strcmp(op, "ret") == 0 && argc == 0) { buf_append(text, "  ret\n"); return; }
    if (strcmp(op, "syscall") == 0) { emit_x86_syscall(text, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count");
}

static void emit_rv_instruction(Buffer *text, const char *op, const char *size, char **args, int argc, int line_no) {
    if (strcmp(op, "func") == 0 && argc == 1) { buf_appendf(text, "%s:\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "endfunc") == 0 && argc == 0) return;
    if (strcmp(op, "enter") == 0 && argc == 1) {
        buf_append(text, "  addi sp, sp, -16\n  sd ra, 8(sp)\n  sd s0, 0(sp)\n  mv s0, sp\n");
        if (strcmp(args[0], "0") != 0) {
            buf_appendf(text, "  addi sp, sp, -%s\n", args[0], NULL, NULL);
        }
        return;
    }
    if (strcmp(op, "leave") == 0 && argc == 0) {
        buf_append(text, "  mv sp, s0\n  ld s0, 0(sp)\n  ld ra, 8(sp)\n  addi sp, sp, 16\n"); return;
    }
    if (strcmp(op, "mov") == 0 && argc == 2) {
        int src = virtual_reg_index(args[1]);
        if (src >= 0) buf_appendf(text, "  mv %s, %s\n", rv_reg(args[0], line_no, op), rv_regs[src], NULL);
        else buf_appendf(text, "  li %s, %s\n", rv_reg(args[0], line_no, op), args[1], NULL);
        return;
    }
    if (strcmp(op, "load_addr") == 0 && argc == 2) { buf_appendf(text, "  la %s, %s\n", rv_reg(args[0], line_no, op), args[1], NULL); return; }
    if ((strcmp(op, "load") == 0 || strcmp(op, "store") == 0) && argc == 2) {
        long off = 0;
        const char *base;
        if (strcmp(op, "load") == 0) {
            rv_emit_address_setup(text, args[1], "a6", line_no, op);
            base = rv_address_base(args[1], "a6", line_no, op, &off);
            char offstr[32]; snprintf(offstr, sizeof(offstr), "%ld", off);
            buf_appendf(text, "  %s %s, ", rv_load_op(size), rv_reg(args[0], line_no, op), NULL);
            buf_append(text, offstr); buf_appendf(text, "(%s)\n", base, NULL, NULL);
        } else {
            int src = virtual_reg_index(args[1]);
            if (src < 0) buf_appendf(text, "  li a7, %s\n", args[1], NULL, NULL);
            rv_emit_address_setup(text, args[0], "a6", line_no, op);
            base = rv_address_base(args[0], "a6", line_no, op, &off);
            char offstr[32]; snprintf(offstr, sizeof(offstr), "%ld", off);
            buf_appendf(text, "  %s %s, ", rv_store_op(size), src >= 0 ? rv_regs[src] : "a7", NULL);
            buf_append(text, offstr); buf_appendf(text, "(%s)\n", base, NULL, NULL);
        }
        return;
    }
    if ((strcmp(op, "add") == 0 || strcmp(op, "sub") == 0 || strcmp(op, "and") == 0 ||
         strcmp(op, "or") == 0 || strcmp(op, "xor") == 0) && argc == 2) {
        int src = virtual_reg_index(args[1]);
        const char *dst = rv_reg(args[0], line_no, op);
        if (src >= 0) {
            buf_appendf(text, "  %s %s, %s, ", op, dst, dst);
            buf_append(text, rv_regs[src]); buf_append(text, "\n");
        } else if (strcmp(op, "add") == 0 || strcmp(op, "and") == 0 || strcmp(op, "or") == 0 || strcmp(op, "xor") == 0) {
            const char *immop = strcmp(op, "add") == 0 ? "addi" : (strcmp(op, "and") == 0 ? "andi" : (strcmp(op, "or") == 0 ? "ori" : "xori"));
            buf_appendf(text, "  %s %s, %s, ", immop, dst, dst);
            buf_append(text, args[1]); buf_append(text, "\n");
        } else {
            buf_appendf(text, "  addi %s, %s, -", dst, dst, NULL);
            buf_append(text, args[1]); buf_append(text, "\n");
        }
        return;
    }
    if ((strcmp(op, "shl") == 0 || strcmp(op, "shr") == 0 || strcmp(op, "sar") == 0) && argc == 2) {
        int src = virtual_reg_index(args[1]);
        const char *native = strcmp(op, "shl") == 0 ? "sll" : (strcmp(op, "shr") == 0 ? "srl" : "sra");
        const char *nativei = strcmp(op, "shl") == 0 ? "slli" : (strcmp(op, "shr") == 0 ? "srli" : "srai");
        if (src >= 0) buf_appendf(text, "  %s %s, %s, ", native, rv_reg(args[0], line_no, op), rv_reg(args[0], line_no, op));
        else buf_appendf(text, "  %s %s, %s, ", nativei, rv_reg(args[0], line_no, op), rv_reg(args[0], line_no, op));
        buf_append(text, src >= 0 ? rv_regs[src] : args[1]); buf_append(text, "\n"); return;
    }
    if ((strcmp(op, "neg") == 0 || strcmp(op, "not") == 0 || strcmp(op, "inc") == 0 || strcmp(op, "dec") == 0) && argc == 1) {
        const char *dst = rv_reg(args[0], line_no, op);
        if (strcmp(op, "neg") == 0) buf_appendf(text, "  neg %s, %s\n", dst, dst, NULL);
        else if (strcmp(op, "not") == 0) buf_appendf(text, "  not %s, %s\n", dst, dst, NULL);
        else buf_appendf(text, strcmp(op, "inc") == 0 ? "  addi %s, %s, 1\n" : "  addi %s, %s, -1\n", dst, dst, NULL);
        return;
    }
    if ((strcmp(op, "mul") == 0 || strcmp(op, "div") == 0 || strcmp(op, "mod") == 0) && argc == 2) {
        int src = virtual_reg_index(args[1]);
        const char *native = strcmp(op, "mul") == 0 ? "mul" : (strcmp(op, "div") == 0 ? "div" : "rem");
        if (src < 0) buf_appendf(text, "  li a6, %s\n", args[1], NULL, NULL);
        buf_appendf(text, "  %s %s, %s, ", native, rv_reg(args[0], line_no, op), rv_reg(args[0], line_no, op));
        buf_append(text, src >= 0 ? rv_regs[src] : "a6"); buf_append(text, "\n"); return;
    }
    if (strcmp(op, "cmp") == 0 && argc == 2) {
        int src = virtual_reg_index(args[1]);
        if (src >= 0) {
            buf_appendf(text, "  sub a6, %s, ", rv_reg(args[0], line_no, op), NULL, NULL);
            buf_append(text, rv_regs[src]); buf_append(text, "\n");
        } else {
            buf_appendf(text, "  li a7, %s\n", args[1], NULL, NULL);
            buf_appendf(text, "  sub a6, %s, a7\n", rv_reg(args[0], line_no, op), NULL, NULL);
        }
        return;
    }
    if (strcmp(op, "je") == 0 && argc == 1) { buf_appendf(text, "  beqz a6, %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "jne") == 0 && argc == 1) { buf_appendf(text, "  bnez a6, %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "jg") == 0 && argc == 1) { buf_appendf(text, "  bgtz a6, %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "jl") == 0 && argc == 1) { buf_appendf(text, "  bltz a6, %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "jge") == 0 && argc == 1) { buf_appendf(text, "  bgez a6, %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "jle") == 0 && argc == 1) { buf_appendf(text, "  blez a6, %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "ja") == 0 && argc == 1) { buf_appendf(text, "  bgtu a6, zero, %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "jb") == 0 && argc == 1) { buf_appendf(text, "  bltu a6, zero, %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "jae") == 0 && argc == 1) { buf_appendf(text, "  bgeu a6, zero, %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "jbe") == 0 && argc == 1) { buf_appendf(text, "  bleu a6, zero, %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "push") == 0 && argc == 1) {
        int src = virtual_reg_index(args[0]);
        if (src < 0) buf_appendf(text, "  li a6, %s\n", args[0], NULL, NULL);
        buf_append(text, "  addi sp, sp, -8\n");
        buf_appendf(text, "  sd %s, 0(sp)\n", src >= 0 ? rv_regs[src] : "a6", NULL, NULL); return;
    }
    if (strcmp(op, "pop") == 0 && argc == 1) { buf_appendf(text, "  ld %s, 0(sp)\n  addi sp, sp, 8\n", rv_reg(args[0], line_no, op), NULL, NULL); return; }
    if (strcmp(op, "jmp") == 0 && argc == 1) { buf_appendf(text, "  j %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "call") == 0 && argc == 1) { buf_appendf(text, "  call %s\n", args[0], NULL, NULL); return; }
    if (strcmp(op, "ret") == 0 && argc == 0) { buf_append(text, "  ret\n"); return; }
    if (strcmp(op, "syscall") == 0) { emit_rv_syscall(text, args, argc, line_no); return; }
    line_error(line_no, op, "unsupported instruction or wrong argument count");
}

static void emit_text_line(Buffer *text, char *line, int line_no, const char *target) {
    char *space;
    char *args[16];
    int argc = 0;
    char base_op[64];
    const char *size;
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == ':') {
        line[len - 1] = '\0';
        if (strcmp(target, "mmixal") == 0) buf_appendf(text, "%s\n", line, NULL, NULL);
        else if (strcmp(target, "dcpu16") == 0) buf_appendf(text, ":%s\n", line, NULL, NULL);
        else buf_appendf(text, "%s:\n", line, NULL, NULL);
        return;
    }
    if (strncmp(line, "global ", 7) == 0) {
        const char *name = trim(line + 7);
        if (strcmp(target, "x86_64-nasm") == 0) buf_appendf(text, "global %s\n", name, NULL, NULL);
        else if (strcmp(target, "riscv64-gnu") == 0) buf_appendf(text, ".globl %s\n", name, NULL, NULL);
        else if (strcmp(target, "mmixal") == 0) buf_appendf(text, "%s IS @\n", name, NULL, NULL);
        else if (strcmp(target, "dcpu16") == 0) buf_appendf(text, "; global %s\n", name, NULL, NULL);
        return;
    }
    if (strncmp(line, "extern ", 7) == 0) {
        const char *name = trim(line + 7);
        if (strcmp(target, "x86_64-nasm") == 0) buf_appendf(text, "extern %s\n", name, NULL, NULL);
        else if (strcmp(target, "riscv64-gnu") == 0) buf_appendf(text, ".extern %s\n", name, NULL, NULL);
        else if (strcmp(target, "mmixal") == 0) buf_appendf(text, "        %% extern %s\n", name, NULL, NULL);
        else if (strcmp(target, "dcpu16") == 0) buf_appendf(text, "        ; extern %s\n", name, NULL, NULL);
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
    if (strcmp(target, "x86_64-nasm") == 0) emit_x86_instruction(text, base_op, size, args, argc, line_no);
    else if (strcmp(target, "riscv64-gnu") == 0) emit_rv_instruction(text, base_op, size, args, argc, line_no);
    else if (strcmp(target, "mmixal") == 0) emit_mmix_instruction(text, base_op, size, args, argc, line_no);
    else if (strcmp(target, "dcpu16") == 0) emit_dcpu_instruction(text, base_op, size, args, argc, line_no);
    else line_error(line_no, base_op, "unknown instruction target");
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
                buf_appendf(&out, "%s", cell == '1' ? "1" : "0", NULL, NULL);
                state = ((state << 1) ^ ch ^ (unsigned int)bit) & 0xffffu;
            }
        }
        char trailer[128];
        snprintf(trailer, sizeof(trailer), "\nchecksum %u\n", state);
        buf_append(&out, trailer);
    }
    return out;
}

static Buffer compile_source(char *source, const char *target) {
    Buffer constants, data, rodata, bss, text, out;
    char *cursor = source;
    const char *section = NULL;
    int line_no = 0;
    const bool x86 = strcmp(target, "x86_64-nasm") == 0;
    const bool rv = strcmp(target, "riscv64-gnu") == 0;
    const bool mmix = strcmp(target, "mmixal") == 0;
    const bool dcpu = strcmp(target, "dcpu16") == 0;
    if (strcmp(target, "fractran") == 0 || strcmp(target, "cellular-automaton") == 0) {
        return compile_encoded_esolang(source, target);
    }
    buf_init(&constants); buf_init(&data); buf_init(&rodata); buf_init(&bss); buf_init(&text);
    while (*cursor) {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        line_no++;
        if (newline) { *newline = '\0'; cursor = newline + 1; }
        else cursor += strlen(cursor);
        strip_comment(line);
        line = trim(line);
        if (*line == '\0') continue;
        if (strcmp(line, ".data") == 0 || strcmp(line, ".rodata") == 0 || strcmp(line, ".bss") == 0 || strcmp(line, ".text") == 0) {
            section = line + 1;
            continue;
        }
        if (strncmp(line, "const ", 6) == 0) {
            char *name = trim(line + 6);
            char *eq = strchr(name, '=');
            if (!eq) line_error(line_no, "const", "expected const NAME = VALUE");
            *eq = '\0';
            name = trim(name);
            char *value = trim(eq + 1);
            remember_constant(name);
            if (x86) buf_appendf(&constants, "%s equ %s\n", name, value, NULL);
            else if (rv) buf_appendf(&constants, ".equ %s, %s\n", name, value, NULL);
            else if (mmix) buf_appendf(&constants, "%s IS %s\n", name, value, NULL);
            else if (dcpu) buf_appendf(&constants, "%s EQU %s\n", name, value, NULL);
            continue;
        }
        if (strncmp(line, "global ", 7) == 0 || strncmp(line, "extern ", 7) == 0) {
            emit_text_line(&text, line, line_no, target);
            continue;
        }
        if (!section) line_error(line_no, "section", "expected .data, .rodata, .bss, or .text");
        if (strcmp(section, "data") == 0) emit_data_line(&data, &constants, line, line_no, target, section);
        else if (strcmp(section, "rodata") == 0) emit_data_line(&rodata, &constants, line, line_no, target, section);
        else if (strcmp(section, "bss") == 0) emit_data_line(&bss, &constants, line, line_no, target, section);
        else emit_text_line(&text, line, line_no, target);
    }
    buf_init(&out);
    if (x86) {
        buf_append(&out, "default rel\n");
        buf_append(&out, constants.data);
        if (rodata.len) { buf_append(&out, "\nsection .rodata\n"); buf_append(&out, rodata.data); }
        if (data.len) { buf_append(&out, "\nsection .data\n"); buf_append(&out, data.data); }
        if (bss.len) { buf_append(&out, "\nsection .bss\n"); buf_append(&out, bss.data); }
        buf_append(&out, "\nsection .text\n");
    } else if (rv) {
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
    } else {
        die("unknown target");
    }
    buf_append(&out, text.data);
    return out;
}

int main(int argc, char **argv) {
    const char *input = NULL;
    const char *target = NULL;
    const char *output = NULL;
    char *source;
    Buffer compiled;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) target = argv[++i];
        else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) output = argv[++i];
        else if (!input) input = argv[i];
        else die("usage: commonasmc input.cas --target x86_64-nasm|riscv64-gnu|mmixal|dcpu16|fractran|cellular-automaton [-o output]");
    }
    if (!input || !target) die("usage: commonasmc input.cas --target x86_64-nasm|riscv64-gnu|mmixal|dcpu16|fractran|cellular-automaton [-o output]");
    source = read_file(input);
    compiled = compile_source(source, target);
    write_file_or_stdout(output, &compiled);
    free(source);
    free(compiled.data);
    return 0;
}

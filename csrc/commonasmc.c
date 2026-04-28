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

static const char *x86_regs[] = {"rbx", "r12", "r13", "r14", "r15", "r8", "r9", "r10"};
static const char *rv_regs[] = {"t0", "t1", "t2", "t3", "t4", "t5", "t6", "s1"};
static char known_constants[128][64];
static int known_constant_count = 0;

static void die(const char *message) {
    fprintf(stderr, "commonasmc: error: %s\n", message);
    exit(1);
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

static void buf_append(Buffer *buf, const char *text) {
    size_t extra = strlen(text);
    buf_grow(buf, extra);
    memcpy(buf->data + buf->len, text, extra + 1);
    buf->len += extra;
}

static void buf_appendf(Buffer *buf, const char *fmt, const char *a, const char *b, const char *c) {
    char tmp[1024];
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
        if (!in_string && (ch == ';' || ch == '#')) {
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

static void remember_constant(const char *name) {
    if (known_constant_count >= 128) {
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
    if (name[0] == 'r' && name[1] >= '0' && name[1] <= '7' && name[2] == '\0') {
        return name[1] - '0';
    }
    return -1;
}

static const char *x86_operand(const char *value) {
    int reg = virtual_reg_index(value);
    if (reg >= 0) {
        return x86_regs[reg];
    }
    return value;
}

static const char *x86_reg(const char *value, int line_no) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        fprintf(stderr, "commonasmc: error: line %d: expected virtual register r0-r7\n", line_no);
        exit(1);
    }
    return x86_regs[reg];
}

static const char *rv_reg(const char *value, int line_no) {
    int reg = virtual_reg_index(value);
    if (reg < 0) {
        fprintf(stderr, "commonasmc: error: line %d: expected virtual register r0-r7\n", line_no);
        exit(1);
    }
    return rv_regs[reg];
}

static int split_args(char *arg_text, char **args, int max_args) {
    int count = 0;
    char *cursor = arg_text;
    while (*cursor && count < max_args) {
        char *comma = strchr(cursor, ',');
        if (comma) {
            *comma = '\0';
        }
        args[count++] = trim(cursor);
        if (!comma) {
            break;
        }
        cursor = comma + 1;
    }
    return count;
}

static void emit_data(Buffer *data, Buffer *constants, char *line, int line_no, const char *target) {
    char *colon = strchr(line, ':');
    char *kind;
    char *quote;
    char *name;
    if (!colon) {
        fprintf(stderr, "commonasmc: error: line %d: expected name: string \"text\"\n", line_no);
        exit(1);
    }
    *colon = '\0';
    name = trim(line);
    kind = trim(colon + 1);
    if (strncmp(kind, "bytes", 5) == 0 && isspace((unsigned char)kind[5])) {
        char *values = trim(kind + 5);
        buf_appendf(data, "%s: %s ", name, strcmp(target, "x86_64-nasm") == 0 ? "db" : ".byte", NULL);
        buf_append(data, values);
        buf_append(data, "\n");
        return;
    }
    if (strncmp(kind, "string", 6) != 0 || !isspace((unsigned char)kind[6])) {
        fprintf(stderr, "commonasmc: error: line %d: expected string data\n", line_no);
        exit(1);
    }
    quote = trim(kind + 6);
    if (*quote != '"') {
        fprintf(stderr, "commonasmc: error: line %d: expected string literal\n", line_no);
        exit(1);
    }
    buf_appendf(data, "%s: %s ", name, strcmp(target, "x86_64-nasm") == 0 ? "db" : ".byte", NULL);
    quote++;
    int byte_count = 0;
    for (size_t i = 0; quote[i] && quote[i] != '"'; i++) {
        unsigned char ch = (unsigned char)quote[i];
        char num[32];
        if (ch == '\\') {
            i++;
            if (quote[i] == 'n') {
                ch = '\n';
            } else if (quote[i] == 't') {
                ch = '\t';
            } else {
                ch = (unsigned char)quote[i];
            }
        }
        snprintf(num, sizeof(num), "%s%u", byte_count == 0 ? "" : ", ", ch);
        buf_append(data, num);
        byte_count++;
    }
    buf_append(data, "\n");
    char len_value[32];
    char len_name[128];
    snprintf(len_value, sizeof(len_value), "%d", byte_count);
    snprintf(len_name, sizeof(len_name), "%s_len", name);
    remember_constant(len_name);
    if (strcmp(target, "x86_64-nasm") == 0) {
        buf_appendf(constants, "%s_len equ %s\n", name, len_value, NULL);
    } else {
        buf_appendf(constants, ".equ %s_len, %s\n", name, len_value, NULL);
    }
}

static void emit_x86_syscall(Buffer *text, char **args, int argc, int line_no) {
    const char *arg_regs[] = {"rdi", "rsi", "rdx", "r10", "r8", "r9"};
    int number;
    if (argc < 1) {
        fprintf(stderr, "commonasmc: error: line %d: syscall needs a name\n", line_no);
        exit(1);
    }
    if (strcmp(args[0], "write") == 0) {
        number = 1;
    } else if (strcmp(args[0], "exit") == 0) {
        number = 60;
    } else {
        fprintf(stderr, "commonasmc: error: line %d: unknown syscall\n", line_no);
        exit(1);
    }
    char num[32];
    snprintf(num, sizeof(num), "%d", number);
    buf_appendf(text, "  mov rax, %s\n", num, NULL, NULL);
    for (int i = 1; i < argc; i++) {
        buf_appendf(text, "  mov %s, %s\n", arg_regs[i - 1], x86_operand(args[i]), NULL);
    }
    buf_append(text, "  syscall\n");
}

static void emit_rv_syscall(Buffer *text, char **args, int argc, int line_no) {
    const char *arg_regs[] = {"a0", "a1", "a2", "a3", "a4", "a5"};
    int number;
    if (argc < 1) {
        fprintf(stderr, "commonasmc: error: line %d: syscall needs a name\n", line_no);
        exit(1);
    }
    if (strcmp(args[0], "write") == 0) {
        number = 64;
    } else if (strcmp(args[0], "exit") == 0) {
        number = 93;
    } else {
        fprintf(stderr, "commonasmc: error: line %d: unknown syscall\n", line_no);
        exit(1);
    }
    for (int i = 1; i < argc; i++) {
        int reg = virtual_reg_index(args[i]);
        if (reg >= 0) {
            buf_appendf(text, "  mv %s, %s\n", arg_regs[i - 1], rv_regs[reg], NULL);
        } else if (is_int(args[i]) || is_known_constant(args[i])) {
            buf_appendf(text, "  li %s, %s\n", arg_regs[i - 1], args[i], NULL);
        } else {
            buf_appendf(text, "  la %s, %s\n", arg_regs[i - 1], args[i], NULL);
        }
    }
    char num[32];
    snprintf(num, sizeof(num), "%d", number);
    buf_appendf(text, "  li a7, %s\n", num, NULL, NULL);
    buf_append(text, "  ecall\n");
}

static void emit_text(Buffer *text, char *line, int line_no, const char *target) {
    char *space;
    char *op;
    char *args[16];
    int argc = 0;
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == ':') {
        buf_appendf(text, "%s\n", line, NULL, NULL);
        return;
    }
    if (strncmp(line, "global ", 7) == 0) {
        const char *name = trim(line + 7);
        buf_appendf(text, strcmp(target, "x86_64-nasm") == 0 ? "global %s\n" : ".globl %s\n", name, NULL, NULL);
        return;
    }
    space = line;
    while (*space && !isspace((unsigned char)*space)) {
        space++;
    }
    if (*space) {
        *space++ = '\0';
        argc = split_args(space, args, 16);
    }
    op = line;
    if (strcmp(target, "x86_64-nasm") == 0) {
        if (strcmp(op, "mov") == 0 && argc == 2) {
            buf_appendf(text, "  mov %s, %s\n", x86_reg(args[0], line_no), x86_operand(args[1]), NULL);
        } else if (strcmp(op, "load_addr") == 0 && argc == 2) {
            buf_appendf(text, "  lea %s, [rel %s]\n", x86_reg(args[0], line_no), args[1], NULL);
        } else if ((strcmp(op, "add") == 0 || strcmp(op, "sub") == 0) && argc == 2) {
            buf_appendf(text, "  %s %s, ", op, x86_reg(args[0], line_no), NULL);
            buf_append(text, x86_operand(args[1]));
            buf_append(text, "\n");
        } else if (strcmp(op, "cmp") == 0 && argc == 2) {
            buf_appendf(text, "  cmp %s, ", x86_reg(args[0], line_no), NULL, NULL);
            buf_append(text, x86_operand(args[1]));
            buf_append(text, "\n");
        } else if ((strcmp(op, "je") == 0 || strcmp(op, "jne") == 0) && argc == 1) {
            buf_appendf(text, "  %s %s\n", op, args[0], NULL);
        } else if (strcmp(op, "jmp") == 0 && argc == 1) {
            buf_appendf(text, "  jmp %s\n", args[0], NULL, NULL);
        } else if (strcmp(op, "call") == 0 && argc == 1) {
            buf_appendf(text, "  call %s\n", args[0], NULL, NULL);
        } else if (strcmp(op, "ret") == 0 && argc == 0) {
            buf_append(text, "  ret\n");
        } else if (strcmp(op, "syscall") == 0) {
            emit_x86_syscall(text, args, argc, line_no);
        } else {
            fprintf(stderr, "commonasmc: error: line %d: unsupported instruction\n", line_no);
            exit(1);
        }
    } else {
        if (strcmp(op, "mov") == 0 && argc == 2) {
            int src = virtual_reg_index(args[1]);
            if (src >= 0) {
                buf_appendf(text, "  mv %s, %s\n", rv_reg(args[0], line_no), rv_regs[src], NULL);
            } else {
                buf_appendf(text, "  li %s, %s\n", rv_reg(args[0], line_no), args[1], NULL);
            }
        } else if (strcmp(op, "load_addr") == 0 && argc == 2) {
            buf_appendf(text, "  la %s, %s\n", rv_reg(args[0], line_no), args[1], NULL);
        } else if ((strcmp(op, "add") == 0 || strcmp(op, "sub") == 0) && argc == 2) {
            int src = virtual_reg_index(args[1]);
            if (src >= 0) {
                buf_appendf(text, strcmp(op, "add") == 0 ? "  add %s, %s, " : "  sub %s, %s, ", rv_reg(args[0], line_no), rv_reg(args[0], line_no), NULL);
                buf_append(text, rv_regs[src]);
                buf_append(text, "\n");
            } else if (strcmp(op, "add") == 0) {
                buf_appendf(text, "  addi %s, %s, ", rv_reg(args[0], line_no), rv_reg(args[0], line_no), NULL);
                buf_append(text, args[1]);
                buf_append(text, "\n");
            } else {
                buf_appendf(text, "  addi %s, %s, -", rv_reg(args[0], line_no), rv_reg(args[0], line_no), NULL);
                buf_append(text, args[1]);
                buf_append(text, "\n");
            }
        } else if (strcmp(op, "cmp") == 0 && argc == 2) {
            int src = virtual_reg_index(args[1]);
            if (src >= 0) {
                buf_appendf(text, "  sub t6, %s, ", rv_reg(args[0], line_no), NULL, NULL);
                buf_append(text, rv_regs[src]);
                buf_append(text, "\n");
            } else {
                buf_appendf(text, "  addi t6, %s, -", rv_reg(args[0], line_no), NULL, NULL);
                buf_append(text, args[1]);
                buf_append(text, "\n");
            }
        } else if (strcmp(op, "je") == 0 && argc == 1) {
            buf_appendf(text, "  beqz t6, %s\n", args[0], NULL, NULL);
        } else if (strcmp(op, "jne") == 0 && argc == 1) {
            buf_appendf(text, "  bnez t6, %s\n", args[0], NULL, NULL);
        } else if (strcmp(op, "jmp") == 0 && argc == 1) {
            buf_appendf(text, "  j %s\n", args[0], NULL, NULL);
        } else if (strcmp(op, "call") == 0 && argc == 1) {
            buf_appendf(text, "  call %s\n", args[0], NULL, NULL);
        } else if (strcmp(op, "ret") == 0 && argc == 0) {
            buf_append(text, "  ret\n");
        } else if (strcmp(op, "syscall") == 0) {
            emit_rv_syscall(text, args, argc, line_no);
        } else {
            fprintf(stderr, "commonasmc: error: line %d: unsupported instruction\n", line_no);
            exit(1);
        }
    }
}

static Buffer compile_source(char *source, const char *target) {
    Buffer constants;
    Buffer data;
    Buffer text;
    Buffer out;
    char *cursor = source;
    char *section = NULL;
    int line_no = 0;
    buf_init(&constants);
    buf_init(&data);
    buf_init(&text);
    while (*cursor) {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        line_no++;
        if (newline) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor += strlen(cursor);
        }
        strip_comment(line);
        line = trim(line);
        if (*line == '\0') {
            continue;
        }
        if (strcmp(line, ".data") == 0 || strcmp(line, ".text") == 0) {
            section = line + 1;
            continue;
        }
        if (strncmp(line, "const ", 6) == 0) {
            char *name = trim(line + 6);
            char *eq = strchr(name, '=');
            if (!eq) {
                fprintf(stderr, "commonasmc: error: line %d: expected const NAME = VALUE\n", line_no);
                exit(1);
            }
            *eq = '\0';
            name = trim(name);
            char *value = trim(eq + 1);
            remember_constant(name);
            if (strcmp(target, "x86_64-nasm") == 0) {
                buf_appendf(&constants, "%s equ %s\n", name, value, NULL);
            } else {
                buf_appendf(&constants, ".equ %s, %s\n", name, value, NULL);
            }
            continue;
        }
        if (!section) {
            fprintf(stderr, "commonasmc: error: line %d: expected .data or .text\n", line_no);
            exit(1);
        }
        if (strcmp(section, "data") == 0) {
            emit_data(&data, &constants, line, line_no, target);
        } else {
            emit_text(&text, line, line_no, target);
        }
    }
    buf_init(&out);
    if (strcmp(target, "x86_64-nasm") == 0) {
        buf_append(&out, "default rel\n");
        buf_append(&out, constants.data);
        if (data.len) {
            buf_append(&out, "\nsection .data\n");
            buf_append(&out, data.data);
        }
        buf_append(&out, "\nsection .text\n");
    } else if (strcmp(target, "riscv64-gnu") == 0) {
        buf_append(&out, constants.data);
        if (data.len) {
            buf_append(&out, ".section .data\n");
            buf_append(&out, data.data);
        }
        buf_append(&out, "\n.section .text\n");
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
        if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            target = argv[++i];
        } else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
            output = argv[++i];
        } else if (!input) {
            input = argv[i];
        } else {
            die("usage: commonasmc input.cas --target x86_64-nasm|riscv64-gnu [-o output]");
        }
    }
    if (!input || !target) {
        die("usage: commonasmc input.cas --target x86_64-nasm|riscv64-gnu [-o output]");
    }
    source = read_file(input);
    compiled = compile_source(source, target);
    write_file_or_stdout(output, &compiled);
    free(source);
    free(compiled.data);
    return 0;
}

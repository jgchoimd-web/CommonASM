from .ir import Constant, DataBytes, DataItem, DataString, Global, Instruction, Label, Program, TextItem


class CompileError(Exception):
    pass


X86_REGS = {
    "r0": "rbx",
    "r1": "r12",
    "r2": "r13",
    "r3": "r14",
    "r4": "r15",
    "r5": "r8",
    "r6": "r9",
    "r7": "r10",
}

RISCV_REGS = {
    "r0": "t0",
    "r1": "t1",
    "r2": "t2",
    "r3": "t3",
    "r4": "t4",
    "r5": "t5",
    "r6": "t6",
    "r7": "s1",
}

LINUX_SYSCALLS = {
    "x86_64-nasm": {"write": 1, "exit": 60},
    "riscv64-gnu": {"write": 64, "exit": 93},
}


def compile_program(program: Program, target: str) -> str:
    if target == "x86_64-nasm":
        return X86Backend().compile(program)
    if target == "riscv64-gnu":
        return RISCVBackend().compile(program)
    raise CompileError(f"unknown target: {target}")


class X86Backend:
    def compile(self, program: Program) -> str:
        self.constants = {item.name for item in program.constants}
        lines = ["default rel"]
        lines.extend(self.emit_constant(item) for item in program.constants)
        lines.extend(self.emit_auto_lengths(item, "equ") for item in program.data if isinstance(item, DataString))
        if program.data:
            lines.extend(["", "section .data"])
            lines.extend(self.emit_data(item) for item in program.data)
        lines.extend(["", "section .text"])
        for item in program.text:
            lines.extend(self.emit_text(item))
        return "\n".join(lines) + "\n"

    def emit_constant(self, item: Constant) -> str:
        return f"{item.name} equ {item.value}"

    def emit_auto_lengths(self, item: DataString, directive: str) -> str:
        return f"{item.name}_len {directive} {len(item.value.encode('utf-8'))}"

    def emit_data(self, item: DataItem) -> str:
        values = item.value.encode("utf-8") if isinstance(item, DataString) else item.values
        bytes_list = ", ".join(str(byte) for byte in values)
        if bytes_list:
            return f"{item.name}: db {bytes_list}"
        return f"{item.name}: db 0"

    def emit_text(self, item: TextItem) -> list[str]:
        if isinstance(item, Global):
            return [f"global {item.name}"]
        if isinstance(item, Label):
            return [f"{item.name}:"]
        return [f"  {line}" for line in self.emit_instruction(item)]

    def emit_instruction(self, ins: Instruction) -> list[str]:
        op, args = ins.op, ins.args
        if op == "mov" and len(args) == 2:
            return [f"mov {self.reg(args[0], ins)}, {self.operand(args[1], ins)}"]
        if op == "load_addr" and len(args) == 2:
            return [f"lea {self.reg(args[0], ins)}, [rel {args[1]}]"]
        if op in {"add", "sub"} and len(args) == 2:
            return [f"{op} {self.reg(args[0], ins)}, {self.operand(args[1], ins)}"]
        if op == "cmp" and len(args) == 2:
            return [f"cmp {self.reg(args[0], ins)}, {self.operand(args[1], ins)}"]
        if op in {"je", "jne"} and len(args) == 1:
            return [f"{op} {args[0]}"]
        if op == "jmp" and len(args) == 1:
            return [f"jmp {args[0]}"]
        if op == "call" and len(args) == 1:
            return [f"call {args[0]}"]
        if op == "ret" and not args:
            return ["ret"]
        if op == "syscall":
            return self.emit_syscall(args, ins)
        raise CompileError(f"line {ins.line}: unsupported instruction: {op} {', '.join(args)}")

    def emit_syscall(self, args: list[str], ins: Instruction) -> list[str]:
        if not args:
            raise CompileError(f"line {ins.line}: syscall needs a name")
        name = args[0]
        number = LINUX_SYSCALLS["x86_64-nasm"].get(name)
        if number is None:
            raise CompileError(f"line {ins.line}: unknown syscall: {name}")
        arg_regs = ["rdi", "rsi", "rdx", "r10", "r8", "r9"]
        lines = [f"mov rax, {number}"]
        for reg, value in zip(arg_regs, args[1:]):
            lines.append(f"mov {reg}, {self.operand(value, ins)}")
        lines.append("syscall")
        return lines

    def operand(self, value: str, ins: Instruction) -> str:
        if value in X86_REGS:
            return X86_REGS[value]
        if _is_int(value):
            return value
        return value

    def reg(self, value: str, ins: Instruction) -> str:
        try:
            return X86_REGS[value]
        except KeyError as exc:
            raise CompileError(f"line {ins.line}: expected virtual register r0-r7, got {value}") from exc


class RISCVBackend:
    def compile(self, program: Program) -> str:
        self.constants = {item.name for item in program.constants}
        self.constants.update(f"{item.name}_len" for item in program.data if isinstance(item, DataString))
        lines = []
        lines.extend(self.emit_constant(item) for item in program.constants)
        lines.extend(self.emit_auto_lengths(item, ".equ") for item in program.data if isinstance(item, DataString))
        if program.data:
            lines.append(".section .data")
            lines.extend(self.emit_data(item) for item in program.data)
        lines.append("")
        lines.append(".section .text")
        for item in program.text:
            lines.extend(self.emit_text(item))
        return "\n".join(lines) + "\n"

    def emit_constant(self, item: Constant) -> str:
        return f".equ {item.name}, {item.value}"

    def emit_auto_lengths(self, item: DataString, directive: str) -> str:
        return f"{directive} {item.name}_len, {len(item.value.encode('utf-8'))}"

    def emit_data(self, item: DataItem) -> str:
        values = item.value.encode("utf-8") if isinstance(item, DataString) else item.values
        bytes_list = ", ".join(str(byte) for byte in values)
        if bytes_list:
            return f"{item.name}: .byte {bytes_list}"
        return f"{item.name}: .byte 0"

    def emit_text(self, item: TextItem) -> list[str]:
        if isinstance(item, Global):
            return [f".globl {item.name}"]
        if isinstance(item, Label):
            return [f"{item.name}:"]
        return [f"  {line}" for line in self.emit_instruction(item)]

    def emit_instruction(self, ins: Instruction) -> list[str]:
        op, args = ins.op, ins.args
        if op == "mov" and len(args) == 2:
            dst = self.reg(args[0], ins)
            if args[1] in RISCV_REGS:
                return [f"mv {dst}, {RISCV_REGS[args[1]]}"]
            return [f"li {dst}, {self.operand(args[1], ins)}"]
        if op == "load_addr" and len(args) == 2:
            return [f"la {self.reg(args[0], ins)}, {args[1]}"]
        if op in {"add", "sub"} and len(args) == 2:
            dst = self.reg(args[0], ins)
            if args[1] in RISCV_REGS:
                native_op = "add" if op == "add" else "sub"
                return [f"{native_op} {dst}, {dst}, {RISCV_REGS[args[1]]}"]
            native_op = "addi" if op == "add" else "addi"
            immediate = self.operand(args[1], ins)
            if op == "sub":
                immediate = f"-{immediate}" if not immediate.startswith("-") else immediate[1:]
            return [f"{native_op} {dst}, {dst}, {immediate}"]
        if op == "cmp" and len(args) == 2:
            dst = self.reg(args[0], ins)
            if args[1] in RISCV_REGS:
                return [f"sub t6, {dst}, {RISCV_REGS[args[1]]}"]
            return [f"addi t6, {dst}, -{self.operand(args[1], ins)}"]
        if op == "je" and len(args) == 1:
            return [f"beqz t6, {args[0]}"]
        if op == "jne" and len(args) == 1:
            return [f"bnez t6, {args[0]}"]
        if op == "jmp" and len(args) == 1:
            return [f"j {args[0]}"]
        if op == "call" and len(args) == 1:
            return [f"call {args[0]}"]
        if op == "ret" and not args:
            return ["ret"]
        if op == "syscall":
            return self.emit_syscall(args, ins)
        raise CompileError(f"line {ins.line}: unsupported instruction: {op} {', '.join(args)}")

    def emit_syscall(self, args: list[str], ins: Instruction) -> list[str]:
        if not args:
            raise CompileError(f"line {ins.line}: syscall needs a name")
        name = args[0]
        number = LINUX_SYSCALLS["riscv64-gnu"].get(name)
        if number is None:
            raise CompileError(f"line {ins.line}: unknown syscall: {name}")
        arg_regs = ["a0", "a1", "a2", "a3", "a4", "a5"]
        lines = []
        for reg, value in zip(arg_regs, args[1:]):
            if value in RISCV_REGS:
                lines.append(f"mv {reg}, {RISCV_REGS[value]}")
            elif _is_int(value) or value in self.constants:
                lines.append(f"li {reg}, {value}")
            else:
                lines.append(f"la {reg}, {value}")
        lines.append(f"li a7, {number}")
        lines.append("ecall")
        return lines

    def operand(self, value: str, ins: Instruction) -> str:
        if _is_int(value):
            return value
        if value in self.constants:
            return value
        raise CompileError(f"line {ins.line}: RISC-V immediate expected, got {value}")

    def reg(self, value: str, ins: Instruction) -> str:
        try:
            return RISCV_REGS[value]
        except KeyError as exc:
            raise CompileError(f"line {ins.line}: expected virtual register r0-r7, got {value}") from exc


def _is_int(value: str) -> bool:
    try:
        int(value, 0)
    except ValueError:
        return False
    return True


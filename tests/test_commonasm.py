from commonasm.backends import compile_program
from commonasm.parser import parse


def test_compiles_hello_to_x86_64_nasm():
    program = parse(
        'const stdout = 1\n.data\nmsg: string "Hi\\n"\n.text\nglobal _start\n_start:\nsyscall write, stdout, msg, msg_len\n'
    )

    output = compile_program(program, "x86_64-nasm")

    assert "stdout equ 1" in output
    assert "msg_len equ 3" in output
    assert "global _start" in output
    assert "mov rax, 1" in output
    assert "mov rsi, msg" in output
    assert "mov rdx, msg_len" in output
    assert "syscall" in output


def test_compiles_hello_to_riscv64_gnu():
    program = parse(
        'const stdout = 1\n.data\nmsg: string "Hi\\n"\n.text\nglobal _start\n_start:\nsyscall write, stdout, msg, msg_len\n'
    )

    output = compile_program(program, "riscv64-gnu")

    assert ".equ stdout, 1" in output
    assert ".equ msg_len, 3" in output
    assert ".globl _start" in output
    assert "li a7, 64" in output
    assert "la a1, msg" in output
    assert "li a2, msg_len" in output
    assert "ecall" in output


def test_compiles_bytes_and_branches():
    program = parse(
        ".data\npalette: bytes 255, 80, 0\n.text\nstart:\nmov r0, 3\ncmp r0, 3\nje done\njne start\ndone:\nret\n"
    )

    x86 = compile_program(program, "x86_64-nasm")
    rv = compile_program(program, "riscv64-gnu")

    assert "palette: db 255, 80, 0" in x86
    assert "cmp rbx, 3" in x86
    assert "je done" in x86
    assert "palette: .byte 255, 80, 0" in rv
    assert "addi t6, t0, -3" in rv
    assert "beqz t6, done" in rv

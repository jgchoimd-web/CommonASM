# CommonASM

CommonASM is a portable assembly IR that compiles into real assembly dialects.

It is meant as a middle layer for a future programming language compiler:

```text
your language -> CommonASM -> x86_64 / riscv64 / more backends later
```

## Supported targets

- `i386-nasm`: IA-32 / 32-bit x86 NASM-style output
- `x86_64-nasm`: Linux x86-64, NASM syntax
- `armv4-gnu`: experimental ARMv4 GNU-style output
- `armv5-gnu`: experimental ARMv5 GNU-style output
- `armv7a-gnu`: experimental ARMv7-A GNU-style output
- `aarch64-gnu`: experimental ARMv8-A / AArch64 GNU-style output
- `thumb-gnu`: experimental Cortex-M Thumb-style output
- `thumb2-gnu`: experimental Cortex-M Thumb-2-style output
- `rv32i-gnu`: experimental RV32I GNU-style output
- `rv64i-gnu`: RV64I alias for the RISC-V 64 backend
- `riscv64-gnu`: Linux RISC-V 64, GNU assembler syntax
- `rv128i-gnu`: experimental RV128I-style output
- `ia64-gnu`: experimental Itanium / IA-64-style output
- `loongarch64-gnu`: experimental LoongArch64 GNU-style output
- `mmixal`: experimental MMIXAL-style output
- `dcpu16`: experimental DCPU-16-style output
- `fractran`: experimental FRACTRAN source-encoding output
- `cellular-automaton`: experimental Rule 110 seed output

The x86-64 and RISC-V 64 backends are the primary maintained backends. IA-32,
ARM, AArch64, RV32/RV128, IA-64, LoongArch, MMIX, and DCPU-16 are experimental
assembly-style outputs for the portable subset. FRACTRAN and Cellular Automaton
targets encode the CommonASM source as esolang artifacts rather than modeling
Linux syscalls or random-access machine memory directly.
DCPU-16 directly maps `r0` through `r7`; wider virtual registers are kept for the
other targets.

## Compiler implementations

- `csrc/commonasmc.c`: C AOT compiler
- `selfhost/compiler.cal`: self-hosting compiler source sketch

## Example

```asm
const stdout = 1

.data
msg: string "Hello from CommonASM\n"
colors: bytes 255, 80, 40

.text
global _start

_start:
  syscall write, stdout, msg, msg_len
  syscall exit, 0
```

Compile it:

```powershell
gcc csrc/commonasmc.c -o build/commonasmc.exe
build/commonasmc.exe examples/hello.cas --target x86_64-nasm -o build/hello_x86.asm
build/commonasmc.exe examples/hello.cas --target i386-nasm -o build/hello_i386.asm
build/commonasmc.exe examples/hello.cas --target riscv64-gnu -o build/hello_rv64.s
build/commonasmc.exe examples/hello.cas --target aarch64-gnu -o build/hello_aarch64.s
build/commonasmc.exe examples/hello.cas --target armv7a-gnu -o build/hello_armv7.s
build/commonasmc.exe examples/hello.cas --target mmixal -o build/hello_mmix.mms
build/commonasmc.exe examples/hello.cas --target dcpu16 -o build/hello_dcpu.dasm
build/commonasmc.exe examples/hello.cas --target fractran -o build/hello.fractran
build/commonasmc.exe examples/hello.cas --target cellular-automaton -o build/hello.ca
```

## Language sketch

Sections:

- `.data`
- `.rodata`
- `.bss`
- `.text`

Data:

- `name: string "text\n"`
- `name: bytes 1, 2, 255`
- `name: byte 1`
- `name: word 1024`
- `name: dword 65536`
- `name: qword 123456`
- `name: zero 64`
- `align 8`

Constants:

- `const stdout = 1`
- String data automatically creates `name_len`.

Text:

- `global _start`
- `extern puts`
- `label:`
- `func name`
- `endfunc`
- `enter 32`
- `leave`
- `mov r0, 123`
- `mov r1, r0`
- `load_addr r0, label`
- `load.q r0, [label]`
- `load.d r0, [r1 + 8]`
- `store.q [label], r0`
- `store.b [r1], 65`
- `add r0, r1`
- `sub r0, 1`
- `mul r0, 2`
- `div r0, r1`
- `mod r0, 10`
- `neg r0`
- `inc r0`
- `dec r0`
- `and r0, 255`
- `or r0, r1`
- `xor r0, 1`
- `not r0`
- `shl r0, 3`
- `shr r0, 1`
- `sar r0, 1`
- `push r0`
- `pop r1`
- `cmp r0, 10`
- `je label`
- `jne label`
- `jg label`
- `jl label`
- `jge label`
- `jle label`
- `ja label`
- `jb label`
- `jae label`
- `jbe label`
- `jmp label`
- `call label`
- `ret`
- `syscall read, fd, buffer, length`
- `syscall write, fd, buffer, length`
- `syscall open, path, flags, mode`
- `syscall close, fd`
- `syscall exit, code`

Virtual registers are `r0` through `r15`. Each backend maps them to native registers.
`cmp a, b` records `a - b` for the following conditional jump.

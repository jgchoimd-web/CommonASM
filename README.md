# CommonASM

CommonASM is a tiny target-neutral assembly language that compiles into real assembly dialects.

It is meant as a middle layer for a future programming language compiler:

```text
your language -> CommonASM -> x86_64 / riscv64 / more backends later
```

## Supported targets

- `x86_64-nasm`: Linux x86-64, NASM syntax
- `riscv64-gnu`: Linux RISC-V 64, GNU assembler syntax

## Compiler implementations

- `commonasm/`: Python reference compiler
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
python -m commonasm examples/hello.cas --target x86_64-nasm -o build/hello_x86.asm
python -m commonasm examples/hello.cas --target riscv64-gnu -o build/hello_rv64.s
```

Or build and use the C AOT compiler:

```powershell
gcc csrc/commonasmc.c -o build/commonasmc.exe
build/commonasmc.exe examples/hello.cas --target x86_64-nasm -o build/hello_x86.asm
```

## Language sketch

Sections:

- `.data`
- `.text`

Data:

- `name: string "text\n"`
- `name: bytes 1, 2, 255`

Constants:

- `const stdout = 1`
- String data automatically creates `name_len`.

Text:

- `global _start`
- `label:`
- `mov r0, 123`
- `mov r1, r0`
- `load_addr r0, label`
- `add r0, r1`
- `sub r0, 1`
- `cmp r0, 10`
- `je label`
- `jne label`
- `jmp label`
- `call label`
- `ret`
- `syscall write, fd, buffer, length`
- `syscall exit, code`

Virtual registers are `r0` through `r7`. Each backend maps them to native registers.

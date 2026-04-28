# commonasmc

`commonasmc.c` is the C AOT compiler for CommonASM.

Build:

```powershell
gcc csrc/commonasmc.c -o build/commonasmc.exe
```

Use:

```powershell
build/commonasmc.exe examples/hello.cas --target x86_64-nasm -o build/hello_from_c.asm
build/commonasmc.exe examples/hello.cas --target i386-nasm -o build/hello_i386.asm
build/commonasmc.exe examples/hello.cas --target riscv64-gnu -o build/hello_from_c.s
build/commonasmc.exe examples/hello.cas --target aarch64-gnu -o build/hello_aarch64.s
build/commonasmc.exe examples/esolang.cas --target mmixal -o build/esolang.mms
build/commonasmc.exe examples/esolang.cas --target dcpu16 -o build/esolang.dasm
build/commonasmc.exe examples/esolang.cas --target fractran -o build/esolang.fractran
build/commonasmc.exe examples/esolang.cas --target cellular-automaton -o build/esolang.ca
```

This is the primary compiler implementation. It stays dependency-free so it can be used as
the bootstrap compiler for the self-hosting compiler.

CommonASM is intentionally portable: the C compiler lowers the same input into
`x86_64-nasm`, `i386-nasm`, `riscv64-gnu`, `rv64i-gnu`, `armv7a-gnu`,
`aarch64-gnu`, `loongarch64-gnu`, `mmixal`, or `dcpu16` assembly output.
FRACTRAN and Cellular Automaton targets are experimental source-encoding outputs.

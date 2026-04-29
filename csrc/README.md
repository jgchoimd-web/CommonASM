# commonasmc

`commonasmc.c` is the C AOT compiler for CommonASM.

Build:

```powershell
gcc csrc/commonasmc.c -o build/commonasmc.exe
```

Use:

```powershell
build/commonasmc.exe --help
build/commonasmc.exe --list-targets
build/commonasmc.exe --target-info wasm
build/commonasmc.exe examples/hello.cas --target x86_64-nasm -o build/hello_from_c.asm
Get-Content examples/hello.cas | build/commonasmc.exe - --target wasm -o -
build/commonasmc.exe examples/hello.cas --target i386-nasm -o build/hello_i386.asm
build/commonasmc.exe examples/hello.cas --target riscv64-gnu -o build/hello_from_c.s
build/commonasmc.exe examples/hello.cas --target aarch64-gnu -o build/hello_aarch64.s
build/commonasmc.exe examples/legacy.cas --target mips32-gnu -o build/legacy_mips.s
build/commonasmc.exe examples/vm_ir.cas --target wasm -o build/vm.wat
build/commonasmc.exe examples/retro_toy.cas --target brainfuck -o build/retro.bf
build/commonasmc.exe examples/esolang.cas --target mmixal -o build/esolang.mms
build/commonasmc.exe examples/esolang.cas --target dcpu16 -o build/esolang.dasm
build/commonasmc.exe examples/esolang.cas --target fractran -o build/esolang.fractran
build/commonasmc.exe examples/esolang.cas --target cellular-automaton -o build/esolang.ca
```

This is the primary compiler implementation. It stays dependency-free so it can be used as
the bootstrap compiler for the self-hosting compiler.

CommonASM is intentionally portable: the C compiler lowers the same input into
primary, experimental assembly/IR, and pseudo/encoding outputs. FRACTRAN,
Cellular Automaton, and many educational targets are intentionally experimental
source-encoding or pseudo-assembly outputs.

Use `--list-targets` to print the supported targets from the compiler itself.
This keeps the CLI usable even as the backend list grows.
Use `--target-info TARGET` to inspect one target's support level and output
style.
Use input `-` or output `-` for shell pipelines.

Compile errors use ANSI terminal colors to show the exact source line, column,
and highlighted token without any external package dependency.

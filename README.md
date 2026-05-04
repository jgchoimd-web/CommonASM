# CommonASM

[![CI](https://github.com/jgchoimd-web/common-assembly-language/actions/workflows/ci.yml/badge.svg)](https://github.com/jgchoimd-web/common-assembly-language/actions/workflows/ci.yml)

![CommonASM official logo](docs/assets/commonasm-logo.jpg)

CommonASM is a portable assembly IR that compiles into real assembly dialects.

Official logo: `docs/assets/commonasm-logo.jpg`

Curious about CommonASM? Go to https://ygsmsite.neocities.org/CommonASM

It is meant as a middle layer for a future programming language compiler:

```text
your language -> CommonASM -> x86_64 / riscv64 / more backends later
```

## Supported targets

Primary:

- `x86_64-nasm`
- `riscv64-gnu`

Experimental assembly/IR:

- `i386-nasm`, `ia32-nasm`
- `armv4-gnu`, `armv5-gnu`, `armv7a-gnu`, `aarch64-gnu`, `thumb-gnu`, `thumb2-gnu`
- `rv32i-gnu`, `rv64i-gnu`, `rv128i-gnu`, `ia64-gnu`, `loongarch64-gnu`
- `mips1-gnu`, `mips32-gnu`, `mips64-gnu`, `micromips-gnu`
- `power1-gnu`, `power2-gnu`, `ppc603-gnu`, `ppcg4-gnu`, `ppcg5-gnu`, `power9-gnu`, `power10-gnu`
- `sparcv8-gnu`, `sparcv9-gnu`, `alpha-gnu`, `parisc-gnu`, `m88k-gnu`
- `m68k`, `coldfire`, `avr`, `i8051`, `msp430`, `xtensa`, `superh`, `rx`, `nios2`, `microblaze`, `arc`
- `ptx`, `amdgcn`, `rdna`, `intelgen`, `cell-spe`, `tms320`, `dsp56000`, `blackfin`, `hexagon`, `ebpf`
- `wasm`, `llvm-ir`, `gcc-gimple`, `gcc-rtl`, `jvm-bytecode`, `cil`, `dalvik`, `lua-bytecode`, `python-bytecode`, `spirv`, `evm`
- `mmixal`, `dcpu16`

Encoding/pseudo:

- `mos6502`, `wdc65c02`, `wdc65816`, `mos6510`, `i8008`, `i8080`, `i8085`, `z80`, `ez80`, `m6800`, `m6809`
- `pic16`, `pic32`, `propeller`
- `pdp1`, `pdp8`, `pdp11`, `vax`, `system360`, `system370`, `zarch`, `cdc6600`, `univac1`, `cray1`
- `mix`, `lc3`, `lmc`, `marie`, `chip8`, `schip8`, `redcode`, `subleq`, `urisc`, `tta`
- `fractran`, `iota`, `jot`, `malbolge-asm`, `brainfuck`, `secd`, `pcode`, `zmachine`, `sweet16`, `befunge`, `bitblt-vm`, `turing-machine`, `cellular-automaton`, `unlambda`

The experimental targets are portable-subset outputs, not complete ABI-level ports.
Pseudo and encoding targets use comments, toy assembly, or source encodings when the
machine model does not match Linux syscalls or random-access memory.

## Compiler implementations

- `csrc/commonasmc.c`: C AOT compiler
- `selfhost/compiler.cal`: self-hosting compiler source sketch

The C compiler prints ANSI-colored diagnostics with the source line, column, and
highlighted token when compilation fails.

## CI

GitHub Actions runs both `scripts/smoke-test.sh` and
`scripts/smoke-test.ps1`. The smoke tests build `csrc/commonasmc.c`, check CLI
helpers, compile every example for the primary targets, compile representative
experimental targets, check stdin/stdout piping, and verify that diagnostics
highlight invalid source tokens. They also check that `-O1` removes simple
no-op instructions and folds adjacent constant arithmetic.

Local shortcuts:

```sh
make build
make version
make smoke
make examples
./scripts/run-example.sh
```

## GitHub Pages site

This repository includes a static project site in `docs/`. In GitHub Pages,
choose "Deploy from a branch", select `main`, and set the folder to `/docs`.

## Community files

- `CODE_OF_CONDUCT.md`: project behavior rules
- `CONTRIBUTING.md`: development and pull request guide
- `SECURITY.md`: vulnerability reporting policy
- `.github/ISSUE_TEMPLATE/`: issue forms
- `.github/pull_request_template.md`: pull request checklist

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

```sh
mkdir -p build
gcc csrc/commonasmc.c -o build/commonasmc
./build/commonasmc --version
./build/commonasmc --help
./build/commonasmc --list-targets
./build/commonasmc --target-info wasm
./build/commonasmc examples/hello.cas --target x86_64-nasm -o build/hello_x86.asm
./build/commonasmc examples/optimize.cas --target x86_64-nasm -O1 -o build/optimize_x86.asm
cat examples/hello.cas | ./build/commonasmc - --target wasm -o -
./build/commonasmc examples/hello.cas --target i386-nasm -o build/hello_i386.asm
./build/commonasmc examples/hello.cas --target riscv64-gnu -o build/hello_rv64.s
./build/commonasmc examples/hello.cas --target aarch64-gnu -o build/hello_aarch64.s
./build/commonasmc examples/hello.cas --target armv7a-gnu -o build/hello_armv7.s
./build/commonasmc examples/hello.cas --target mmixal -o build/hello_mmix.mms
./build/commonasmc examples/hello.cas --target dcpu16 -o build/hello_dcpu.dasm
./build/commonasmc examples/hello.cas --target fractran -o build/hello.fractran
./build/commonasmc examples/hello.cas --target cellular-automaton -o build/hello.ca
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
`cmp a, b` records the relation between `a` and `b` for the following
conditional jump. Signed jumps use `jg`/`jl`/`jge`/`jle`; unsigned jumps use
`ja`/`jb`/`jae`/`jbe`.

## CLI helpers

- `commonasmc input.cas --target x86_64-nasm -O1`: enable peephole
  optimization. `-O1` removes no-op arithmetic, folds adjacent
  `mov immediate` + integer operations, and drops overwritten pending
  immediates. `-O0` is the default.
- `commonasmc --version`: print the compiler version.
- `commonasmc --help`: print command usage.
- `commonasmc --list-targets`: print every supported target grouped by support
  style.
- `commonasmc --target-info wasm`: print one target's support level, output
  style, and portability note.
- `commonasmc - --target wasm -o -`: read CommonASM from stdin and write the
  lowered output to stdout.
- Unknown targets fail before the input file is compiled and suggest
  `--list-targets`.

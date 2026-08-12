# CommonASM

[![CI](https://github.com/jgchoimd-web/CommonASM/actions/workflows/ci.yml/badge.svg)](https://github.com/jgchoimd-web/CommonASM/actions/workflows/ci.yml)

![CommonASM official logo](docs/assets/commonasm-logo-composite.png)

![CommonASM icon logo](docs/assets/commonasm-logo-icon.png)

![CommonASM mascot](docs/assets/commonasm-logo-mascot.png)

CommonASM is a portable assembly IR that compiles into real assembly dialects.

- Official logo: `docs/assets/commonasm-logo-composite.png`
- Icon logo: `docs/assets/commonasm-logo-icon.png`
- Mascot: `docs/assets/commonasm-logo-mascot.png`

The project site is at https://jgchoimd-web.github.io/CommonASM/, built from
`docs/` by `.github/workflows/pages.yml` on every push that touches it.

It is meant as a middle layer for a future programming language compiler:

```text
your language -> CommonASM -> x86_64 / riscv64 / more backends later
```

## Supported targets

A target is in one of three lists below, and which one is decided by what CI
does with it rather than by how finished it feels.

`demos/` has three programs written in the language rather than about it: a
freestanding multiboot kernel, a guessing game on raw syscalls, and a sorting
program that the emulated machines all have to agree on. What writing them
changed is recorded in `demos/README.md`.

Executed on every commit — built, linked, and run under emulation, all
required to print the same thing:

- `x86_64-nasm`, `i386-nasm`
- `aarch64-gnu`, `armv7a-gnu`
- `riscv64-gnu`, `rv32i-gnu`
- `mips32-gnu`, `ppcg4-gnu`, `sparcv8-gnu`, `m68k`, `zarch`
- `loongarch64-gnu`, `alpha-gnu`, `parisc-gnu`, `superh`
- `wasm`, through node

Assembled on every commit — real assembly a real assembler accepts, sharing
the emitters above, but with nothing here to run them on:

- `ia32-nasm`, `rv64i-gnu`, `riscv64-zbb`
- `armv4-gnu`, `armv5-gnu`, `thumb-gnu`, `thumb2-gnu`
- `mips1-gnu`, `mips64-gnu`, `micromips-gnu`
- `power1-gnu`, `power2-gnu`, `ppc603-gnu`, `ppcg5-gnu`, `power9-gnu`, `power10-gnu`
- `sparcv9-gnu`, `coldfire`

Real assembly with nothing here to check it — the backend emits what the
machine's assembler takes, but Ubuntu has no cross-assembler for it, so CI
gets no further than building:

- `nios2`

Every machine this image can both assemble and emulate is now in the executed
list. What is left cannot be checked here: `microblaze`, `xtensa` and `nios2`
have an emulator and no cross-assembler; `arc` and `ia64` have an assembler
and no emulator; `rv128i-gnu` has no assembler anywhere; and every GPU and DSP
target on the list, along with `avr`, `i8051` and `msp430`, has no Linux
system calls to lower onto at all.

Pseudo assembly — readable text in that machine's shape, for reading and for
porting from, not for assembling:

- `rv128i-gnu`, `ia64-gnu`, `m88k-gnu`
- `avr`, `i8051`, `msp430`, `xtensa`, `rx`, `microblaze`, `arc`
- `ptx`, `amdgcn`, `rdna`, `intelgen`, `cell-spe`, `tms320`, `dsp56000`, `blackfin`, `hexagon`, `ebpf`
- `llvm-ir`, `gcc-gimple`, `gcc-rtl`, `jvm-bytecode`, `cil`, `dalvik`, `lua-bytecode`, `python-bytecode`, `spirv`, `evm`
- `mmixal`, `dcpu16`

Encoding/pseudo:

- `mos6502`, `wdc65c02`, `wdc65816`, `mos6510`, `i8008`, `i8080`, `i8085`, `z80`, `ez80`, `m6800`, `m6809`
- `pic16`, `pic32`, `propeller`
- `pdp1`, `pdp8`, `pdp11`, `vax`, `system360`, `system370`, `cdc6600`, `univac1`, `cray1`
- `mix`, `lc3`, `lmc`, `marie`, `chip8`, `schip8`, `redcode`, `subleq`, `urisc`, `tta`
- `fractran`, `iota`, `jot`, `malbolge-asm`, `brainfuck`, `secd`, `pcode`, `zmachine`, `sweet16`, `befunge`, `bitblt-vm`, `turing-machine`, `cellular-automaton`, `unlambda`

Everything above the pseudo line is portable-subset output, not a complete
ABI-level port. Pseudo and encoding targets use comments, toy assembly, or
source encodings when the machine model does not match Linux syscalls or
random-access memory.

`--list-targets` marks the pseudo ones, and `--target-info TARGET` says which
of the three a target is in. Both ask the target rather than consulting a
list, so neither can go stale when a backend is added.

## Compiler implementations

`csrc/commonasmc.c` is the compiler, and the only one. It prints ANSI-colored
diagnostics with the source line, column, and highlighted token when
compilation fails.

`selfhost/compiler.cal` is a design sketch for a self-hosting compiler. The
language it is written in has no implementation, nothing builds or checks the
file, and its target list predates most of the backends — see
[`selfhost/README.md`](selfhost/README.md). Do not read it as a second
implementation.

## CI

GitHub Actions runs both `scripts/smoke-test.sh` and `scripts/smoke-test.ps1`
on every commit. They check four different things, in rising order of what
they can prove:

1. **It compiles.** Every example, test and demo is built for every target, at
   both optimization levels and in both extended-operation modes — 3876
   combinations. This catches a backend that refuses an instruction it should
   accept.
2. **A real assembler accepts it.** NASM takes the x86-64 and i386 output with
   `-w+error=number-overflow`, so a truncated immediate is an error rather
   than a warning; clang takes AArch64, ARMv7 and Thumb-2; the GNU assembler
   takes RISC-V, MIPS, PowerPC, SPARC, m68k, z/Architecture, LoongArch, Alpha,
   PA-RISC and SuperH; and wat2wasm
   validates the wasm module.
3. **It runs and gets the right answer.** Two programs are built for every
   machine there is an emulator for, linked, run under `qemu-user`, and all of
   them have to print the same line — twenty-four runs, plus both programs
   under node as wasm. `tests/exec-kernel.cas` uses arithmetic, all three
   shifts, the extended operations, the atomic ones, a loop with a comparison,
   byte memory access, a call with a stack, and syscalls;
   `demos/sort/sort.cas` is a sorting program, with a loop inside a loop, a
   signed comparison in the inner one, and negative numbers to print. That is
   thirty runs.
   Assembling only says the text was well-formed; this is what catches a
   backend that assembles perfectly and computes the wrong number. Every
   backend bug found since it was added had passed step 2 — including one that
   meant a function could not call a function on seven machines.
4. **It reads back.** Each family's assembly is lifted into CommonASM and
   required to match the source it came from, and the extended operations are
   run against a C reference both natively and expanded.

Tests that are supposed to fail are checked to actually fail, so a test that
has stopped testing anything is caught too.

Local shortcuts:

```sh
make build
make version
make smoke
make examples
./scripts/run-example.sh
```

## GitHub Pages site

The site is `docs/`, published at
<https://jgchoimd-web.github.io/CommonASM/>. `.github/workflows/pages.yml`
deploys it on every push that touches `docs/`, so what is on the site is what
is in the repository, and the deployment is visible in Actions rather than
hidden in a settings page. The workflow checks that every asset the page names
is actually present before publishing.

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
- `syscall rD, read, fd, buffer, length`

A syscall may name a register first, which receives its result — how many
bytes a read delivered, the descriptor an open returned, or a negative errno.
Without it there is no way to tell a short read from a full one:

```asm
  syscall r4, read, stdin, buf, 16
  cmp r4, 0
  jle end_of_input
```

Virtual registers are `r0` through `r15`. A backend never maps one onto a
register it needs for itself: the stack and frame pointers, its scratch
registers, and the registers the syscall convention writes are all held back.
Where a machine has fewer registers left than the sixteen the language
promises, the surplus live in spill slots the compiler reserves, so `r15`
behaves the same everywhere instead of aliasing whatever happened to be
sixteenth in a table.

| target | in registers | in spill slots |
| --- | --- | --- |
| `aarch64-gnu` | `r0`-`r15` | none |
| `riscv64-gnu` | `r0`-`r15` | none |
| `x86_64-nasm` | `r0`-`r11` | `r12`-`r15` |
| `armv7a-gnu` | `r0`-`r9` | `r10`-`r15` |
| `i386-nasm` | `r0`-`r3` | `r4`-`r15` |

Virtual registers keep their values across everything the compiler emits,
including `syscall`, which saves and restores whatever the call would
otherwise overwrite.

A load narrower than the register sign-extends: `load.b`, `load.w` and
`load.d` all read a signed value. This is the only rule under which a 32-bit
machine and a 64-bit one agree, since `load.d` fills the register on the first
and leaves half of it to fill on the second. Everything else in the language
is signed too — `mul`, `div`, `cmp` and `sar` all are — so this is the rule
that matches them. Read an unsigned byte with `load.b` followed by
`and rD, 0xff`.

## Extended operations

A portable assembly that can only reach the instructions every machine shares
is just a slow mid-level language. These operations exist so that a program
can use what a machine actually has: each one becomes the target's own
instruction where there is one, and expands into ordinary CommonASM where
there is not. Nothing has to use them, and using them never costs portability.

- `popcnt rD`: number of set bits
- `clz rD`: leading zeros, the word width for zero
- `ctz rD`: trailing zeros, the word width for zero
- `bswap rD`: reverse byte order
- `rol rD, n` / `ror rD, n`: rotate, by a constant or a register
- `min rD, x` / `max rD, x`: signed, against a register or an immediate
- `abs rD`: signed magnitude; the most negative value is left alone, since
  negating it would not fit
- `sel rD, a, b`: `rD` is the condition on the way in and the result on the
  way out — `a` if it was non-zero, `b` if it was zero

What that means in practice, for `popcnt`:

| target | emitted |
| --- | --- |
| `riscv64-zbb` | `cpop t0, t0` |
| `aarch64-gnu` | `fmov`, `cnt`, `addv`, `fmov` |
| `x86_64-nasm` | `popcnt rbx, rbx` |
| `riscv64-gnu` | 22 instructions, no bit-manipulation extension needed |

and for `min`:

| target | emitted |
| --- | --- |
| `riscv64-zbb` | `min s2, s2, s3` |
| `aarch64-gnu` | `cmp`, `csel ..., le` |
| `x86_64-nasm` | `cmp`, `cmovg` |
| everything else | a compare and a branch over one move |

### Atomics

Four more, which follow the same rule with one difference worth stating
plainly: what a machine without the instruction loses is not speed, it is
indivisibility.

- `fence`: a full memory barrier
- `atomic_add rD, [addr]`: adds `rD` to the word at `addr` and answers with
  what was there before
- `atomic_xchg rD, [addr]`: puts `rD` there and answers with what was there
- `cas rD, [addr], rNew`: replaces the word with `rNew` if it still equals
  `rD`, and answers with what was there either way, so comparing `rD` against
  what it was says whether the swap happened

| target | `atomic_add` |
| --- | --- |
| `x86_64-nasm`, `i386-nasm` | `lock xadd` |
| `aarch64-gnu` | `ldaddal` |
| `riscv64-gnu` | `amoadd.d` |
| `loongarch64-gnu` | `amadd_db.d` |
| `zarch` | `laag` |
| `m68k` | a `cas.l` retry loop |
| `mips32-gnu` | an `ll`/`sc` retry loop |
| `ppcg4-gnu` | an `lwarx`/`stwcx.` retry loop |
| `armv4-gnu`..`armv7a-gnu`, `sparcv8-gnu` | a load, an add and a store |

The AArch64 instructions are the ones ARMv8.1 added, so a program that uses
one raises the floor of its own output to that — announced by an `.arch`
directive the output only carries when an atomic is in it. The alternative
would be a load-exclusive and store-exclusive loop, which needs a scratch
register that backend has not got: all sixteen virtual registers live in
machine registers there.

That last row is a real difference, not a slower way of doing the same thing.
It is correct for a program with one thread and wrong for one with two, so
`--target-info` says which a target is in as many words:

```text
atomics: indivisible, using the machine's own instructions
atomics: a plain load, change and store; right for one thread only
```

An extended operation may leave the flags in any state, so a `cmp` does not
survive across one. That has always been true — the expansions are arithmetic,
and arithmetic sets flags — and it is worth saying out loud now that some of
them compare on purpose.

`commonasmc --target-info TARGET` reports which are native there and which
expand. `--emulate-extended` forces the expansion everywhere, which is what to
use for a CPU that lacks the optional instruction its architecture defines --
an x86-64 without POPCNT, say.

Both paths are checked by running them: `tests/extended-kernel.cas` is built
twice, once each way, linked against `tests/extended-driver.c` and required to
agree with a reference implementation on every input.

## Inline assembly

For everything the extended operations do not cover, an `asm` block goes into
the output exactly as written. Nothing in it is parsed or optimised, so a
program can reach any instruction its machine has.

```asm
  mov r0, 7

  asm x86_64 {
    lea {r0}, [{r0} + {r0} * 2]
  }
  asm aarch64 {
    add {r0}, {r0}, {r0}, lsl #1
  }
  asm portable {
    mov r1, 3
    mul r0, r1
  }
```

`{r0}` is replaced by whatever the backend put virtual register `r0` in, so
the block is wired to the code around it instead of guessing at registers.

Consecutive blocks are alternatives for one spot: the first whose selector
matches is emitted and the rest are skipped. **A run where nothing matches is
an error**, so a hand-written fast path cannot silently become a hole on a
target nobody wrote an arm for.

A selector is one of:

| selector | matches |
| --- | --- |
| a target name, e.g. `x86_64-nasm` | that target only |
| a family: `x86_64`, `i386`, `riscv64`, `aarch64`, `arm32`, `mmix`, `dcpu16` | every target in it |
| `any` | everything; body is still verbatim assembly |
| `portable` | everything; body is **CommonASM**, compiled normally |

`portable` is what makes the fast path optional: write the machine's own
instructions for the targets worth hand-writing, and ordinary CommonASM for
the rest.

Two limits are worth knowing. A block is opaque to the compiler, so anything
it was holding back — a buffered constant, a pending RISC-V comparison — is
settled before the block and not carried across it. And `{rN}` needs a
register the target can name: where a virtual register lives in a spill slot,
x86 can still name it as a memory operand, but ARM cannot, and says so.

### Lifting a block to another machine

`--translate-asm` changes what happens when no arm matches. Instead of the
error, the first block in the run is read back as assembly, turned into the
CommonASM it came from, and lowered for the target actually being compiled:

```
$ commonasmc prog.cas --target riscv64-gnu --translate-asm
```

```asm
  asm x86_64 {              # written only for x86
    lea {r1}, [{r0}]
    imul {r1}, 3
    popcnt {r2}, {r1}
  }
```

becomes, on AArch64, `mul` followed by the `fmov`/`cnt`/`addv` population
count; on RISC-V without the bit-manipulation extension, `mul` followed by the
expanded fold. This works because a block names its registers as `{rN}`: the
operands are already the compiler's, so a recognised instruction maps straight
back to the operation it came from.

Recognised are the ordinary arithmetic, logic, shift, rotate, compare and
stack instructions of x86, ARM, AArch64, RISC-V and MIPS, the bit operations
(`popcnt`/`cpop`, `lzcnt`/`clz`, `tzcnt`/`ctz`, `bswap`/`rev`/`rev8`), ARM's
shifted third operand, and x86's `lea` as the arithmetic it usually stands
for. Anything else is reported by name rather than guessed at — the option
does not turn an unreadable block into a plausible one.

`examples/inline.cas` shows both forms. The block in
`tests/extended-kernel.cas` is checked by running it, and every block in
`tests/translate-kernel.cas` is written for AArch64 so that an x86-64 build
has to lift all of them; running that is what shows the lifting preserved
what the blocks meant.

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

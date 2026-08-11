#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build"
CC=${CC:-gcc}

mkdir -p "$BUILD_DIR"

"$CC" -std=c99 -Wall -Wextra -pedantic -O2 \
  "$ROOT_DIR/csrc/commonasmc.c" \
  -o "$BUILD_DIR/commonasmc"

"$BUILD_DIR/commonasmc" --help > "$BUILD_DIR/help.txt"
"$BUILD_DIR/commonasmc" --version > "$BUILD_DIR/version.txt"
"$BUILD_DIR/commonasmc" --list-targets > "$BUILD_DIR/targets.txt"
"$BUILD_DIR/commonasmc" --target-info wasm > "$BUILD_DIR/wasm-info.txt"
"$BUILD_DIR/commonasmc" --target-info brainfuck > "$BUILD_DIR/brainfuck-info.txt"

grep -q "commonasmc --list-targets" "$BUILD_DIR/help.txt"
grep -q "commonasmc --target-info TARGET" "$BUILD_DIR/help.txt"
grep -q "commonasmc --version" "$BUILD_DIR/help.txt"
grep -q -- "-O1" "$BUILD_DIR/help.txt"
grep -qiE "^commonasmc( version)? [0-9A-Za-z.+_-]+" "$BUILD_DIR/version.txt"
grep -q "x86_64-nasm" "$BUILD_DIR/targets.txt"
grep -q "riscv64-gnu" "$BUILD_DIR/targets.txt"
grep -q "brainfuck" "$BUILD_DIR/targets.txt"
grep -q "cellular-automaton" "$BUILD_DIR/targets.txt"
grep -q "support: VM/IR" "$BUILD_DIR/wasm-info.txt"
grep -q "support: Encoding/pseudo" "$BUILD_DIR/brainfuck-info.txt"

if "$BUILD_DIR/commonasmc" "$ROOT_DIR/examples/hello.cas" --target no-such-target -o "$BUILD_DIR/nope.out" 2> "$BUILD_DIR/unknown-target.txt"; then
  echo "expected unknown target to fail"
  exit 1
fi
grep -q "unknown target" "$BUILD_DIR/unknown-target.txt"

if "$BUILD_DIR/commonasmc" --target-info no-such-target 2> "$BUILD_DIR/unknown-info.txt"; then
  echo "expected unknown target info to fail"
  exit 1
fi
grep -q "unknown target" "$BUILD_DIR/unknown-info.txt"

"$BUILD_DIR/commonasmc" - --target wasm -o - < "$ROOT_DIR/examples/hello.cas" > "$BUILD_DIR/stdout.wat"
grep -q "wasm.syscall write" "$BUILD_DIR/stdout.wat"

for example in "$ROOT_DIR"/examples/*.cas; do
  name=$(basename "$example" .cas)
  for target in x86_64-nasm riscv64-gnu; do
    "$BUILD_DIR/commonasmc" "$example" --target "$target" -o "$BUILD_DIR/${name}-${target}.out"
  done
done

# RISC-V has no flags, so a compare is folded into its branch rather than
# staged in a register pair that later instructions would overwrite.
grep -q "li s11, 42" "$BUILD_DIR/control-riscv64-gnu.out"
grep -q "beq t2, s11, success" "$BUILD_DIR/control-riscv64-gnu.out"

"$BUILD_DIR/commonasmc" "$ROOT_DIR/examples/optimize.cas" --target x86_64-nasm -O0 -o "$BUILD_DIR/optimize-O0.asm"
"$BUILD_DIR/commonasmc" "$ROOT_DIR/examples/optimize.cas" --target x86_64-nasm -O1 -o "$BUILD_DIR/optimize-O1.asm"
grep -q "mov rbx, 42" "$BUILD_DIR/optimize-O1.asm"
grep -q "mov rdx, 0" "$BUILD_DIR/optimize-O1.asm"
grep -q "add rbx, 0" "$BUILD_DIR/optimize-O0.asm"
if grep -q "add rbx, 0" "$BUILD_DIR/optimize-O1.asm" ||
   grep -q "mov rcx, rcx" "$BUILD_DIR/optimize-O1.asm" ||
   grep -q "imul rsi, 1" "$BUILD_DIR/optimize-O1.asm"; then
  echo "optimizer left removable instructions in -O1 output"
  exit 1
fi

# Each check below covers a lowering that used to produce silently wrong or
# unassemblable code.
cat > "$BUILD_DIR/regress.cas" <<'CAS'
const stdout = 1

.data
a_label_long_enough_that_its_length_constant_used_to_be_truncated: string "hi\n"

.bss
cell: zero 8

.text
global _start

_start:
  mov r13, 111
  mov r15, 222
  store.q [cell], 5
  shl r0, r1
  sub r2, -5
  div r4, r5
  cmp r3, 42
  syscall write, stdout, a_label_long_enough_that_its_length_constant_used_to_be_truncated, a_label_long_enough_that_its_length_constant_used_to_be_truncated_len
  je done
done:
  ret
CAS

"$BUILD_DIR/commonasmc" "$BUILD_DIR/regress.cas" --target x86_64-nasm -o "$BUILD_DIR/regress-x86.asm"
"$BUILD_DIR/commonasmc" "$BUILD_DIR/regress.cas" --target riscv64-gnu -o "$BUILD_DIR/regress-rv.s"

# A virtual register must never land on the stack or frame pointer.
if grep -qE "^  (mov|add|sub|xor|and|or|imul|neg|not|inc|dec|shl|shr|sar) (rsp|rbp)[,$]" "$BUILD_DIR/regress-x86.asm"; then
  echo "a virtual register was lowered onto rsp or rbp"
  exit 1
fi
# Virtual registers past the machine register file need backing store.
grep -q "__cas_spill: resq" "$BUILD_DIR/regress-x86.asm"
grep -q "mov qword \[rel __cas_spill+8\], 111" "$BUILD_DIR/regress-x86.asm"
# A variable shift count has to reach cl; "shl reg, reg" does not assemble.
grep -q "shl rax, cl" "$BUILD_DIR/regress-x86.asm"
# idiv overwrites rdx, which carries a virtual register.
grep -q "push rdx" "$BUILD_DIR/regress-x86.asm"

# Subtracting a negative immediate used to emit "addi t2, t2, --5".
grep -q "addi t2, t2, 5" "$BUILD_DIR/regress-rv.s"
if grep -q -- "--" "$BUILD_DIR/regress-rv.s"; then
  echo "riscv lowering emitted a doubled sign"
  exit 1
fi
# A long label keeps its generated length constant, so it stays an immediate
# ("li") rather than being mistaken for an address ("la").
grep -q "li a2, a_label_long_enough_that_its_length_constant_used_to_be_truncated_len" "$BUILD_DIR/regress-rv.s"
# A compare must not be carried in a register the syscall sequence writes.
if grep -qE "^  b(eq|ne|lt|ge|gt|le)u? a[0-7]," "$BUILD_DIR/regress-rv.s"; then
  echo "riscv compare was carried in a syscall argument register"
  exit 1
fi

# Whether the output assembles is checked directly, not just grepped for, on
# every backend an assembler is available for.
# Truncation is promoted to an error: an immediate too wide for the encoding
# assembles with only a warning otherwise, and silently loses its top bits.
NASM_STRICT="-w+error=number-overflow"

if command -v nasm > /dev/null 2>&1; then
  for example in "$ROOT_DIR"/examples/*.cas "$BUILD_DIR/regress.cas"; do
    name=$(basename "$example" .cas)
    for level in -O0 -O1; do
      # Compiled twice: once letting the target use its own instructions for
      # the extended operations, once with them all expanded.
      for mode in native emulated; do
        if [ "$mode" = emulated ]; then ext=--emulate-extended; else ext=; fi
        "$BUILD_DIR/commonasmc" "$example" --target x86_64-nasm "$level" $ext \
          -o "$BUILD_DIR/asm-${mode}-${name}${level}.asm"
        nasm -f elf64 $NASM_STRICT "$BUILD_DIR/asm-${mode}-${name}${level}.asm" \
          -o "$BUILD_DIR/asm-${mode}-${name}${level}.o"
        "$BUILD_DIR/commonasmc" "$example" --target i386-nasm "$level" $ext \
          -o "$BUILD_DIR/asm32-${mode}-${name}${level}.asm"
        nasm -f elf32 $NASM_STRICT "$BUILD_DIR/asm32-${mode}-${name}${level}.asm" \
          -o "$BUILD_DIR/asm32-${mode}-${name}${level}.o"
      done
    done
  done
  echo "nasm accepted every x86_64 and i386 output, native and expanded."
else
  echo "nasm not found; skipped assembling the x86_64 and i386 output."
fi

# The strongest check available: build the extended operations both ways and
# run them against a reference implementation.
if command -v nasm > /dev/null 2>&1 && [ "$(uname -m)" = "x86_64" ]; then
  for mode in native emulated; do
    if [ "$mode" = emulated ]; then ext=--emulate-extended; else ext=; fi
    "$BUILD_DIR/commonasmc" "$ROOT_DIR/tests/extended-kernel.cas" --target x86_64-nasm $ext \
      -o "$BUILD_DIR/extkernel-$mode.asm"
    nasm -f elf64 $NASM_STRICT "$BUILD_DIR/extkernel-$mode.asm" -o "$BUILD_DIR/extkernel-$mode.o"
    "$CC" "$ROOT_DIR/tests/extended-driver.c" "$BUILD_DIR/extkernel-$mode.o" \
      -o "$BUILD_DIR/extrun-$mode"
    echo "  extended operations, $mode:"
    "$BUILD_DIR/extrun-$mode"
  done
  echo "extended operations agree with the reference, native and expanded."
else
  echo "skipped running the extended operation checks."
fi

if command -v riscv64-linux-gnu-as > /dev/null 2>&1; then
  for example in "$ROOT_DIR"/examples/*.cas "$BUILD_DIR/regress.cas"; do
    name=$(basename "$example" .cas)
    for target in riscv64-gnu riscv64-zbb; do
      "$BUILD_DIR/commonasmc" "$example" --target "$target" -o "$BUILD_DIR/rv-${target}-${name}.s"
      riscv64-linux-gnu-as -o "$BUILD_DIR/rv-${target}-${name}.o" "$BUILD_DIR/rv-${target}-${name}.s"
    done
  done
  echo "the RISC-V assembler accepted every riscv64-gnu and riscv64-zbb output."
else
  echo "no RISC-V assembler found; skipped assembling the RISC-V output."
fi

if command -v mips-linux-gnu-as > /dev/null 2>&1; then
  for example in "$ROOT_DIR"/examples/*.cas "$BUILD_DIR/regress.cas"; do
    name=$(basename "$example" .cas)
    for target in mips1-gnu mips32-gnu micromips-gnu; do
      "$BUILD_DIR/commonasmc" "$example" --target "$target" -o "$BUILD_DIR/mips-${target}-${name}.s"
      mips-linux-gnu-as -o "$BUILD_DIR/mips-${target}-${name}.o" "$BUILD_DIR/mips-${target}-${name}.s"
    done
  done
  echo "the MIPS assembler accepted every 32-bit MIPS output."
else
  echo "no MIPS assembler found; skipped assembling the MIPS output."
fi

# mips64-gnu needs an assembler whose ABI is actually 64-bit: the 32-bit one
# rejects a 64-bit immediate even with -march=mips64.
if command -v mips64-linux-gnuabi64-as > /dev/null 2>&1; then
  for example in "$ROOT_DIR"/examples/*.cas "$BUILD_DIR/regress.cas"; do
    name=$(basename "$example" .cas)
    "$BUILD_DIR/commonasmc" "$example" --target mips64-gnu -o "$BUILD_DIR/mips64-${name}.s"
    mips64-linux-gnuabi64-as -o "$BUILD_DIR/mips64-${name}.o" "$BUILD_DIR/mips64-${name}.s"
  done
  echo "the 64-bit MIPS assembler accepted every mips64-gnu output."
else
  echo "no 64-bit MIPS assembler found; skipped assembling the mips64-gnu output."
fi

if command -v clang > /dev/null 2>&1; then
  for example in "$ROOT_DIR"/examples/*.cas "$BUILD_DIR/regress.cas"; do
    name=$(basename "$example" .cas)
    for level in -O0 -O1; do
      "$BUILD_DIR/commonasmc" "$example" --target aarch64-gnu "$level" -o "$BUILD_DIR/a64-${name}${level}.s"
      clang -cc1as -triple aarch64-unknown-linux-gnu -filetype obj \
        -o "$BUILD_DIR/a64-${name}${level}.o" "$BUILD_DIR/a64-${name}${level}.s"
      "$BUILD_DIR/commonasmc" "$example" --target armv7a-gnu "$level" -o "$BUILD_DIR/arm-${name}${level}.s"
      clang -cc1as -triple armv7-unknown-linux-gnueabi -filetype obj \
        -o "$BUILD_DIR/arm-${name}${level}.o" "$BUILD_DIR/arm-${name}${level}.s"
      "$BUILD_DIR/commonasmc" "$example" --target thumb2-gnu "$level" -o "$BUILD_DIR/thumb-${name}${level}.s"
      clang -cc1as -triple thumbv7-unknown-linux-gnueabi -filetype obj \
        -o "$BUILD_DIR/thumb-${name}${level}.o" "$BUILD_DIR/thumb-${name}${level}.s"
    done
  done
  echo "clang accepted every aarch64, armv7a and thumb2 output."
else
  echo "clang not found; skipped assembling the aarch64 and ARM output."
fi

targets="
  i386-nasm
  aarch64-gnu
  armv7a-gnu
  rv32i-gnu
  rv128i-gnu
  mips32-gnu
  ppcg4-gnu
  sparcv9-gnu
  m68k
  avr
  z80
  pdp11
  ptx
  ebpf
  wasm
  llvm-ir
  jvm-bytecode
  chip8
  subleq
  brainfuck
  mmixal
  dcpu16
  fractran
  cellular-automaton
"

for target in $targets; do
  "$BUILD_DIR/commonasmc" "$ROOT_DIR/examples/legacy.cas" --target "$target" -o "$BUILD_DIR/legacy-${target}.out"
done

cat > "$BUILD_DIR/bad.cas" <<'CAS'
.text
global _start

_start:
  mov r99, 1
CAS

if "$BUILD_DIR/commonasmc" "$BUILD_DIR/bad.cas" --target x86_64-nasm -o "$BUILD_DIR/bad.out" 2> "$BUILD_DIR/diagnostic.txt"; then
  echo "expected invalid register to fail"
  exit 1
fi

grep -q "expected virtual register r0-r15" "$BUILD_DIR/diagnostic.txt"
grep -q "bad.cas:5" "$BUILD_DIR/diagnostic.txt"
grep -q "r99" "$BUILD_DIR/diagnostic.txt"

if find "$BUILD_DIR" -type f \( -name "*.out" -o -name "commonasmc" \) -size 0 -print -quit | grep -q .; then
  echo "found empty build output"
  exit 1
fi

echo "CommonASM smoke tests passed."

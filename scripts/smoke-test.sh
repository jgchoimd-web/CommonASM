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
grep -q "(module" "$BUILD_DIR/stdout.wat"
grep -q "call \$fd_write" "$BUILD_DIR/stdout.wat"

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

# Multiplying a value the compiler cannot see by a power of two is a shift.
cat > "$BUILD_DIR/strength.cas" <<'CAS'
.bss
v: zero 8
.text
global _start
_start:
  load.q r0, [v]
  mul r0, 16
  store.q [v], r0
  load.q r1, [v]
  mul r1, 24
  store.q [v], r1
  syscall exit, 0
CAS
"$BUILD_DIR/commonasmc" "$BUILD_DIR/strength.cas" --target x86_64-nasm -O1 -o "$BUILD_DIR/strength.asm"
grep -q "shl rbx, 4" "$BUILD_DIR/strength.asm"
# and one that is not a power of two stays a multiply
grep -q "imul rcx, 24" "$BUILD_DIR/strength.asm"
if grep -q "imul rbx, 16" "$BUILD_DIR/strength.asm"; then
  echo "optimizer left a multiply by a power of two in -O1 output"
  exit 1
fi
# Folding a constant still reaches through the shift the multiply became,
# including when the constant is negative.
printf '.text\nglobal _start\n_start:\n  mov r0, -7\n  mul r0, 8\n  syscall exit, 0\n' \
  > "$BUILD_DIR/foldneg.cas"
"$BUILD_DIR/commonasmc" "$BUILD_DIR/foldneg.cas" --target x86_64-nasm -O1 -o "$BUILD_DIR/foldneg.asm"
grep -q "mov rbx, -56" "$BUILD_DIR/foldneg.asm"

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

  # Every block in this kernel is written for AArch64, so an x86-64 build has
  # to lift all of them. Running it is what shows the lifting kept the meaning.
  "$BUILD_DIR/commonasmc" "$ROOT_DIR/tests/translate-kernel.cas" --target x86_64-nasm \
    --translate-asm -o "$BUILD_DIR/translate-kernel.asm"
  nasm -f elf64 $NASM_STRICT "$BUILD_DIR/translate-kernel.asm" -o "$BUILD_DIR/translate-kernel.o"
  "$CC" "$ROOT_DIR/tests/translate-driver.c" "$BUILD_DIR/translate-kernel.o" \
    -o "$BUILD_DIR/translate-run"
  echo "  lifted AArch64 blocks running on x86-64:"
  "$BUILD_DIR/translate-run"

  # Without the option the same source must be rejected rather than quietly
  # losing the blocks.
  if "$BUILD_DIR/commonasmc" "$ROOT_DIR/tests/translate-kernel.cas" --target x86_64-nasm \
       -o "$BUILD_DIR/translate-nope.asm" 2> "$BUILD_DIR/translate-nope.txt"; then
    echo "expected an unmatched asm run to fail without --translate-asm"
    exit 1
  fi
  grep -q "no asm block" "$BUILD_DIR/translate-nope.txt"
  echo "an unmatched asm run still fails without --translate-asm."

  # A whole assembly file read back into CommonASM and compiled again has to
  # land on the assembly it started from, for every family that can be read.
  cat > "$BUILD_DIR/roundtrip.cas" <<'CAS'
.text
global compute

compute:
  mov r0, 7
  mul r0, 3
  add r0, 5
  sub r0, 2
  and r0, 255
  xor r1, r1
  or r1, r0
  shl r1, 2
  shr r1, 1
  mov r2, r1
  neg r2
  not r2
  inc r0
  dec r0
  cmp r0, 26
  je done
  jne other
  mov r1, 0
other:
  mov r2, 1
done:
  ret
CAS
  for pair in "x86_64:x86_64-nasm" "i386:i386-nasm" "aarch64:aarch64-gnu" \
              "arm32:armv7a-gnu" "riscv64:riscv64-gnu" "mips:mips32-gnu"; do
    family=${pair%%:*}
    rt_target=${pair##*:}
    "$BUILD_DIR/commonasmc" "$BUILD_DIR/roundtrip.cas" --target "$rt_target" \
      -o "$BUILD_DIR/rt-${family}-1.s"
    "$BUILD_DIR/commonasmc" "$BUILD_DIR/rt-${family}-1.s" --from "$family" --emit-cas \
      -o "$BUILD_DIR/rt-${family}.cas"
    "$BUILD_DIR/commonasmc" "$BUILD_DIR/rt-${family}.cas" --target "$rt_target" \
      -o "$BUILD_DIR/rt-${family}-2.s"
    if ! cmp -s "$BUILD_DIR/rt-${family}-1.s" "$BUILD_DIR/rt-${family}-2.s"; then
      echo "$family assembly did not survive being read back"
      exit 1
    fi
  done
  echo "every family's assembly reads back into the CommonASM it came from."

  # The demos are programs rather than test cases, so they are built the way a
  # user would build them, and the game is run far enough to reach the read
  # that reports end of input.
  for demo_target in x86_64-nasm:elf64 i386-nasm:elf32; do
    demo_name=${demo_target%%:*}
    demo_fmt=${demo_target##*:}
    for demo in "$ROOT_DIR"/demos/os/kernel.cas "$ROOT_DIR"/demos/game/guess.cas; do
      demo_base=$(basename "$demo" .cas)
      "$BUILD_DIR/commonasmc" "$demo" --target "$demo_name" -O1 \
        -o "$BUILD_DIR/demo-$demo_base-$demo_fmt.asm"
      nasm -f "$demo_fmt" $NASM_STRICT "$BUILD_DIR/demo-$demo_base-$demo_fmt.asm" \
        -o "$BUILD_DIR/demo-$demo_base-$demo_fmt.o"
    done
  done

  # The game makes Linux syscalls directly, so it can only be run on Linux.
  if [ "$(uname -s)" = "Linux" ] && command -v ld > /dev/null 2>&1; then
    ld "$BUILD_DIR/demo-guess-elf64.o" -o "$BUILD_DIR/guess"
    printf '50\n' | "$BUILD_DIR/guess" > "$BUILD_DIR/guess.out" 2>&1 || true
    grep -q "Guess a number" "$BUILD_DIR/guess.out"
    grep -qE "Higher|Lower|Correct" "$BUILD_DIR/guess.out"
    echo "the guessing game ran and stopped when the read reported no input."
  else
    echo "not Linux; skipped running the guessing game."
  fi

  ld -m elf_i386 -T "$ROOT_DIR/demos/os/link.ld" "$BUILD_DIR/demo-kernel-elf32.o" \
    -o "$BUILD_DIR/kernel.elf" 2> /dev/null && echo "the kernel links as a multiboot image."
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
    # The same assembler builds rv32 when told which machine it is. The
    # multiply and divide are the M extension, which the target has always
    # emitted and which the bare name rv32i does not promise.
    "$BUILD_DIR/commonasmc" "$example" --target rv32i-gnu -o "$BUILD_DIR/rv-rv32i-${name}.s"
    riscv64-linux-gnu-as -march=rv32im -mabi=ilp32 \
      -o "$BUILD_DIR/rv-rv32i-${name}.o" "$BUILD_DIR/rv-rv32i-${name}.s"
  done
  echo "the RISC-V assembler accepted every riscv64-gnu, riscv64-zbb and rv32i-gnu output."
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

if command -v powerpc-linux-gnu-as > /dev/null 2>&1; then
  for example in "$ROOT_DIR"/examples/*.cas "$BUILD_DIR/regress.cas"; do
    name=$(basename "$example" .cas)
    for target in power1-gnu power2-gnu ppc603-gnu ppcg4-gnu; do
      "$BUILD_DIR/commonasmc" "$example" --target "$target" -o "$BUILD_DIR/ppc-${target}-${name}.s"
      powerpc-linux-gnu-as -o "$BUILD_DIR/ppc-${target}-${name}.o" "$BUILD_DIR/ppc-${target}-${name}.s"
    done
  done
  echo "the PowerPC assembler accepted every 32-bit PowerPC output."
else
  echo "no PowerPC assembler found; skipped assembling the PowerPC output."
fi

if command -v powerpc64-linux-gnu-as > /dev/null 2>&1; then
  for example in "$ROOT_DIR"/examples/*.cas "$BUILD_DIR/regress.cas"; do
    name=$(basename "$example" .cas)
    for target in ppcg5-gnu power9-gnu power10-gnu; do
      "$BUILD_DIR/commonasmc" "$example" --target "$target" -o "$BUILD_DIR/ppc64-${target}-${name}.s"
      powerpc64-linux-gnu-as -o "$BUILD_DIR/ppc64-${target}-${name}.o" "$BUILD_DIR/ppc64-${target}-${name}.s"
    done
  done
  echo "the 64-bit PowerPC assembler accepted every 64-bit PowerPC output."
else
  echo "no 64-bit PowerPC assembler found; skipped assembling it."
fi

# One SPARC assembler covers both, told which width to use.
if command -v sparc64-linux-gnu-as > /dev/null 2>&1; then
  for example in "$ROOT_DIR"/examples/*.cas "$BUILD_DIR/regress.cas"; do
    name=$(basename "$example" .cas)
    "$BUILD_DIR/commonasmc" "$example" --target sparcv8-gnu -o "$BUILD_DIR/sparc8-${name}.s"
    sparc64-linux-gnu-as -32 -o "$BUILD_DIR/sparc8-${name}.o" "$BUILD_DIR/sparc8-${name}.s"
    "$BUILD_DIR/commonasmc" "$example" --target sparcv9-gnu -o "$BUILD_DIR/sparc9-${name}.s"
    sparc64-linux-gnu-as -64 -Av9 -o "$BUILD_DIR/sparc9-${name}.o" "$BUILD_DIR/sparc9-${name}.s"
  done
  echo "the SPARC assembler accepted every sparcv8-gnu and sparcv9-gnu output."
else
  echo "no SPARC assembler found; skipped assembling the SPARC output."
fi

if command -v m68k-linux-gnu-as > /dev/null 2>&1; then
  for example in "$ROOT_DIR"/examples/*.cas "$BUILD_DIR/regress.cas"; do
    name=$(basename "$example" .cas)
    for target in m68k coldfire; do
      "$BUILD_DIR/commonasmc" "$example" --target "$target" -o "$BUILD_DIR/m68k-${target}-${name}.s"
      m68k-linux-gnu-as -o "$BUILD_DIR/m68k-${target}-${name}.o" "$BUILD_DIR/m68k-${target}-${name}.s"
    done
  done
  echo "the m68k assembler accepted every m68k and coldfire output."
else
  echo "no m68k assembler found; skipped assembling the m68k output."
fi

if command -v s390x-linux-gnu-as > /dev/null 2>&1; then
  for example in "$ROOT_DIR"/examples/*.cas "$BUILD_DIR/regress.cas"; do
    name=$(basename "$example" .cas)
    "$BUILD_DIR/commonasmc" "$example" --target zarch -o "$BUILD_DIR/s390-${name}.s"
    s390x-linux-gnu-as -o "$BUILD_DIR/s390-${name}.o" "$BUILD_DIR/s390-${name}.s"
  done
  echo "the z/Architecture assembler accepted every zarch output."
else
  echo "no z/Architecture assembler found; skipped assembling the zarch output."
fi

# The strongest check there is: build tests/exec-kernel.cas for every machine
# that has an assembler, a linker and an emulator here, run it, and require
# all of them to print the same thing. Assembling says the text was
# well-formed; this says the arithmetic, the control flow, the memory and the
# syscalls all do what they were supposed to.
EXEC_PROGRAM=tests/exec-kernel.cas
EXEC_NAME=kernel
EXEC_EXPECTED="67 6 60 61 15 96 64 25 8 8 64 45 7 "
exec_failures=0
exec_ran=0

run_exec_case() {
  case_target=$1
  case_as=$2
  case_asflags=$3
  case_ld=$4
  case_ldflags=$5
  case_qemu=$6
  if [ -n "$case_as" ] && ! command -v "$case_as" > /dev/null 2>&1; then
    echo "  $EXEC_NAME/$case_target: skipped, no $case_as here"; return 0
  fi
  if ! command -v "$case_ld" > /dev/null 2>&1; then
    echo "  $EXEC_NAME/$case_target: skipped, no $case_ld here"; return 0
  fi
  if [ -n "$case_qemu" ] && ! command -v "$case_qemu" > /dev/null 2>&1; then
    echo "  $EXEC_NAME/$case_target: skipped, no $case_qemu here"; return 0
  fi
  "$BUILD_DIR/commonasmc" "$ROOT_DIR/$EXEC_PROGRAM" --target "$case_target" -O1 \
    -o "$BUILD_DIR/exec-$EXEC_NAME-$case_target.s" || { echo "  $EXEC_NAME/$case_target: did not compile"; exec_failures=$((exec_failures+1)); return 0; }
  if [ "$case_as" = "nasm" ]; then
    nasm $case_asflags "$BUILD_DIR/exec-$EXEC_NAME-$case_target.s" -o "$BUILD_DIR/exec-$EXEC_NAME-$case_target.o" \
      || { echo "  $EXEC_NAME/$case_target: did not assemble"; exec_failures=$((exec_failures+1)); return 0; }
  else
    $case_as $case_asflags -o "$BUILD_DIR/exec-$EXEC_NAME-$case_target.o" "$BUILD_DIR/exec-$EXEC_NAME-$case_target.s" \
      || { echo "  $EXEC_NAME/$case_target: did not assemble"; exec_failures=$((exec_failures+1)); return 0; }
  fi
  $case_ld $case_ldflags "$BUILD_DIR/exec-$EXEC_NAME-$case_target.o" -o "$BUILD_DIR/exec-$EXEC_NAME-$case_target" \
    || { echo "  $EXEC_NAME/$case_target: did not link"; exec_failures=$((exec_failures+1)); return 0; }
  if [ -n "$case_qemu" ]; then
    "$case_qemu" "$BUILD_DIR/exec-$EXEC_NAME-$case_target" > "$BUILD_DIR/exec-$EXEC_NAME-$case_target.txt" 2>&1 || true
  else
    "$BUILD_DIR/exec-$EXEC_NAME-$case_target" > "$BUILD_DIR/exec-$EXEC_NAME-$case_target.txt" 2>&1 || true
  fi
  exec_ran=$((exec_ran+1))
  if [ "$(cat "$BUILD_DIR/exec-$EXEC_NAME-$case_target.txt")" = "$EXEC_EXPECTED" ]; then
    echo "  $EXEC_NAME/$case_target: ran and printed the expected line"
  else
    echo "  $EXEC_NAME/$case_target: printed [$(cat "$BUILD_DIR/exec-$EXEC_NAME-$case_target.txt")]"
    echo "                 expected [$EXEC_EXPECTED]"
    exec_failures=$((exec_failures+1))
  fi
}

if [ "$(uname -s)" != "Linux" ]; then
  echo "not Linux; skipped running the program on the emulated machines."
  exec_skipped=1
else
  exec_skipped=0
fi

run_exec_everywhere() {
  run_exec_case x86_64-nasm nasm "-f elf64"  ld ""              ""
  run_exec_case i386-nasm   nasm "-f elf32"  ld "-m elf_i386"   ""
  run_exec_case aarch64-gnu aarch64-linux-gnu-as "" aarch64-linux-gnu-ld "" qemu-aarch64-static
  run_exec_case armv7a-gnu  arm-linux-gnueabi-as "" arm-linux-gnueabi-ld "" qemu-arm-static
  run_exec_case riscv64-gnu riscv64-linux-gnu-as "" riscv64-linux-gnu-ld "" qemu-riscv64-static
  run_exec_case rv32i-gnu   riscv64-linux-gnu-as "-march=rv32im -mabi=ilp32" \
                            riscv64-linux-gnu-ld "-m elf32lriscv" qemu-riscv32-static
  # A MIPS linker looks for __start rather than _start, so it is told the name.
  run_exec_case mips32-gnu  mips-linux-gnu-as "" mips-linux-gnu-ld "-e _start" qemu-mips-static
  run_exec_case ppcg4-gnu   powerpc-linux-gnu-as "" powerpc-linux-gnu-ld "" qemu-ppc-static
  run_exec_case sparcv8-gnu sparc64-linux-gnu-as "-32" sparc64-linux-gnu-ld "-m elf32_sparc" qemu-sparc-static
  run_exec_case m68k        m68k-linux-gnu-as "" m68k-linux-gnu-ld "" qemu-m68k-static
  run_exec_case zarch       s390x-linux-gnu-as "" s390x-linux-gnu-ld "" qemu-s390x-static
}

if [ "$exec_skipped" = "0" ]; then
echo "running the same programs on every machine that can be emulated here:"
run_exec_everywhere

# The sorting demo is a second opinion: longer, nested loops, a signed
# comparison in the inner one, negative numbers to print, and the comparing
# extended operations. One program agreeing everywhere can still be a program
# that never reaches the awkward paths.
EXEC_PROGRAM=demos/sort/sort.cas
EXEC_NAME=sort
EXEC_EXPECTED="-8 3 4 7 12 15 23 41 55 62 88 91 | min -8 max 91 sum 393 spread 99 over50 4 "
run_exec_everywhere

if [ "$exec_failures" -ne 0 ]; then
  echo "$exec_failures runs disagreed with the rest"
  exit 1
fi
echo "$exec_ran runs of two programs printed the same line as each other."
fi

# wasm is text that has to be assembled and validated, not linked.
if command -v wat2wasm > /dev/null 2>&1; then
  for example in "$ROOT_DIR"/examples/*.cas "$BUILD_DIR/regress.cas"; do
    name=$(basename "$example" .cas)
    "$BUILD_DIR/commonasmc" "$example" --target wasm -o "$BUILD_DIR/wasm-${name}.wat"
    wat2wasm "$BUILD_DIR/wasm-${name}.wat" -o "$BUILD_DIR/wasm-${name}.wasm"
  done
  echo "wat2wasm assembled and validated every wasm output."
  # And then the same program the emulated machines run, run here too. wasm
  # needs no emulator and no cross toolchain, so this works anywhere node and
  # wat2wasm both are, Linux or not.
  if command -v node > /dev/null 2>&1; then
    "$BUILD_DIR/commonasmc" "$ROOT_DIR/tests/exec-kernel.cas" --target wasm -O1 \
      -o "$BUILD_DIR/exec-wasm.wat"
    wat2wasm "$BUILD_DIR/exec-wasm.wat" -o "$BUILD_DIR/exec-wasm.wasm"
    node "$ROOT_DIR/tests/wasm-run.js" "$BUILD_DIR/exec-wasm.wasm" > "$BUILD_DIR/exec-wasm.txt" 2>&1 || true
    if [ "$(cat "$BUILD_DIR/exec-wasm.txt")" = "$EXEC_EXPECTED" ]; then
      echo "  wasm: ran and printed the expected line"
    else
      echo "  wasm: printed [$(cat "$BUILD_DIR/exec-wasm.txt")]"
      echo "        expected [$EXEC_EXPECTED]"
      exit 1
    fi
  else
    echo "no node found; skipped running the wasm output."
  fi
else
  echo "no wat2wasm found; skipped validating the wasm output."
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

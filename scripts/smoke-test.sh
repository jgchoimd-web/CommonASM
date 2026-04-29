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
grep -q "commonasmc 0.1.0-dev" "$BUILD_DIR/version.txt"
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

# Demos

Two programs written to find out what CommonASM is actually like to use, and
kept because what they turned up is worth not regressing.

## `os/kernel.cas`

A freestanding multiboot kernel. No syscalls, no libc, nothing underneath it:
it writes straight into the VGA text buffer at `0xb8000`, where each cell is a
character byte followed by an attribute byte.

```sh
commonasmc demos/os/kernel.cas --target i386-nasm -o kernel.asm
nasm -f elf32 kernel.asm -o kernel.o
ld -m elf_i386 -T demos/os/link.ld kernel.o -o kernel.elf
qemu-system-i386 -kernel kernel.elf
```

## `game/guess.cas`

A guessing game on Linux syscalls and nothing else. It reads from standard
input, parses the decimal itself, and stirs its secret with an ordinary linear
congruential step so it is not the same number every run.

```sh
commonasmc demos/game/guess.cas --target x86_64-nasm -O1 -o guess.asm
nasm -f elf64 guess.asm -o guess.o
ld guess.o -o guess
./guess
```

## What writing them changed

Three things that only showed up once there was a program to write rather than
a backend to test.

**A syscall had no result.** The kernel returns a value in a register — how
many bytes a read actually delivered, whether an open failed — and CommonASM
threw it away. Every backend put the number somewhere the language could not
name, so a program could not tell a short read from a full one, or end of
input from a blank line. The guessing game only appeared to work because it
ignored the count and re-parsed whatever was left in the buffer. A syscall can
now name a register to receive its result:

```asm
  syscall r4, read, stdin, buf, BUFSZ
  cmp r4, 0
  jle quit
```

**Storing a constant went the long way round.** `store.b [r0], 32` staged the
value in a register first, so the kernel's screen-clearing loop was two
instructions per byte where one would do. Literals and named constants now go
straight into the store.

**Byte stores on i386 always used the scratch register.** That was needed for
`esi` and `edi`, which have no 8-bit form, but not for `ebx` and `ecx`, which
do. The two that can now store directly.

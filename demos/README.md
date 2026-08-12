# Demos

Three programs written to find out what CommonASM is actually like to use,
and kept because what they turned up is worth not regressing.

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

## `sort/sort.cas`

Twelve numbers sorted in place, then the smallest, the largest, the total, the
spread and how many are over fifty. It exists because everything else being
tested was a straight line of independent operations: this one has a loop
inside a loop, a signed comparison in the inner one, addresses computed from
an index, negative numbers to print, and the comparing extended operations
doing the statistics.

```sh
commonasmc demos/sort/sort.cas --target x86_64-nasm -O1 -o sort.asm
nasm -f elf64 sort.asm -o sort.o && ld sort.o -o sort && ./sort
-8 3 4 7 12 15 23 41 55 62 88 91 | min -8 max 91 sum 393 spread 99 over50 4
```

It is built for every machine the smoke test can run — sixteen of them — and
all sixteen have to print that line.

## What writing them changed

Things that only showed up once there was a program to write rather than a
backend to test.

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

**A function could not call a function.** On the seven machines where the call
instruction leaves the return address in a register rather than pushing it,
an inner call overwrote the address the outer one was going to return to, and
`func` emitted a label and nothing else. Everything tested until then had been
exactly one call deep, which is the one depth where a link register survives.
The sorting program calls `show`, which calls `print`. RISC-V dumped core, and
six others spun until the timeout. A function that calls now saves the link
register; a leaf still does not pay for it.

**A narrow load meant different things on different machines.** `load.d` read
four bytes and left the rest of the register to the machine's taste: some
zero-extended, RISC-V sign-extended, and a 32-bit machine had no choice
because four bytes is the whole register there. The demo printed `-8` on some
machines and `4294967288` on others. Loads sign-extend now, which is the only
rule under which the 32-bit and 64-bit members of a family can agree.

**RISC-V addresses went through a register nobody had set up.** The linker is
allowed to fold `auipc`+`addi` into one instruction relative to `gp`, which
the C runtime's startup code initialises and freestanding output does not
have. Every folded address came out around minus two thousand. The demo has a
`.data` section, which is what put its symbols in range of the fold; nothing
tested before had one.

**A comparison stayed alive after its branch.** On machines with no condition
flags the compiler holds a comparison until a branch spells it out, copying
the operands aside if anything writes them meanwhile. It kept doing that after
the branch had already used it, and `min` and `max` expand into exactly the
shape that triggers the copy. Six per cent of the program on RISC-V and MIPS
was dead moves.

**z/Architecture borrowed a register and did not give it back.** Division
needs a third register for a divisor it has to materialise, and took the one
carrying virtual `r10`. The running total lived there.

**m68k could not compare against a spilled register.** `cmp` is the one
instruction in its group whose destination has to be a data register, and it
was being lowered like `add`. It needed a program that compares something
living above `r5`, which is where m68k starts spilling.

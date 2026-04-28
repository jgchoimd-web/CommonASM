# Self-Hosting Plan

The self-hosting compiler will be written in the language it compiles.

Bootstrapping stages:

1. C compiler: dependency-free AOT compiler.
2. Self-host compiler source: `compiler.cal`.
3. Bootstrap: C compiler compiles the self-host compiler runtime pieces.
4. Self-host: the generated compiler compiles future versions of itself.

Current status: `compiler.cal` is a concrete source sketch for the self-hosted compiler.
The CommonASM language now has constants, byte arrays, automatic string length symbols,
arithmetic, stack operations, memory load/store, and compare/branch operations.
It also sketches portable data sizes, `.rodata`/`.bss`, functions, bit operations,
and Linux syscall names shared by the C compiler.
Experimental minority targets include MMIXAL, DCPU-16, FRACTRAN source encoding,
and Cellular Automaton Rule 110 seed output.
Experimental common architecture targets include IA-32, ARMv4/v5/v7-A, AArch64,
Thumb/Thumb-2, RV32I/RV128I, Itanium, and LoongArch64.
The sketch also recognizes the expanded legacy, retro, MCU, mainframe, GPU/DSP,
VM/IR, and educational/esoteric target names used by the C compiler.

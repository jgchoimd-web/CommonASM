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

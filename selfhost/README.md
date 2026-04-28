# Self-Hosting Plan

The self-hosting compiler will be written in the language it compiles.

Bootstrapping stages:

1. Python compiler: fast reference implementation.
2. C compiler: dependency-free AOT compiler.
3. Self-host compiler source: `compiler.cal`.
4. Bootstrap: C compiler compiles the self-host compiler runtime pieces.
5. Self-host: the generated compiler compiles future versions of itself.

Current status: `compiler.cal` is a concrete source sketch for the self-hosted compiler.
The CommonASM language now has constants, byte arrays, automatic string length symbols,
and simple compare/branch operations.

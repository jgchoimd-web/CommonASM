# selfhost/

`compiler.cal` is a design sketch, not a program. Read it as a proposal for
what a self-hosting CommonASM compiler could look like, and nothing more.

Specifically:

- **The language it is written in does not exist.** There is no compiler or
  interpreter for `.cal`, here or anywhere. `module`, `type Target = ...` and
  the rest are invented notation for the sketch's own use.
- **Nothing builds or checks it.** It is not in the Makefile and CI never
  opens it, so it cannot be known to be correct, or even consistent.
- **It has drifted.** Its target list stops at the ones that existed when it
  was written, so it knows nothing about the MIPS, PowerPC, SPARC, m68k,
  z/Architecture or wasm backends, and nothing about extended operations,
  inline assembly, or reading assembly back into CommonASM.

The compiler that works is [`csrc/commonasmc.c`](../csrc/commonasmc.c). It is
the only implementation, and every claim the project makes is about that one.

Turning this sketch into something real would mean writing a `.cal` front end
first, which no one has started. Until then the useful part of the file is the
shape of the idea, not the contents.

# Contributing to CommonASM

Thanks for helping CommonASM grow. The project goal is a portable assembly IR
that can be lowered into real assembly, VM IR, and experimental pseudo targets.

## Development setup

Build the reference C AOT compiler:

```sh
mkdir -p build
gcc csrc/commonasmc.c -o build/commonasmc
```

On systems with `make`:

```sh
make build
make examples
```

Compile an example:

```sh
./build/commonasmc examples/hello.cas --target x86_64-nasm -o build/hello_x86.asm
./build/commonasmc examples/hello.cas --target riscv64-gnu -o build/hello_rv64.s
```

Quick start helper (build + representative examples):

```sh
./scripts/run-example.sh
```

Run the smoke test suite on systems with a POSIX shell:

```sh
sh scripts/smoke-test.sh
make smoke
```

On Windows PowerShell or PowerShell Core:

```powershell
.\scripts\smoke-test.ps1
```

If a C compiler is not available, still run:

```powershell
git diff --check
```

GitHub Actions runs both smoke test scripts on every push and pull request:

- Build `csrc/commonasmc.c` with `gcc`.
- Check CLI help, target listing, and target metadata.
- Check stdin/stdout piping.
- Compile every example for `x86_64-nasm` and `riscv64-gnu`.
- Compile representative experimental, VM, toy, and esolang targets.
- Verify that diagnostics point at invalid source tokens.

## Project rules

- Keep `csrc/commonasmc.c` as the reference compiler implementation.
- Keep `selfhost/compiler.cal` as a self-hosting compiler design source until it
  becomes complete enough to build itself.
- Do not reintroduce a Python compiler path.
- Keep CommonASM portable. Target-specific raw assembly blocks are outside the
  current language direction.
- Add examples when new syntax or target behavior becomes visible to users.
- Update `README.md`, `csrc/README.md`, and `docs/` when the user-facing
  language or target list changes.

## Backend expectations

Primary targets should be tested first:

- `x86_64-nasm`
- `riscv64-gnu`

Experimental targets may emit assembly-style or IR-style text for the portable
subset. Toy, historic, or esoteric targets may emit pseudo assembly or source
encodings when the machine model does not map cleanly to CommonASM.

## Pull request checklist

Before opening a pull request:

- Explain the behavior change.
- List the targets you tested.
- Include any new examples or docs.
- Run `git diff --check`.
- Build the C compiler when a C toolchain is available.

Small focused pull requests are easier to review than large unrelated rewrites.

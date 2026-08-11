[CmdletBinding()]
param(
    [string]$Compiler = $(if ($env:CC) { $env:CC } else { "gcc" }),
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"

$RootDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildPath = Join-Path $RootDir "build"
} else {
    $BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) {
        $BuildDir
    } else {
        Join-Path $RootDir $BuildDir
    }
}

$IsWindowsPlatform = ($PSVersionTable.PSEdition -eq "Desktop") -or ($env:OS -eq "Windows_NT")
$ExeName = if ($IsWindowsPlatform) { "commonasmc-pwsh.exe" } else { "commonasmc-pwsh" }
$CompilerExe = Join-Path $BuildPath $ExeName

New-Item -ItemType Directory -Force -Path $BuildPath | Out-Null

function Invoke-Native {
    param(
        [string]$File,
        [string[]]$Arguments
    )
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "command failed with exit code ${LASTEXITCODE}: $File $($Arguments -join ' ')"
    }
}

function Invoke-NativeToFile {
    param(
        [string]$OutputPath,
        [string]$File,
        [string[]]$Arguments
    )
    & $File @Arguments > $OutputPath
    if ($LASTEXITCODE -ne 0) {
        throw "command failed with exit code ${LASTEXITCODE}: $File $($Arguments -join ' ')"
    }
}

function Invoke-NativeExpectFailure {
    param(
        [string]$ErrorPath,
        [string]$File,
        [string[]]$Arguments,
        [string]$Message
    )
    & $File @Arguments 2> $ErrorPath
    if ($LASTEXITCODE -eq 0) {
        throw $Message
    }
    # The non-zero exit code is the expected result, so consume it here.
    # $LASTEXITCODE is global and would otherwise leak past the end of this
    # script, where the CI runner turns it into the step's exit code.
    $global:LASTEXITCODE = 0
}

function Assert-Contains {
    param(
        [string]$Path,
        [string]$Needle
    )
    $Text = Get-Content -Raw -Path $Path
    if (-not $Text.Contains($Needle)) {
        throw "expected '$Path' to contain '$Needle'"
    }
}

Invoke-Native $Compiler @(
    "-std=c99",
    "-Wall",
    "-Wextra",
    "-pedantic",
    "-O2",
    (Join-Path $RootDir "csrc/commonasmc.c"),
    "-o",
    $CompilerExe
)

$HelpPath = Join-Path $BuildPath "help-pwsh.txt"
$VersionPath = Join-Path $BuildPath "version-pwsh.txt"
$TargetsPath = Join-Path $BuildPath "targets-pwsh.txt"
$WasmInfoPath = Join-Path $BuildPath "wasm-info-pwsh.txt"
$BrainfuckInfoPath = Join-Path $BuildPath "brainfuck-info-pwsh.txt"

Invoke-NativeToFile $HelpPath $CompilerExe @("--help")
Invoke-NativeToFile $VersionPath $CompilerExe @("--version")
Invoke-NativeToFile $TargetsPath $CompilerExe @("--list-targets")
Invoke-NativeToFile $WasmInfoPath $CompilerExe @("--target-info", "wasm")
Invoke-NativeToFile $BrainfuckInfoPath $CompilerExe @("--target-info", "brainfuck")

Assert-Contains $HelpPath "commonasmc --list-targets"
Assert-Contains $HelpPath "commonasmc --target-info TARGET"
Assert-Contains $HelpPath "commonasmc --version"
Assert-Contains $HelpPath "-O1"
Assert-Contains $VersionPath "commonasmc 0.1.0-dev"
Assert-Contains $TargetsPath "x86_64-nasm"
Assert-Contains $TargetsPath "riscv64-gnu"
Assert-Contains $TargetsPath "brainfuck"
Assert-Contains $TargetsPath "cellular-automaton"
Assert-Contains $WasmInfoPath "support: VM/IR"
Assert-Contains $BrainfuckInfoPath "support: Encoding/pseudo"

$UnknownTargetPath = Join-Path $BuildPath "unknown-target-pwsh.txt"
$NopePath = Join-Path $BuildPath "nope-pwsh.out"
Invoke-NativeExpectFailure $UnknownTargetPath $CompilerExe @(
    (Join-Path $RootDir "examples/hello.cas"),
    "--target",
    "no-such-target",
    "-o",
    $NopePath
) "expected unknown target to fail"
Assert-Contains $UnknownTargetPath "unknown target"

$UnknownInfoPath = Join-Path $BuildPath "unknown-info-pwsh.txt"
Invoke-NativeExpectFailure $UnknownInfoPath $CompilerExe @(
    "--target-info",
    "no-such-target"
) "expected unknown target info to fail"
Assert-Contains $UnknownInfoPath "unknown target"

$StdoutPath = Join-Path $BuildPath "stdout-pwsh.wat"
Get-Content -Raw -Path (Join-Path $RootDir "examples/hello.cas") |
    & $CompilerExe "-" "--target" "wasm" "-o" "-" > $StdoutPath
if ($LASTEXITCODE -ne 0) {
    throw "stdin/stdout pipeline failed"
}
Assert-Contains $StdoutPath "(module"
Assert-Contains $StdoutPath "call `$fd_write"

Get-ChildItem -Path (Join-Path $RootDir "examples") -Filter "*.cas" | ForEach-Object {
    $Name = [System.IO.Path]::GetFileNameWithoutExtension($_.Name)
    foreach ($Target in @("x86_64-nasm", "riscv64-gnu")) {
        Invoke-Native $CompilerExe @(
            $_.FullName,
            "--target",
            $Target,
            "-o",
            (Join-Path $BuildPath "$Name-$Target-pwsh.out")
        )
    }
}

# RISC-V has no flags, so a compare is folded into its branch rather than
# staged in a register pair that later instructions would overwrite.
$ControlRvPath = Join-Path $BuildPath "control-riscv64-gnu-pwsh.out"
Assert-Contains $ControlRvPath "li s11, 42"
Assert-Contains $ControlRvPath "beq t2, s11, success"

$OptimizeO0Path = Join-Path $BuildPath "optimize-O0-pwsh.asm"
$OptimizeO1Path = Join-Path $BuildPath "optimize-O1-pwsh.asm"
Invoke-Native $CompilerExe @(
    (Join-Path $RootDir "examples/optimize.cas"),
    "--target",
    "x86_64-nasm",
    "-O0",
    "-o",
    $OptimizeO0Path
)
Invoke-Native $CompilerExe @(
    (Join-Path $RootDir "examples/optimize.cas"),
    "--target",
    "x86_64-nasm",
    "-O1",
    "-o",
    $OptimizeO1Path
)
Assert-Contains $OptimizeO1Path "mov rbx, 42"
Assert-Contains $OptimizeO1Path "mov rdx, 0"
Assert-Contains $OptimizeO0Path "add rbx, 0"
$OptimizeO1Text = Get-Content -Raw -Path $OptimizeO1Path
foreach ($Needle in @("add rbx, 0", "mov rcx, rcx", "imul rsi, 1")) {
    if ($OptimizeO1Text.Contains($Needle)) {
        throw "optimizer left removable instruction in -O1 output: $Needle"
    }
}

# Each check below covers a lowering that used to produce silently wrong or
# unassemblable code.
$RegressPath = Join-Path $BuildPath "regress-pwsh.cas"
$RegressX86 = Join-Path $BuildPath "regress-pwsh-x86.asm"
$RegressRv = Join-Path $BuildPath "regress-pwsh-rv.s"
$LongLabel = "a_label_long_enough_that_its_length_constant_used_to_be_truncated"
@"
const stdout = 1

.data
${LongLabel}: string "hi\n"

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
  syscall write, stdout, ${LongLabel}, ${LongLabel}_len
  je done
done:
  ret
"@ | Set-Content -Path $RegressPath -Encoding ascii

Invoke-NativeToFile $RegressX86 $CompilerExe @($RegressPath, "--target", "x86_64-nasm", "-o", "-")
Invoke-NativeToFile $RegressRv $CompilerExe @($RegressPath, "--target", "riscv64-gnu", "-o", "-")

$RegressX86Text = Get-Content -Raw -Path $RegressX86
$RegressRvText = Get-Content -Raw -Path $RegressRv

# A virtual register must never land on the stack or frame pointer.
if ($RegressX86Text -match "(?m)^  (mov|add|sub|xor|and|or|imul|neg|not|inc|dec|shl|shr|sar) (rsp|rbp)[,\r\n]") {
    throw "a virtual register was lowered onto rsp or rbp"
}
# Virtual registers past the machine register file need backing store.
Assert-Contains $RegressX86 "__cas_spill: resq"
Assert-Contains $RegressX86 "mov qword [rel __cas_spill+8], 111"
# A variable shift count has to reach cl; "shl reg, reg" does not assemble.
Assert-Contains $RegressX86 "shl rax, cl"
# idiv overwrites rdx, which carries a virtual register.
Assert-Contains $RegressX86 "push rdx"

# Subtracting a negative immediate used to emit "addi t2, t2, --5".
Assert-Contains $RegressRv "addi t2, t2, 5"
if ($RegressRvText.Contains("--")) {
    throw "riscv lowering emitted a doubled sign"
}
# A long label keeps its generated length constant, so it stays an immediate
# ("li") rather than being mistaken for an address ("la").
Assert-Contains $RegressRv "li a2, ${LongLabel}_len"
# A compare must not be carried in a register the syscall sequence writes.
if ($RegressRvText -match "(?m)^  b(eq|ne|lt|ge|gt|le)u? a[0-7],") {
    throw "riscv compare was carried in a syscall argument register"
}

# Whether the output assembles is checked directly, not just grepped for, on
# every backend an assembler is available for.
$Examples = Get-ChildItem -Path (Join-Path $RootDir "examples") -Filter "*.cas"

# Truncation is promoted to an error: an immediate too wide for the encoding
# assembles with only a warning otherwise, and silently loses its top bits.
$NasmStrict = "-w+error=number-overflow"
$HostObjFormat = if ($IsWindowsPlatform) { "win64" } else { "elf64" }

if (Get-Command nasm -ErrorAction SilentlyContinue) {
    foreach ($Example in $Examples) {
        $Name = [System.IO.Path]::GetFileNameWithoutExtension($Example.Name)
        foreach ($Level in @("-O0", "-O1")) {
            # Built twice: once letting the target use its own instructions for
            # the extended operations, once with them all expanded.
            foreach ($Mode in @(@{N = "native"; A = @() }, @{N = "emulated"; A = @("--emulate-extended") })) {
                foreach ($Pair in @(@{T = "x86_64-nasm"; F = "elf64"; P = "asm64" }, @{T = "i386-nasm"; F = "elf32"; P = "asm32" })) {
                    $Stem = "$($Pair.P)-$($Mode.N)-$Name$Level-pwsh"
                    $AsmPath = Join-Path $BuildPath "$Stem.asm"
                    Invoke-Native $CompilerExe (@($Example.FullName, "--target", $Pair.T, $Level) + $Mode.A + @("-o", $AsmPath))
                    Invoke-Native "nasm" @("-f", $Pair.F, $NasmStrict, $AsmPath, "-o", (Join-Path $BuildPath "$Stem.o"))
                }
            }
        }
    }
    Write-Host "nasm accepted every x86_64 and i386 output, native and expanded."
} else {
    Write-Host "nasm not found; skipped assembling the x86_64 and i386 output."
}

# The strongest check available: build the extended operations both ways and
# run them against a reference implementation.
if ((Get-Command nasm -ErrorAction SilentlyContinue) -and (Get-Command $Compiler -ErrorAction SilentlyContinue)) {
    foreach ($Mode in @(@{N = "native"; A = @() }, @{N = "emulated"; A = @("--emulate-extended") })) {
        $AsmPath = Join-Path $BuildPath "extkernel-$($Mode.N)-pwsh.asm"
        $ObjPath = Join-Path $BuildPath "extkernel-$($Mode.N)-pwsh.o"
        $RunPath = Join-Path $BuildPath "extrun-$($Mode.N)-pwsh$(if ($IsWindowsPlatform) { '.exe' } else { '' })"
        Invoke-Native $CompilerExe (@((Join-Path $RootDir "tests/extended-kernel.cas"), "--target", "x86_64-nasm") + $Mode.A + @("-o", $AsmPath))
        Invoke-Native "nasm" @("-f", $HostObjFormat, $NasmStrict, $AsmPath, "-o", $ObjPath)
        Invoke-Native $Compiler @((Join-Path $RootDir "tests/extended-driver.c"), $ObjPath, "-o", $RunPath)
        Write-Host "  extended operations, $($Mode.N):"
        Invoke-Native $RunPath @()
    }
    Write-Host "extended operations agree with the reference, native and expanded."

    # Every block in this kernel is written for AArch64, so an x86-64 build has
    # to lift all of them. Running it is what shows the lifting kept the meaning.
    $TrAsm = Join-Path $BuildPath "translate-kernel-pwsh.asm"
    $TrObj = Join-Path $BuildPath "translate-kernel-pwsh.o"
    $TrRun = Join-Path $BuildPath "translate-run-pwsh$(if ($IsWindowsPlatform) { '.exe' } else { '' })"
    Invoke-Native $CompilerExe @((Join-Path $RootDir "tests/translate-kernel.cas"), "--target", "x86_64-nasm",
        "--translate-asm", "-o", $TrAsm)
    Invoke-Native "nasm" @("-f", $HostObjFormat, $NasmStrict, $TrAsm, "-o", $TrObj)
    Invoke-Native $Compiler @((Join-Path $RootDir "tests/translate-driver.c"), $TrObj, "-o", $TrRun)
    Write-Host "  lifted AArch64 blocks running on x86-64:"
    Invoke-Native $TrRun @()

    # Without the option the same source must be rejected rather than quietly
    # losing the blocks.
    $TrNope = Join-Path $BuildPath "translate-nope-pwsh.txt"
    Invoke-NativeExpectFailure $TrNope $CompilerExe @(
        (Join-Path $RootDir "tests/translate-kernel.cas"), "--target", "x86_64-nasm",
        "-o", (Join-Path $BuildPath "translate-nope-pwsh.asm")
    ) "expected an unmatched asm run to fail without --translate-asm"
    Assert-Contains $TrNope "no asm block"
    Write-Host "an unmatched asm run still fails without --translate-asm."
} else {
    Write-Host "skipped running the extended operation checks."
}

if (Get-Command riscv64-linux-gnu-as -ErrorAction SilentlyContinue) {
    foreach ($Example in $Examples) {
        $Name = [System.IO.Path]::GetFileNameWithoutExtension($Example.Name)
        foreach ($Target in @("riscv64-gnu", "riscv64-zbb")) {
            $AsmPath = Join-Path $BuildPath "rv-$Target-$Name-pwsh.s"
            Invoke-Native $CompilerExe @($Example.FullName, "--target", $Target, "-o", $AsmPath)
            Invoke-Native "riscv64-linux-gnu-as" @("-o", (Join-Path $BuildPath "rv-$Target-$Name-pwsh.o"), $AsmPath)
        }
    }
    Write-Host "the RISC-V assembler accepted every riscv64-gnu and riscv64-zbb output."
} else {
    Write-Host "no RISC-V assembler found; skipped assembling the RISC-V output."
}

if (Get-Command mips-linux-gnu-as -ErrorAction SilentlyContinue) {
    foreach ($Example in $Examples) {
        $Name = [System.IO.Path]::GetFileNameWithoutExtension($Example.Name)
        foreach ($Target in @("mips1-gnu", "mips32-gnu", "micromips-gnu")) {
            $AsmPath = Join-Path $BuildPath "mips-$Target-$Name-pwsh.s"
            $ObjPath = Join-Path $BuildPath "mips-$Target-$Name-pwsh.o"
            Invoke-Native $CompilerExe @($Example.FullName, "--target", $Target, "-o", $AsmPath)
            Invoke-Native "mips-linux-gnu-as" @("-o", $ObjPath, $AsmPath)
        }
    }
    Write-Host "the MIPS assembler accepted every 32-bit MIPS output."
} else {
    Write-Host "no MIPS assembler found; skipped assembling the MIPS output."
}

# mips64-gnu needs an assembler whose ABI is actually 64-bit: the 32-bit one
# rejects a 64-bit immediate even with -march=mips64.
if (Get-Command mips64-linux-gnuabi64-as -ErrorAction SilentlyContinue) {
    foreach ($Example in $Examples) {
        $Name = [System.IO.Path]::GetFileNameWithoutExtension($Example.Name)
        $AsmPath = Join-Path $BuildPath "mips64-$Name-pwsh.s"
        Invoke-Native $CompilerExe @($Example.FullName, "--target", "mips64-gnu", "-o", $AsmPath)
        Invoke-Native "mips64-linux-gnuabi64-as" @("-o", (Join-Path $BuildPath "mips64-$Name-pwsh.o"), $AsmPath)
    }
    Write-Host "the 64-bit MIPS assembler accepted every mips64-gnu output."
} else {
    Write-Host "no 64-bit MIPS assembler found; skipped assembling the mips64-gnu output."
}

if (Get-Command clang -ErrorAction SilentlyContinue) {
    $ClangBackends = @(
        @{ T = "aarch64-gnu"; Triple = "aarch64-unknown-linux-gnu"; P = "a64" },
        @{ T = "armv7a-gnu"; Triple = "armv7-unknown-linux-gnueabi"; P = "arm" },
        @{ T = "thumb2-gnu"; Triple = "thumbv7-unknown-linux-gnueabi"; P = "thumb" }
    )
    foreach ($Example in $Examples) {
        $Name = [System.IO.Path]::GetFileNameWithoutExtension($Example.Name)
        foreach ($Level in @("-O0", "-O1")) {
            foreach ($Backend in $ClangBackends) {
                $AsmPath = Join-Path $BuildPath "$($Backend.P)-$Name$Level-pwsh.s"
                Invoke-Native $CompilerExe @($Example.FullName, "--target", $Backend.T, $Level, "-o", $AsmPath)
                Invoke-Native "clang" @("-cc1as", "-triple", $Backend.Triple, "-filetype", "obj",
                    "-o", (Join-Path $BuildPath "$($Backend.P)-$Name$Level-pwsh.o"), $AsmPath)
            }
        }
    }
    Write-Host "clang accepted every aarch64, armv7a and thumb2 output."
} else {
    Write-Host "clang not found; skipped assembling the aarch64 and ARM output."
}

$RepresentativeTargets = @(
    "i386-nasm",
    "aarch64-gnu",
    "armv7a-gnu",
    "rv32i-gnu",
    "rv128i-gnu",
    "mips32-gnu",
    "ppcg4-gnu",
    "sparcv9-gnu",
    "m68k",
    "avr",
    "z80",
    "pdp11",
    "ptx",
    "ebpf",
    "wasm",
    "llvm-ir",
    "jvm-bytecode",
    "chip8",
    "subleq",
    "brainfuck",
    "mmixal",
    "dcpu16",
    "fractran",
    "cellular-automaton"
)

foreach ($Target in $RepresentativeTargets) {
    Invoke-Native $CompilerExe @(
        (Join-Path $RootDir "examples/legacy.cas"),
        "--target",
        $Target,
        "-o",
        (Join-Path $BuildPath "legacy-$Target-pwsh.out")
    )
}

$BadPath = Join-Path $BuildPath "bad-pwsh.cas"
@"
.text
global _start

_start:
  mov r99, 1
"@ | Set-Content -Path $BadPath -Encoding ascii

$DiagnosticPath = Join-Path $BuildPath "diagnostic-pwsh.txt"
Invoke-NativeExpectFailure $DiagnosticPath $CompilerExe @(
    $BadPath,
    "--target",
    "x86_64-nasm",
    "-o",
    (Join-Path $BuildPath "bad-pwsh.out")
) "expected invalid register to fail"

Assert-Contains $DiagnosticPath "expected virtual register r0-r15"
Assert-Contains $DiagnosticPath "bad-pwsh.cas:5"
Assert-Contains $DiagnosticPath "r99"

$EmptyOutput = Get-ChildItem -Path $BuildPath -File -Recurse |
    Where-Object { ($_.Name -like "*.out" -or $_.Name -eq $ExeName) -and $_.Length -eq 0 } |
    Select-Object -First 1
if ($EmptyOutput) {
    throw "found empty build output: $($EmptyOutput.FullName)"
}

Write-Host "CommonASM PowerShell smoke tests passed."

# Reaching this line means every check passed. Exit explicitly so no stray
# $LASTEXITCODE from a native call can decide the exit code for us.
exit 0

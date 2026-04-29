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
Assert-Contains $VersionPath "commonasmc 0.1.0-dev"
Assert-Contains $TargetsPath "x86_64-nasm"
Assert-Contains $TargetsPath "riscv64-gnu"
Assert-Contains $TargetsPath "brainfuck"
Assert-Contains $TargetsPath "cellular-automaton"
Assert-Contains $WasmInfoPath "support: VM/IR"
Assert-Contains $BrainfuckInfoPath "support: Encoding/pseudo"

$UnknownTargetPath = Join-Path $BuildPath "unknown-target-pwsh.txt"
$NopePath = Join-Path $BuildPath "nope-pwsh.out"
& $CompilerExe (Join-Path $RootDir "examples/hello.cas") "--target" "no-such-target" "-o" $NopePath 2> $UnknownTargetPath
if ($LASTEXITCODE -eq 0) {
    throw "expected unknown target to fail"
}
Assert-Contains $UnknownTargetPath "unknown target"

$UnknownInfoPath = Join-Path $BuildPath "unknown-info-pwsh.txt"
& $CompilerExe "--target-info" "no-such-target" 2> $UnknownInfoPath
if ($LASTEXITCODE -eq 0) {
    throw "expected unknown target info to fail"
}
Assert-Contains $UnknownInfoPath "unknown target"

$StdoutPath = Join-Path $BuildPath "stdout-pwsh.wat"
Get-Content -Raw -Path (Join-Path $RootDir "examples/hello.cas") |
    & $CompilerExe "-" "--target" "wasm" "-o" "-" > $StdoutPath
if ($LASTEXITCODE -ne 0) {
    throw "stdin/stdout pipeline failed"
}
Assert-Contains $StdoutPath "wasm.syscall write"

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
& $CompilerExe $BadPath "--target" "x86_64-nasm" "-o" (Join-Path $BuildPath "bad-pwsh.out") 2> $DiagnosticPath
if ($LASTEXITCODE -eq 0) {
    throw "expected invalid register to fail"
}

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

# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [string]$Compiler = '',
    [switch]$DebugBuild,
    [Alias('Tests')][switch]$Test
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$zigPath = Join-Path $projectRoot 'tools/zig-x86_64-windows-0.14.1/zig.exe'

if ($Compiler) {
    $compilerCommand = Get-Command $Compiler -ErrorAction Stop
} elseif (Test-Path -LiteralPath $zigPath) {
    $compilerCommand = Get-Command $zigPath -ErrorAction Stop
} else {
    $compilerCommand = Get-Command clang++, g++ -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $compilerCommand) {
        throw 'No C++ compiler found. Install Zig 0.14.1 in tools/zig-x86_64-windows-0.14.1, or pass -Compiler to a C++17 compiler. CMake also supports MSVC.'
    }
}

$compilerPath = $compilerCommand.Source
$compilerArguments = @()
if ([IO.Path]::GetFileNameWithoutExtension($compilerPath) -eq 'zig') {
    $compilerArguments += @('c++', '-target', 'x86_64-windows-gnu')
}
$compilerArguments += @('-std=c++17', '-pthread', '-static', '-DNO_PREFETCH')
if ([Environment]::Is64BitOperatingSystem) {
    $compilerArguments += '-DIS_64BIT'
}
if ($DebugBuild) {
    $compilerArguments += @('-O0', '-g')
} else {
    $compilerArguments += @('-O3', '-DNDEBUG')
}
$compilerArguments += @('-I', (Join-Path $projectRoot 'vendor/stockfish/src'))
$compilerArguments += @('-I', (Join-Path $projectRoot 'src'))
$coreSources = @(
    (Join-Path $projectRoot 'vendor/stockfish/src/bitboard.cpp'),
    (Join-Path $projectRoot 'vendor/stockfish/src/movegen.cpp'),
    (Join-Path $projectRoot 'src/stockfish_position.cpp'),
    (Join-Path $projectRoot 'src/engine.cpp')
    (Join-Path $projectRoot 'src/search.cpp')
)
$outputDirectory = Join-Path $projectRoot 'bin'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$enginePath = Join-Path $outputDirectory 'selfmate.exe'
Write-Host "Building $enginePath"
& $compilerPath @compilerArguments @coreSources (Join-Path $projectRoot 'src/main.cpp') '-o' $enginePath
if ($LASTEXITCODE -ne 0) { throw "Engine build failed with exit code $LASTEXITCODE" }

if ($Test) {
    $testPath = Join-Path $outputDirectory 'selfmate_tests.exe'
    Write-Host "Building $testPath"
    & $compilerPath @compilerArguments @coreSources (Join-Path $projectRoot 'tests/engine_tests.cpp') '-o' $testPath
    if ($LASTEXITCODE -ne 0) { throw "Test build failed with exit code $LASTEXITCODE" }
    & $testPath
    if ($LASTEXITCODE -ne 0) { throw "Engine tests failed with exit code $LASTEXITCODE" }
    $searchTestPath = Join-Path $outputDirectory 'selfmate_search_tests.exe'
    Write-Host "Building $searchTestPath"
    & $compilerPath @compilerArguments @coreSources (Join-Path $projectRoot 'tests/search_tests.cpp') '-o' $searchTestPath
    if ($LASTEXITCODE -ne 0) { throw "Search test build failed with exit code $LASTEXITCODE" }
    & $searchTestPath
    if ($LASTEXITCODE -ne 0) { throw "Search tests failed with exit code $LASTEXITCODE" }
}

Write-Host 'Build complete.'

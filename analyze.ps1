# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [string]$Fen = 'startpos',
    [ValidateRange(1, 96)][int]$Depth = 6,
    [ValidateRange(0, 3600000)][int]$MoveTimeMs = 1000,
    [string[]]$Moves = @(),
    [string]$EnginePath = (Join-Path $PSScriptRoot 'bin/selfmate.exe')
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $EnginePath -PathType Leaf)) {
    throw "Engine not found at '$EnginePath'. Build it first with .\build.ps1 -Test."
}
if ($Fen -match '[\r\n]') { throw 'Fen must contain one line.' }
foreach ($move in $Moves) {
    if ($move -notmatch '^[a-h][1-8][a-h][1-8][qrbn]?$') {
        throw "Invalid UCI move '$move'. Use coordinates such as e2e4 or a7a8n."
    }
}

$engine = New-Object System.Diagnostics.Process
$engine.StartInfo.FileName = (Resolve-Path -LiteralPath $EnginePath).Path
$engine.StartInfo.WorkingDirectory = $PSScriptRoot
$engine.StartInfo.UseShellExecute = $false
$engine.StartInfo.CreateNoWindow = $true
$engine.StartInfo.RedirectStandardInput = $true
$engine.StartInfo.RedirectStandardOutput = $true
$engine.StartInfo.RedirectStandardError = $true
$started = $false
$stderr = $null

function Read-UciUntil {
    param([string]$Pattern, [int]$TimeoutMs)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ($true) {
        $remaining = [int][Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        $read = $engine.StandardOutput.ReadLineAsync()
        if (-not $read.Wait($remaining)) { throw 'Timed out waiting for the engine.' }
        $line = $read.Result
        if ($null -eq $line) { throw 'Engine exited before completing the UCI response.' }
        Write-Output $line
        if ($line -match '^info string (search )?error:') { throw $line }
        if ($line -match $Pattern) { return }
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Timed out waiting for the engine.' }
    }
}

try {
    if (-not $engine.Start()) { throw 'Could not start the engine.' }
    $started = $true
    $stderr = $engine.StandardError.ReadToEndAsync()
    $engine.StandardInput.AutoFlush = $true
    $engine.StandardInput.WriteLine('uci')
    Read-UciUntil '^uciok$' 15000
    $engine.StandardInput.WriteLine('isready')
    Read-UciUntil '^readyok$' 15000
    $engine.StandardInput.WriteLine('ucinewgame')

    $position = if ($Fen -eq 'startpos') { 'position startpos' } else { 'position fen ' + $Fen.Trim() }
    if ($Moves.Count -gt 0) { $position += ' moves ' + ($Moves -join ' ') }
    $engine.StandardInput.WriteLine($position)
    $engine.StandardInput.WriteLine('isready')
    Read-UciUntil '^readyok$' 15000
    $go = 'go depth ' + $Depth
    if ($MoveTimeMs -gt 0) { $go += ' movetime ' + $MoveTimeMs }
    $engine.StandardInput.WriteLine($go)
    $timeout = if ($MoveTimeMs -gt 0) { [Math]::Max(30000, $MoveTimeMs + 15000) } else { 300000 }
    Read-UciUntil '^bestmove ' $timeout
} finally {
    if ($started -and -not $engine.HasExited) {
        try {
            $engine.StandardInput.WriteLine('quit')
            $engine.StandardInput.Close()
        } catch {
            # The child may already have closed its input while exiting.
        }
        if (-not $engine.WaitForExit(2000)) { $engine.Kill(); $engine.WaitForExit() }
    }
    if ($stderr -and $stderr.IsCompleted) {
        $diagnostic = $stderr.Result
        if ($diagnostic) { Write-Warning $diagnostic.Trim() }
    }
    $engine.Dispose()
}

<#
.SYNOPSIS
  Start the Fairy-Stockfish oracle with Makruk selected and the Makruk NNUE net loaded.

.DESCRIPTION
  Sends "setoption name UCI_Variant value makruk" and "setoption name EvalFile value <net>"
  to Fairy-Stockfish, then forwards stdin (interactive or piped) to the engine.

  Tools live in the shared, git-ignored external\ folder of the MAIN checkout, so every
  worktree uses the same binaries. Override with $env:MAKRUK_EXTERNAL.

.EXAMPLE
  .\makruk\tools\run_fairy.ps1                        # interactive UCI session
  "position startpos`ngo perft 5" | .\makruk\tools\run_fairy.ps1
  .\makruk\tools\run_fairy.ps1 -NoNet                 # classical eval
#>
param(
    [switch]$NoNet
)

$External = $env:MAKRUK_EXTERNAL
if (-not $External) { $External = Join-Path $PSScriptRoot '..\..\external' }

$Engine = Join-Path $External 'Fairy-Stockfish\src\stockfish.exe'
$Net    = Join-Path $External 'nets\makruk-a8c621e24a8c.nnue'

if (-not (Test-Path $Engine)) { Write-Error "Fairy-Stockfish not found: $Engine (set MAKRUK_EXTERNAL)"; exit 1 }
if (-not $NoNet -and -not (Test-Path $Net)) { Write-Error "Makruk net not found: $Net (set MAKRUK_EXTERNAL)"; exit 1 }

# The child's stdin encoding comes from the console. Use UTF-8 without BOM:
# Fairy-SF rejects a BOM-prefixed first command ("Unknown command: <BOM>setoption").
try { [Console]::InputEncoding = New-Object System.Text.UTF8Encoding $false } catch {}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Engine
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$p = [System.Diagnostics.Process]::Start($psi)
$p.StandardInput.AutoFlush = $true
$p.StandardInput.WriteLine('setoption name UCI_Variant value makruk')
if (-not $NoNet) { $p.StandardInput.WriteLine("setoption name EvalFile value $($Net -replace '\\','/')") }

if ($MyInvocation.ExpectingInput) {
    foreach ($line in $input) { $p.StandardInput.WriteLine($line) }
    $p.StandardInput.WriteLine('quit')
} else {
    while (-not $p.HasExited) {
        $line = [Console]::In.ReadLine()
        if ($null -eq $line) { $p.StandardInput.WriteLine('quit'); break }
        $p.StandardInput.WriteLine($line)
        if ($line -eq 'quit') { break }
    }
}
$p.WaitForExit()
exit $p.ExitCode

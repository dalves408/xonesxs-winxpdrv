# Records USB traffic (including transfer data) while the Xbox controller is
# replugged, so the XP driver can copy the exact handshake Windows uses.
# Run from an elevated (Administrator) PowerShell.

$ErrorActionPreference = 'Stop'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host 'Please run this from an Administrator PowerShell.' -ForegroundColor Red
    exit 1
}

$name = 'xboxgip-usb'
# logman is picky about paths with spaces, so record to the folder Microsoft's
# USB tracing guide uses and copy the result next to this script afterwards.
$tracingDir = Join-Path $env:SystemRoot 'Tracing'
$etl  = Join-Path $tracingDir 'xboxgip-usb.etl'
$dest = Join-Path $PSScriptRoot 'usbtrace.etl'

# Runs logman, shows what it printed, and stops on failure.
function Invoke-Logman {
    param([string[]]$Arguments, [switch]$AllowFailure)
    $output = & logman @Arguments 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -and -not $AllowFailure) {
        Write-Host "logman $($Arguments -join ' ') failed:" -ForegroundColor Red
        Write-Host $output
        throw "logman exited with code $LASTEXITCODE"
    }
}

New-Item -ItemType Directory -Force $tracingDir | Out-Null

# Clean up anything left from an earlier run.
Invoke-Logman @('stop', $name) -AllowFailure
Invoke-Logman @('delete', $name) -AllowFailure
Remove-Item $etl, $dest -ErrorAction SilentlyContinue

# Same providers as Microsoft's "capture USB traces" guidance. PartialDataBusTrace
# adds the first bytes of each transfer, which is where the GIP packets are.
Invoke-Logman @('create', 'trace', $name, '-o', $etl, '-nb', '128', '640', '-bs', '128', '-ow')
Invoke-Logman @('update', 'trace', $name, '-p', 'Microsoft-Windows-USB-USBXHCI', '(Default,PartialDataBusTrace)')
Invoke-Logman @('update', 'trace', $name, '-p', 'Microsoft-Windows-USB-UCX', '(Default,PartialDataBusTrace)')
Invoke-Logman @('update', 'trace', $name, '-p', 'Microsoft-Windows-USB-USBHUB3', '(Default,PartialDataBusTrace)')
Invoke-Logman @('start', $name)

Write-Host ''
Write-Host 'Recording USB traffic.' -ForegroundColor Green
Write-Host '  1. Unplug the Xbox controller.'
Write-Host '  2. Wait 5 seconds, then plug it back in.'
Write-Host '  3. Wait until the Xbox button lights up, then about 5 more seconds.'
Write-Host '  4. Press A, then B, and move the left stick a little.'
Write-Host ''
Read-Host 'Press Enter here when done'

Invoke-Logman @('stop', $name)
Invoke-Logman @('delete', $name) -AllowFailure

# logman may add a numeric suffix to the file name; take the newest match.
$written = Get-ChildItem $tracingDir -Filter 'xboxgip-usb*.etl' |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $written) {
    Write-Host "No trace file was written to $tracingDir." -ForegroundColor Red
    exit 1
}

Copy-Item $written.FullName $dest -Force
Write-Host ("Saved {0} ({1:N0} KB). Tell Claude it's ready." -f $dest, ($written.Length / 1KB)) -ForegroundColor Green

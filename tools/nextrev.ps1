# Prints "<release> <beta>" for build.cmd: the next release number (one more
# than the highest published-style releases\rNN folder) and the next beta of it
# (one more than the highest releases\rNN-betaK folder for that release).
param([string]$Releases)

$dirs = Get-ChildItem -Directory -LiteralPath $Releases -ErrorAction SilentlyContinue

$release = 0
foreach ($d in $dirs) {
    if ($d.Name -match '^r(\d+)$') {
        $release = [Math]::Max($release, [int]$Matches[1])
    }
}
$next = $release + 1

$beta = 0
foreach ($d in $dirs) {
    if ($d.Name -match '^r(\d+)-beta(\d+)$' -and [int]$Matches[1] -eq $next) {
        $beta = [Math]::Max($beta, [int]$Matches[2])
    }
}

"$next $($beta + 1)"

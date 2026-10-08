param(
    [ValidateRange(1, 256)][int]$Jobs = 8,
    [string]$BashPath
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$makePath = (Get-Command make -ErrorAction Stop).Source
Get-Command gcc, g++, windres -ErrorAction Stop | Out-Null

if (-not $BashPath) {
    $gitPath = (Get-Command git -ErrorAction Stop).Source
    $gitRoot = Split-Path (Split-Path $gitPath -Parent) -Parent
    $BashPath = Join-Path $gitRoot 'bin\bash.exe'
}
if (-not (Test-Path -LiteralPath $BashPath)) {
    throw 'Git Bash was not found. Supply -BashPath with the path to Git for Windows bash.exe.'
}

$savedPath = $env:PATH
$gitUsrBin = Join-Path (Split-Path (Split-Path $BashPath -Parent) -Parent) 'usr\bin'
$env:PATH = $env:PATH + ';' + $gitUsrBin
$buildDir = Join-Path $repoRoot 'CPP\7zip\Bundles\Alone2'
Push-Location $buildDir
try {
    & $makePath "-j$Jobs" -f ../../cmpl_gcc.mak O=_o_pigz USE_PIGZ=1 MSYSTEM=MINGW64 "SHELL=$($BashPath.Replace('\', '/'))"
    if ($LASTEXITCODE -ne 0) {
        throw "7-Zip + pigz build failed with exit code $LASTEXITCODE. zlib headers and a static libz must be available to GCC."
    }
} finally {
    Pop-Location
    $env:PATH = $savedPath
}

$binDir = Join-Path $repoRoot 'bin'
New-Item -ItemType Directory -Path $binDir -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildDir '_o_pigz\7zz.exe') -Destination (Join-Path $binDir '7z-pigz.exe')
Copy-Item -LiteralPath (Join-Path $repoRoot 'DOC\License.txt') -Destination (Join-Path $binDir '7zip-License.txt')
Copy-Item -LiteralPath (Join-Path $repoRoot 'DOC\pigz.txt') -Destination $binDir
Write-Output "Built: $(Join-Path $binDir '7z-pigz.exe')"

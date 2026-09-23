<#
.SYNOPSIS
    Build cftts.exe, the speech synthesiser, into the game's Release directory.

.DESCRIPTION
    THIS IS THE ONLY PART OF THE COMPANION THAT NEEDS A C COMPILER. sherpa-onnx
    is reached through cgo, so this build wants the MinGW toolchain the engine
    already requires. cfvoice itself no longer needs one -- dropping that
    dependency is a side effect of moving the synthesiser out here, and a
    welcome one.

    It lands in Release\tts\ together with the libraries it loads, because
    Windows resolves a DLL from the directory of the exe that loads it and
    onnxruntime has no business sitting beside a cfvoice.exe that does not use
    it any more.

    The DLLs are not in the repository -- they are prebuilt binaries that come
    down with the Go module -- so they are copied out of the module cache here
    rather than being committed. That is the same arrangement that used to put
    them beside cfvoice.exe; only the destination has changed.
#>
[CmdletBinding()]
param(
    [string]$Release = "$PSScriptRoot\..\build-catfight\Release",
    [switch]$SkipDlls
)

$ErrorActionPreference = 'Stop'

$go = 'C:\Program Files\Go\bin\go.exe'
if (-not (Test-Path $go)) {
    $cmd = Get-Command go -ErrorAction SilentlyContinue
    if (-not $cmd) { throw "go not found -- install it or put it on PATH" }
    $go = $cmd.Source
}

$out = Join-Path $Release 'tts'
New-Item -ItemType Directory -Path $out -Force | Out-Null

# MinGW for cgo, and Go ahead of it on PATH.
$env:PATH = "$(Split-Path $go);C:\msys64\mingw64\bin;$env:PATH"
$env:CGO_ENABLED = '1'

Push-Location $PSScriptRoot
try {
    Write-Host "building cftts.exe" -ForegroundColor Cyan
    & $go build -o (Join-Path $out 'cftts.exe') .
    if ($LASTEXITCODE -ne 0) { throw "go build failed" }
} finally {
    Pop-Location
}

if (-not $SkipDlls) {
    <#
        The three libraries sherpa needs, out of the Go module cache.

        Located by asking Go where the module is rather than by guessing a
        version-stamped path: the cache directory carries the version in its
        name, so a hardcoded one silently stops matching on the next bump and
        the failure is 0xc0000135 at runtime with no other explanation.
    #>
    Push-Location $PSScriptRoot
    try {
        $mod = (& $go list -m -f '{{.Dir}}' github.com/k2-fsa/sherpa-onnx-go-windows 2>$null)
    } finally {
        Pop-Location
    }

    if (-not $mod -or -not (Test-Path $mod)) {
        throw "could not locate sherpa-onnx-go-windows in the module cache -- run 'go mod download' in tts\"
    }

    $libDir = Join-Path $mod 'lib\x86_64-pc-windows-gnu'
    if (-not (Test-Path $libDir)) { throw "no lib directory at $libDir" }

    foreach ($dll in @('onnxruntime.dll', 'sherpa-onnx-c-api.dll', 'sherpa-onnx-cxx-api.dll')) {
        $src = Join-Path $libDir $dll
        if (-not (Test-Path $src)) { throw "missing $dll in $libDir" }
        Copy-Item $src $out -Force
    }
    Write-Host "  copied sherpa libraries from the module cache" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "  $out" -ForegroundColor Green
Get-ChildItem $out | ForEach-Object {
    "    {0,-28} {1,8:N0} KB" -f $_.Name, ($_.Length / 1KB)
}
Write-Host ""
Write-Host "  cftts.exe is GPLv3 -- see tts\README.md" -ForegroundColor DarkGray

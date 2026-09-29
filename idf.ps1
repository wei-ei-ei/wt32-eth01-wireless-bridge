#!/usr/bin/env pwsh

$ErrorActionPreference = "Stop"

$repoRoot = $PSScriptRoot
$idfPath = if ($env:WT32_IDF_PATH) {
    $env:WT32_IDF_PATH
} else {
    "E:\esp-idf\esp-idf-v6.0.2"
}
$idfToolsPath = if ($env:WT32_IDF_TOOLS_PATH) {
    $env:WT32_IDF_TOOLS_PATH
} else {
    "E:\.espressif"
}
$buildTemp = if ($env:WT32_TEMP) {
    $env:WT32_TEMP
} else {
    Join-Path $repoRoot "tmp\esp_temp"
}

$exportScript = Join-Path $idfPath "export.ps1"
if (-not (Test-Path -LiteralPath $exportScript)) {
    throw "ESP-IDF export.ps1 not found: $exportScript. Set WT32_IDF_PATH first."
}

New-Item -ItemType Directory -Force -Path $buildTemp | Out-Null
$env:IDF_TOOLS_PATH = $idfToolsPath
$env:TEMP = $buildTemp
$env:TMP = $buildTemp

Push-Location $repoRoot
try {
    . $exportScript
    $env:IDF_CCACHE_ENABLE = "0"

    if ($args.Count -eq 0) {
        Write-Host "WT32 bridge ESP-IDF environment is ready."
        Write-Host "Examples:"
        Write-Host "  .\idf.ps1 build"
        Write-Host "  .\idf.ps1 menuconfig"
        Write-Host "  .\idf.ps1 -p COM15 flash monitor"
        return
    }

    & idf.py @args
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}

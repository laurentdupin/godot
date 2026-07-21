param(
    [Parameter(Mandatory = $true)]
    [string]$EditorPath,
    [string]$ConsumerPath = ""
)

$ErrorActionPreference = "Stop"
$editor = (Resolve-Path -LiteralPath $EditorPath).Path
$consumer = $editor
if ($ConsumerPath) {
    $consumer = (Resolve-Path -LiteralPath $ConsumerPath).Path
}
$testsRoot = $PSScriptRoot
$successProject = Join-Path $testsRoot "export_package_success"
$failureProject = Join-Path $testsRoot "export_package_failure"
$outputRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("hcsr-export-smoke-" + [System.Guid]::NewGuid().ToString("N"))
$successPack = Join-Path $outputRoot "success.pck"
$linuxPack = Join-Path $outputRoot "success-linux.pck"
$failurePack = Join-Path $outputRoot "failure.pck"

foreach ($project in @($successProject, $failureProject)) {
    $projectCache = Join-Path $project ".godot"
    if (Test-Path -LiteralPath $projectCache) {
        Remove-Item -LiteralPath $projectCache -Recurse -Force
    }
}

New-Item -ItemType Directory -Path $outputRoot | Out-Null
try {
    & $editor --headless --path $successProject --export-pack "HCSR Package Smoke" $successPack
    if ($LASTEXITCODE -ne 0) {
        throw "Successful HCSR package export returned exit code $LASTEXITCODE."
    }

    & $consumer --headless --main-pack $successPack --script res://verify_export.gd
    if ($LASTEXITCODE -ne 0) {
        throw "HCSR package consumer verification returned exit code $LASTEXITCODE."
    }

    & $editor --headless --path $successProject --export-pack "HCSR Package Linux Cross Smoke" $linuxPack
    if ($LASTEXITCODE -ne 0) {
        throw "Windows-to-Linux HCSR package export returned exit code $LASTEXITCODE."
    }

    & $consumer --headless --main-pack $linuxPack --script res://verify_export.gd
    if ($LASTEXITCODE -ne 0) {
        throw "Windows-to-Linux HCSR package consumer verification returned exit code $LASTEXITCODE."
    }

    & $editor --headless --path $failureProject --export-pack "HCSR Package Failure Smoke" $failurePack
    if ($LASTEXITCODE -eq 0) {
        throw "Malformed HCSR entry export unexpectedly succeeded."
    }

    Write-Host "HCSR package selection, auxiliary-source, and failure-propagation smokes passed."
    exit 0
} finally {
    if (Test-Path -LiteralPath $outputRoot) {
        Remove-Item -LiteralPath $outputRoot -Recurse -Force
    }
}

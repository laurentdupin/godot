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

function Invoke-PackageConsumer([string]$PackPath) {
    if ($consumer -eq $editor) {
        & $consumer --headless --main-pack $PackPath | Out-Host
        return $LASTEXITCODE
    }

    $consumerMain = $consumer.Replace(".console.exe", ".exe")
    if (-not (Test-Path -LiteralPath $consumerMain)) {
        throw "Release consumer main executable is missing: $consumerMain"
    }
    $packagedMain = Join-Path $outputRoot "hcsr_package_consumer.exe"
    $packagedConsole = Join-Path $outputRoot "hcsr_package_consumer.console.exe"
    $packagedPck = Join-Path $outputRoot "hcsr_package_consumer.pck"
    Copy-Item -LiteralPath $consumerMain -Destination $packagedMain -Force
    Copy-Item -LiteralPath $consumer -Destination $packagedConsole -Force
    Copy-Item -LiteralPath $PackPath -Destination $packagedPck -Force
    & $packagedConsole --headless | Out-Host
    return $LASTEXITCODE
}

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

    $consumerExitCode = Invoke-PackageConsumer $successPack
    if ($consumerExitCode -ne 0) {
        throw "HCSR package consumer verification returned exit code $consumerExitCode."
    }

    & $editor --headless --path $successProject --export-pack "HCSR Package Linux Cross Smoke" $linuxPack
    if ($LASTEXITCODE -ne 0) {
        throw "Windows-to-Linux HCSR package export returned exit code $LASTEXITCODE."
    }

    $consumerExitCode = Invoke-PackageConsumer $linuxPack
    if ($consumerExitCode -ne 0) {
        throw "Windows-to-Linux HCSR package consumer verification returned exit code $consumerExitCode."
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

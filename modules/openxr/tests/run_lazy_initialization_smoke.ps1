param(
    [Parameter(Mandatory = $true)]
    [string]$EditorPath
)

$ErrorActionPreference = "Stop"
$editor = (Resolve-Path -LiteralPath $EditorPath).Path
$project = Join-Path $PSScriptRoot "lazy_initialization"
$previousRuntime = $env:XR_RUNTIME_JSON
$env:XR_RUNTIME_JSON = Join-Path ([System.IO.Path]::GetTempPath()) "hcsr-openxr-deliberately-missing-runtime.json"

try {
    $ErrorActionPreference = "Continue"
    $startupOutput = (& $editor --headless --path $project --script res://verify_lazy_initialization.gd 2>&1 | Out-String)
    $startupExitCode = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($startupExitCode -ne 0) {
        throw "Deferred OpenXR startup smoke returned exit code $startupExitCode.`n$startupOutput"
    }
    if (-not $startupOutput.Contains("OPENXR_LAZY_STARTUP_READY")) {
        throw "Deferred OpenXR startup marker is missing.`n$startupOutput"
    }
    $startupDiagnostics = $startupOutput.Replace("OPENXR_LAZY_STARTUP_READY", "")
    if ($startupDiagnostics -match "OpenXR|XR_ERROR|XR_RUNTIME_JSON") {
        throw "Deferred OpenXR startup queried or diagnosed the runtime.`n$startupOutput"
    }

    $ErrorActionPreference = "Continue"
    $probeOutput = (& $editor --headless --path $project --script res://verify_lazy_initialization.gd -- --probe 2>&1 | Out-String)
    $probeExitCode = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($probeExitCode -ne 0 -or -not $probeOutput.Contains("OPENXR_LAZY_PROBE_RESULTS=false,false;initialized=false")) {
        throw "Repeatable OpenXR availability probe failed.`n$probeOutput"
    }
    $probeDiagnostics = $probeOutput.Replace("OPENXR_LAZY_STARTUP_READY", "").Replace("OPENXR_LAZY_PROBE_RESULTS=false,false;initialized=false", "")
    if ($probeDiagnostics -match "OpenXR|XR_ERROR|XR_RUNTIME_JSON") {
        throw "Unavailable OpenXR probe emitted diagnostics.`n$probeOutput"
    }

    $ErrorActionPreference = "Continue"
    $initializeOutput = (& $editor --headless --path $project --script res://verify_lazy_initialization.gd -- --initialize 2>&1 | Out-String)
    $initializeExitCode = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($initializeExitCode -ne 0) {
        throw "Explicit OpenXR initialization smoke returned exit code $initializeExitCode.`n$initializeOutput"
    }
    if (-not $initializeOutput.Contains("OPENXR_LAZY_INITIALIZE_RESULT=false")) {
        throw "Explicit OpenXR initialization did not report the forced missing-runtime failure.`n$initializeOutput"
    }
    if ($initializeOutput -notmatch "OpenXR|XR_ERROR|XR_RUNTIME_JSON") {
        throw "Explicit OpenXR initialization did not query the forced missing runtime.`n$initializeOutput"
    }

    Write-Host "OpenXR deferred startup and explicit initialization smoke passed."
    exit 0
} finally {
    $env:XR_RUNTIME_JSON = $previousRuntime
}

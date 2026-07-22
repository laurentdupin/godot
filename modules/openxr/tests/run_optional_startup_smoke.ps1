param(
    [Parameter(Mandatory = $true)]
    [string]$EditorPath
)

$ErrorActionPreference = "Stop"
$editor = (Resolve-Path -LiteralPath $EditorPath).Path
$project = Join-Path $PSScriptRoot "optional_startup"
$previousRuntime = $env:XR_RUNTIME_JSON
$env:XR_RUNTIME_JSON = Join-Path ([System.IO.Path]::GetTempPath()) "godot-openxr-deliberately-missing-runtime.json"

try {
    $ErrorActionPreference = "Continue"
    $optionalOutput = (& $editor --headless --path $project --script res://verify_optional_startup.gd 2>&1 | Out-String)
    $optionalExitCode = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($optionalExitCode -ne 0) {
        throw "Optional OpenXR startup smoke returned exit code $optionalExitCode.`n$optionalOutput"
    }
    if (-not $optionalOutput.Contains("OPENXR_OPTIONAL_STARTUP_INITIALIZED=false")) {
        throw "Optional OpenXR startup did not continue in desktop mode.`n$optionalOutput"
    }
    $optionalDiagnostics = $optionalOutput.Replace("OPENXR_OPTIONAL_STARTUP_INITIALIZED=false", "")
    if ($optionalDiagnostics -match "OpenXR|XR_ERROR|XR_RUNTIME_JSON") {
        throw "Optional OpenXR startup reported the expected missing runtime.`n$optionalOutput"
    }

    $ErrorActionPreference = "Continue"
    $explicitOutput = (& $editor --headless --xr-mode on --path $project --script res://verify_optional_startup.gd 2>&1 | Out-String)
    $explicitExitCode = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($explicitExitCode -ne 0) {
        throw "Explicit OpenXR startup smoke returned exit code $explicitExitCode.`n$explicitOutput"
    }
    if ($explicitOutput -notmatch "OpenXR|XR_ERROR|XR_RUNTIME_JSON") {
        throw "Explicit --xr-mode on startup did not report the forced missing runtime.`n$explicitOutput"
    }

    Write-Host "OpenXR optional and explicit startup smoke passed."
    exit 0
} finally {
    $env:XR_RUNTIME_JSON = $previousRuntime
}

param(
    [Parameter(Mandatory = $true)]
    [string]$EditorPath
)

$ErrorActionPreference = "Stop"
$editor = (Resolve-Path -LiteralPath $EditorPath).Path
$project = Join-Path $PSScriptRoot "lazy_initialization"

$ErrorActionPreference = "Continue"
$output = (& $editor --headless --path $project --script res://verify_repeatable_lifecycle.gd 2>&1 | Out-String)
$exitCode = $LASTEXITCODE
$ErrorActionPreference = "Stop"

if ($exitCode -ne 0) {
    throw "Repeatable OpenXR lifecycle smoke returned exit code $exitCode.`n$output"
}
if ($output.Contains("OPENXR_REPEATABLE_LIFECYCLE_UNAVAILABLE")) {
    Write-Host "OpenXR repeatable lifecycle smoke skipped: no HMD/runtime is available."
    exit 0
}
if (-not $output.Contains("OPENXR_REPEATABLE_LIFECYCLE_OK")) {
    throw "Repeatable OpenXR lifecycle completion marker is missing.`n$output"
}
if ($output -match "RID allocations.*were leaked|nonexistent connection|ERROR:") {
    throw "Repeatable OpenXR lifecycle emitted teardown diagnostics.`n$output"
}

Write-Host "OpenXR repeatable initialize/uninitialize lifecycle smoke passed."

param(
    [Parameter(Mandatory = $true)]
    [string]$EditorPath,

    [string]$RenderingDriver = "d3d12"
)

$ErrorActionPreference = "Stop"
$editor = (Resolve-Path -LiteralPath $EditorPath).Path
$project = Join-Path $PSScriptRoot "lazy_initialization"

& $editor --rendering-driver $RenderingDriver --path $project --script res://verify_deferred_controller_poses.gd
if ($LASTEXITCODE -ne 0) {
    throw "Deferred OpenXR controller pose smoke failed with exit code $LASTEXITCODE."
}

Write-Host "OpenXR deferred controller pose smoke passed."

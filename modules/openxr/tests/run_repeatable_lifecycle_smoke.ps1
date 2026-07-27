param(
    [Parameter(Mandatory = $true)]
    [string]$EditorPath
)

$ErrorActionPreference = "Stop"
$editor = (Resolve-Path -LiteralPath $EditorPath).Path
$project = Join-Path $PSScriptRoot "lazy_initialization"
$standardOutputPath = [System.IO.Path]::GetTempFileName()
$standardErrorPath = [System.IO.Path]::GetTempFileName()

try {
    $process = Start-Process `
        -FilePath $editor `
        -ArgumentList @(
            "--rendering-method", "mobile",
            "--rendering-driver", "d3d12",
            "--path", $project,
            "--script", "res://verify_repeatable_lifecycle.gd"
        ) `
        -RedirectStandardOutput $standardOutputPath `
        -RedirectStandardError $standardErrorPath `
        -PassThru

    if (-not $process.WaitForExit(45000)) {
        $childProcessIds = Get-CimInstance Win32_Process |
            Where-Object ParentProcessId -eq $process.Id |
            Select-Object -ExpandProperty ProcessId
        foreach ($childProcessId in $childProcessIds) {
            Get-Process -Id $childProcessId -ErrorAction SilentlyContinue | Stop-Process -Force
        }
        Get-Process -Id $process.Id -ErrorAction SilentlyContinue | Stop-Process -Force
        throw "Repeatable OpenXR lifecycle smoke did not exit naturally within 45 seconds."
    }

    $output = (Get-Content -LiteralPath $standardOutputPath -Raw) +
        (Get-Content -LiteralPath $standardErrorPath -Raw)
} finally {
    Remove-Item -LiteralPath $standardOutputPath, $standardErrorPath -ErrorAction SilentlyContinue
}

if ($output.Contains("OPENXR_REPEATABLE_LIFECYCLE_UNAVAILABLE")) {
    Write-Host "OpenXR repeatable lifecycle smoke skipped: no HMD/runtime is available."
    exit 0
}
if (-not $output.Contains("OPENXR_REPEATABLE_RENDERED_LIFECYCLE_OK")) {
    throw "Repeatable OpenXR lifecycle completion marker is missing.`n$output"
}
if ($output -match "RID allocations.*were leaked|nonexistent connection|ERROR:|XR_ERROR_|Couldn't locate views|failed to (begin|end) frame") {
    throw "Repeatable OpenXR lifecycle emitted teardown diagnostics.`n$output"
}

Write-Host "OpenXR repeatable initialize/uninitialize lifecycle smoke passed."

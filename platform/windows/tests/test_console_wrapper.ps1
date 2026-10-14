[CmdletBinding()]
param([string]$GodotConsole = (Join-Path $PSScriptRoot '../../../bin/godot.windows.editor.x86_64.mono.console.exe'))
$ErrorActionPreference = 'Stop'
$GodotConsole = [IO.Path]::GetFullPath($GodotConsole)
$direct = $GodotConsole.Substring(0, $GodotConsole.Length - '.console.exe'.Length) + '.exe'
$fixture = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('godot console wait ' + [guid]::NewGuid().ToString('N'))))
$prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
if (-not $fixture.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe fixture path.' }
try {
    New-Item -ItemType Directory -Path $fixture | Out-Null
    'config_version=5' | Set-Content -LiteralPath (Join-Path $fixture 'project.godot')
    @'
extends SceneTree
func _initialize() -> void:
    var args := OS.get_cmdline_user_args()
    if args.has("--helper"):
        var pid := OS.create_process("powershell.exe", PackedStringArray(["-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 12"]), false)
        print("HELPER_PID=", pid)
        if pid <= 0:
            quit(1)
            return
    print("SCRIPT_FINISHED")
    quit(37 if args.has("--failure") else 0)
'@ | Set-Content -LiteralPath (Join-Path $fixture 'probe.gd')
    foreach ($case in @(
        @{name='wrapper'; binary=$GodotConsole; args=''; code=0; detached=$false},
        @{name='wrapper-helper'; binary=$GodotConsole; args='--helper'; code=0; detached=$false},
        @{name='wrapper-failure'; binary=$GodotConsole; args='--helper --failure'; code=37; detached=$false},
        @{name='direct-helper'; binary=$direct; args='--helper'; code=0; detached=$true}
    )) {
        $output = Join-Path $fixture ($case.name + '.out')
        $errors = Join-Path $fixture ($case.name + '.err')
        $helper = $null
        $process = $null
        try {
            $watch = [Diagnostics.Stopwatch]::StartNew()
            $process = Start-Process -FilePath $case.binary -WindowStyle Hidden -PassThru `
                -ArgumentList ('--headless --path "' + $fixture + '" --script res://probe.gd -- ' + $case.args) `
                -RedirectStandardOutput $output -RedirectStandardError $errors
            $null = $process.Handle
            if (-not $process.WaitForExit(30000)) { throw "Timed out: $($case.name)" }
            $watch.Stop()
            $text = Get-Content -LiteralPath $output -Raw
            if ($text -match 'HELPER_PID=(\d+)') { $helper = Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue }
            if ($case.detached) {
                if ($null -eq $helper) { throw 'Direct launch did not leave its background helper alive.' }
            } elseif ($helper -and -not $helper.WaitForExit(3000)) {
                throw 'Console wrapper did not clean up its remaining descendant.'
            }
            if ($process.ExitCode -ne $case.code -or $text -notmatch 'SCRIPT_FINISHED' -or $watch.Elapsed.TotalSeconds -gt 5) {
                throw "Incorrect wrapper result: $($case.name), exit=$($process.ExitCode), seconds=$($watch.Elapsed.TotalSeconds). $(Get-Content -LiteralPath $errors -Raw)"
            }
            Write-Output "CONSOLE_WRAPPER_OK case=$($case.name) exit=$($process.ExitCode) seconds=$($watch.Elapsed.TotalSeconds)"
        } finally {
            if ($process -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
            if ($helper -and -not $helper.HasExited) {
                $owned = Get-CimInstance Win32_Process -Filter "ProcessId=$($helper.Id)"
                if ($owned.Name -eq 'powershell.exe' -and $owned.CommandLine -like '*Start-Sleep -Seconds 12*') { Stop-Process -Id $helper.Id }
            }
        }
    }
} finally {
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
}

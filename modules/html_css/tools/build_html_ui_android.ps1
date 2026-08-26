param(
    [ValidateSet("template_debug", "template_release")]
    [string]$Target = "template_debug",

    [ValidateRange(1, 64)]
    [int]$Jobs = 6,

    [ValidateSet("arm64", "x86_64")]
    [string]$Architecture = "arm64",

    [string]$AndroidSdkRoot = $env:ANDROID_HOME,

    [string]$JavaHome = $env:JAVA_HOME,

    [string]$BuildToolsVersion = $env:ANDROID_BUILD_TOOLS_VERSION,

    [switch]$ExportBackdrop,

    [switch]$Install,

    [switch]$Launch,

    [string]$DeviceSerial = ""
)

$ErrorActionPreference = "Stop"

if ($Install -or $Launch) {
    $ExportBackdrop = $true
}

$godotRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$buildProfile = Join-Path $godotRoot "modules\html_css\build_profiles\html_ui_release.build"
$projectDirectory = Join-Path $godotRoot "modules\html_css\examples\backdrop_2d"
$outputApk = Join-Path $projectDirectory "build-android\HCSRBackdropGallery.apk"

if ([string]::IsNullOrWhiteSpace($AndroidSdkRoot)) {
    throw "Set ANDROID_HOME or pass -AndroidSdkRoot. Godot requires an SDK containing NDK 29.0.14206865."
}
$AndroidSdkRoot = (Resolve-Path $AndroidSdkRoot).Path
$requiredNdk = Join-Path $AndroidSdkRoot "ndk\29.0.14206865"
if (-not (Test-Path -LiteralPath $requiredNdk)) {
    throw "Godot's required Android NDK was not found at $requiredNdk."
}

if ([string]::IsNullOrWhiteSpace($JavaHome)) {
    throw "Set JAVA_HOME or pass -JavaHome with a JDK suitable for the Android Gradle build."
}
$JavaHome = (Resolve-Path $JavaHome).Path

if ([string]::IsNullOrWhiteSpace($BuildToolsVersion)) {
    $BuildToolsVersion = Get-ChildItem -LiteralPath (Join-Path $AndroidSdkRoot "build-tools") -Directory |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1 -ExpandProperty Name
}
if ([string]::IsNullOrWhiteSpace($BuildToolsVersion)) {
    throw "No Android Build Tools installation was found under $AndroidSdkRoot."
}

$env:ANDROID_HOME = $AndroidSdkRoot
$env:ANDROID_SDK_ROOT = $AndroidSdkRoot
$env:ANDROID_NDK_ROOT = $requiredNdk
$env:JAVA_HOME = $JavaHome
$env:ANDROID_BUILD_TOOLS_VERSION = $BuildToolsVersion

$pythonScripts = Join-Path $env:APPDATA "Python\Python312\Scripts"
if (Test-Path -LiteralPath $pythonScripts) {
    $env:PATH = "$pythonScripts;$env:PATH"
}

$sconsArguments = @(
    "-3.12",
    "-m",
    "SCons",
    "platform=android",
    "arch=$Architecture",
    "target=$Target",
    "generate_android_binaries=yes",
    "swappy=no",
    "modules_enabled_by_default=no",
    "module_freetype_enabled=yes",
    "module_gdscript_enabled=yes",
    "module_glslang_enabled=yes",
    "module_html_css_enabled=yes",
    "module_msdfgen_enabled=yes",
    "module_svg_enabled=yes",
    "module_text_server_fb_enabled=yes",
    "module_html_css_renderer=hcsr_old",
    "build_profile=$buildProfile",
    "-j$Jobs"
)

Push-Location $godotRoot
try {
    & py @sconsArguments
    if ($LASTEXITCODE -ne 0) {
        throw "The Godot HCSR Android build failed with exit code $LASTEXITCODE."
    }

    $templateName = if ($Target -eq "template_debug") { "android_debug.apk" } else { "android_release.apk" }
    $template = Join-Path $godotRoot "bin\$templateName"
    if (-not (Test-Path -LiteralPath $template)) {
        throw "The expected Android template was not produced at $template."
    }
    Write-Host "HCSR Android template: $template"

    if ($ExportBackdrop) {
        if ($Target -ne "template_debug") {
            throw "The backdrop preset currently exports the debug template. Use -Target template_debug with -ExportBackdrop."
        }

        $editor = Join-Path $godotRoot "bin\godot.windows.editor.dev.x86_64.console.exe"
        if (-not (Test-Path -LiteralPath $editor)) {
            throw "A Godot editor build is required to export the backdrop example: $editor"
        }

        New-Item -ItemType Directory -Force -Path (Split-Path $outputApk) | Out-Null
        & $editor --headless --path $projectDirectory --export-debug Android $outputApk
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $outputApk)) {
            throw "The backdrop Android export failed with exit code $LASTEXITCODE."
        }
        Write-Host "Backdrop APK: $outputApk"
    }

    if ($Install -or $Launch) {
        $adb = Join-Path $AndroidSdkRoot "platform-tools\adb.exe"
        if (-not (Test-Path -LiteralPath $adb)) {
            throw "adb was not found at $adb."
        }
        $adbTarget = if ([string]::IsNullOrWhiteSpace($DeviceSerial)) { @() } else { @("-s", $DeviceSerial) }
    }

    if ($Install) {
        & $adb @adbTarget install -r $outputApk
        if ($LASTEXITCODE -ne 0) {
            throw "Installing the backdrop APK failed with exit code $LASTEXITCODE."
        }
    }

    if ($Launch) {
        & $adb @adbTarget shell am start -W -n "com.hcsr.backdropgallery/com.godot.game.GodotAppLauncher"
        if ($LASTEXITCODE -ne 0) {
            throw "Launching the backdrop application failed with exit code $LASTEXITCODE."
        }
    }
} finally {
    Pop-Location
}

param(
    [ValidateSet("hcsr", "none")]
    [string]$Renderer = "hcsr",

    [ValidateRange(1, 64)]
    [int]$Jobs = 6,

    [switch]$ExportBackdrop,

    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"

$godotRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$buildProfile = Join-Path $godotRoot "modules\html_css\build_profiles\html_ui_release.build"
$suffix = "html_ui"

$sconsArguments = @(
    "-3.12",
    "-m",
    "SCons",
    "platform=windows",
    "target=template_release",
    "production=yes",
    "optimize=size_extra",
    "lto=full",
    "angle=no",
    "opengl3=no",
    "vulkan=yes",
    "d3d12=yes",
    "modules_enabled_by_default=no",
    "module_freetype_enabled=yes",
    "module_gdscript_enabled=yes",
    "module_glslang_enabled=yes",
    "module_html_css_enabled=yes",
    "module_msdfgen_enabled=yes",
    "module_svg_enabled=yes",
    "module_text_server_fb_enabled=yes",
    "module_html_css_renderer=$Renderer",
    "build_profile=$buildProfile",
    "extra_suffix=$suffix",
    "-j$Jobs"
)

Push-Location $godotRoot
try {
    & py @sconsArguments
    if ($LASTEXITCODE -ne 0) {
        throw "The Godot HTML UI release template build failed with exit code $LASTEXITCODE."
    }

    $template = Join-Path $godotRoot "bin\godot.windows.template_release.x86_64.$suffix.exe"
    if (-not (Test-Path -LiteralPath $template)) {
        throw "The expected release template was not produced at $template."
    }

    Write-Host "HTML UI release template: $template"

    if (-not $ExportBackdrop) {
        return
    }

    if ($Renderer -ne "hcsr") {
        throw "-ExportBackdrop currently requires -Renderer hcsr so the editor's HCSR package compiler and release runtime match."
    }

    $projectDirectory = Join-Path $godotRoot "modules\html_css\examples\backdrop_2d"
    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
        $OutputDirectory = Join-Path $projectDirectory "build-size-optimized"
    } elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
        $OutputDirectory = Join-Path $godotRoot $OutputDirectory
    }

    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $outputExecutable = Join-Path $OutputDirectory "HCSRBackdropGallery.exe"
    $outputPackage = Join-Path $OutputDirectory "HCSRBackdropGallery.pck"
    $diagnosticConsole = Join-Path $OutputDirectory "HCSRBackdropGallery.console.exe"
    Remove-Item -LiteralPath $outputExecutable, $outputPackage, $diagnosticConsole -Force -ErrorAction SilentlyContinue
    Copy-Item -LiteralPath $template -Destination $outputExecutable -Force

    $editor = Join-Path $godotRoot "bin\godot.windows.editor.dev.x86_64.console.exe"
    if (-not (Test-Path -LiteralPath $editor)) {
        throw "A Godot editor build is required to compile the release PCK: $editor"
    }

    & $editor --headless --path $projectDirectory --export-pack "Windows Desktop" $outputPackage
    if (-not (Test-Path -LiteralPath $outputPackage)) {
        throw "The backdrop release package was not produced. Editor exit code: $LASTEXITCODE"
    }

    Write-Host "Backdrop executable: $outputExecutable"
    Write-Host "Backdrop package:    $outputPackage"
} finally {
    Pop-Location
}

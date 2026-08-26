# HTML/CSS Renderer Integration Notes

## Renderer selection

The module has one mutually exclusive renderer selector:

```text
module_html_css_renderer=hcsr_old|hcsr_runtime|none
```

`hcsr_old` is the default. It uses the frozen old architecture in
`thirdparty/hcsr_old` and statically links its NativeAOT bridge. `hcsr_runtime`
uses the current architecture and its runtime ABI from the checkout selected by
`module_html_css_hcsr_runtime_root`. The choices are mutually exclusive; one
Godot executable contains at most one HCSR version and keeps the normal Godot
executable name. `none` keeps the raw CPU-frame receiver without an HTML engine.

The HCSR integration supports x86_64 and ARM64 builds on Windows, Linux, and
macOS, plus Android ARM64 and x86_64. A Windows editor can be built with:

```text
python -m SCons platform=windows target=editor module_html_css_renderer=hcsr_old
```

The repository build script selects the sibling `HCSR` checkout automatically
when `hcsr_runtime` is chosen. A direct SCons build must provide that checkout;
the current runtime editor is currently limited to Windows x86_64 with D3D12:

```text
python -m SCons platform=windows target=editor module_html_css_renderer=hcsr_runtime module_html_css_hcsr_runtime_root=../HCSR
```

An old-HCSR-enabled Linux release template can select the provider explicitly:

```bash
scons platform=linuxbsd target=template_release production=yes module_html_css_renderer=hcsr_old -j8
```

Select `module_html_css_renderer=none` explicitly to build the module without an
external renderer provider. Such a build is not a valid packaged-player template
for an HCSR project.

For Android, set `ANDROID_HOME` to an SDK containing Godot's pinned NDK
`29.0.14206865`, set `JAVA_HOME`, and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File modules\html_css\tools\build_html_ui_android.ps1 -ExportBackdrop
```

The script builds the HCSR NativeAOT/codecs archive, statically links it into
the ARM64 Godot template, and optionally exports the backdrop APK. Pass
`-Architecture x86_64` when producing an emulator template. Add
`-Install -Launch -DeviceSerial <serial>` to deploy it through ADB. The APK
contains HCSR inside `libgodot_android.so`; it does not ship an HCSR DLL or
sidecar shared library. The initial Android profile disables Swappy because it
is not vendored in this checkout; add Swappy before doing final frame-pacing
performance work.

An animated 2D backdrop-filter gallery is available under
`modules/html_css/examples/backdrop_2d`. It demonstrates eight simultaneous
rounded regions over live Godot canvas content and is intended as the visual
smoke example for backdrop resize behavior.

## Size-optimized HTML UI release

`build_profiles/html_ui_release.build` keeps the HTML/CSS module, GDScript,
fallback text rendering, Godot's runtime shader compiler, 2D and 3D rendering,
while removing unused physics, navigation, XR, deprecated compatibility code,
and modules that are not explicit dependencies. The build script supplies the
module-selection flags because SCons resolves modules before applying
feature-profile options. FreeType, MSDF generation, and SVG font support are
retained for ordinary Godot UI. Vulkan and D3D12 remain available, while the
unused OpenGL compatibility backend is disabled.

Build the size-optimized statically linked HCSR template with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File modules\html_css\tools\build_html_ui_release.ps1
```

Add `-ExportBackdrop` to also create a runnable backdrop gallery under
`modules/html_css/examples/backdrop_2d/build-size-optimized`.

By default, a missing HCSR archive is produced by the matching platform build
script under `thirdparty/hcsr_old/tools`. Windows and Android packages can be built
from Windows. Linux packages must be built on the matching Linux architecture;
macOS can produce either architecture from a macOS host. Set
`module_html_css_hcsr_auto_build=no` to require an existing archive, or pass
`module_html_css_hcsr_lib_path=<path>` to use an explicit one. HCSR currently
supports CPU, Vulkan, D3D12, and Metal presentation. `BACKEND_GPU_AUTO` selects
the backend matching Godot's active Vulkan, D3D12, or Metal renderer; explicit
GPU requests never silently upload a CPU frame while claiming GPU mode. Windows
supports CPU, Vulkan, and D3D12; Linux and Android support CPU and Vulkan; macOS
supports CPU and Metal.

Current platform packages:

| Godot target | Architectures | HCSR backends |
| --- | --- | --- |
| Windows | x86_64, ARM64 | CPU, Vulkan, D3D12 |
| Linux | x86_64, ARM64 | CPU, Vulkan |
| Android API 24+ | ARM64, x86_64 | CPU, Vulkan |
| macOS | x86_64, ARM64 | CPU, Metal |

BSD, iOS, visionOS, and Web remain rejected by the HCSR build selection.

## Frame lifecycle notifications

`HTMLView` and `HTMLRenderTarget` expose `frame_queued(generation)` and
`frame_activated(generation)`. Queued means that the immutable logical frame
has been submitted to the selected backend. Activated means that Godot has
atomically selected that generation's sampled texture, frame metadata, and
hit-test snapshot. `HTMLRenderTarget.rendered` is a compatibility alias for
activation; successful DOM mutation setters schedule work on an engine frame
and do not emit it early.

For HCSR GPU output, activation uses the borrowed Godot command queue's GPU
ordering and never waits for a fence on the game or render thread. HCSR's
opaque submission token correlates the queued frame with later nonblocking
producer-completion polling. It is not exposed as a native D3D12, Vulkan, or
Metal synchronization object and must not be CPU-waited. Godot keeps sampling
the last active texture while a newer packet is pending. Once an engine-queue-
ordered packet is submitted, Godot atomically activates its primary and every
secondary texture together with the matching metadata and hit-test snapshot.
Later engine consumers are ordered after all of those producers on the same
queue. Producer completion is polled asynchronously for retirement only. Each
activated native resource remains borrowed until the engine frame that last
sampled it retires on the GPU. `RenderingDevice` then returns its exact handle,
resource generation, logical frame generation, and submission token through
HCSR's consumer-release API. The callback runs as part of normal frame-resource
reclamation; it adds no render-thread fence wait or queue flush. Resize,
presentation reset, and renderer teardown retain old producer generations until
those callbacks run. D3D12, Vulkan, and Metal use this same public lifecycle
contract.

The render-thread activation edge is published back to the scene-thread surface
as a one-shot changed flag. Consuming that flag refreshes the cached texture and
frame metadata before `frame_activated` observers use them; an activation racing
the poll remains set for the following engine frame.

To prepare Unix packages explicitly before invoking SCons, run one of:

```bash
bash thirdparty/hcsr_old/tools/build-unix-native.sh --platform linux --architecture x86_64
bash thirdparty/hcsr_old/tools/build-unix-native.sh --platform linux --architecture arm64
bash thirdparty/hcsr_old/tools/build-unix-native.sh --platform macos --architecture x86_64
bash thirdparty/hcsr_old/tools/build-unix-native.sh --platform macos --architecture arm64
```

Linux ARM64 currently requires an ARM64 Linux build host. macOS packaging
requires CMake, .NET 10, `llvm-objcopy`, and Xcode's Metal Toolchain; install
the latter with `xcodebuild -downloadComponent MetalToolchain`. Ninja is used
when available and the default CMake generator is supported otherwise.
Homebrew's keg-only LLVM
installation is detected in its standard Apple Silicon and Intel prefixes.
macOS can build either architecture from one host when the corresponding .NET
SDK architecture and Rosetta (for executing x86_64 tests on Apple Silicon) are
available. HCSR macOS packages and HCSR-enabled Godot binaries target macOS 12
or newer, matching the .NET 10 NativeAOT runtime requirement. Set
`HCSR_MACOS_DEPLOYMENT_TARGET` only when deliberately targeting a newer macOS.

After packaging, validate the static C ABI, system-font path, and raster-image
codec without Godot:

```bash
bash thirdparty/hcsr_old/tools/run-linux-static-smoke.sh --architecture x86_64
bash thirdparty/hcsr_old/tools/run-linux-static-smoke.sh --architecture arm64
bash thirdparty/hcsr_old/tools/run-macos-static-smoke.sh --architecture arm64
bash thirdparty/hcsr_old/tools/run-macos-static-smoke.sh --architecture x86_64
bash thirdparty/hcsr_old/tools/run-macos-metal-static-smoke.sh --architecture arm64
bash thirdparty/hcsr_old/tools/run-macos-metal-static-smoke.sh --architecture x86_64
```

The smoke executables link only the public `hcsr_renderer.h`, the combined
archive, its NativeAOT initializer, and Apple system frameworks. The Metal
smoke borrows a host `MTLDevice` and `MTLCommandQueue`, validates the returned
`MTLTexture`, and enables Metal API Validation by default. Both reject an output
that has an HCSR dynamic-library dependency.

## HCSR source and release packages

Author pages as ordinary browser-compatible `.html` and `.css` files. Standard
`<link rel="stylesheet" href="...">` references work in HCSR, so the same page
can be opened directly in Chromium while it is being edited. Editor and debug
builds load those raw files and preserve immediate reload behavior.

During a release export, the HCSR export plugin compiles selected HTML entry
documents into versioned `.hcsrpkg` files. An `HTMLDocument` resource explicitly
identifies its `html_file` as an entry. GDScript code that constructs an
`HTMLDocument` identifies a selected entry through a literal `html_file`
assignment or `set_html_file()` call; a named string constant may supply that
path. Other selected `.html` and `.htm` files are auxiliary source regardless of
whether they are syntactically complete documents. This distinction supports
projects that load complete `<template>` catalogs at runtime without compiling
their placeholders as document markup.

The export plugin only processes files selected by the export preset. Selected
HTML and CSS source remains in the exported package, including auxiliary source,
and each entry receives a sibling `.hcsrpkg`. Release runtime code derives
`res://page.hcsrpkg` from `res://page.html`. A failure to compile a selected entry
is an export error rather than a successful but incomplete package. Set
`HTMLDocument.package_file` to test a specific package explicitly in the editor.

The focused export regression exercises preset selection, resource-owned and
programmatically constructed entries, a complete-document auxiliary template
catalog, package contents, and fatal compilation errors:

```powershell
modules/html_css/tests/run_html_package_export_smoke.ps1 `
  -EditorPath bin/godot.windows.editor.x86_64.mono.console.exe
```

## Backdrop input for canvas and world-space HTML

Backdrop filtering must use an explicit Godot render-graph input. The HTML
renderer must not capture the window or reach back into `RenderingServer` on
its own. For a canvas `HTMLView`, Godot supplies the resolved canvas or camera
color immediately before the HTML pass. For an `HTMLSurface3D`, the scene
supplies a camera, viewport, portal, or application texture representing what
is behind the panel from the intended viewpoint.

The host-owned GPU request should carry the color image/view or D3D12 resource,
its generation, physical size, format and color space, UV-to-backdrop transform,
current layout/resource state, and synchronization token. HCSR copies only
conservative filter regions, applies blur and color operations, clips them to
the CSS shape, and composites into the same host-owned HTML target. Godot remains
responsible for capture timing, HDR policy, MSAA resolve, depth/occlusion, mip
generation, and mapping a 3D ray hit's UV back to HTML pixel coordinates.

This keeps the HTML paint contract identical for 2D and 3D. Only the source of
the backdrop texture and the coordinate transform differ. If a world-space
panel has no meaningful scene-color source, backdrop filtering should be
disabled or use an explicitly assigned texture instead of silently sampling the
screen-space viewport.

For canvas UI, the default mapping is one scene-color texel per viewport pixel.
For a flat 3D panel, prefer a panel-local camera/portal texture; alternatively,
pass a 3x3 projective mapping from HTML pixel coordinates to the active camera
texture. Curved panels require a host-generated UV map or a panel-local
reprojection because one projective transform cannot describe them.

The scene-color capture must happen before the HTML panel is drawn, and HCSR
must copy each conservative filter region before applying blur or color passes.
Sampling the destination texture in place would create undefined GPU feedback;
capturing after the panel draw would create a recursive previous-frame image.
Panels may share a camera-color capture, while portal/reprojected textures are
normally panel-specific. Pointer interaction remains independent: Godot raycasts
the panel, converts hit UV to HTML pixels, and sends the same pointer input used
by a canvas `HTMLView`.

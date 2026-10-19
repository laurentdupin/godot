# HTML/CSS Renderer Integration Notes

## Renderer selection

The module has one mutually exclusive renderer selector:

```text
module_html_css_renderer=hcsr_newest|hcsr_newest_dll|hcsr_old|hcsr_old_dll|hcsr_old_2|none
```

`hcsr_old` is the default. It uses the frozen old architecture in
`thirdparty/hcsr_old` and statically links its NativeAOT bridge. `hcsr_old_2`
uses the sunset replacement architecture pinned in `thirdparty/hcsr_old_2` and
links its `hcsr_runtime` ABI. The choices are mutually exclusive; one
Godot executable contains at most one HCSR version and keeps the normal Godot
executable name. `none` keeps the raw CPU-frame receiver without an HTML engine.
On Windows x86_64, `hcsr_old_dll` uses the same frozen old backend and C ABI as
`hcsr_old`, but links its NativeAOT publication through `hcsr_renderer.dll`.

### Newest HCSR on Metal (macOS ARM64)

`hcsr_newest` uses the packet-based native renderer in `thirdparty/hcsr_newest`.
On macOS ARM64, `BACKEND_AUTO` and `BACKEND_GPU_AUTO` select Metal when Godot is
running its Metal driver. `BACKEND_METAL` selects it explicitly; `BACKEND_CPU`
remains available. This requires a build with `metal=yes`; Godot currently
disables its Metal driver for x86_64 builds.

```sh
scons platform=macos target=editor arch=arm64 metal=yes module_mono_enabled=yes module_html_css_renderer=hcsr_newest extra_suffix=hcsr_newest generate_bundle=yes -j8
bin/godot.macos.editor.arm64.hcsr_newest.mono --rendering-method mobile --rendering-driver metal --path /path/to/project
```

For a Finder launch, the suffixed command above produces
`bin/godot_macos_editor_hcsr_newest_mono.app`. To update the regular
`bin/godot_macos_editor_mono.app`, use the build helper with your saved macOS
ARM64/Mono/editor settings:

```sh
python3 build_script/build.py --non-interactive --renderer hcsr_newest --suffix ''
```

The helper also generates matching managed assemblies before packaging the app.
Restart an already-running editor to use the new build.

The first build creates the macOS NativeAOT bundle, including the native Metal
renderer. It requires the .NET 10 SDK, CMake, Xcode tools, and llvm-objcopy. SDK
runtime packs are resolved from the exact published version, whether installed
through NuGet or bundled with the SDK. Homebrew SDKs use Homebrew's static Brotli
archives; `HCSR_BROTLI_LIB_DIR` can select another matching-architecture directory.
For deployment to older macOS versions, these archives must also match your
minimum OS version. The Homebrew archives used in local testing target macOS 15;
this build has not been validated on older systems.

Godot supplies the device, textures and command submission. Empty scenes,
solid paint, text and images all use the same RenderingDevice submission path
on Metal, Vulkan and D3D12, including partial uploads and output mipmaps.
Secondary outputs share prepared scene data and own only presentation targets.
A preparation failure is reported; it does not switch to a separate packet
renderer with different painting behavior.

`get_frame_scheduler_diagnostics().frame_synchronization.renderer` reports
`metal`, `vulkan`, `d3d12`, or `cpu`. `render_path` identifies `rendering_device`
or `cpu_reference`; `gpu_recordings` counts recorded RenderingDevice scenes,
not GPU completions.

Run the Metal integration smoke with any project directory and the absolute path
to `modules/html_css/tests/hcsr_newest_metal_smoke.gd`, using `--script` and the
Metal launch flags above. Set `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` for Metal
validation. The test checks empty-scene clearing, text/image uploads, orientation,
canvas sampling, multiple outputs and resize. Also run `hcsr_newest_image_atlas_smoke.gd`,
`hcsr_newest_direct_paint_smoke.gd`, `hcsr_newest_secondary_output_smoke.gd`, and
`hcsr_newest_3d_mipmap_smoke.gd` with the same flags. The 3D test covers explicit
residency of shared Metal texture views under the default barrier synchronization
mode. These GPU tests require a display session; `--headless` disables GPU rendering.

The HCSR integration supports x86_64 and ARM64 builds on Windows, Linux, and
macOS, plus Android ARM64 and x86_64. A Windows editor can be built with:

```text
python -m SCons platform=windows target=editor module_html_css_renderer=hcsr_old
```

The sunset `hcsr_old_2` editor is limited to Windows x86_64 with D3D12 and is
built only from its pinned submodule:

```text
python -m SCons platform=windows target=editor module_html_css_renderer=hcsr_old_2
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

## Atlas packing diagnostics

Set `HCSR_DUMP_ATLAS_DIR` to an absolute directory before launching Godot to
dump each HCSR newest view's live atlas allocations. The variable is read once;
unset or empty disables all dump work. Dumps run on the render thread from the
existing CPU pixels, without GPU readback. PNG encoding and file I/O are
deliberately synchronous debug work; do not benchmark with this flag enabled.

Each changed atlas revision produces
`<document-stem>-<process-id>-<surface-instance-id>-page<N>-rev<R>` files and
overwrites the matching `-latest` set: a checkerboard-composited PNG, an
`-overlay.png` with glyphs green, surfaces orange and images blue, and JSON.
Page zero (the permanent sampling pixel) is excluded. PNGs retain the full
physical page width (initially 1024, growing to 4096) and crop to the highest live allocation. The JSON includes
allocation rectangles, identities, packed area, packing efficiency and the ten
tallest entries. Surface identities include current owning element/object IDs
and packet shape kinds (`raster`/`raster_slice`); original CSS painter shape
types are not exposed by the ABI. Mask-only surfaces can have no draw owner.

Dumps are throttled to once per 500 ms per view. While a changed revision waits
for that interval, debug-only frame requests keep preparation reachable so a
final idle revision is also saved. No dump timers or frame requests run when
the flag is unset. Empty appearance pages use a one-row PNG with `used_rows: 0`;
older revision files remain historical snapshots.

`get_frame_scheduler_diagnostics().frame_synchronization.image_atlas` also
reports `atlas_used_rows` (sum over appearance pages), `atlas_used_rows_by_page`
(page 1 onward), `atlas_packed_area_pixels`, and `atlas_largest_entry`
(`w`, `h`, `kind`, largest by area). These statistics describe live packed
rectangles, including their sampling padding, rather than CPU buffer capacity.

Appearance pages start at 1024 square and grow to 2048/4096 without moving
allocations. `atlas_page_sizes` reports their actual physical extents (page 1
onward); the legacy `page_size` statistic remains the maximum 4096 address space.
The 4096 CPU row stride and mask-plane page encoding are logical addresses,
independent of GPU texture dimensions. RenderingDevice backends copy existing
pixels on the GPU when growing, then upload new allocation patches. OpenGL 3
recreates its array and restores only live bounds, using no newer copy-image API.
Initial page creation also uploads only live bounds; empty space is GPU-cleared.

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

## Per-view memory and hidden panels

For the newest renderer, `HTMLView.release_targets_when_hidden` is opt-in
(default `false`). When enabled,
visibility changes release the main view's output/mipmap textures, its compositing
scratch targets, and its backdrop mask on the rendering thread. The document,
layout, font references, raster appearances and atlas remain warm. Showing the
view requests a new host frame and recreates the presentation resources.
`HTMLViewOutput` targets are independent consumers: they remain live and continue
to render even when their source Control is hidden. This preserves 3D/XR use.

Under `OS.low_processor_usage_mode`, newest-renderer views wake RenderingServer
while a frame request, animation, or output completion is pending. Hidden views
and secondary-output producers request the draw directly because a hidden
CanvasItem's `queue_redraw()` does not mark the rendering server changed. Once
the surface is idle, internal processing stops and no draw is requested.
`tests/hcsr_hidden_output_low_processor_smoke.gd` verifies hidden text mutations,
secondary-output resizing, finite animations/transitions, ancestor visibility,
visible-view updates, and zero draws after settling. Copy it into an otherwise
empty Godot project and run it with `--script res://hcsr_hidden_output_low_processor_smoke.gd`
and a real rendering driver (not `--headless`). It counts process-loop iterations
instead of waiting for draw signals, which would hide the starvation being tested.

Base system fonts and explicit `@font-face` files are shared across HCSR views.
Variation instances, fallback-stack construction and shaping scratch remain local.
Explicit assets still use the document asset provider; sharing uses the resolved
path/package and file modification time. The last view using a shared base font
releases the pool's reference. Godot's separate implicit system-fallback cache
already shares font-file bytes process-wide and has its own engine lifetime.
TTC collections still use Godot's full-file loader; no face extraction is attempted.

`HTMLView.get_frame_scheduler_diagnostics().memory` reports:

- `scene_objects`, computed/cascaded property counts, `scene_raster_bytes` and
  raster-entry count: scene-owned content, excluding allocator/object overhead.
- `cached_native_packet_payload_bytes`: requested unmanaged bytes for the cached
  publication, counting shared definitions once. Excludes old in-flight packets,
  pinned managed pixels and allocator overhead.
- Cached managed draw/surface counts; these are counts, not heap-size estimates.
- CPU atlas pixel bytes and shared base-font byte counts. Shared font bytes are
  references to process resources and must not be summed across views. Implicit
  TextServer fallback fonts are outside these font-pool counters.
- `process_managed_heap_bytes`: non-collecting `GC.GetTotalMemory(false)` across
  all HCSR views, including uncollected garbage. GC committed/heap/fragmentation
  figures describe the last GC, not the current view. Before the first GC those
  last-GC counters can be zero. `process_server_gc` identifies the active policy.

The memory snapshot is collected only when diagnostics are requested. It does not
force GC, impose a process heap limit, or add per-frame allocation tracing. It is
not a full private-RAM census: engine allocations, native font/shader/driver
allocations, runtime metadata and allocator slack require process profiling.

Regression: `modules/html_css/tests/hcsr_view_memory_smoke.gd` tests repeated
hide/show pixel equality, target retirement, surviving independent outputs and
shared-font lifetime on RenderingDevice and OpenGL backends. An optional
`res://memory-font.ttf` fixture also exercises shared authored fonts.

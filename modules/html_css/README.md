# HTML/CSS Renderer Integration Notes

## Renderer selection

The module has one mutually exclusive renderer selector:

```text
module_html_css_renderer=none|blink|hcsr
```

`none` keeps the raw CPU-frame receiver without compiling an HTML engine.
`blink` uses the nested `thirdparty/blink-standalone-ui` dependency. `hcsr`
uses the nested `thirdparty/hcsr` dependency and statically links its NativeAOT
bridge, so an HCSR build does not ship an HCSR DLL.

The current HCSR integration supports Windows x86_64 and can be built with:

```text
python -m SCons platform=windows target=editor module_html_css_renderer=hcsr
```

By default, a missing HCSR archive is produced with `dotnet publish` from
`thirdparty/hcsr/src/Renderer.NativeBridge`. Set
`module_html_css_hcsr_auto_build=no` to require an existing archive, or pass
`module_html_css_hcsr_lib_path=<path>` to use an explicit one. HCSR currently
backs `Auto` with its CPU renderer. Explicit Vulkan and D3D12 surface requests
remain unavailable until the HCSR C ABI accepts Godot-owned GPU devices and
render targets; they never silently upload CPU output while claiming GPU mode.

## Blink C API link modes

The `html_css` module can compile the optional Blink C API backend with:

```text
module_html_css_enabled=yes
module_html_css_renderer=blink
```

`module_html_css_blink_enabled=yes` remains a compatibility alias for
`module_html_css_renderer=blink`. Once Blink is enabled, the default and primary
consumption mode is static:

```text
module_html_css_blink_link_mode=static
module_html_css_blink_lib_path=<optional Blink static package directory>
module_html_css_blink_static_manifest=<optional explicit manifest path>
```

If `module_html_css_blink_lib_path` is not supplied, Godot uses the nested package under:

```text
thirdparty/blink-standalone-ui/build/cmake-msvc-static-proof/package/c_api_static
```

and, when `module_html_css_blink_package_profile=generated_v8_chromium_llvm` is selected:

```text
thirdparty/blink-standalone-ui/build/cmake-generated-v8-chromium-llvm/package/c_api_static
```

With `module_html_css_blink_package_profile=auto`, Godot uses the current platform's static package profile first. On Windows this is the native MSVC static package. It runs package commands only when `module_html_css_blink_auto_build=yes` is set; otherwise a missing package is reported with the source-owned nested package command to run.

An optional local or release artifact cache can also be used when generated package output is absent. The cache root defaults to:

```text
thirdparty/blink-standalone-ui/prebuilt
```

Use `module_html_css_blink_package_root=<path>` to point at another local artifact cache without overriding the final package path. This path is not the normal source-controlled dependency path and should not be used to justify checking generated package ZIPs into the Godot source tree.

The optional cache can be either an unpacked static package directory:

```text
thirdparty/blink-standalone-ui/prebuilt/windows-x86_64-msvc-static/c_api_static
```

or a single static package `.zip` archive in the profile directory:

```text
thirdparty/blink-standalone-ui/prebuilt/windows-x86_64-msvc-static/*.zip
```

Archives are extracted into generated output under `bin/.html_css_blink_runtime/<platform>-<arch>-<profile>-static/<archive-stem>/`, so normal builds do not dirty `thirdparty`. If more than one archive is present for a profile, SCons fails and asks for a single artifact or an explicit `module_html_css_blink_lib_path`.

Normal Godot builds consume an existing nested static package. They do not bootstrap Blink from source by default. If the expected nested artifact is missing, SCons fails with a repo-local static package command.

Developers who intentionally want SCons to invoke the nested Blink package build can opt in with:

```text
module_html_css_blink_auto_build=yes
```

This source bootstrap runs only from `thirdparty/blink-standalone-ui`; it never falls back to an external sibling checkout. First-run package builds may require V8/depot_tools/CIPD network access or pre-populated build artifacts. The default Windows/MSVC static command is:

```powershell
cd thirdparty\blink-standalone-ui
powershell -NoProfile -ExecutionPolicy Bypass -File upstream\chromium\standalone_renderer\tools\build_msvc_static_package.ps1 -SkipV8Build -SkipDawnBuild -Jobs 8
```

If the reusable V8/Dawn outputs are not present, run the same script without `-SkipV8Build -SkipDawnBuild` from a Visual Studio x64 developer environment. The generated-V8 ChromiumLLVM profile uses the matching CMake package targets for `c_api_static`.

Dynamic/DLL mode is retained only as an explicit diagnostic, legacy, or development override:

```text
module_html_css_blink_link_mode=dynamic
module_html_css_blink_lib_path=<optional Blink dynamic package directory>
module_html_css_blink_lib=blink_standalone_renderer_c_api.lib
```

On Windows, explicit dynamic mode links the import library and requires the package runtime files next to the Godot binary or otherwise discoverable by the loader. It must not be used as the normal product path. A dynamic package may include:

- `blink_standalone_renderer_c_api.dll`
- `blink_standalone_renderer_c_api.lib`
- `blink_standalone_renderer_c_api_link_manifest.json`
- `icudtl.dat`
- `libEGL.dll`
- `libGLESv2.dll`
- `d3dcompiler_47.dll`
- optional Dawn/DirectX compiler sidecars such as `dxcompiler.dll` and `dxil.dll`

When `module_html_css_blink_copy_runtime_sidecars=yes` (the default), explicit dynamic mode copies package-local runtime files from the selected `c_api_runtime` directory into `bin` during the editor/template build, excluding `.lib` link libraries and public headers. Newer Blink packages provide package-relative file lists and metadata in `blink_standalone_renderer_c_api_link_manifest.json`; Godot uses those package-relative entries when available and falls back to enumerating files in `c_api_runtime` for older packages. Static mode may copy manifest-declared data sidecars such as `bin/icudtl.dat`, but it rejects packages that still require Blink, ANGLE, Dawn, Tint, or compiler code shared-library sidecars such as `blink_standalone_renderer_c_api.dll`, `libEGL.dll`, or `libGLESv2.dll`.

Static mode consumes the Blink static C API package:

```text
module_html_css_blink_link_mode=static
module_html_css_blink_lib_path=<optional Blink static package directory>
module_html_css_blink_static_manifest=<optional explicit manifest path>
```

If `module_html_css_blink_lib_path` is not supplied, Godot first looks for the nested generated static package directory:

```text
thirdparty/blink-standalone-ui/build/cmake-msvc-static-proof/package/c_api_static
```

An optional local/release artifact cache may also provide an unpacked static package directory:

```text
thirdparty/blink-standalone-ui/prebuilt/windows-x86_64-msvc-static/c_api_static
```

or a single static package `.zip` archive in the static cache profile directory:

```text
thirdparty/blink-standalone-ui/prebuilt/windows-x86_64-msvc-static/*.zip
```

Static archives are extracted into generated output under `bin/.html_css_blink_runtime/<platform>-<arch>-<profile>-static/<archive-stem>/`, and the extracted `c_api_static` package is consumed from there. Do not treat this optional cache as source repository content; the normal path is a source-built nested package output or an explicit package path.

If no explicit manifest path is supplied, static mode looks for `blink_standalone_renderer_c_api_static_link_manifest.json` inside `module_html_css_blink_lib_path`. The manifest is expected to provide the C API static archive, transitive static archives, import libraries, system libraries, compile definitions, and any whole-archive requirements.

Static packages that declare `full_host_static_link_supported=false` or `symbol_ownership.product_supported=false` are rejected by default because they are not safe for full editor/export-template host linking. They can be forced only for local diagnostics with:

```text
module_html_css_blink_static_allow_unsupported_host=yes
```

Do not use that override for production editor or template builds; it may produce binaries that link but crash in unrelated renderer startup paths.

Static package auto-build is opt-in only. The Blink static package can require a large V8 compatibility archive or bootstrap step; build the nested static package output first, pass a compatible explicit package path, or use `module_html_css_blink_auto_build=yes` after preparing the required source-owned build artifacts.

Whole-archive linking is disabled by default. Enable `module_html_css_blink_static_whole_archive=yes` only for packages that require it and do not duplicate symbols already provided by Godot, such as libpng, ICU, Vulkan Memory Allocator, Embree, or other third-party libraries.

`module_html_css_blink_lib` and `module_html_css_blink_static_libs` remain as a manual fallback for experiments with packages that do not provide a manifest. Godot should not hardcode Blink's static transitive libraries. Static mode may still require data sidecars such as ICU data depending on how Blink packages those resources, but it must not require Blink, ANGLE, or compiler code DLL/SO/DYLIB sidecars.

Export templates must be built with the same module and link-mode flags as the editor. The default static export path must not depend on Blink, ANGLE, Dawn, Tint, or compiler code DLL/SO/DYLIB sidecars; manifest-declared data such as `icudtl.dat` may still need to be packaged. Dynamic exports are explicit diagnostic/legacy builds and must package the C API runtime files with the exported executable.

## OpenGL3 / Compatibility GPU backend plan

The current explicit GPU backends are Vulkan and D3D12. Compatibility/OpenGL3 must not be advertised as an end-to-end GPU backend until Blink exposes a public GL/ANGLE external-target ABI.

Godot has a GLES3 import surface through `RenderingServer::texture_create_from_native_handle()`, and the GLES3 texture storage wraps a native `GLuint` texture id as a `Texture2D` RID. A future OpenGL3 backend should therefore use this shape:

1. Run all GL target creation, resize, import, and destruction on the render thread.
2. Create and own a 2D GL texture with the active Godot GL context current.
3. Pass the `GLuint`, format, size, and synchronization contract to Blink through a public GL/ANGLE C API.
4. Import the same `GLuint` into Godot with `RenderingServer::texture_create_from_native_handle()`.
5. Preserve GL state around Blink calls or require Blink to do so explicitly.
6. Destroy the Godot texture wrapper before deleting the module-owned GL texture.

The Blink ABI must define context ownership/current-context requirements, same-context versus shared-context behavior, texture format, alpha semantics, resize lifetime, fences or synchronous completion, and that Blink never deletes Godot-owned GL resources.

Until that ABI exists, `BACKEND_AUTO` may use the CPU/raw path on OpenGL3, but explicit GPU requests must report unsupported and must not draw stale CPU output.

## Backdrop input for canvas and world-space HTML

Backdrop filtering must use an explicit Godot render-graph input. The HTML
renderer must not capture the window or reach back into `RenderingServer` on
its own. For a canvas `HTMLView`, Godot supplies the resolved canvas or camera
color immediately before the HTML pass. For an `HTMLSurface3D`, the scene
supplies a camera, viewport, portal, or application texture representing what
is behind the panel from the intended viewpoint.

The host-owned GPU request should carry the color image/view or D3D12 resource,
its generation, physical size, format and color space, UV-to-backdrop transform,
current layout/resource state, and synchronization token. HCSR/Blink then copy
only conservative filter regions, apply blur and color operations, clip them to
the CSS shape, and composite into the same host-owned HTML target. Godot remains
responsible for capture timing, HDR policy, MSAA resolve, depth/occlusion, mip
generation, and mapping a 3D ray hit's UV back to HTML pixel coordinates.

This keeps the HTML paint contract identical for 2D and 3D. Only the source of
the backdrop texture and the coordinate transform differ. If a world-space
panel has no meaningful scene-color source, backdrop filtering should be
disabled or use an explicitly assigned texture instead of silently sampling the
screen-space viewport.

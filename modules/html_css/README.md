# HTML/CSS Renderer Integration Notes

## Blink C API link modes

The `html_css` module can compile the optional Blink C API backend with:

```text
module_html_css_enabled=yes
module_html_css_blink_enabled=yes
```

The default Blink consumption mode is dynamic:

```text
module_html_css_blink_link_mode=dynamic
module_html_css_blink_lib_path=<optional Blink dynamic package directory>
module_html_css_blink_lib=blink_standalone_renderer_c_api.lib
```

If `module_html_css_blink_lib_path` is not supplied, Godot uses the nested package under:

```text
thirdparty/blink-standalone-ui/prebuilt/windows-x86_64-msvc/c_api_runtime
```

The prebuilt package root defaults to:

```text
thirdparty/blink-standalone-ui/prebuilt
```

Use `module_html_css_blink_package_root=<path>` to point at another repo-owned artifact root without overriding the final package path. With `module_html_css_blink_package_profile=auto`, Godot first uses an existing nested dynamic package if one is present. On Windows it checks the MSVC prebuilt package first, then the generated-V8 ChromiumLLVM prebuilt package, and then the matching developer build-output packages. If no existing package is present, `auto` selects the default expected package path for the platform; it runs package commands only when `module_html_css_blink_auto_build=yes` is set.

The prebuilt package can be either an unpacked package directory:

```text
thirdparty/blink-standalone-ui/prebuilt/windows-x86_64-msvc/c_api_runtime
```

or a single runtime `.zip` archive in the profile directory:

```text
thirdparty/blink-standalone-ui/prebuilt/windows-x86_64-msvc/*.zip
```

Archives are extracted into generated output under `bin/.html_css_blink_runtime/<platform>-<arch>-<profile>/<archive-stem>/`, so normal builds do not dirty `thirdparty`. If more than one archive is present for a profile, SCons fails and asks for a single artifact or an explicit `module_html_css_blink_lib_path`.

Developer build-output fallback paths are:

```text
thirdparty/blink-standalone-ui/build/cmake-msvc-release/package/c_api_runtime
```

and, when `module_html_css_blink_package_profile=generated_v8_chromium_llvm` is selected:

```text
thirdparty/blink-standalone-ui/build/cmake-generated-v8-chromium-llvm/package/c_api_runtime
```

Normal Godot builds consume an existing nested dynamic package. They do not bootstrap Blink from source by default. If the expected nested artifact is missing, SCons fails with a repo-local package command.

Developers who intentionally want SCons to invoke the nested Blink package build can opt in with:

```text
module_html_css_blink_auto_build=yes
```

This source bootstrap runs only from `thirdparty/blink-standalone-ui`; it never falls back to an external sibling checkout. First-run package builds may require V8/depot_tools/CIPD network access or pre-populated build artifacts. The default Windows/MSVC commands are:

```powershell
cd thirdparty\blink-standalone-ui
cmake --preset x64-Release-MSVC
cmake --build --preset x64-Release-MSVC-c-api-package --parallel 8
```

The generated-V8 ChromiumLLVM profile uses:

```powershell
cd thirdparty\blink-standalone-ui
$env:DEPOT_TOOLS_WIN_TOOLCHAIN='0'
cmake --preset x64-Release-GeneratedV8
cmake --build --preset x64-Release-GeneratedV8-v8-compat --parallel 8
cmake --preset x64-Release-GeneratedV8-ChromiumLLVM
cmake --build --preset x64-Release-GeneratedV8-ChromiumLLVM-c-api-package --parallel 8
```

On Windows, dynamic mode links the import library and requires the package runtime files next to the Godot binary or otherwise discoverable by the loader. The current package may include:

- `blink_standalone_renderer_c_api.dll`
- `blink_standalone_renderer_c_api.lib`
- `blink_standalone_renderer_c_api_link_manifest.json`
- `icudtl.dat`
- `libEGL.dll`
- `libGLESv2.dll`
- `d3dcompiler_47.dll`
- optional Dawn/DirectX compiler sidecars such as `dxcompiler.dll` and `dxil.dll`

When `module_html_css_blink_copy_runtime_sidecars=yes` (the default), dynamic mode copies package-local runtime files from the selected `c_api_runtime` directory into `bin` during the editor/template build, excluding `.lib` link libraries and public headers. Newer Blink packages provide package-relative file lists and metadata in `blink_standalone_renderer_c_api_link_manifest.json`; Godot uses those package-relative entries when available and falls back to enumerating files in `c_api_runtime` for older packages. Export packaging still needs to include the same runtime files beside the exported executable.

Static mode is a build-time staging hook for a future Blink static package:

```text
module_html_css_blink_link_mode=static
module_html_css_blink_lib_path=<optional Blink static package directory>
module_html_css_blink_static_manifest=<optional explicit manifest path>
```

If `module_html_css_blink_lib_path` is not supplied in static mode, Godot uses:

```text
thirdparty/blink-standalone-ui/build/cmake-generated-v8-chromium-llvm/package/c_api_static
```

If no explicit manifest path is supplied, static mode looks for `blink_standalone_renderer_c_api_static_link_manifest.json` inside `module_html_css_blink_lib_path`. The manifest is expected to provide the C API static archive, transitive static archives, import libraries, system libraries, compile definitions, and any whole-archive requirements.

Static package auto-build is intentionally disabled at the current nested Blink pin because the documented `blink_standalone_renderer_c_api_static_package` target is not defined there. Create or provide a compatible static package explicitly before using static mode.

Whole-archive linking is disabled by default. Enable `module_html_css_blink_static_whole_archive=yes` only for packages that require it and do not duplicate symbols already provided by Godot, such as libpng, ICU, Vulkan Memory Allocator, Embree, or other third-party libraries.

`module_html_css_blink_lib` and `module_html_css_blink_static_libs` remain as a manual fallback for experiments with packages that do not provide a manifest. Godot should not hardcode Blink's static transitive libraries. Static mode may still require data or compiler sidecars such as ICU data or shader compiler DLLs depending on how Blink packages those resources.

Export templates must be built with the same module and link-mode flags as the editor. Dynamic exports must package the C API runtime files with the exported executable. Static exports remove only the C API runtime library if Blink is truly statically linked; they do not automatically embed runtime data or compiler sidecars.

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

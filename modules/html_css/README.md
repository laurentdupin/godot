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
module_html_css_blink_lib_path=<Blink package directory>
module_html_css_blink_lib=blink_standalone_renderer_c_api.lib
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

Static mode is a build-time staging hook for a future Blink static package:

```text
module_html_css_blink_link_mode=static
module_html_css_blink_lib_path=<Blink static package directory>
module_html_css_blink_lib=<primary static C API archive>
module_html_css_blink_static_libs=<extra linker input>;...
```

Godot does not hardcode Blink's static transitive libraries. The Blink static package must provide the archive list, system-library requirements, and any whole-archive requirements. Static mode may still require data or compiler sidecars such as ICU data or shader compiler DLLs depending on how Blink packages those resources.

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

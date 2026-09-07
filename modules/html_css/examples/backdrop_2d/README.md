# HCSR 2D backdrop gallery

This example uses a standard `<link rel="stylesheet">` in `gallery.html`, so
the same files open directly in a browser and load unchanged in Godot. It then
places one transparent `HTMLView` over animated Godot
canvas content. Eight rounded HTML panels demonstrate the current backdrop
compositor limit with these filters:

- `blur(16px)`
- `contrast(180%)`
- `sepia(100%)`
- `saturate(240%)`
- `brightness(155%)`
- `grayscale(100%)`
- `invert(100%)`
- `blur(9px) contrast(135%) saturate(180%) sepia(30%)`

Build Godot with the statically linked HCSR provider, then run the scene:

```powershell
python -m SCons platform=windows target=editor arch=x86_64 dev_build=yes module_mono_enabled=yes module_html_css_renderer=hcsr_newest extra_suffix=hcsr_newest -j8
.\bin\godot.windows.editor.dev.x86_64.hcsr_newest.mono.exe --path modules\html_css\examples\backdrop_2d res://main.tscn
```

The example selects `HTMLView.BACKEND_GPU_AUTO`, which uses the active Vulkan,
D3D12, or Metal renderer and reports an unsupported backend instead of silently uploading
CPU pixels when no host-device GPU path is available. The backdrop itself is
composited by Godot's canvas shader from the live screen texture; no scene
pixels are read back into HCSR. Resize or maximize the window to exercise
surface recreation and backdrop-region scaling. The example uses
`VIEWPORT_SIZE_PHYSICAL_SIZE` with stretch disabled, so HCSR renders at the
window's current physical pixel dimensions instead of the 1280x720 startup
size.

For CPU/Metal comparison captures on macOS, append
`-- --html-backend=cpu` or `-- --html-backend=metal` to the Godot command line.

To build and deploy the ARM64 Android version, configure `ANDROID_HOME` and
`JAVA_HOME`, then run from the Godot root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File modules\html_css\tools\build_html_ui_android.ps1 -ExportBackdrop -Install -Launch -DeviceSerial <serial>
```

Android uses Godot's mobile Vulkan renderer. HCSR and its native text/image
codecs are linked into the Godot shared library; no HCSR DLL is packaged.

With `hcsr_newest`, HCSR supplies rounded, transformed and clipped coverage plus
ordered operations. Godot creates a cached ID/coverage mask and applies filters
to the live canvas backbuffer. `HTMLView.get_backdrop_filter_frame()` exposes the
same mask and effect IDs to other host compositors. The foreground texture stays
independent; filters are not baked into a secondary 3D output texture.

Regression tests in `../../tests/hcsr_newest_backdrop_mask_smoke.gd` cover alpha,
rounded clips, transforms, resizing, mask reuse and removal. Run
`../../tests/hcsr_newest_backdrop_gallery_smoke.gd` with this example as the project
to validate all eight mask IDs. The existing single-ID compositor still uses the
last covering effect for overlapping regions; nested CSS backdrop-root semantics
and Chromium-exact blur kernels remain future work.

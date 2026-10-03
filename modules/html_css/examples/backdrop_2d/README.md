# HCSR 2D backdrop gallery

This example uses a standard `<link rel="stylesheet">` in `gallery.html`, so
the same files open directly in a browser and load unchanged in Godot. It then
places one transparent `HTMLView` over animated Godot
canvas content. Eight rounded HTML panels demonstrate the current backdrop
ordered backdrop filters:

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
CPU pixels when no host-device GPU path is available. On D3D12/Vulkan the backdrop
is composed by the ordered scene renderer from the live canvas texture; no scene
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
ordered operations. The scene renderer samples the live Godot canvas input at
the appropriate backdrop root. Cached atlas mask slices use GPU placement and
compatible slices share an instanced draw. Blur intermediates cover the projected
effect bounds plus their complete sampling halo. `HTMLView.get_backdrop_filter_frame()` exposes the
same mask and effect IDs to other host compositors. The foreground texture stays
independent; filters are not baked into a secondary 3D output texture.

Regression tests in `../../tests/hcsr_newest_backdrop_mask_smoke.gd` cover alpha,
rounded clips, transforms, resizing, mask reuse and removal. Run
`../../tests/hcsr_newest_backdrop_gallery_smoke.gd` with this example as the project
to validate all eight mask IDs. Ordered scene composition preserves overlapping
effects and nested backdrop roots; the exported ID mask is a separate host API.

For sustained measurements, run `--script res://benchmark.gd` with the optimized
editor, `--rendering-method mobile` and `--rendering-driver d3d12` or `vulkan`.
The script disables VSync and the frame cap, warms up 90 presented frames and
measures 600. `GALLERY_CASE` selects `original`, `no_filters`, `colors_only`,
`blur_only` or `no_html`. Set `GALLERY_OUTPUT` to an absolute JSON path to save
results. `GALLERY_CAPTURE=1` freezes the animated canvas at 1.25 seconds and saves
a PNG next to the JSON; leave it unset for the animated sustained benchmark.
`GALLERY_RENDER_SIZE=640x360` with `GALLERY_FIXED_LOGICAL_SIZE=1280x720` tests
reduced physical resolution while retaining the logical layout size.

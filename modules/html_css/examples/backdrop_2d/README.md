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
python -m SCons platform=windows target=editor dev_build=yes module_html_css_renderer=hcsr angle=no -j1
.\bin\godot.windows.editor.dev.x86_64.exe --path modules\html_css\examples\backdrop_2d res://main.tscn
```

The example selects `HTMLView.BACKEND_GPU_AUTO`, which uses the active Vulkan or
D3D12 renderer and reports an unsupported backend instead of silently uploading
CPU pixels when no host-device GPU path is available. The backdrop itself is
composited by Godot's canvas shader from the live screen texture; no scene
pixels are read back into HCSR. Resize or maximize the window to exercise
surface recreation and backdrop-region scaling. The example uses
`VIEWPORT_SIZE_PHYSICAL_SIZE` with stretch disabled, so HCSR renders at the
window's current physical pixel dimensions instead of the 1280x720 startup
size.

To build and deploy the ARM64 Android version, configure `ANDROID_HOME` and
`JAVA_HOME`, then run from the Godot root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File modules\html_css\tools\build_html_ui_android.ps1 -ExportBackdrop -Install -Launch -DeviceSerial <serial>
```

Android uses Godot's mobile Vulkan renderer. HCSR and its native text/image
codecs are linked into the Godot shared library; no HCSR DLL is packaged.

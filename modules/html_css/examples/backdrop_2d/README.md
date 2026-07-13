# HCSR 2D backdrop gallery

This example places one transparent `HTMLView` over animated Godot canvas
content. Eight rounded HTML panels demonstrate the current backdrop compositor
limit with these filters:

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

The example intentionally selects `HTMLView.BACKEND_CPU` for the HTML surface.
The backdrop itself is still composited by Godot's canvas shader from the live
screen texture; no scene pixels are read back into HCSR. Resize or maximize the
window to exercise surface recreation and backdrop-region scaling.


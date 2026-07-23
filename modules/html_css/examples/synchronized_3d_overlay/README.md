# Synchronized HTML 3D Overlay

This example projects a moving `MeshInstance3D` through the active `Camera3D` and updates one absolutely positioned HTML element with the resulting viewport coordinates.

The normal `HTMLView` GPU path is engine-frame synchronized:

- Godot DOM mutations are applied atomically before the frame's HTML packet is prepared instead of entering a second asynchronous document timeline;
- HCSR records and submits raster work on Godot's RenderingDevice queue;
- the `HTMLTexture2D` proxy is latched to that submission before the frame's consumers are submitted;
- producer completion remains asynchronous and is used for resource retirement, not for choosing which logical generation the engine frame displays.

`HTMLViewOutput` remains the explicit independent/asynchronous output API. It is appropriate when its cadence is intentionally independent from the engine frame, but it does not provide the same-frame overlay contract.

The CPU raster fallback remains a correctness path and does not promise same-frame motion when raster work is slower than the engine cadence.

Run the visual example with the normal editor:

```powershell
godot.windows.editor.x86_64.mono.exe --path modules/html_css/examples/synchronized_3d_overlay
```

Run the composed-frame alignment check:

```powershell
godot.windows.editor.x86_64.mono.console.exe `
  --rendering-driver d3d12 `
  --path modules/html_css/examples/synchronized_3d_overlay `
  -- --validate
```

The validation captures 30 composed frames and requires the HTML tracker and cube centers from each same final framebuffer to remain within 1.5 physical pixels.

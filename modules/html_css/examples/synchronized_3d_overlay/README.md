# Synchronized HTML 3D Overlay

This example projects a moving `MeshInstance3D` through the active `Camera3D` and updates one absolutely positioned HTML element with the resulting viewport coordinates.

The normal `HTMLView` GPU path is engine-frame synchronized:

- Godot DOM mutations are applied atomically before the frame's HTML packet is prepared instead of entering a second asynchronous document timeline;
- HCSR records and submits raster work on Godot's RenderingDevice queue;
- the `HTMLTexture2D` proxy is latched to that submission before the frame's consumers are submitted;
- producer completion remains asynchronous and is used for resource retirement, not for choosing which logical generation the engine frame displays.

`HTMLViewOutput` provides additional physical projections of the same immutable
logical frame. On engine-queue-ordered GPU backends, the automatic Control
texture and every secondary output are activated as one render-thread
transaction. Producer completion remains asynchronous and is used only for
resource retirement.

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

After the initial document warm-up, validation requires at least 120 distinct
HTML generations and 150 physical pixels of accumulated projected cube motion.
Every final framebuffer is checked, including engine frames on which HTML has
not advanced, so a stale tracker cannot pass merely because the sample window
is short. The tracker and cube centers must remain within 1.5 physical pixels,
and every post-warm-up request-to-activation interval must remain within
33.333 ms. A budget miss is reported with its exact logical generation and
stage; it does not relax the pixel-alignment requirement.

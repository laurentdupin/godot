# Synchronized HTML 3D Overlay

This example projects a moving `MeshInstance3D` through the active `Camera3D` and updates four absolutely positioned HTML edges with the resulting viewport coordinates.

With `hcsr_newest` and `hcsr_newest_dll`, HTMLView prepares the scene at
`frame_pre_draw`, after normal script processing and Godot's synchronization
with the previous rendering frame. The renderer registers a callback in the
current RenderingDevice graph before the viewport samples the HTML texture.
Upload slots follow Godot rendering frames, independently of scene generations.
The CPU path uploads the same frame's rectangles at this boundary as well.

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

Slow synchronous preparation can lengthen a frame; it must not display an older
HTML state alongside a newer cube state.

Run the visual example with the normal editor:

```powershell
bin/godot.windows.editor.dev.x86_64.hcsr_newest_dll.mono.exe --path modules/html_css/examples/synchronized_3d_overlay
```

Run the composed-frame alignment check:

```powershell
bin/godot.windows.editor.dev.x86_64.hcsr_newest_dll.mono.console.exe `
  --rendering-driver d3d12 `
  --path modules/html_css/examples/synchronized_3d_overlay `
  -- --validate
```

After the initial document warm-up, validation requires at least 120 distinct
HTML generations and 150 physical pixels of accumulated projected cube motion.
Every final framebuffer is checked, including engine frames on which HTML has
not advanced, so a stale tracker cannot pass merely because the sample window
is short. The tracker and cube centers must remain within 1.5 physical pixels,
and the tracker must also match the current scripted projection. Validation
recognizes both the legacy red tracker and the grayscale scene renderer.
It drives mutations from a late-processing node with a fixed simulation step.
Readback is used only by validation and is excluded from performance claims.
A successful run saves `user://synchronized-overlay-validation.png`.

Repeat with `--rendering-driver vulkan`, or add `--cpu` after `--` to test the
CPU renderer. `--script res://pending_submission_teardown.gd` checks destruction
after callback registration and before graph execution.

Validation on 2026-09-05 (AMD Radeon RX 9070): D3D12 and Vulkan each passed
120 advancing generations with a maximum center error of 1.414 pixels; CPU
passed with 1.000 pixel. Static D3D12 and DLL Vulkan also passed in
low-processor mode (`--low-processor` after `--`). Pending-submission teardown
passed eight iterations on each GPU backend. The separate-render-thread
D3D12 run passed alignment but emitted a RenderingDevice finalization thread
error at engine shutdown; it is not a clean shutdown validation pass.

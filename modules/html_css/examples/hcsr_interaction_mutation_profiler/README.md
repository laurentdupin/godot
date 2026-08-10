# HCSR interaction and mutation profiler

This standalone Godot example reproduces the real engine path under an
uncapped, VSync-disabled workload. It uses one `HTMLView` and runs distinct
lanes so an inexpensive hover frame is not averaged together with deliberate
DOM topology replacement:

- continuously animated dashboard baseline with sixteen independently changing bars, a scan line and activity indicators;
- continuous pointer hover transitions;
- click, pointer focus, keyboard text input, checkbox and form-value interaction;
- high-rate wheel scrolling;
- stable-topology text, class and inline-style mutations;
- subtree and stylesheet replacement;
- a mixed open-loop lane combining all of the above.

The primary HTML surface is always visible below the status panel. It uses
`HTMLView.VIEWPORT_SIZE_CONTROL_PHYSICAL_ADJUSTED`, is anchored to all four
window edges, and recomputes its CSS viewport and physical raster when the
window is resized. Pointer targets are derived from the current logical size.
The default backend is GPU Auto so running the scene from the editor follows
its active rendering driver; explicit command-line runs select D3D12 or Vulkan.
With `--secondary-output`, a same-aspect output is recreated for the current
window and shown as a responsive inset preview instead of being invisible.

Each timed lane has an active cache-warm ramp, an action-free transition-settle
interval, and a two-second measurement window. Time-gated actions use a fixed open-loop
schedule, and the result reports/asserts the exact number issued so a slower
backend cannot improve its score by silently doing less work. Synthetic input
events and mutation payload strings are reused or prebuilt before measurement.

The timed lanes never read back a texture, take a screenshot, force a draw, or
poll HCSR custom monitors per frame. The example records raw process-frame
intervals and prints p50/p95/p99/max, counts of frames below 240/120/100 FPS,
the exact workload counts, mutation issue time, frame generations and scheduler
outcomes. HCSR monitor values are sampled once after measurement. Use a separate
traced run for attribution because native JSONL tracing intentionally adds
overhead.

The project itself requests `DisplayServer.VSYNC_DISABLED`, `max_fps = 0` and
unsmoothed deltas. An operating-system or graphics-driver override can still
force synchronization; the requested mode and observed frame rate are always
printed.

No engine rebuild is needed when only these example files change. From the
Godot repository root, run the normal executable names:

```powershell
$godot = '.\bin\godot.windows.editor.x86_64.mono.console.exe'
$project = '.\modules\html_css\examples\hcsr_interaction_mutation_profiler'

& $godot --display-driver windows --rendering-driver d3d12 `
  --rendering-method mobile --xr-mode off --disable-vsync --max-fps 0 `
  --delta-smoothing disable --path $project -- `
  --automated --d3d12

& $godot --display-driver windows --rendering-driver vulkan `
  --rendering-method mobile --xr-mode off --disable-vsync --max-fps 0 `
  --delta-smoothing disable --path $project -- `
  --automated --vulkan
```

Append `--secondary-output` to add a presentation output up to 640 pixels wide,
with its height derived from the current responsive surface aspect ratio. It
shares the same logical frame and is shown as an inset. Measure primary-only
and multi-output runs separately.
Without `--automated`, the workload loops and keeps its live HUD and HTML
surface visible. Add `--print-fps` to that interactive demonstration if you
want Godot's title/console FPS display; it is intentionally omitted from the
canonical automated timing commands.

For attribution only, set a unique process-local `HCSR_FRAME_TRACE_PATH` and
repeat the identical command. Do not compare traced and untraced timings as
one population.

This repository's debug editor exposes HCSR stage/allocation monitors and is
therefore an instrumented build. Treat those measurements separately from an
uncapped release-template capacity run.

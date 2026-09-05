# Engine-frame contract for hcsr_newest

`hcsr_newest` and `hcsr_newest_dll` prepare through HTMLRenderSurface at
Godot's `frame_pre_draw` boundary. This covers HTMLView, HTMLRenderTarget,
and HTMLSurface3D through their shared surface implementation. Normal script
processing and Godot's synchronization with the preceding render submission
have finished at this point. Scripts should apply their changes during normal
input/process callbacks, before this boundary.

Mutation requests are staged by synchronous API calls. Source compilation,
mutation application, animation, layout, and packet preparation run synchronously
at this boundary. Slow preparation increases the frame's CPU cost. There is no
background scene publication or restoration of an earlier document.

Each prepared packet carries its captured host process-frame number, timeline,
physical size, and background through submission. `hcsr_frame_desc_t.frame_id`
identifies the host frame; the packet's `scene_generation` independently
identifies scene state. GPU upload slots follow RenderingDevice's frame ring.
The native callback declares its texture access in the current render graph,
before Godot samples the result. Initialization clears precede that callback.
All surfaces use the same `Engine.get_frame_ticks()` timestamp, so preparation
cost in one scene cannot advance another scene's sampled animation time.

The backend reports a synchronization failure if the preceding packet is still
pending at the next preparation boundary, or if a GPU callback executes in a
different device frame from the one where it was registered. It does not silently
skip the requested update in either case. GPU completion is still asynchronous
and governs resource retirement. "Recorded" does not mean the GPU has finished
or that the monitor has presented the frame.

An unchanged scene can keep its existing prepared state. Its recorded host-frame
number then remains unchanged; that alone is not a stale-frame violation. The
engine continues drawing its output. Multiple surfaces have independent scene
state and layout, even when they share an HTMLDocument source. Different scene
resolutions do not require different mutation timelines. This change does not
add the currently unsupported secondary `HTMLView.create_output` backend path.

## Diagnostics

For HTMLView, inspect
`get_frame_scheduler_diagnostics()["frame_synchronization"]`. HTMLRenderTarget
exposes the same dictionary through `get_frame_synchronization()`.

| Field | Meaning |
| --- | --- |
| `prepared_host_frame`, `recorded_host_frame` | Godot process frame of the most recent preparation and recording |
| `prepared_generation`, `recorded_generation` | Corresponding scene generations |
| `prepared_time_seconds` | Common engine-frame timestamp used for scene advancement |
| `preparations`, `recordings` | Per-surface counts, independent of mutation count |
| `pending` | A prepared packet is awaiting recording |
| `failures` | Detected violations of the frame contract |
| `preparation_ms`, `maximum_preparation_ms` | Last and maximum complete synchronous preparation cost, including rebuild when required |

`HTMLView.get_host_frame_number()` reads the recorded host-frame number, not an
incrementing local counter or metadata from an earlier main-thread poll.

`frame_synchronization["scene_stages"]` exposes full-cascade stage durations in
milliseconds (`rule_compilation_ms`, `cascade_ms`, `computed_tree_ms`,
`generated_content_ms`, `animation_configuration_ms`) plus `full_cascade_count`
and `layout_pass_count`. Stages are summed across the completed scene step and
zero when no full cascade ran; an unchanged surface retains its last step's
diagnostics. Rule compilation includes resetting style-driven state. These
stages do not include application markup generation, and are not all of the
work represented by the broader Style Time performance monitor.

## Regression checks

Run the suffixed editor with this project and `res://frame_contract.tscn` as the
scene. The fixture uses two HTMLViews and one HTMLRenderTarget, all starting
from the same document, at different sizes. A late-processing script issues
two consecutive menu replacements per scene and changes viewport dimensions.
Only the final menu is allowed in the composed frame. Its exact pixel bounds
must match native Godot ColorRects and the expected geometry. Every changed
scene must prepare exactly once and record the current script-frame number.

The test requires 60 consecutive frames (360 menu replacement requests), fails
on any missing or stale geometry, and saves `user://frame-contract-success.png`
on success. Use `--rendering-driver d3d12` or `vulkan`; append `-- --cpu` for CPU.
`-- --no-resize` isolates mutation behavior. This fixture's frame-ID assertions
use the normal render-thread mode; asynchronous post-draw/readback callbacks
in separate-thread mode require associating the sampled framebuffer first.

The existing `--validate` moving-cube test additionally covers continuous motion
and the separate render thread. Its readback overhead is not a performance
benchmark. `pending_submission_teardown.gd` covers destruction between packet
registration and command recording. Legacy backend scheduling is unchanged.

Validated on 2026-09-05 with the Radeon RX 9070: the DLL D3D12, Vulkan and CPU
paths and static D3D12 each passed all 60 contract frames with exact pixel
bounds, matching timestamps, and zero synchronization failures. The moving
cube passed 120 generations in low-processor mode with maximum error 1.414 px.
The pending-submission teardown check passed eight iterations. Preparation
timings are exposed for subsequent profiling; these readback tests do not
establish production frame-rate performance.

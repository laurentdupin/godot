extends SceneTree

var backend := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"
var activation_frames := {}

func _initialize() -> void:
	if "--vulkan" in OS.get_cmdline_user_args():
		backend = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	elif "--metal" in OS.get_cmdline_user_args():
		backend = HTMLView.BACKEND_METAL
		backend_name = "Metal"
	elif "--cpu" in OS.get_cmdline_user_args():
		backend = HTMLView.BACKEND_CPU
		backend_name = "CPU"
	call_deferred("_run")

func _run() -> void:
	var root := Control.new()
	root.size = Vector2(128, 64)
	get_root().add_child(root)
	var view := HTMLView.new()
	view.backend_preference = backend
	view.logical_size = Vector2i(128, 64)
	view.size = Vector2(128, 64)
	view.html = "<html><head><style>html,body{margin:0;background:#142434}.card{position:absolute;inset:8px;background:#286840}.card:hover{background:#b43838}</style></head><body><div class='card'></div></body></html>"
	view.frame_activated.connect(_on_frame_activated)
	root.add_child(view)
	var output := view.create_output(Vector2i(256, 128))
	if output == null:
		_fail("%s could not create the alignment output." % backend_name)
		return
	if not await _wait_for_aligned_visible_frame(view, output, Color8(40, 104, 64), 0):
		return

	var previous_generation := view.get_generation()
	var motion := InputEventMouseMotion.new()
	motion.position = Vector2(32, 32)
	motion.global_position = motion.position
	get_root().push_input(motion, true)
	if not await _wait_for_aligned_visible_frame(view, output, Color8(180, 56, 56), previous_generation):
		return
	var input_to_visible_seconds: float = Performance.get_custom_monitor("HCSR/Input To Visible Time")
	if input_to_visible_seconds <= 0.0 or input_to_visible_seconds >= 1.0:
		_fail("%s input-to-visible latency was not recorded in a plausible range: %.6f seconds." % [backend_name, input_to_visible_seconds])
		return
	var resolved_updates: float = Performance.get_custom_monitor("HCSR/Resolved Updates")
	var draw_batches: float = Performance.get_custom_monitor("HCSR/Draw Batches")
	var gpu_dispatches: float = Performance.get_custom_monitor("HCSR/GPU Dispatches")
	# Vulkan's direct physical compiler executes analytic batches as graphics draws,
	# so a valid frame need not contain a compute dispatch.
	if backend != HTMLView.BACKEND_CPU and (resolved_updates <= 0.0 or draw_batches <= 0.0):
		_fail("%s did not expose nonzero execution telemetry: updates=%.0f batches=%.0f dispatches=%.0f." % [backend_name, resolved_updates, draw_batches, gpu_dispatches])
		return
	print("HCSR_FRAME_ALIGNMENT backend=%s generation=%d host=%d activation=%d visible=%d input_to_visible_ms=%.3f updates=%.0f batches=%.0f dispatches=%.0f" % [
		backend_name,
		view.get_generation(),
		view.get_host_frame_number(),
		activation_frames[view.get_generation()],
		Engine.get_process_frames(),
		input_to_visible_seconds * 1000.0,
		resolved_updates,
		draw_batches,
		gpu_dispatches,
	])
	print("HCSR frame-alignment smoke passed on %s." % backend_name)
	quit(0)

func _wait_for_aligned_visible_frame(view: HTMLView, output: HTMLViewOutput, expected: Color, after_generation: int) -> bool:
	for _frame in range(240):
		await process_frame
		var generation := view.get_generation()
		if generation <= after_generation or output.generation != generation or not activation_frames.has(generation):
			continue
		await process_frame
		if view.get_generation() != generation or output.generation != generation:
			continue
		var host_frame := view.get_host_frame_number()
		var activation_frame: int = activation_frames[generation]
		var visible_frame := Engine.get_process_frames()
		if host_frame <= 0 or host_frame > activation_frame + 1 or activation_frame > visible_frame:
			_fail("%s frame lineage is unordered: host=%d activation=%d visible=%d." % [backend_name, host_frame, activation_frame, visible_frame])
			return false
		var primary := view.get_texture().get_image() if view.get_texture() != null else null
		var secondary := output.texture.get_image() if output.texture != null else null
		var canvas := get_root().get_texture().get_image()
		if primary == null or secondary == null or canvas == null or primary.is_empty() or secondary.is_empty() or canvas.is_empty():
			continue
		if _difference(primary.get_pixel(32, 32), expected) > 0.04 \
				or _difference(secondary.get_pixel(64, 64), expected) > 0.04 \
				or _difference(canvas.get_pixel(32, 32), expected) > 0.04:
			_fail("%s generation %d was not simultaneously visible in primary, secondary, and CanvasItem composition." % [backend_name, generation])
			return false
		return true
	_fail("%s timed out waiting for one aligned logical, primary, secondary, activation, and visible frame." % backend_name)
	return false

func _on_frame_activated(generation: int) -> void:
	activation_frames[generation] = Engine.get_process_frames()

func _difference(left: Color, right: Color) -> float:
	return max(abs(left.r - right.r), abs(left.g - right.g), abs(left.b - right.b), abs(left.a - right.a))

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

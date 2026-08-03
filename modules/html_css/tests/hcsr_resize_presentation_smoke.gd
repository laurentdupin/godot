extends SceneTree

const INITIAL_SIZE := Vector2i(320, 180)
const INTERMEDIATE_SIZE := Vector2i(480, 210)
const RESIZED_SIZE := Vector2i(640, 240)
const REPLACEMENT_TIMEOUT_MILLISECONDS := 3000

var backend_preference := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"


func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	elif OS.get_cmdline_user_args().has("--cpu"):
		backend_preference = HTMLView.BACKEND_CPU
		backend_name = "CPU"
	call_deferred("_run")


func _run() -> void:
	DisplayServer.window_set_size(RESIZED_SIZE)
	root.size = RESIZED_SIZE

	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.logical_size = INITIAL_SIZE
	view.size = INITIAL_SIZE
	var repeated_nodes := ""
	for node_index in range(1200):
		repeated_nodes += "<i style='position:absolute;left:%dpx;top:%dpx;width:1px;height:1px;background:#fff'></i>" % [node_index % INITIAL_SIZE.x, node_index % INITIAL_SIZE.y]
	view.html = """<!DOCTYPE html><html><head><style>
		*{box-sizing:border-box}html,body{margin:0;width:100%;height:100%;overflow:hidden}
		body{background:linear-gradient(90deg,#ef4444 0 50%,#2563eb 50% 100%)}
		#edge{position:absolute;right:0;bottom:0;width:12px;height:100%;background:#22c55e}
	</style></head><body>__REPEATED_NODES__<div id='edge'></div></body></html>""".replace("__REPEATED_NODES__", repeated_nodes)
	root.add_child(view)
	if not await _wait_for_generation(view, 0):
		_fail("%s resize smoke did not publish its initial frame." % backend_name)
		return

	var initial_generation := view.get_generation()
	if OS.get_environment("HCSR_SEMANTIC_WORKER") == "1":
		# The intermediate request starts a semantic-worker frame which the final
		# viewport can supersede. It must recover without unrelated input.
		view.logical_size = INTERMEDIATE_SIZE
		view.size = INTERMEDIATE_SIZE
		await process_frame
		view.logical_size = RESIZED_SIZE
		view.size = RESIZED_SIZE
		if not await _wait_for_generation(view, initial_generation):
			_fail("%s superseded resize did not publish its final replacement without input." % backend_name)
			return
	else:
		view.process_mode = Node.PROCESS_MODE_DISABLED
		view.logical_size = RESIZED_SIZE
		view.size = RESIZED_SIZE
		view.queue_redraw()

		# The active texture is intentionally still the old generation here. The
		# Control must nevertheless fill its new destination rect while replacement
		# layout/GPU work is pending, instead of leaving old-sized transparent space.
		await process_frame
		await RenderingServer.frame_post_draw
		if view.get_generation() != initial_generation:
			_fail("%s resize smoke did not hold its replacement frame for presentation testing." % backend_name)
			return
		var composed := root.get_texture().get_image()
		if composed == null or composed.is_empty():
			_fail("%s resize smoke could not capture the pending replacement frame." % backend_name)
			return
		var right_sample := composed.get_pixel(RESIZED_SIZE.x - 24, RESIZED_SIZE.y / 2)
		if right_sample.a < 0.9:
			_fail("%s retained HTML texture did not stretch across the resized destination while replacement was pending: %s." % [backend_name, right_sample])
			return

		view.process_mode = Node.PROCESS_MODE_INHERIT
		if not await _wait_for_generation(view, initial_generation):
			_fail("%s resize did not publish a replacement generation without input." % backend_name)
			return
	if view.get_texture() == null \
			or Vector2i(view.get_texture().get_width(), view.get_texture().get_height()) != RESIZED_SIZE:
		_fail("%s resize replacement texture has the wrong dimensions." % backend_name)
		return

	print("HCSR Godot %s resize presentation passed: generation %d -> %d, size %s." % [
		backend_name, initial_generation, view.get_generation(), RESIZED_SIZE])
	quit()


func _wait_for_generation(view: HTMLView, generation_before_change: int) -> bool:
	var deadline := Time.get_ticks_msec() + REPLACEMENT_TIMEOUT_MILLISECONDS
	while Time.get_ticks_msec() < deadline:
		await process_frame
		if view.get_generation() > generation_before_change:
			return true
	return false


func _fail(message: String) -> void:
	push_error(message)
	quit(1)

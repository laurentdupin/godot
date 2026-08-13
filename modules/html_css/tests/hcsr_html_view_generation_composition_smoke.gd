extends SceneTree

const LOGICAL_SIZE := Vector2i(2648, 1440)
const PHYSICAL_SIZE := Vector2i(2560, 1392)
const SECONDARY_SIZE := LOGICAL_SIZE / 2

var backend_preference := HTMLView.BACKEND_CPU
var backend_name := "CPU"
var active_generation := 0
var queued_generation := 0
var secondary_output: HTMLViewOutput


func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--d3d12"):
		backend_preference = HTMLView.BACKEND_D3D12
		backend_name = "D3D12"
	elif OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	elif OS.get_cmdline_user_args().has("--metal"):
		backend_preference = HTMLView.BACKEND_METAL
		backend_name = "Metal"
	else:
		_fail("HTMLView generation composition smoke requires --d3d12, --vulkan, or --metal.")
		return

	var viewport := SubViewport.new()
	viewport.size = PHYSICAL_SIZE
	viewport.transparent_bg = false
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(viewport)
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><style>html,body{margin:0;width:100vw;height:100vh;background:#111827;color:#edf2f7;font:32px sans-serif}.target{position:absolute;inset:137px 173px;background:#2563eb}.target span{position:absolute;left:29px;top:31px}.new{background:#dc2626}</style><body><div id='target' class='target'><span>None (2D only) · VRAM</span></div></body></html>"
	var transform_parent := Control.new()
	transform_parent.scale = Vector2(
		PHYSICAL_SIZE.x / float(LOGICAL_SIZE.x),
		PHYSICAL_SIZE.y / float(LOGICAL_SIZE.y))
	viewport.add_child(transform_parent)
	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_CONTROL_PHYSICAL_ADJUSTED
	view.logical_size = LOGICAL_SIZE
	view.size = Vector2(LOGICAL_SIZE)
	view.frame_queued.connect(_on_frame_queued)
	view.frame_activated.connect(_on_frame_activated)
	view.document = document
	transform_parent.add_child(view)
	await process_frame
	secondary_output = view.create_output(SECONDARY_SIZE)
	if secondary_output == null or not secondary_output.is_valid():
		_fail("%s could not create the different-resolution synchronized output." % backend_name)
		return

	if not await _wait_for_activation(0):
		return
	var baseline_generation := active_generation
	if view.set_element_attribute(&"target", &"class", "target new") != OK:
		_fail("%s HTMLView generation smoke could not queue its mutation." % backend_name)
		return
	if not await _wait_for_activation(baseline_generation):
		return
	if queued_generation != active_generation:
		_fail("%s did not activate the complete queue-ordered generation transaction: queued=%d active=%d." % [backend_name, queued_generation, active_generation])
		return
	if secondary_output.generation != active_generation:
		_fail("%s exposed mixed primary/secondary generations: primary=%d secondary=%d." % [backend_name, active_generation, secondary_output.generation])
		return

	RenderingServer.force_draw(false)
	await RenderingServer.frame_post_draw
	var imported_image := view.get_texture().get_image()
	var composed_image := viewport.get_texture().get_image()
	if imported_image == null or composed_image == null:
		_fail("%s HTMLView generation smoke could not capture both presentation layers." % backend_name)
		return
	if imported_image.get_size() != composed_image.get_size():
		_fail("%s HTMLView texture and composed viewport sizes differed for generation %d." % [backend_name, active_generation])
		return
	var maximum_difference := 0.0
	var differing_pixels := 0
	for y in range(PHYSICAL_SIZE.y):
		for x in range(PHYSICAL_SIZE.x):
			var imported_pixel := imported_image.get_pixel(x, y)
			var composed_pixel := composed_image.get_pixel(x, y)
			var difference: float = maxf(maxf(abs(imported_pixel.r - composed_pixel.r), abs(imported_pixel.g - composed_pixel.g)), maxf(abs(imported_pixel.b - composed_pixel.b), abs(imported_pixel.a - composed_pixel.a)))
			maximum_difference = maxf(maximum_difference, difference)
			if difference > 3.0 / 255.0:
				differing_pixels += 1
	if differing_pixels != 0:
		_fail("%s activated HTMLView texture and composed viewport differed for generation %d (pixels=%d max=%f)." % [backend_name, active_generation, differing_pixels, maximum_difference])
		return
	var secondary_image := secondary_output.texture.get_image()
	if secondary_image == null or secondary_image.get_size() != SECONDARY_SIZE:
		_fail("%s could not capture the different-resolution synchronized output." % backend_name)
		return
	var secondary_center := secondary_image.get_pixel(secondary_image.get_width() / 2, secondary_image.get_height() / 2)
	if secondary_center.r < 0.7 or secondary_center.g > 0.3 or secondary_center.b > 0.3:
		_fail("%s different-resolution output did not contain the activated red generation: %s." % [backend_name, secondary_center])
		return

	secondary_output.release()
	print("HCSR HTMLView same-generation composition smoke passed on %s." % backend_name)
	quit()


func _wait_for_activation(after_generation: int) -> bool:
	var deadline_usec := Time.get_ticks_usec() + 5_000_000
	while Time.get_ticks_usec() < deadline_usec:
		RenderingServer.force_draw(false)
		await RenderingServer.frame_post_draw
		await process_frame
		if active_generation > after_generation and secondary_output != null and secondary_output.generation == active_generation:
			return true
	_fail("%s HTMLView generation smoke timed out after generation %d (queued=%d active=%d)." % [backend_name, after_generation, queued_generation, active_generation])
	return false


func _on_frame_activated(generation: int) -> void:
	active_generation = generation


func _on_frame_queued(generation: int) -> void:
	queued_generation = generation


func _fail(message: String) -> void:
	push_error(message)
	quit(1)

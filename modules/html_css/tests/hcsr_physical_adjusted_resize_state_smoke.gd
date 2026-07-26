extends SceneTree

const DOCUMENT_HTML := """<!DOCTYPE html><html><head><style>
html,body{margin:0;width:100%;height:100%;background:#18212b;color:#edf2f7;font:18px sans-serif}
main{padding:32px}select{display:block;width:260px;height:42px;margin:16px 0}
</style></head><body><main>
<select id="quick-gpu"><option value="0">Default GPU</option><option value="1">Alternate GPU</option></select>
<select id="quick-model">
<option value="0">Model 0</option><option value="1">Model 1</option><option value="2">Model 2</option>
<option value="3">Model 3</option><option value="4">Model 4</option><option value="5">Model 5</option>
<option value="6">Model 6</option><option value="7">Model 7</option><option value="8">Model 8</option>
<option value="9">Model 9</option><option value="10">Model 10</option>
</select>
</main></body></html>"""

var backend_preference := HTMLView.BACKEND_CPU
var backend_name := "CPU"
var expected_physical_size := Vector2i(2560, 1392)
var activated_size_error := ""
var tested_view: HTMLView
var monitor_activated_size := false


func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--d3d12"):
		backend_preference = HTMLView.BACKEND_D3D12
		backend_name = "D3D12"
	elif OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"

	var viewport := SubViewport.new()
	viewport.size = Vector2i(2560, 1392)
	viewport.disable_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(viewport)
	var transform_parent := Control.new()
	viewport.add_child(transform_parent)
	var document := HTMLDocument.new()
	document.html = DOCUMENT_HTML
	var view := HTMLView.new()
	tested_view = view
	view.backend_preference = backend_preference
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_CONTROL_PHYSICAL_ADJUSTED
	view.size = Vector2(2648, 1440)
	view.frame_activated.connect(_on_frame_activated)
	transform_parent.scale = Vector2(2560.0 / 2648.0, 1392.0 / 1440.0)
	view.document = document
	transform_parent.add_child(view)
	if not await _wait_for_texture_size(view, expected_physical_size):
		_fail("%s initial physical texture did not converge to %s." % [backend_name, expected_physical_size])
		return
	monitor_activated_size = true

	if view.set_form_control_value(&"quick-gpu", "1") != OK \
			or view.set_form_control_value(&"quick-model", "10") != OK:
		_fail("Physical-adjusted resize smoke could not establish form state.")
		return

	var resize_steps := [
		[Vector2i(2648, 1440), Vector2i(2560, 1392)],
		[Vector2i(2200, 1440), Vector2i(2126, 1392)],
		[Vector2i(1920, 1080), Vector2i(1600, 900)],
		[Vector2i(2648, 1440), Vector2i(2560, 1392)],
	]
	for resize_step in resize_steps:
		var logical_size: Vector2i = resize_step[0]
		var physical_size: Vector2i = resize_step[1]
		var generation_before_resize := view.get_generation()
		expected_physical_size = physical_size
		viewport.size = physical_size
		view.size = Vector2(logical_size)
		transform_parent.scale = Vector2(physical_size) / Vector2(logical_size)
		if not await _wait_for_texture_size(view, physical_size, generation_before_resize):
			_fail("%s physical-adjusted resize did not converge to %s." % [backend_name, physical_size])
			return
		var texture := view.get_texture()
		if texture == null or Vector2i(texture.get_size()) != physical_size:
			_fail("%s physical-adjusted resize produced %s instead of %s." % [backend_name, texture.get_size() if texture != null else Vector2i.ZERO, physical_size])
			return
		var gpu: Dictionary = view.get_form_control_state(&"quick-gpu")
		var model: Dictionary = view.get_form_control_state(&"quick-model")
		if gpu.get("value") != "1" or gpu.get("selected_index") != 1 or model.get("value") != "10" or model.get("selected_index") != 10:
			_fail("%s resize changed Quick Start form state: gpu=%s model=%s." % [backend_name, gpu, model])
			return

	# Exercise the completion race rather than waiting for every size to settle.
	var final_resize_after_generation := view.get_generation()
	for resize_index in range(resize_steps.size()):
		var resize_step: Array = resize_steps[resize_index]
		var logical_size: Vector2i = resize_step[0]
		var physical_size: Vector2i = resize_step[1]
		if resize_index == resize_steps.size() - 1:
			final_resize_after_generation = view.get_generation()
		expected_physical_size = physical_size
		viewport.size = physical_size
		view.size = Vector2(logical_size)
		transform_parent.scale = Vector2(physical_size) / Vector2(logical_size)
		await process_frame
	if not activated_size_error.is_empty():
		_fail(activated_size_error)
		return
	if not await _wait_for_texture_size(view, expected_physical_size, final_resize_after_generation):
		_fail("%s rapid resize sequence did not converge to %s." % [backend_name, expected_physical_size])
		return
	if not activated_size_error.is_empty():
		_fail(activated_size_error)
		return

	var clean_document := HTMLDocument.new()
	clean_document.html = DOCUMENT_HTML
	var clean_view := HTMLView.new()
	clean_view.backend_preference = backend_preference
	clean_view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_CONTROL_PHYSICAL_ADJUSTED
	clean_view.size = view.size
	clean_view.scale = transform_parent.scale
	clean_view.document = clean_document
	viewport.add_child(clean_view)
	if not await _wait_for_texture_size(clean_view, expected_physical_size):
		_fail("%s clean resize oracle did not converge to %s." % [backend_name, expected_physical_size])
		return
	if not activated_size_error.is_empty():
		_fail(activated_size_error)
		return
	if clean_view.set_form_control_value(&"quick-gpu", "1") != OK \
			or clean_view.set_form_control_value(&"quick-model", "10") != OK:
		_fail("%s clean resize oracle could not establish matching form state." % backend_name)
		return
	await _wait_frames()
	var resized_image := view.get_texture().get_image()
	var clean_image := clean_view.get_texture().get_image()
	if resized_image == null or clean_image == null or resized_image.get_size() != clean_image.get_size() or resized_image.get_data() != clean_image.get_data():
		_fail("%s post-resize pixels differed from a clean surface at the same logical and physical metrics: %s" % [backend_name, _describe_image_difference(resized_image, clean_image)])
		return

	print("HCSR physical-adjusted resize/state smoke passed on %s." % backend_name)
	quit()


func _wait_frames() -> void:
	for _frame in range(16):
		await process_frame


func _wait_for_texture_size(view: HTMLView, required_size: Vector2i, after_generation: int = 0) -> bool:
	for _frame in range(120):
		await process_frame
		var texture := view.get_texture()
		if texture != null and Vector2i(texture.get_size()) == required_size and view.get_generation() > after_generation:
			return true
	return false


func _on_frame_activated(_generation: int) -> void:
	if not monitor_activated_size or tested_view == null:
		return
	var texture := tested_view.get_texture()
	if texture != null and Vector2i(texture.get_size()) != expected_physical_size:
		activated_size_error = "%s activated stale physical texture %s while the view required %s." % [backend_name, texture.get_size(), expected_physical_size]


func _describe_image_difference(first: Image, second: Image) -> String:
	if first == null or second == null:
		return "one or both images were null"
	if first.get_size() != second.get_size():
		return "sizes were %s and %s" % [first.get_size(), second.get_size()]
	var first_data := first.get_data()
	var second_data := second.get_data()
	if first_data.size() != second_data.size():
		return "data lengths were %d and %d" % [first_data.size(), second_data.size()]
	var pixel_count := first.get_width() * first.get_height()
	var bytes_per_pixel := first_data.size() / pixel_count
	var mismatch_count := 0
	var minimum := Vector2i(first.get_width(), first.get_height())
	var maximum := Vector2i(-1, -1)
	var maximum_channel_delta := 0
	for pixel_index in range(pixel_count):
		var pixel_differs := false
		for channel in range(bytes_per_pixel):
			var byte_index := pixel_index * bytes_per_pixel + channel
			var channel_delta: int = absi(first_data[byte_index] - second_data[byte_index])
			if channel_delta != 0:
				pixel_differs = true
				maximum_channel_delta = max(maximum_channel_delta, channel_delta)
		if pixel_differs:
			mismatch_count += 1
			var coordinate := Vector2i(pixel_index % first.get_width(), pixel_index / first.get_width())
			minimum = minimum.min(coordinate)
			maximum = maximum.max(coordinate)
	return "%d mismatched pixels, bounds %s..%s, maximum channel delta %d" % [mismatch_count, minimum, maximum, maximum_channel_delta]


func _fail(message: String) -> void:
	push_error(message)
	quit(1)

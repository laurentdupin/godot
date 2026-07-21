extends SceneTree

var backend_preference := HTMLView.BACKEND_CPU
var backend_name := "CPU"


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
	document.html_file = "res://thirdparty/hcsr/Examples/DeepDesktopQuickStart/DeepDesktopQuickStart.html"
	document.resource_root = "res://thirdparty/hcsr/Examples/DeepDesktopQuickStart"
	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_CONTROL_PHYSICAL_ADJUSTED
	view.size = Vector2(2648, 1440)
	transform_parent.scale = Vector2(2560.0 / 2648.0, 1392.0 / 1440.0)
	view.document = document
	transform_parent.add_child(view)
	await _wait_frames()

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
		viewport.size = physical_size
		view.size = Vector2(logical_size)
		transform_parent.scale = Vector2(physical_size) / Vector2(logical_size)
		await _wait_frames()
		var texture := view.get_texture()
		if texture == null or Vector2i(texture.get_size()) != physical_size:
			_fail("%s physical-adjusted resize produced %s instead of %s." % [backend_name, texture.get_size() if texture != null else Vector2i.ZERO, physical_size])
			return
		var gpu: Dictionary = view.get_form_control_state(&"quick-gpu")
		var model: Dictionary = view.get_form_control_state(&"quick-model")
		if gpu.get("value") != "1" or gpu.get("selected_index") != 1 or model.get("value") != "10" or model.get("selected_index") != 10:
			_fail("%s resize changed Quick Start form state: gpu=%s model=%s." % [backend_name, gpu, model])
			return

	print("HCSR physical-adjusted resize/state smoke passed on %s." % backend_name)
	quit()


func _wait_frames() -> void:
	for _frame in range(16):
		await process_frame


func _fail(message: String) -> void:
	push_error(message)
	quit(1)

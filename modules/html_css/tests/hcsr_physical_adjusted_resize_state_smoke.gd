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
	viewport.size = Vector2i(1000, 600)
	viewport.disable_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(viewport)
	var transform_parent := Control.new()
	viewport.add_child(transform_parent)
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><style>html,body{margin:0;width:100vw;height:100vh;background:#17324d}.scroll{width:100%;height:300px;overflow:auto}.content{width:100%;height:900px;background:#234}.row{height:80px;background:#345;margin-bottom:4px}</style><body><div class='scroll'><div class='content'><div class='row'><select id='choice'><option value='a'>Alpha</option><option value='b'>Beta</option></select><input id='enabled' type='checkbox'></div>" + "<div class='row'></div>".repeat(10) + "</div></div></body></html>"
	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_CONTROL_PHYSICAL_ADJUSTED
	view.size = Vector2(800, 360)
	transform_parent.scale = Vector2(773.0 / 800.0, 348.0 / 360.0)
	view.document = document
	transform_parent.add_child(view)
	await _wait_frames()

	if view.set_form_control_value(&"choice", "b") != OK \
			or view.set_form_control_checked(&"enabled", true) != OK:
		_fail("Physical-adjusted resize smoke could not establish form state.")
		return

	var resize_steps := [
		[Vector2(800, 360), Vector2(773.0 / 800.0, 348.0 / 360.0), Vector2i(773, 348)],
		[Vector2(662, 360), Vector2(640.0 / 662.0, 348.0 / 360.0), Vector2i(640, 348)],
		[Vector2(913, 360), Vector2(882.0 / 913.0, 348.0 / 360.0), Vector2i(883, 348)],
		[Vector2(800, 360), Vector2(773.0 / 800.0, 348.0 / 360.0), Vector2i(773, 348)],
	]
	for resize_step in resize_steps:
		view.size = resize_step[0]
		transform_parent.scale = resize_step[1]
		await _wait_frames()
		var texture := view.get_texture()
		if texture == null or Vector2i(texture.get_size()) != resize_step[2]:
			_fail("%s physical-adjusted resize produced %s instead of %s." % [backend_name, texture.get_size() if texture != null else Vector2i.ZERO, resize_step[2]])
			return
		var choice: Dictionary = view.get_form_control_state(&"choice")
		var enabled: Dictionary = view.get_form_control_state(&"enabled")
		if choice.get("value") != "b" or choice.get("selected_index") != 1 or enabled.get("checked") != true:
			_fail("%s resize changed logical form state: choice=%s enabled=%s." % [backend_name, choice, enabled])
			return

	print("HCSR physical-adjusted resize/state smoke passed on %s." % backend_name)
	quit()


func _wait_frames() -> void:
	for _frame in range(16):
		await process_frame


func _fail(message: String) -> void:
	push_error(message)
	quit(1)

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
	elif OS.get_cmdline_user_args().has("--metal"):
		backend_preference = HTMLView.BACKEND_METAL
		backend_name = "Metal"

	var viewport := SubViewport.new()
	viewport.size = Vector2i(1000, 600)
	viewport.disable_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(viewport)
	var parent := Control.new()
	parent.scale = Vector2(2.0, 2.0)
	viewport.add_child(parent)
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><style>html,body{margin:0;background:#17324d;color:white}#probe{width:50vw;height:50vh;background:#e07020}</style><body><div id='probe'></div></body></html>"
	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.size = Vector2(320, 180)
	view.document = document
	parent.add_child(view)

	await _assert_mode(view, HTMLView.VIEWPORT_SIZE_CONTROL, Vector2i(320, 180), Vector2(160, 90), "Control Size")
	await _assert_mode(view, HTMLView.VIEWPORT_SIZE_CONTROL_PHYSICAL_ADJUSTED, Vector2i(640, 360), Vector2(160, 90), "Control Physical Adjusted")
	await _assert_mode(view, HTMLView.VIEWPORT_SIZE_PHYSICAL_SIZE, Vector2i(640, 360), Vector2(320, 180), "Physical Size")

	view.fixed_viewport_size = Vector2i(400, 240)
	view.fixed_viewport_device_scale_factor = 0.0
	await _assert_mode(view, HTMLView.VIEWPORT_SIZE_FIXED, Vector2i(400, 240), Vector2(200, 120), "Fixed")
	view.fixed_viewport_device_scale_factor = 1.5
	await _assert_mode(view, HTMLView.VIEWPORT_SIZE_FIXED, Vector2i(600, 360), Vector2(200, 120), "Fixed 1.5x")

	parent.scale = Vector2(0.5, 0.5)
	view.fixed_viewport_device_scale_factor = 0.0
	await _assert_mode(view, HTMLView.VIEWPORT_SIZE_FIXED, Vector2i(400, 240), Vector2(200, 120), "Fixed after downscale")
	await _assert_mode(view, HTMLView.VIEWPORT_SIZE_CONTROL, Vector2i(320, 180), Vector2(160, 90), "Control Size after downscale")
	await _assert_mode(view, HTMLView.VIEWPORT_SIZE_CONTROL_PHYSICAL_ADJUSTED, Vector2i(160, 90), Vector2(160, 90), "Control Physical Adjusted downscale")
	await _assert_mode(view, HTMLView.VIEWPORT_SIZE_PHYSICAL_SIZE, Vector2i(160, 90), Vector2(80, 45), "Physical Size after downscale")

	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_CONTROL
	view.size = Vector2(400, 200)
	for _frame in range(12):
		await process_frame
	if view.get_texture() == null or view.get_texture().get_size() != Vector2(400, 200):
		_fail("Control Size did not follow a later transformed control resize.")
		return

	var staged_errors: Array[String] = []
	var staged_view := HTMLView.new()
	staged_view.backend_preference = backend_preference
	staged_view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_CONTROL_PHYSICAL_ADJUSTED
	staged_view.render_error.connect(func(message: String) -> void: staged_errors.append(message))
	staged_view.document = document
	parent.add_child(staged_view)
	staged_view.logical_size = Vector2i(400, 200)
	staged_view.size = Vector2(400, 200)
	for _frame in range(12):
		await process_frame
	if not staged_errors.is_empty():
		_fail("Deferred Control Physical Adjusted initialization emitted transient render errors: %s." % staged_errors)
		return
	if staged_view.get_texture() == null or staged_view.get_texture().get_size() != Vector2(200, 100):
		_fail("Deferred Control Physical Adjusted initialization did not publish its first valid physical target.")
		return

	print("HCSR HTMLView viewport-size modes passed on %s." % backend_name)
	quit()


func _assert_mode(view: HTMLView, mode: int, expected_texture_size: Vector2i, expected_html_center: Vector2, label: String) -> void:
	view.viewport_size_mode = mode
	for _frame in range(12):
		await process_frame
	var texture := view.get_texture()
	if texture == null or texture.get_size() != Vector2(expected_texture_size):
		_fail("%s produced %s instead of %s." % [label, texture.get_size() if texture != null else Vector2.ZERO, expected_texture_size])
		return
	var html_center := view.local_to_html_position(Vector2(160, 90))
	if not html_center.is_equal_approx(expected_html_center):
		_fail("%s mapped the local center to %s instead of %s." % [label, html_center, expected_html_center])


func _fail(message: String) -> void:
	push_error(message)
	quit(1)

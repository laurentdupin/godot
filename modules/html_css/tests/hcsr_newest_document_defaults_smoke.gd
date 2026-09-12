extends SceneTree

func _initialize():
	run.call_deferred()
	create_timer(45).timeout.connect(func(): quit(2))

func settle():
	for i in 8:
		await process_frame
		await RenderingServer.frame_post_draw

func check(ok: bool, message: String):
	if not ok:
		push_error(message)
		quit(1)
		assert(false, message)

func run():
	Engine.max_fps = 60
	root.size = Vector2i(320, 160)
	var document = HTMLDocument.new()
	document.html = "<style>body{background:white}#marker{width:20px;height:20px;background:red}input{display:block;width:100px;height:30px;padding:4px;border:1px solid black;font:16px monospace;color:black;background:white}</style><div id='marker'></div><input id='input' value='ABCDEFGHIJKLMNOPQRSTUVWXYZ'>"
	var view = HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_VULKAN if OS.get_environment("HCSR_TEST_GPU") == "vulkan" else HTMLView.BACKEND_D3D12
	view.size = Vector2(320, 160)
	view.logical_size = Vector2i(320, 160)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.document = document
	root.add_child(view)
	await settle()
	var initial = view.get_texture().get_image()
	check(initial.get_pixel(8, 8).r > .9 and initial.get_pixel(8, 8).g < .1, "Implicit body did not put marker at 8,8")
	check(initial.get_pixel(7, 7).g > .9, "Default margin or body background missing")
	check(view.set_form_control_value("input", "ABCDEFGHIJKLMNOPQRSTUVWXYZEXTRA") == OK, "Value update failed")
	await settle()
	check(initial.get_data() == view.get_texture().get_image().get_data(), "Unfocused value update scrolled the input")
	for pressed in [true, false]:
		var click = InputEventMouseButton.new()
		click.position = Vector2(13.1, 35)
		click.global_position = click.position
		click.button_index = MOUSE_BUTTON_LEFT
		click.pressed = pressed
		view.dispatch_input_event(click)
	await settle()
	var key = InputEventKey.new()
	key.keycode = KEY_X
	key.unicode = 88
	key.pressed = true
	view.dispatch_input_event(key)
	await settle()
	check(view.get_form_control_state("input").get("value", "") == "XABCDEFGHIJKLMNOPQRSTUVWXYZEXTRA", "First click targeted hidden text after focus")
	print("DOCUMENT_DEFAULTS_RENDER_OK implicit body margin/background, unfocused viewport, first click and typing")
	quit()

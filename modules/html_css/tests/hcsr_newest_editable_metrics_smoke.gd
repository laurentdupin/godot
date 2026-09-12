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
	root.size = Vector2i(300, 220)
	var document = HTMLDocument.new()
	document.resource_root = "res://Examples/DeepDesktopQuickStart"
	document.html = "<style>@font-face{font-family:Probe;src:url('NotoSans.ttf')}body{margin:0;background:white;color:black}textarea{font:28px Probe;display:block;box-sizing:border-box;width:200px;padding:12px;border:4px solid black}</style><textarea id='area' rows='3'>A\nA\nA</textarea>"
	var view = HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_VULKAN if OS.get_environment("HCSR_TEST_GPU") == "vulkan" else HTMLView.BACKEND_D3D12
	view.size = Vector2(300, 220)
	view.logical_size = Vector2i(300, 220)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.document = document
	root.add_child(view)
	await settle()
	var image = view.get_texture().get_image()
	if not OS.get_environment("HCSR_EDITABLE_OUTPUT").is_empty():
		image.save_png(OS.get_environment("HCSR_EDITABLE_OUTPUT"))
	var starts: Array[int] = []
	var previous = false
	var first_x = 200
	for y in range(5, 135):
		var ink = false
		for x in range(5, 100):
			var color = image.get_pixel(x, y)
			if color.a > .8 and color.r < .2:
				ink = true
				first_x = mini(first_x, x)
		if ink and not previous: starts.append(y)
		previous = ink
	check(starts.size() == 3, "Expected three separate text lines: " + str(starts))
	if starts.size() != 3: return
	check(starts[1] - starts[0] == 38 and starts[2] - starts[1] == 38, "Text does not use Chromium's 38px Noto line spacing")
	check(first_x >= 16 and first_x <= 17, "Text ignored border and authored padding: " + str(first_x))
	for padding in [12, 24]:
		check(view.set_element_style("area", "padding:%dpx" % padding) == OK, "Padding mutation failed")
		check(view.set_form_control_value("area", "A\nA\nA") == OK, "Value reset failed")
		await settle()
		var point = Vector2(padding + 4.1, padding + 4 + 37)
		var motion = InputEventMouseMotion.new()
		motion.position = point
		motion.global_position = point
		view.dispatch_input_event(motion)
		for pressed in [true, false]:
			var click = InputEventMouseButton.new()
			click.position = point
			click.global_position = point
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
		check(view.get_form_control_state("area").get("value", "") == "XA\nA\nA", "Click/typing used a different line or padding from painting: " + str(view.get_form_control_state("area")))
	print("EDITABLE_METRICS_RENDER_OK padding, normal line spacing, click/typing after mutation")
	quit()

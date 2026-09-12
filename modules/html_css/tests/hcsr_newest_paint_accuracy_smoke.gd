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
	root.size = Vector2i(640, 480)
	var document = HTMLDocument.new()
	document.html = "<style>body{margin:0;background:white;font:20px/20px Arial}fieldset{margin:0;width:300px;padding:20px;border:10px solid red;border-radius:16px;background:lime}legend{padding:0 5px}#content{height:30px}input{position:absolute;left:20px;top:120px;width:200px;height:40px;padding:4px;border:1px solid black;font:20px/30px Arial} .font{position:absolute;left:0;width:600px;height:40px;font-size:26px;line-height:40px}</style><fieldset id='field'><legend id='legend'>Legend</legend><div id='content'></div></fieldset><input id='rtl' dir='rtl' value='אבג'><div class='font' style='top:200px;font-family:system-ui'>Hamburgefonts 012345</div><div class='font' style='top:240px;font-family:Segoe UI'>Hamburgefonts 012345</div>"
	var view = HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_VULKAN if OS.get_environment("HCSR_TEST_GPU") == "vulkan" else HTMLView.BACKEND_D3D12
	view.size = Vector2(640, 480)
	view.logical_size = Vector2i(640, 480)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.document = document
	root.add_child(view)
	await settle()
	var image = view.get_texture().get_image()
	if not OS.get_environment("HCSR_ACCURACY_OUTPUT").is_empty(): image.save_png(OS.get_environment("HCSR_ACCURACY_OUTPUT"))
	check(image.get_pixel(20, 2).g > .9, "Fieldset border not lowered")
	check(image.get_pixel(20, 10).r > .9 and image.get_pixel(20, 10).g < .1, "Top border missing")
	check(image.get_pixel(32, 10).g > .9 and image.get_pixel(32, 10).r < .1, "Legend gap does not preserve background")
	if OS.get_name() == "Windows":
		check(image.get_region(Rect2i(0,200,600,40)).get_data() == image.get_region(Rect2i(0,240,600,40)).get_data(), "system-ui differs from Segoe UI")
	var right_ink = 0
	var left_ink = 0
	for y in range(126,154):
		for x in range(26,214):
			if image.get_pixel(x,y).r < .2:
				if x < 120: left_ink += 1
				else: right_ink += 1
	check(right_ink > 20 and left_ink == 0, "RTL text is not aligned to the right")
	for pressed in [true,false]:
		var click = InputEventMouseButton.new()
		click.position = Vector2(214,130)
		click.global_position = click.position
		click.button_index = MOUSE_BUTTON_LEFT
		click.pressed = pressed
		view.dispatch_input_event(click)
	await settle()
	var key = InputEventKey.new()
	key.keycode = KEY_NONE
	key.unicode = 0x05d3
	key.pressed = true
	view.dispatch_input_event(key)
	await settle()
	check(view.get_form_control_state("rtl").get("value","") == "דאבג", "RTL first click and typing chose wrong logical caret")
	check(view.set_element_style("field","border-style:groove") == OK, "Groove mutation failed")
	await settle()
	image = view.get_texture().get_image()
	check(image.get_pixel(32,10).g > .9 and image.get_pixel(32,10).r < .1, "Non-solid border ignored legend gap")
	check(view.set_element_style("legend","display:none") == OK, "Hide legend failed")
	await settle()
	image = view.get_texture().get_image()
	check(image.get_pixel(40,2).g < .3, "Hidden legend left stale border inset")
	print("PAINT_ACCURACY_RENDER_OK solid/groove legend, mutation, RTL paint/click/type, Windows system-ui")
	quit()

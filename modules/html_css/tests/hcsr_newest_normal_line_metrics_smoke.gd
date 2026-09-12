extends SceneTree

func _initialize():
	run.call_deferred()
	create_timer(45).timeout.connect(func(): quit(2))

func settle():
	for i in 8:
		await process_frame
		await RenderingServer.frame_post_draw

func check_marker(view: HTMLView, expected: int):
	var image = view.get_texture().get_image()
	var first_blue = -1
	for y in image.get_height():
		var color = image.get_pixel(250, y)
		if color.b > .9 and color.r < .1:
			first_blue = y
			break
	if first_blue != expected:
		push_error("Normal line metric drift: expected marker at %d, got %d" % [expected, first_blue])
		quit(1)
		assert(false)

func run():
	# Chromium on Windows: ten 16px Times New Roman normal lines are 180px.
	# The old 64px-rounded Godot metrics accumulated to 190px.
	if OS.get_name() != "Windows":
		print("NORMAL_LINE_METRICS_SKIP Windows system-font reference")
		quit()
		return
	Engine.max_fps = 60
	root.size = Vector2i(300, 400)
	var document = HTMLDocument.new()
	document.html = "<style>body{margin:0;background:white}#rows{font:16px 'Times New Roman'}.end{height:10px;background:blue}</style><div id='rows'>" + "<div>Repeated text</div>".repeat(10) + "</div><div class='end'></div>"
	var view = HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_VULKAN if OS.get_environment("HCSR_TEST_GPU") == "vulkan" else HTMLView.BACKEND_D3D12
	view.size = Vector2(300, 400)
	view.logical_size = Vector2i(300, 400)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.document = document
	root.add_child(view)
	await settle()
	check_marker(view, 180)
	view.set_element_style("rows", "font:16px Times New Roman;line-height:24px")
	await settle()
	check_marker(view, 240)
	view.set_element_style("rows", "font:16px Times New Roman;line-height:normal")
	await settle()
	check_marker(view, 180)
	print("NORMAL_LINE_METRICS_OK Chromium normal height, explicit height, reset")
	quit()


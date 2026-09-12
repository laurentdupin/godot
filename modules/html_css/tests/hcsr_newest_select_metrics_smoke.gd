extends SceneTree

func _initialize():
	run.call_deferred()
	create_timer(45).timeout.connect(func(): quit(2))

func settle():
	for i in 8:
		await process_frame
		await RenderingServer.frame_post_draw

func run():
	Engine.max_fps = 60
	root.size = Vector2i(300, 100)
	var document = HTMLDocument.new()
	document.resource_root = "res://Examples/DeepDesktopQuickStart"
	document.html = "<style>@font-face{font-family:Probe;src:url('NotoSans.ttf')}body{margin:0;background:white}select{font:28px Probe;display:block;width:300px;padding:0;border:0}#marker{height:10px;background:blue}</style><select id='choice'><option>Example</option></select><div id='marker'></div>"
	var view = HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_VULKAN if OS.get_environment("HCSR_TEST_GPU") == "vulkan" else HTMLView.BACKEND_D3D12
	view.size = Vector2(300, 100)
	view.logical_size = Vector2i(300, 100)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.document = document
	root.add_child(view)
	for case in [["", 40], ["appearance:none", 38], ["line-height:60px", 40], ["appearance:none;line-height:60px", 60], ["", 40]]:
		await settle()
		if view.set_element_style("choice", case[0]) != OK:
			push_error("Select style mutation failed")
			quit(1)
			return
		await settle()
		var image = view.get_texture().get_image()
		var top = -1
		for y in image.get_height():
			var color = image.get_pixel(250, y)
			if color.b > .9 and color.r < .1:
				top = y
				break
		if top != case[1]:
			push_error("Chromium select height for '%s': expected %d, got %d" % [case[0], case[1], top])
			quit(1)
			return
	print("SELECT_METRICS_RENDER_OK native, appearance:none, explicit line height, repeated mutation")
	quit()

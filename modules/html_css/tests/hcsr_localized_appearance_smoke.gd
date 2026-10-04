extends SceneTree

var failed := false

func _initialize() -> void:
	run.call_deferred()
	create_timer(60).timeout.connect(func(): quit(2))

func require(value: bool, label: String) -> void:
	if not value:
		failed = true
		push_error("LOCAL_APPEARANCE_FAILED " + label)

func settle() -> void:
	for i in 12:
		await process_frame
		await RenderingServer.frame_post_draw

func make_view(html: String) -> HTMLView:
	var view := HTMLView.new()
	view.size = Vector2(320, 200)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.logical_size = Vector2i(320, 200)
	view.backend_preference = HTMLView.BACKEND_GPU_AUTO
	var document := HTMLDocument.new()
	document.html = html
	document.background_color = Color.TRANSPARENT
	view.document = document
	root.add_child(view)
	return view

func run() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	var image := Image.create(12, 6, false, Image.FORMAT_RGBA8)
	image.fill(Color.RED)
	var image_source := "data:image/png;base64," + Marshalls.raw_to_base64(image.save_png_to_buffer())
	var html := "<style>html,body{margin:0;font:16px Arial}#scroll{position:absolute;left:10px;top:10px;width:100px;height:120px;overflow:auto;background:#123456}.row{height:40px;background:#789abc}#scroll.changed{background:green;box-shadow:3px 4px 2px red}#group{position:absolute;left:150px;top:20px;width:150px;height:70px;opacity:.6;mask-image:linear-gradient(90deg,transparent,black)}#label{color:red}#label.changed{color:blue}#image{position:absolute;left:150px;top:110px;width:60px;height:60px;object-fit:contain}#image.changed{object-fit:cover;box-shadow:2px 3px 2px blue}#check{position:absolute;left:250px;top:110px;width:24px;height:24px;accent-color:#00aaff}</style><div id='scroll'>"
	for i in 20:
		html += "<div class='row' id='row%d'></div>" % i
	html += "</div><div id='group'><span id='label'>Caption</span></div><img id='image' src='%s'><input id='check' type='checkbox'>" % image_source
	var actual := make_view(html)
	var output := actual.create_output(Vector2i(320, 200), true)
	await settle()
	require(actual.scroll_element_into_view("row10", "start") == OK, "initial scroll")
	await settle()
	for cycle in 4:
		var changed := cycle % 2 == 0
		var css_class := "changed" if changed else ""
		for id in ["scroll", "label", "image"]:
			require(actual.set_element_attribute(id, "class", css_class) == OK, "class " + id)
		require(actual.set_form_control_checked("check", changed) == OK, "checked state")
		await settle()
		var expected_html := html
		if changed:
			for id in ["scroll", "label", "image"]:
				expected_html = expected_html.replace("id='%s'" % id, "id='%s' class='changed'" % id)
			expected_html = expected_html.replace("type='checkbox'", "type='checkbox' checked")
		var expected := make_view(expected_html)
		var expected_output := expected.create_output(Vector2i(320, 200), true)
		await settle()
		require(expected.scroll_element_into_view("row10", "start") == OK, "reference scroll")
		await settle()
		var a: Image = output.texture.get_image()
		var b: Image = expected_output.texture.get_image()
		var different := 0
		for y in 200:
			for x in 320:
				if a.get_pixel(x, y) != b.get_pixel(x, y):
					different += 1
		require(different == 0, "fresh pixels cycle=%d different=%d" % [cycle, different])
		expected.queue_free()
		await settle()
	print("LOCAL_APPEARANCE_SMOKE ", "FAIL" if failed else "OK", " cycles=4 text=true images=true checkbox=true mask_opacity=true scrolled_thumb=true")
	quit(1 if failed else 0)

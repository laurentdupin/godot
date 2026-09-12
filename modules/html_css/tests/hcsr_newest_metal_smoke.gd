extends SceneTree

var failed := false
var view: HTMLView
var phase := "startup"

func require(condition: bool, label: String) -> void:
	if not condition:
		failed = true
		push_error("METAL_CHECK_FAILED " + label)

func _initialize() -> void:
	run.call_deferred()
	create_timer(60).timeout.connect(func():
		push_error("METAL_CHECK_TIMEOUT " + phase)
		quit(2)
	)

func settle(frames := 8) -> void:
	for i in frames:
		await process_frame
		await RenderingServer.frame_post_draw

func diagnostics() -> Dictionary:
	return view.get_frame_scheduler_diagnostics().frame_synchronization

func run() -> void:
	require(RenderingServer.get_current_rendering_driver_name() == "metal", "run with --rendering-driver metal")
	view = HTMLView.new()
	view.size = Vector2(240, 160)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.logical_size = Vector2i(240, 160)
	view.backend_preference = HTMLView.BACKEND_GPU_AUTO
	var document := HTMLDocument.new()
	# An empty packet exercises native recording and target clear inside Godot's
	# encoder; colored paint/text/images then exercise the shared RD atlas path.
	document.html = "<style>html,body{margin:0;background:transparent}</style><div id='content'></div>"
	view.document = document
	root.add_child(view)
	var small := view.create_output(Vector2i(240, 160), false)
	var large := view.create_output(Vector2i(480, 320), true)
	phase = "initial native rendering"
	await settle()
	require(diagnostics().get("renderer") == "metal", "GPU_AUTO selects Metal, not CPU fallback")
	require(int(diagnostics().get("native_recordings", 0)) > 0, "native Metal callback executed")
	require(not diagnostics().terminal, "native render succeeded: " + str(diagnostics()))
	if diagnostics().terminal:
		quit(1)
		return
	var image := small.texture.get_image()
	require(image != null and image.get_pixel(20, 20).a < .01, "transparent native clear")
	view.backend_preference = HTMLView.BACKEND_METAL
	phase = "explicit backend selection"
	await settle(2)
	require(diagnostics().get("renderer") == "metal", "explicit Metal selection")
	var png := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	png.fill(Color(0, 1, 0, .5))
	var source := "data:image/png;base64," + Marshalls.raw_to_base64(png.save_png_to_buffer())
	var html := """<div style='position:absolute;left:10px;top:10px;width:60px;height:20px;background:red'></div>
	<div style='position:absolute;left:20px;top:115px;width:30px;height:25px;background:blue'></div>
	<img style='position:absolute;left:100px;top:10px;width:40px;height:40px' src='%s'>
	<div style='position:absolute;left:10px;top:55px;font-family:Arial;font-size:24px;color:white'>Metal atlas</div>""" % source
	require(view.set_element_inner_html("content", html) == OK, "populate paint, image and glyphs")
	phase = "atlas rendering"
	await settle(16)
	for output in [small, large]:
		image = output.texture.get_image()
		var scale := image.get_width() / 240.0
		var red := image.get_pixel(int(20 * scale), int(20 * scale))
		var blue := image.get_pixel(int(30 * scale), int(125 * scale))
		var green := image.get_pixel(int(120 * scale), int(30 * scale))
		require(red.r > .9 and red.b < .05, "top red geometry and orientation")
		require(blue.b > .9 and blue.r < .05, "bottom blue geometry and orientation")
		require(abs(green.a - .5) < .03 and green.g > .45 and green.r < .05, "image atlas color/alpha")
		var ink := 0
		for y in range(int(52 * scale), int(90 * scale)):
			for x in range(int(8 * scale), int(190 * scale)):
				if image.get_pixel(x, y).a > .2:
					ink += 1
		require(ink > 100 * scale * scale and ink < 2500 * scale * scale, "glyph atlas contains text and transparent gaps")
		require(output.generation == view.get_generation(), "shared generation")
	var canvas := root.get_texture().get_image()
	var canvas_red := canvas.get_pixel(20, 20)
	require(canvas_red.r > .9 and canvas_red.b < .05, "Godot canvas samples the shared HCSR texture")
	require(int(diagnostics().image_atlas.get("rasterized_glyphs", 0)) > 0, "glyph upload path")
	require(int(diagnostics().image_atlas.uploaded_bytes) > 4, "atlas uploads reached GPU")
	large.size = Vector2i(360, 240)
	phase = "output resize"
	await settle()
	require(large.texture.get_image().get_size() == Vector2i(360, 240), "resize")
	# Return to the native path after atlas rendering and resizing. This checks
	# resource/state transitions and clears old atlas content from both outputs.
	require(view.set_element_inner_html("content", "") == OK, "remove atlas content")
	var native_before := int(diagnostics().native_recordings)
	phase = "native rendering after atlas"
	await settle()
	require(int(diagnostics().native_recordings) > native_before, "native recording resumes after atlas drawing")
	for output in [small, large]:
		image = output.texture.get_image()
		require(image.get_pixel(20, 20).a < .01, "native clear removes prior content")
	require(int(diagnostics().failures) == 0 and not diagnostics().terminal, "frame synchronization")
	small.release()
	large.release()
	view.queue_free()
	phase = "deferred cleanup"
	await settle()
	print("METAL_INTEGRATION_", "FAILED" if failed else "OK")
	quit(1 if failed else 0)

extends SceneTree

var view: HTMLView

func _initialize():
	run.call_deferred()
	create_timer(120).timeout.connect(func(): quit(2))

func settle():
	for frame in 12:
		await process_frame
		await RenderingServer.frame_post_draw

func stats() -> Dictionary:
	return view.get_frame_scheduler_diagnostics().frame_synchronization.get("image_atlas", {})

func fail(message: String):
	push_error(message)
	quit(1)

func run():
	Engine.max_fps = 0
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	DisplayServer.window_set_size(Vector2i(640, 360))
	var background := ColorRect.new()
	background.size = Vector2(640, 360)
	background.color = Color("#1acc33")
	root.add_child(background)
	view = HTMLView.new()
	view.size = Vector2(640, 360)
	view.logical_size = Vector2i(640, 360)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.backend_preference = HTMLView.BACKEND_GPU_AUTO
	view.backdrop_filter_enabled = true
	var document := HTMLDocument.new()
	document.html = "<style>html,body{margin:0;background:#1acc33}#outer{position:absolute;left:20px;top:20px;opacity:.7}#inner{opacity:.8}#prior{width:120px;height:100px;background:#c84058}</style><div id=outer><div id=inner><div id=prior></div></div></div>"
	document.background_color = Color.WHITE
	view.document = document
	root.add_child(view)
	await settle()
	var initial := stats()
	if initial.get("host_compositing_bounds_required", true) or int(initial.get("cpu_compositing_bounds_evaluations", -1)) != 0 or int(initial.get("nested_group_passes", 0)) < 3:
		fail("Nested passes still require CPU coverage: " + str(initial))
		return
	var image: Image = view.get_texture().get_image()
	var original: PackedByteArray = image.get_data()
	var pixel := image.get_pixel(70, 60)
	var expected := Color("#1acc33").lerp(Color("#c84058"), .7 * .8)
	if abs(pixel.r-expected.r) > .015 or abs(pixel.g-expected.g) > .015 or abs(pixel.b-expected.b) > .015:
		fail("Nested batching changed group opacity: " + str(pixel))
		return
	for cycle in 3:
		if view.set_element_attribute("prior", "style", "transform:translateX(25px)") != OK:
			fail("Group movement rejected")
			return
		await settle()
		if view.get_texture().get_image().get_data() == original:
			fail("Moving nested content did not change pixels")
			return
		if view.set_element_attribute("prior", "style", "transform:translateX(0px)") != OK:
			fail("Group reset rejected")
			return
		await settle()
		if view.get_texture().get_image().get_data() != original or int(stats().get("cpu_compositing_bounds_evaluations", -1)) != 0:
			fail("Group restoration lost pixels or evaluated CPU bounds")
			return
	var large = view.create_output(Vector2i(960, 540), false)
	var small = view.create_output(Vector2i(320, 180), false)
	await settle()
	var large_pixels: PackedByteArray = large.texture.get_image().get_data()
	var small_pixels: PackedByteArray = small.texture.get_image().get_data()
	view.logical_size = Vector2i(800, 450)
	await settle()
	if large.texture.get_image().get_data() == large_pixels or small.texture.get_image().get_data() == small_pixels:
		fail("Group GPU regions did not follow logical resize")
		return
	view.logical_size = Vector2i(640, 360)
	await settle()
	if large.texture.get_image().get_data() != large_pixels or small.texture.get_image().get_data() != small_pixels or int(stats().get("cpu_compositing_bounds_evaluations", -1)) != 0:
		fail("Multiple-output groups did not restore their GPU regions")
		return
	large.release()
	small.release()
	print("GPU_NESTED_GROUPS_OK cycles=3 nested_opacity=true cpu_evaluations=0 outputs=3 logical_resize=true pixels_restored=true")
	quit()

extends SceneTree

var view: HTMLView

func _initialize():
	run.call_deferred()
	create_timer(120).timeout.connect(func(): quit(2))

func settle():
	for frame in 20:
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
	view = HTMLView.new()
	view.size = Vector2(320, 180)
	view.logical_size = Vector2i(320, 180)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.backend_preference = HTMLView.BACKEND_GPU_AUTO
	var svg := "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12' preserveAspectRatio='none'><rect width='16' height='12' fill='#ff0000'/></svg>"
	var source := "data:image/svg+xml;base64," + Marshalls.utf8_to_base64(svg)
	var document := HTMLDocument.new()
	document.html = "<style>html,body{margin:0}#appearance{position:absolute;left:20px;top:20px;width:100px;height:60px;background-image:url('" + source + "');background-size:cover}#mask{position:absolute;left:160px;top:20px;width:70px;height:60px;background:blue;mask-image:url('" + source + "');mask-size:cover}img{position:absolute;left:20px;top:110px;width:32px;height:24px}</style><div id='appearance'></div><div id='mask'></div><img src='" + source + "'>"
	document.background_color = Color.WHITE
	view.document = document
	root.add_child(view)
	await settle()
	var initial := stats()
	if int(initial.get("decoded_source_pixel_bytes", -1)) != 0:
		fail("Published images still retain decoded source pixels: " + str(initial))
		return
	var image: Image = view.get_texture().get_image()
	if image.get_pixel(40, 40).r < .95 or image.get_pixel(180, 40).b < .95 or image.get_pixel(30, 120).r < .95:
		fail("Background, mask or ordinary SVG failed to publish")
		return
	var original: PackedByteArray = image.get_data()
	var decodes := int(initial.decoded_images)
	for cycle in 3:
		view.set_element_attribute("appearance", "style", "transform:translate(30px,10px)")
		await settle()
		if view.get_texture().get_image().get_data() == original:
			fail("Appearance movement did not change pixels")
			return
		view.set_element_attribute("appearance", "style", "transform:translate(0px,0px)")
		await settle()
		if view.get_texture().get_image().get_data() != original or int(stats().decoded_images) != decodes:
			fail("Movement lost pixels or decoded the source again")
			return
	view.set_element_attribute("appearance", "style", "width:150px;height:80px")
	await settle()
	if view.get_texture().get_image().get_pixel(140, 40).r < .95 or int(stats().get("decoded_source_pixel_bytes", -1)) != 0:
		fail("New raster demand failed to publish or retained duplicate pixels")
		return
	print("DECODED_OWNERSHIP_OK background=true mask=true ordinary_svg=true movement_decodes=0 decoded_source_pixel_bytes=0 resize=true")
	quit()

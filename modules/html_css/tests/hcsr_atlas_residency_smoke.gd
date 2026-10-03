extends SceneTree

var view: HTMLView

func _initialize():
	run.call_deferred()
	create_timer(180).timeout.connect(func(): quit(2))

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
	DisplayServer.window_set_size(Vector2i(320, 180))
	view = HTMLView.new()
	view.size = Vector2(320, 180)
	view.logical_size = Vector2i(320, 180)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.backend_preference = HTMLView.BACKEND_GPU_AUTO
	var large := ""
	for index in 20:
		large += "<div style='position:absolute;left:0;top:0;width:1000px;height:1000px;background:linear-gradient(123deg,rgb(%d,30,50),blue 37%%,yellow)'></div>" % (50 + index * 7)
	var small := "<div style='width:240px;height:120px;background:#1234ab;border:2px solid #63a1e0;border-radius:12px'></div>"
	var document := HTMLDocument.new()
	document.html = "<style>html,body{margin:0}</style><div id='app'>" + large + "</div>"
	document.background_color = Color.WHITE
	view.document = document
	root.add_child(view)
	await settle()
	var initial := stats()
	if int(initial.get("gpu_atlas_pages", 0)) < 3:
		fail("Fixture did not allocate multiple GPU atlas pages: " + str(initial))
		return
	var original: PackedByteArray = view.get_texture().get_image().get_data()
	for cycle in 3:
		if view.set_element_inner_html("app", small) != OK:
			fail("Small mutation rejected")
			return
		await settle()
		var reduced := stats()
		if int(reduced.get("gpu_atlas_bytes", 0)) >= int(initial.gpu_atlas_bytes) or int(reduced.get("gpu_atlas_retired_pages", 0)) <= cycle:
			fail("Empty GPU pages were not retired: " + str(reduced))
			return
		var image: Image = view.get_texture().get_image()
		var pixel := image.get_pixel(50, 50)
		if pixel.b < 0.5 or pixel.r > 0.15:
			fail("Small live appearance lost its pixels")
			return
		if view.set_element_inner_html("app", large) != OK:
			fail("Large mutation rejected")
			return
		await settle()
		if view.get_texture().get_image().get_data() != original:
			fail("Reused atlas pages do not reproduce the original appearance")
			return
		print("ATLAS_RESIDENCY_CYCLE ", cycle, " large=", initial.gpu_atlas_bytes, " small=", reduced.gpu_atlas_bytes, " retired=", reduced.gpu_atlas_retired_pages)
	# New instances reuse the same glyph pixels but grow hierarchy/geometry buffers.
	# Atlas revision reuse must not skip descriptor rebinding for those buffers.
	var glyph_document := HTMLDocument.new()
	glyph_document.html = "<style>html,body{margin:0;overflow:hidden}span{position:absolute;font:16px Arial;color:#1234ab}</style><div id='app'><span id='glyph' style='left:20px;top:20px'>A</span></div>"
	glyph_document.background_color = Color.WHITE
	view.document = glyph_document
	await settle()
	var glyph_pixels: PackedByteArray = view.get_texture().get_image().get_data()
	if view.set_element_attribute("glyph", "style", "left:80px;top:50px") != OK:
		fail("Reference glyph movement rejected")
		return
	await settle()
	var expected_glyph_pixels: PackedByteArray = view.get_texture().get_image().get_data()
	if expected_glyph_pixels == glyph_pixels:
		fail("Reference glyph movement did not change visible pixels")
		return
	if view.set_element_attribute("glyph", "style", "left:20px;top:20px") != OK:
		fail("Reference glyph reset rejected")
		return
	await settle()
	var glyph_stats := stats()
	var repeated := ""
	for index in 1500:
		repeated += "<span style='left:2000px;top:2000px'>A</span>"
	# Put the visible glyph after the new instances so stale old GPU buffers
	# cannot pass by retaining their original first visible primitive.
	repeated += "<span style='left:80px;top:50px'>A</span>"
	if view.set_element_inner_html("app", repeated) != OK:
		fail("Shared glyph instance mutation rejected")
		return
	await settle()
	var grown := stats()
	if int(grown.hierarchy_nodes) < int(glyph_stats.hierarchy_nodes) + 1000:
		fail("Shared glyph fixture did not grow GPU hierarchy buffers")
		return
	if int(grown.uploaded_bytes) != int(glyph_stats.uploaded_bytes) or int(grown.rasterized_glyphs) != int(glyph_stats.rasterized_glyphs):
		fail("Shared glyph instance mutation unexpectedly repainted/uploaded atlas pixels: initial=" + str(glyph_stats) + " grown=" + str(grown))
		return
	if view.get_texture().get_image().get_data() != expected_glyph_pixels:
		fail("Geometry buffer rebinding lost unchanged atlas pixels")
		return
	print("ATLAS_REBIND_OK unchanged_atlas=true relocated_pixels_match=true new_instances=1500 hierarchy_nodes=", grown.hierarchy_nodes)
	print("ATLAS_RESIDENCY_OK cycles=3 stable_pixels=true deferred_retirement=true")
	quit()

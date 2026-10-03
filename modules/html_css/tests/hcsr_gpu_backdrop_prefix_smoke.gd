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
	document.html = """
<style>
html,body{margin:0;background:#1acc33}
.case{position:absolute;top:20px;width:200px;height:180px}
#document{left:20px}#internal{left:240px;will-change:opacity;background:#1acc33}
.under{position:absolute;left:20px;top:20px;width:120px;height:100px;background:rgb(40 80 220 / 30%)}
.prior{position:absolute;left:20px;top:20px;width:120px;height:100px;background:rgb(200 64 88 / 40%);transform:translateX(0px)}
.glass{position:absolute;left:60px;top:40px;width:120px;height:100px;backdrop-filter:invert(1);background:rgb(255 255 255 / 12%)}
</style>
<div class='case' id='document'><div style='isolation:isolate'><div class='under'></div><div style='isolation:isolate'><div class='prior' id='prior'></div><div class='glass'></div></div></div></div>
<div class='case' id='internal'><div style='isolation:isolate'><div class='under'></div><div style='isolation:isolate'><div class='prior'></div><div class='glass'></div></div></div></div>
"""
	document.background_color = Color.WHITE
	view.document = document
	root.add_child(view)
	await settle()
	var initial := stats()
	if initial.get("host_compositing_bounds_required", true) or int(initial.get("cpu_compositing_bounds_evaluations", -1)) != 0 or int(initial.get("gpu_backdrop_prefix_draws", 0)) == 0:
		fail("Ordered prefixes did not consume GPU coverage: " + str(initial))
		return
	var image: Image = view.get_texture().get_image()
	var original: PackedByteArray = image.get_data()
	var doc_pixel := image.get_pixel(100, 80)
	var internal_pixel := image.get_pixel(320, 80)
	if abs(doc_pixel.r-internal_pixel.r) > .005 or abs(doc_pixel.g-internal_pixel.g) > .005 or abs(doc_pixel.b-internal_pixel.b) > .005:
		fail("Document and internal roots sample different prefix colors")
		return
	if doc_pixel.r < .4 or doc_pixel.b < .4:
		fail("Fixture did not invert the translucent prefix input")
		return
	for cycle in 3:
		if view.set_element_attribute("prior", "style", "transform:translateX(25px)") != OK:
			fail("Prefix movement rejected")
			return
		await settle()
		if view.get_texture().get_image().get_data() == original:
			fail("Moving a sampled prefix did not change backdrop pixels")
			return
		if view.set_element_attribute("prior", "style", "transform:translateX(0px)") != OK:
			fail("Prefix reset rejected")
			return
		await settle()
		if view.get_texture().get_image().get_data() != original or int(stats().get("cpu_compositing_bounds_evaluations", -1)) != 0:
			fail("Prefix restoration lost pixels or evaluated CPU bounds")
			return
	var large = view.create_output(Vector2i(960, 540), false)
	var small = view.create_output(Vector2i(320, 180), false)
	await settle()
	var large_pixels: PackedByteArray = large.texture.get_image().get_data()
	var small_pixels: PackedByteArray = small.texture.get_image().get_data()
	view.logical_size = Vector2i(800, 450)
	await settle()
	if large.texture.get_image().get_data() == large_pixels or small.texture.get_image().get_data() == small_pixels:
		fail("Prefix GPU regions did not follow logical resize")
		return
	view.logical_size = Vector2i(640, 360)
	await settle()
	if large.texture.get_image().get_data() != large_pixels or small.texture.get_image().get_data() != small_pixels or int(stats().get("cpu_compositing_bounds_evaluations", -1)) != 0:
		fail("Multiple-output prefixes did not restore their GPU regions")
		return
	large.release()
	small.release()
	print("GPU_BACKDROP_PREFIX_OK cycles=3 nested_isolation=true document_root=true internal_root=true cpu_evaluations=0 outputs=3 logical_resize=true pixels_restored=true")
	quit()

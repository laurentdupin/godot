extends SceneTree

var view: HTMLView

func _initialize():
	run.call_deferred()
	create_timer(120).timeout.connect(func(): quit(2))

func settle():
	for frame in 10:
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
	var document := HTMLDocument.new()
	document.html = "<style>html,body{margin:0}#fx{position:absolute;left:20px;top:30px;filter:blur(2px) brightness(.8);opacity:.8}#box{width:100px;height:60px;background:red;border-radius:12px}</style><div id='fx'><div id='box'></div></div>"
	document.background_color = Color.WHITE
	view.document = document
	root.add_child(view)
	await settle()
	var original: PackedByteArray = view.get_texture().get_image().get_data()
	var initial := stats()
	if int(initial.get("coverage_groups", 0)) == 0 or int(initial.get("draw_calls", 0)) < 2:
		fail("Fixture did not create compositing passes: " + str(initial))
		return
	if initial.get("host_compositing_bounds_required", true) or int(initial.get("cpu_compositing_bounds_evaluations", -1)) != 0:
		fail("Spatial foreground compositor still evaluated CPU coverage: " + str(initial))
		return
	var pixel := view.get_texture().get_image().get_pixel(70, 60)
	if pixel.r < .5 or pixel.g > .5:
		fail("Filtered group did not paint visible content")
		return
	for cycle in 3:
		if view.set_element_attribute("fx", "style", "transform:translate(40px,10px)") != OK:
			fail("Movement rejected")
			return
		await settle()
		if view.get_texture().get_image().get_data() == original:
			fail("GPU placement did not move filtered content")
			return
		if view.set_element_attribute("fx", "style", "transform:translate(0px,0px)") != OK:
			fail("Reset rejected")
			return
		await settle()
		if view.get_texture().get_image().get_data() != original or int(stats().get("cpu_compositing_bounds_evaluations", -1)) != 0:
			fail("GPU-only coverage lost pixels or evaluated CPU placement")
			return
	if view.set_element_attribute("fx", "style", "mix-blend-mode:multiply") != OK:
		fail("Blend mutation rejected")
		return
	await settle()
	var blending := stats()
	if blending.get("host_compositing_bounds_required", true) or int(blending.get("cpu_compositing_bounds_evaluations", -1)) != 0 or int(blending.get("gpu_blend_region_copies", 0)) == 0:
		fail("Blend copy did not consume GPU-only coverage: " + str(blending))
		return
	# Multiply over white preserves these pixels. This checks copied content,
	# rather than merely checking that a compute dispatch was recorded.
	if view.get_texture().get_image().get_data() != original:
		fail("GPU blend copy changed multiply-over-white pixels")
		return
	if view.set_element_attribute("fx", "style", "mix-blend-mode:multiply;transform:translate(40px,10px)") != OK:
		fail("Blended movement rejected")
		return
	await settle()
	if view.get_texture().get_image().get_data() == original or int(stats().get("cpu_compositing_bounds_evaluations", -1)) != 0:
		fail("Blended movement lost GPU coverage")
		return
	if view.set_element_attribute("fx", "style", "") != OK:
		fail("Blend reset rejected")
		return
	await settle()
	if stats().get("host_compositing_bounds_required", true) or view.get_texture().get_image().get_data() != original:
		fail("Coverage dependency did not reset with pass topology")
		return
	if view.set_element_attribute("fx", "style", "mix-blend-mode:multiply") != OK:
		fail("Multiple-output blend mutation rejected")
		return
	var large = view.create_output(Vector2i(640, 360), false)
	var small = view.create_output(Vector2i(160, 90), false)
	await settle()
	var large_pixels: PackedByteArray = large.texture.get_image().get_data()
	var small_pixels: PackedByteArray = small.texture.get_image().get_data()
	view.logical_size = Vector2i(640, 360)
	await settle()
	if large.texture.get_image().get_data() == large_pixels or small.texture.get_image().get_data() == small_pixels:
		fail("Output-specific GPU region did not follow logical resize")
		return
	view.logical_size = Vector2i(320, 180)
	await settle()
	if large.texture.get_image().get_data() != large_pixels or small.texture.get_image().get_data() != small_pixels or int(stats().get("cpu_compositing_bounds_evaluations", -1)) != 0:
		fail("Multiple-output blend copy did not restore density/regions")
		return
	large.release()
	small.release()
	print("GPU_GROUP_COVERAGE_OK cycles=3 spatial_cpu_evaluations=0 blend_cpu_evaluations=0 indirect_blend_copy=true pixels_restored=true outputs=3 logical_resize=true")
	quit()

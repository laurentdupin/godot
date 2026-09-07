extends SceneTree

var failed := false
var view: HTMLView

func require(value: bool, message: String) -> void:
	if not value:
		failed = true
		push_error("TEXT_CHECK_FAILED " + message)

func _initialize() -> void:
	_run.call_deferred()
	create_timer(60).timeout.connect(func(): quit(2))

func settle(frames := 12) -> void:
	for i in frames:
		await process_frame
		await RenderingServer.frame_post_draw

func stats() -> Dictionary:
	return view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas

func coverage(image: Image) -> Rect2:
	var scale := image.get_width() / 400.0
	var result := Rect2()
	var count := 0
	for y in range(int(12 * scale), int(70 * scale)):
		for x in range(int(12 * scale), int(295 * scale)):
			if image.get_pixel(x,y).a > .2:
				var point := Vector2(x,y) / scale
				result = Rect2(point,Vector2.ONE / scale) if count == 0 else result.expand(point)
				count += 1
	require(count > 100 * scale * scale, "glyph pixels are present")
	require(count < result.get_area() * scale * scale * .7, "text contains transparent gaps instead of solid bars")
	return result

func _run() -> void:
	view = HTMLView.new()
	view.size = Vector2(400,240)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.logical_size = Vector2i(400,240)
	view.backend_preference = HTMLView.BACKEND_CPU if "--cpu" in OS.get_cmdline_user_args() else HTMLView.BACKEND_GPU_AUTO
	var doc := HTMLDocument.new()
	doc.css_files = PackedStringArray(["res://Assets/Shared/UI/RetainedUIFonts.css"])
	doc.html = """<html><head><style>html,body{margin:0}body{font-family:'Application Sans';font-size:20px;color:white}#label{position:absolute;left:20px;top:20px;transform-origin:0 0}#wrap{position:absolute;left:20px;top:90px;width:100px}#clip{position:absolute;left:200px;top:90px;width:40px;height:25px;overflow:hidden}#clip span{white-space:nowrap}input{position:absolute;left:20px;top:190px;width:160px;height:30px}</style></head><body><div id='label'>Hello atlas 123</div><div id='wrap'>Wrapped words here</div><div id='clip'><span>CLIPPED TEXT</span></div><input value='Editable text'></body></html>"""
	var unicode := "--unicode" in OS.get_cmdline_user_args()
	var prefix := "text-atlas-unicode" if unicode else "text-atlas"
	if unicode:
		doc.html = doc.html.replace("font-family:'Application Sans'", "font-family:'Application Sans','Application CJK','Application Emoji'").replace("Hello atlas 123", "你好🙂 atlas مرحبا")
	view.document = doc
	root.add_child(view)
	var small := view.create_output(Vector2i(400,240), false)
	await settle()
	var first := small.texture.get_image()
	var original := coverage(first)
	first.save_png("res://Build/%s-1x.png" % prefix)
	if unicode:
		var colored := 0
		for y in range(12,70):
			for x in range(12,295):
				var pixel := first.get_pixel(x,y)
				if pixel.a > .4 and maxf(pixel.r,maxf(pixel.g,pixel.b)) - minf(pixel.r,minf(pixel.g,pixel.b)) > .2:
					colored += 1
		require(colored > 5, "color emoji preserves intrinsic RGB")
	var initial := stats()
	require(int(initial.get("rasterized_glyphs",0)) > 10, "glyphs populate atlas: " + str(initial))
	require(int(initial.get("glyph_pages",0)) == 1, "dedicated glyph page")
	var large := view.create_output(Vector2i(800,480), false)
	await settle(1)
	coverage(large.texture.get_image())
	require(int(stats().pending_glyphs) > 0, "cached glyphs remain visible while higher levels queue")
	await settle(24)
	var enlarged := large.texture.get_image()
	var high := coverage(enlarged)
	enlarged.save_png("res://Build/%s-2x.png" % prefix)
	require(original.position.distance_to(high.position) < 2 and original.size.distance_to(high.size) < 3, "resolution preserves logical placement: " + str(original) + " / " + str(high))
	var upgraded := stats()
	require(int(upgraded.rasterized_glyphs) > int(initial.rasterized_glyphs), "higher resolution creates a new level")
	require(int(upgraded.pending_glyphs) == 0, "upgrades drain")
	for x in range(245,285):
		require(enlarged.get_pixel(x*2,100*2).a < .05, "ancestor clipping")
	require(view.set_element_style("label", "transform:scale(1.035)") == OK, "hover mutation")
	await settle()
	require(int(stats().rasterized_glyphs) == int(upgraded.rasterized_glyphs), "hover reuses glyph resolution")
	var hover := coverage(large.texture.get_image())
	require(hover.size.x > high.size.x, "hover scales visible text")
	var uploads := int(stats().uploaded_bytes)
	await settle()
	require(int(stats().uploaded_bytes) == uploads, "settled text has zero uploads")
	large.size = Vector2i(400,240)
	await settle()
	require(int(stats().rasterized_glyphs) == int(upgraded.rasterized_glyphs), "downsize reuses cached level")
	require(int(view.get_frame_scheduler_diagnostics().frame_synchronization.failures) == 0, "frame synchronization")
	print("TEXT_ATLAS_", "FAILED" if failed else "OK", " initial=", initial, " final=", stats())
	large.release()
	small.release()
	view.queue_free()
	await settle(4)
	quit(1 if failed else 0)

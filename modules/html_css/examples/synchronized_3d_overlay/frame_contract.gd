extends Node

# Menu replacement, independent scenes sharing a source, different resolutions,
# resize, and late script processing must all reach the same composed frame.
var views: Array[HTMLView] = []
var target: HTMLRenderTarget
var target_rect: TextureRect
var references: Array[ColorRect] = []
var expected: Array[Rect2i] = []
var previous_counts: Array[int] = [0, 0, 0]
var preparation_max_ms := 0.0
var tick := 0
var checks := 0
var mutated_frame := -1

func _ready() -> void:
	process_priority = 1000
	RenderingServer.set_default_clear_color(Color(0.01, 0.02, 0.04))
	var doc := HTMLDocument.new()
	doc.html = """<html><head><style>
	html,body { margin:0; width:100%; height:100%; }
	#app { display:contents; }
	#app > div { background:white; }
	</style></head><body><div id='app'><div style='position:absolute;left:10px;top:20px;width:24px;height:30px'></div></div></body></html>"""
	var backend := HTMLView.BACKEND_CPU if "--cpu" in OS.get_cmdline_user_args() else HTMLView.BACKEND_GPU_AUTO
	for index in range(2):
		var view := HTMLView.new()
		view.position = Vector2(10 + index * 320, 10)
		view.size = Vector2(220 + index * 20, 150)
		view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_CONTROL
		view.backend_preference = backend
		view.document = doc
		add_child(view)
		views.append(view)
	target = HTMLRenderTarget.new()
	target.backend_preference = backend
	target.size = Vector2i(240, 160)
	target.document = doc
	add_child(target)
	target_rect = TextureRect.new()
	target_rect.position = Vector2(650, 10)
	target_rect.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	target_rect.size = target.size
	target_rect.texture = target.get_texture()
	add_child(target_rect)
	for index in range(3):
		var reference := ColorRect.new()
		reference.color = Color(0.1, 0.4, 1.0)
		add_child(reference)
		references.append(reference)
		expected.append(Rect2i())
	RenderingServer.frame_post_draw.connect(_check)

func _status(index: int) -> Dictionary:
	return views[index].get_frame_scheduler_diagnostics()["frame_synchronization"] if index < 2 else target.get_frame_synchronization()

func _process(_delta: float) -> void:
	tick += 1
	if tick <= 6:
		return
	mutated_frame = Engine.get_process_frames()
	for index in range(3):
		previous_counts[index] = int(_status(index)["preparations"])
		var width := 220 + index * 20 + (40 if tick % 2 == 0 else 0)
		var height := 150 + (20 if tick % 3 == 0 else 0)
		if "--no-resize" in OS.get_cmdline_user_args():
			width = 220 + index * 20
			height = 150
		if index < 2:
			views[index].size = Vector2(width, height)
		else:
			target.size = Vector2i(width, height)
			target_rect.size = Vector2(width, height)
		var percent := 10 + ((tick + index * 3) % 5) * 10
		var box_width := 24 + ((tick + index) % 4) * 4
		var fragment := "<div style='position:absolute;left:%d%%;top:20px;width:%dpx;height:30px'></div>" % [percent, box_width]
		# The intermediate menu must never be displayed; final data wins this frame.
		var error: int
		if index < 2:
			error = views[index].set_element_inner_html("app", "<div style='position:absolute;left:0;top:0;width:5px;height:5px'></div>")
			if error == OK:
				error = views[index].set_element_inner_html("app", fragment)
		else:
			error = target.set_element_inner_html("app", "<div style='width:5px;height:5px'></div>")
			if error == OK:
				error = target.set_element_inner_html("app", fragment)
		if error != OK:
			_fail("Mutation failed for scene %d: %s" % [index, error_string(error)])
			return
		var x := 10 + index * 320 + width * percent / 100
		expected[index] = Rect2i(x, 30, box_width, 30)
		references[index].position = Vector2(x, 230)
		references[index].size = Vector2(box_width, 30)

func _check() -> void:
	if mutated_frame < 0:
		return
	var image := get_viewport().get_texture().get_image()
	var frame_time := float(_status(0)["prepared_time_seconds"])
	for index in range(3):
		var status := _status(index)
		if float(status["prepared_time_seconds"]) != frame_time:
			_fail("Scenes sampled different animation times within one host frame", image)
			return
		if int(status["failures"]) != 0 or bool(status["pending"]) or int(status["recorded_host_frame"]) != mutated_frame or int(status["preparations"]) != previous_counts[index] + 1 or status["prepared_generation"] != status["recorded_generation"]:
			_fail("Frame contract failed for scene %d, script frame %d: %s" % [index, mutated_frame, status], image)
			return
		if index < 2 and views[index].get_host_frame_number() != mutated_frame:
			_fail("Public host frame getter disagrees with recorded frame", image)
			return
		preparation_max_ms = maxf(preparation_max_ms, float(status["preparation_ms"]))
		var html_box := _bounds(image, Rect2i(10 + index * 320, 10, 300, 190), false)
		var engine_box := _bounds(image, Rect2i(10 + index * 320, 220, 300, 60), true)
		engine_box.position.y -= 200
		if html_box != expected[index] or engine_box != expected[index]:
			_fail("Scene %d has mixed/stale geometry: HTML=%s Godot=%s expected=%s" % [index, html_box, engine_box, expected[index]], image)
			return
	checks += 1
	if checks == 60:
		image.save_png("user://frame-contract-success.png")
		print("HTML_ENGINE_FRAME_CONTRACT_OK frames=%d scenes=3 menu_replacements=%d max_preparation_ms=%.3f" % [checks, checks * 6, preparation_max_ms])
		get_tree().quit()

func _bounds(image: Image, region: Rect2i, blue: bool) -> Rect2i:
	var low := Vector2i(100000, 100000)
	var high := Vector2i(-1, -1)
	for y in range(region.position.y, region.end.y):
		for x in range(region.position.x, region.end.x):
			var color := image.get_pixel(x, y)
			var match_pixel := color.b > 0.8 and color.r < 0.3 if blue else color.r > 0.04 and absf(color.r - color.g) < 0.01 and absf(color.g - color.b) < 0.01
			if match_pixel:
				low = Vector2i(mini(low.x, x), mini(low.y, y))
				high = Vector2i(maxi(high.x, x), maxi(high.y, y))
	return Rect2i(low, high - low + Vector2i.ONE) if high.x >= 0 else Rect2i()

func _fail(message: String, image: Image = null) -> void:
	set_process(false)
	mutated_frame = -1
	if image != null:
		image.save_png("user://frame-contract-failure.png")
	push_error(message)
	get_tree().quit(1)

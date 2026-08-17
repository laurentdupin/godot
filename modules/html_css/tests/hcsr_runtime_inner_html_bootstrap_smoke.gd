extends SceneTree

const WIDTH := 160
const HEIGHT := 96
const BACKGROUND := Color8(17, 24, 39)
const RETAINED := Color8(37, 99, 235)
const REPLACEMENT := Color8(220, 38, 38)

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	root.size = Vector2i(WIDTH * 2, HEIGHT)
	var host := Control.new()
	host.size = Vector2(WIDTH * 2, HEIGHT)
	root.add_child(host)
	var retained := _make_view(false)
	host.add_child(retained)
	if not await _wait_for_generation(retained, 0):
		return
	var bootstrap_generation := retained.get_generation()
	RenderingServer.force_draw(false)
	await RenderingServer.frame_post_draw
	var bootstrap := root.get_texture().get_image()
	if bootstrap == null or _count_color(bootstrap, RETAINED) == 0:
		_fail("The retained HTMLView did not expose its coherent bootstrap texture.")
		return
	var error := retained.set_element_inner_html(
		"app",
		"<div id='panel' style='position:absolute;left:20px;top:20px;width:96px;height:48px;background:#dc2626;color:#ffffff'><div id='label'>ready</div></div>")
	if error != OK:
		_fail("The replacement runtime rejected strict inner HTML: %s." % error_string(error))
		return
	# Submission prepares a successor author revision, but it cannot blank or
	# partially modify the currently published texture.
	if retained.get_generation() != bootstrap_generation:
		_fail("Inner HTML activated synchronously instead of retaining the prior publication.")
		return
	RenderingServer.force_draw(false)
	await RenderingServer.frame_post_draw
	var pending := root.get_texture().get_image()
	if pending == null or _count_color(pending, RETAINED) == 0:
		_fail("The coherent bootstrap disappeared while fragment work was pending.")
		return
	if not await _wait_for_generation(retained, bootstrap_generation):
		return
	var clean := _make_view(true)
	clean.position = Vector2(WIDTH, 0)
	host.add_child(clean)
	if not await _wait_for_generation(clean, 0):
		return
	RenderingServer.force_draw(false)
	await RenderingServer.frame_post_draw
	var composed := root.get_texture().get_image()
	if composed == null or not _halves_equal(composed):
		_fail("Retained fragment activation differs from an independently compiled clean endpoint.")
		return
	var replacement_pixels := _count_color_region(composed, REPLACEMENT, Rect2i(0, 0, WIDTH, HEIGHT))
	if replacement_pixels == 0:
		_fail("The atomically activated fragment remained visually blank.")
		return
	print("HCSR_RUNTIME_INNER_HTML_BOOTSTRAP_OK bootstrap=%d active=%d red_pixels=%d" % [
		bootstrap_generation,
		retained.get_generation(),
		replacement_pixels,
	])
	quit(0)

func _make_view(final_endpoint: bool) -> HTMLView:
	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_D3D12
	view.logical_size = Vector2i(WIDTH, HEIGHT)
	view.size = Vector2(WIDTH, HEIGHT)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_CONTROL_PHYSICAL_ADJUSTED
	var body := "<div id='app' style='position:absolute;left:20px;top:20px;width:100px;height:50px;background:#2563eb'></div>"
	if final_endpoint:
		body = "<div id='app' style='position:absolute;left:20px;top:20px;width:100px;height:50px;background:#2563eb'><div id='panel' style='position:absolute;left:20px;top:20px;width:96px;height:48px;background:#dc2626;color:#ffffff'><div id='label'>ready</div></div></div>"
	var document := HTMLDocument.new()
	document.html = "<html><head><style>html{display:block;position:relative;width:160px;height:96px;background:#111827}</style></head>%s</html>" % body
	view.document = document
	return view

func _wait_for_generation(view: HTMLView, after: int) -> bool:
	for _frame in range(600):
		await process_frame
		RenderingServer.force_draw(false)
		await RenderingServer.frame_post_draw
		await create_timer(0.01).timeout
		if view.get_generation() > after:
			return true
	_fail("Timed out waiting for HTML generation after %d (queued=%d active=%d texture=%s)." % [
		after,
		view.get_queued_generation(),
		view.get_generation(),
		view.get_texture(),
	])
	return false

func _count_color(image: Image, expected: Color) -> int:
	var count := 0
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			if image.get_pixel(x, y).is_equal_approx(expected):
				count += 1
	return count

func _count_color_region(image: Image, expected: Color, region: Rect2i) -> int:
	var count := 0
	for y in range(region.position.y, region.end.y):
		for x in range(region.position.x, region.end.x):
			if image.get_pixel(x, y).is_equal_approx(expected):
				count += 1
	return count

func _halves_equal(image: Image) -> bool:
	if image.get_width() < WIDTH * 2 or image.get_height() < HEIGHT:
		return false
	for y in range(HEIGHT):
		for x in range(WIDTH):
			if image.get_pixel(x, y) != image.get_pixel(x + WIDTH, y):
				return false
	return true

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

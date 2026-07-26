extends SceneTree

const WIDTH := 360
const HEIGHT := 180
const TEARDOWN_ITERATIONS := 12

var backend_preference := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"

func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	elif OS.get_cmdline_user_args().has("--metal"):
		backend_preference = HTMLView.BACKEND_METAL
		backend_name = "Metal"
	elif OS.get_cmdline_user_args().has("--cpu"):
		backend_preference = HTMLView.BACKEND_CPU
		backend_name = "CPU"

	DisplayServer.window_set_size(Vector2i(WIDTH * 2, HEIGHT))
	root.size = Vector2i(WIDTH * 2, HEIGHT)

	for iteration in range(TEARDOWN_ITERATIONS):
		var retiring_view := _make_view(_make_document("retiring-%02d-a" % iteration, "#1d4ed8"))
		root.add_child(retiring_view)
		if not await _wait_for_generation(retiring_view, 0):
			_fail("%s lifetime stress did not activate teardown generation %d." % [backend_name, iteration])
			return
		var active_generation := retiring_view.get_generation()
		retiring_view.document = _make_document("retiring-%02d-b" % iteration, "#be123c")
		await process_frame
		if retiring_view.get_generation() < active_generation:
			_fail("%s lifetime stress regressed its active generation before teardown." % backend_name)
			return
		retiring_view.queue_free()
		await process_frame
		await process_frame

	var replaced_view := _make_view(_make_document("initial", "#1d4ed8"))
	var clean_view := _make_view(_make_document("replacement", "#15803d"))
	clean_view.position = Vector2(WIDTH, 0)
	root.add_child(replaced_view)
	root.add_child(clean_view)
	if not await _wait_for_generation(replaced_view, 0) or not await _wait_for_generation(clean_view, 0):
		_fail("%s lifetime stress could not activate its clean comparison views." % backend_name)
		return

	var generation_before_replacement := replaced_view.get_generation()
	replaced_view.document = _make_document("replacement", "#15803d")
	if not await _wait_for_generation(replaced_view, generation_before_replacement):
		_fail("%s lifetime stress did not activate its replacement document." % backend_name)
		return
	for _frame in range(3):
		await process_frame
	await RenderingServer.frame_post_draw

	var composed_frame := root.get_texture().get_image()
	if composed_frame == null or composed_frame.get_width() < WIDTH * 2 or composed_frame.get_height() < HEIGHT:
		_fail("%s lifetime stress could not capture its composed recovery frame." % backend_name)
		return
	var replaced_frame := composed_frame.get_region(Rect2i(0, 0, WIDTH, HEIGHT))
	var clean_frame := composed_frame.get_region(Rect2i(WIDTH, 0, WIDTH, HEIGHT))
	if _frame_hash(replaced_frame) != _frame_hash(clean_frame):
		_fail("%s document replacement did not match a clean final render after repeated teardown." % backend_name)
		return

	replaced_view.queue_free()
	clean_view.queue_free()
	await process_frame
	await process_frame
	print("HCSR Godot %s document replacement and late-teardown stress passed." % backend_name)
	quit()

func _make_view(document: HTMLDocument) -> HTMLView:
	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.size = Vector2(WIDTH, HEIGHT)
	view.document = document
	return view

func _make_document(label: String, color: String) -> HTMLDocument:
	var document := HTMLDocument.new()
	document.html = """<!DOCTYPE html><html><head><style>
*{box-sizing:border-box}html,body{margin:0;width:100%;height:100%;overflow:hidden;background:__COLOR__;color:white;font-family:Arial;font-size:20px}
main{width:100%;height:100%;padding:20px}h1{margin:0 0 16px;font-size:28px}.panel{height:72px;padding:22px;background:rgba(15,23,42,.75)}
</style></head><body><main><h1>Lifetime recovery</h1><div class="panel">__LABEL__</div></main></body></html>""".replace("__COLOR__", color).replace("__LABEL__", label)
	return document

func _wait_for_generation(view: HTMLView, generation_before_change: int) -> bool:
	var deadline := Time.get_ticks_msec() + 3000
	while Time.get_ticks_msec() < deadline:
		await process_frame
		if view.get_generation() > generation_before_change:
			return true
	return false

func _frame_hash(frame: Image) -> PackedByteArray:
	if frame.get_format() != Image.FORMAT_RGBA8:
		frame.convert(Image.FORMAT_RGBA8)
	var hashing := HashingContext.new()
	hashing.start(HashingContext.HASH_SHA256)
	hashing.update(frame.get_data())
	return hashing.finish()

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

extends SceneTree

const WIDTH := 240
const HEIGHT := 130
const BACKGROUND := Color("f1f5f9")

var backend_preference := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"

func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--cpu"):
		backend_preference = HTMLView.BACKEND_CPU
		backend_name = "CPU"
	elif OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	elif OS.get_cmdline_user_args().has("--metal"):
		backend_preference = HTMLView.BACKEND_METAL
		backend_name = "Metal"

	root.size = Vector2i(WIDTH, HEIGHT)
	root.gui_embed_subwindows = true
	var view := _make_view()
	root.add_child(view)
	await _settle(6)

	var initial := await _capture()
	if initial == null:
		return
	if not _pixel_is_close(initial.get_pixel(30, 75), Color8(100, 40, 40)):
		_fail("%s did not paint the authored range track color." % backend_name)
		return
	if not _pixel_is_close(initial.get_pixel(128, 75), Color8(200, 80, 80)):
		_fail("%s did not paint the authored range thumb color independently." % backend_name)
		return

	var regions := [
		Rect2i(16, 16, 26, 26),
		Rect2i(56, 16, 26, 26),
		Rect2i(16, 62, 180, 30),
	]
	for region in regions:
		var baseline_pixels := _count_non_background_pixels(initial, region)
		if baseline_pixels == 0:
			_fail("%s generated control baseline is empty in %s." % [backend_name, region])
			return

		_send_pointer(Vector2(region.position + region.size / 2))
		await _settle(4)
		var hovered := await _capture()
		if hovered == null:
			return
		var hovered_pixels := _count_non_background_pixels(hovered, region)
		if hovered_pixels < baseline_pixels / 2:
			_fail("%s generated control disappeared on hover in %s: baseline=%d hovered=%d." % [backend_name, region, baseline_pixels, hovered_pixels])
			return

	_send_pointer(Vector2(225, 115))
	await _settle(4)
	var restored := await _capture()
	if restored == null:
		return
	if restored.get_data() != initial.get_data():
		_fail("%s generated controls did not return to the exact pre-hover frame." % backend_name)
		return

	print("HCSR Godot %s generated-control hover smoke passed." % backend_name)
	quit()

func _make_view() -> HTMLView:
	var document := HTMLDocument.new()
	document.html = """<!DOCTYPE html><html><head><style>
html,body{margin:0;width:100%%;height:100%%;background:#f1f5f9}
input{position:absolute}input:hover{border-color:#2563eb}
#checkbox{left:20px;top:20px}#radio{left:60px;top:20px}
#range{left:20px;top:65px;width:170px;height:20px}
#range::-webkit-slider-runnable-track{background:rgb(100,40,40);height:6px;border-radius:3px}
#range::-webkit-slider-thumb{background:rgb(200,80,80);width:18px;height:18px;border-radius:9px}
#range:hover::-webkit-slider-thumb{background:rgb(255,255,255)}
</style></head><body><input id='checkbox' type='checkbox'><input id='radio' type='radio'><input id='range' type='range' value='65'></body></html>"""
	var result := HTMLView.new()
	result.backend_preference = backend_preference
	result.size = Vector2(WIDTH, HEIGHT)
	result.document = document
	return result

func _send_pointer(position: Vector2) -> void:
	var motion := InputEventMouseMotion.new()
	motion.position = position
	motion.global_position = position
	root.push_input(motion, true)

func _settle(frame_count: int) -> void:
	for _frame in range(frame_count):
		await process_frame
		RenderingServer.force_draw(true)
	await RenderingServer.frame_post_draw

func _capture() -> Image:
	var image := root.get_texture().get_image()
	if image == null or image.get_width() < WIDTH or image.get_height() < HEIGHT:
		_fail("%s could not read the composed viewport." % backend_name)
		return null
	return image

func _count_non_background_pixels(image: Image, region: Rect2i) -> int:
	var count := 0
	for y in range(region.position.y, region.end.y):
		for x in range(region.position.x, region.end.x):
			var pixel := image.get_pixel(x, y)
			if abs(pixel.r - BACKGROUND.r) > 0.02 or abs(pixel.g - BACKGROUND.g) > 0.02 or abs(pixel.b - BACKGROUND.b) > 0.02:
				count += 1
	return count

func _pixel_is_close(actual: Color, expected: Color) -> bool:
	return abs(actual.r - expected.r) < 0.025 and abs(actual.g - expected.g) < 0.025 and abs(actual.b - expected.b) < 0.025

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

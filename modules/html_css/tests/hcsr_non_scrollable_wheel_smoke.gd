extends SceneTree

const WIDTH := 640
const HEIGHT := 360

func _initialize() -> void:
	DisplayServer.window_set_size(Vector2i(WIDTH, HEIGHT))
	root.size = Vector2i(WIDTH, HEIGHT)

	var background := ColorRect.new()
	background.color = Color("245f73")
	background.size = Vector2(WIDTH, HEIGHT)
	root.add_child(background)

	var document := HTMLDocument.new()
	document.html = """<!DOCTYPE html><html><head><style>
html,body{margin:0;width:100vw;height:100vh;overflow:hidden;background:transparent}
.glass{position:absolute;left:80px;top:60px;width:480px;height:240px;background:rgba(12,20,42,.25);border:4px solid white;border-radius:24px;backdrop-filter:invert(1)}
</style></head><body><div class='glass'></div></body></html>"""
	document.background_color = Color(0, 0, 0, 0)

	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_D3D12
	view.size = Vector2(WIDTH, HEIGHT)
	view.backdrop_filter_enabled = true
	view.document = document
	root.add_child(view)

	for _frame in range(8):
		await process_frame
	var reference := root.get_texture().get_image()
	if reference == null:
		_fail("Non-scrollable wheel smoke could not capture its reference frame.")
		return

	_send_wheel(Vector2(WIDTH / 2.0, HEIGHT / 2.0))
	for frame_index in range(4):
		await process_frame
		var candidate := root.get_texture().get_image()
		var changed_pixels := _count_changed_pixels(reference, candidate)
		if changed_pixels != 0:
			_fail("Wheel input exposed a translated frame for a non-scrollable document at follow-up frame %d (%d changed pixels)." % [frame_index, changed_pixels])
			return

	print("HCSR non-scrollable GPU wheel smoke passed.")
	quit()

func _send_wheel(position: Vector2) -> void:
	var wheel := InputEventMouseButton.new()
	wheel.button_index = MOUSE_BUTTON_WHEEL_DOWN
	wheel.factor = 1.0
	wheel.position = position
	wheel.global_position = position
	wheel.pressed = true
	root.push_input(wheel, true)

func _count_changed_pixels(left: Image, right: Image) -> int:
	if left == null or right == null or left.get_size() != right.get_size():
		return -1
	var changed_pixels := 0
	for y in range(HEIGHT):
		for x in range(WIDTH):
			if left.get_pixel(x, y) != right.get_pixel(x, y):
				changed_pixels += 1
	return changed_pixels

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

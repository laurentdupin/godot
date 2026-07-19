extends SceneTree

const WIDTH := 1280
const HEIGHT := 720
const CARD_COUNT := 18

func _initialize() -> void:
	DisplayServer.window_set_size(Vector2i(WIDTH, HEIGHT))
	root.size = Vector2i(WIDTH, HEIGHT)
	var document := HTMLDocument.new()
	document.html = _build_document()
	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_D3D12
	view.size = Vector2(WIDTH, HEIGHT)
	view.document = document
	root.add_child(view)
	for _frame in range(6):
		await process_frame

	_send_wheel(Vector2(960, 500), MOUSE_BUTTON_WHEEL_DOWN, 14.0)
	var bottom_reference := await _wait_for_settled_frame()
	if bottom_reference == null:
		_fail("D3D12 shared-surface stress could not capture its reference frame.")
		return

	_send_wheel(Vector2(960, 500), MOUSE_BUTTON_WHEEL_UP, 14.0)
	var top_reference := await _wait_for_settled_frame()
	if top_reference == null:
		_fail("D3D12 shared-surface stress could not capture its previous committed frame.")
		return
	if _count_changed_pixels(top_reference, bottom_reference) == 0:
		_fail("D3D12 shared-surface stress endpoints did not produce distinct completed frames.")
		return

	for iteration in range(13):
		var button := MOUSE_BUTTON_WHEEL_DOWN if iteration % 2 == 0 else MOUSE_BUTTON_WHEEL_UP
		_send_wheel(Vector2(960, 500), button, 14.0)
		await process_frame
	await RenderingServer.frame_post_draw
	var immediate_frame := root.get_texture().get_image()
	if immediate_frame == null or immediate_frame.get_size() != bottom_reference.get_size() or not _frame_has_complete_surface(immediate_frame):
		_fail("D3D12 shared-surface stress could not capture its stressed frame.")
		return

	var completed_frame := await _wait_for_settled_frame()
	var changed_pixels := _count_changed_pixels(completed_frame, bottom_reference) if completed_frame != null else -1
	if changed_pixels != 0:
		_fail("D3D12 shared presentation did not converge to the final completed scroll commit; %d pixels differ." % changed_pixels)
		return

	print("HCSR Godot D3D12 shared-surface retained-scroll stress passed.")
	quit()

func _build_document() -> String:
	var cards := ""
	for index in range(CARD_COUNT):
		cards += "<article><h3>Depth model %02d</h3><p>Author and description text for retained glyph and background coverage.</p><div class='facts'><span>Speed</span><span>Depth</span><span>Detail</span><span>VRAM</span></div><button>Install model %02d</button></article>" % [index, index]
	return """<!DOCTYPE html><html><head><style>
*{box-sizing:border-box}html,body{margin:0;width:100vw;height:100vh;overflow:hidden;background:#e2e8f0;color:#f8fafc;font-family:Arial;font-size:18px}
main{position:absolute;inset:0;display:flex;flex-direction:column;min-height:0;background:#334155;padding:24px}
header{height:72px;flex-shrink:0;background:#475569;padding:20px;font-size:30px}
.columns{flex:1;min-height:0;display:flex;gap:20px;padding-top:20px}.column{width:50%;min-height:0;display:flex;flex-direction:column}
h2{height:54px;flex-shrink:0;margin:0;background:#475569;padding:12px}.scroll{flex:1;min-height:0;overflow-y:auto;background:#0f172a;padding:8px}
article{height:270px;margin-bottom:10px;background:#1e293b;padding:16px}h3{margin:0 0 20px;font-size:25px}p{height:52px;margin:0 0 16px;color:#cbd5e1}
.facts{display:flex;justify-content:space-between;margin-bottom:18px;color:#94a3b8}button{display:block;width:100%;height:64px;border:0;background:#3b82f6;color:white;font-size:22px}
</style></head><body><main><header>Fixed retained header</header><div class='columns'><section class='column'><h2>Installed</h2><div class='scroll'>__CARDS__</div></section><section class='column'><h2>For installation</h2><div class='scroll'>__CARDS__</div></section></div></main></body></html>""".replace("__CARDS__", cards)

func _send_wheel(position: Vector2, button: MouseButton, factor: float) -> void:
	var wheel := InputEventMouseButton.new()
	wheel.button_index = button
	wheel.factor = factor
	wheel.position = position
	wheel.global_position = position
	wheel.pressed = true
	root.push_input(wheel, true)

func _wait_for_settled_frame() -> Image:
	var deadline := Time.get_ticks_msec() + 250
	var frame: Image
	while Time.get_ticks_msec() < deadline:
		await process_frame
		await RenderingServer.frame_post_draw
		frame = root.get_texture().get_image()
		if frame == null or not _frame_has_complete_surface(frame):
			return null
	return frame

func _frame_has_complete_surface(frame: Image) -> bool:
	if frame.get_width() < WIDTH or frame.get_height() < HEIGHT:
		return false
	for y in range(HEIGHT):
		var black_run := 0
		for x in range(WIDTH):
			var color := frame.get_pixel(x, y)
			if color.r < 0.02 and color.g < 0.02 and color.b < 0.02:
				black_run += 1
				if black_run >= 40:
					return false
			else:
				black_run = 0
	return true

func _count_changed_pixels(left: Image, right: Image) -> int:
	if left.get_size() != right.get_size():
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

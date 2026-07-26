extends SceneTree

const WIDTH := 640
const HEIGHT := 360
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

	var generation_before_bottom := view.get_generation()
	var scroll_position := Vector2(WIDTH * 0.75, HEIGHT * 0.7)
	_send_wheel(scroll_position, MOUSE_BUTTON_WHEEL_DOWN, 14.0)
	var bottom_reference := await _wait_for_settled_frame(view, generation_before_bottom)
	if bottom_reference == null:
		_fail("D3D12 shared-surface stress could not capture its reference frame.")
		return

	var generation_before_top := view.get_generation()
	_send_wheel(scroll_position, MOUSE_BUTTON_WHEEL_UP, 14.0)
	var top_reference := await _wait_for_settled_frame(view, generation_before_top)
	if top_reference == null:
		_fail("D3D12 shared-surface stress could not capture its previous committed frame.")
		return
	var top_hash := _frame_hash(top_reference)
	var bottom_hash := _frame_hash(bottom_reference)
	if top_hash == bottom_hash:
		_fail("D3D12 shared-surface stress endpoints did not produce distinct completed frames.")
		return

	var generation_before_stress := view.get_generation()
	for iteration in range(13):
		var button := MOUSE_BUTTON_WHEEL_DOWN if iteration % 2 == 0 else MOUSE_BUTTON_WHEEL_UP
		_send_wheel(scroll_position, button, 14.0)
		await process_frame
	await RenderingServer.frame_post_draw
	var immediate_frame := _capture_frame()
	if immediate_frame == null or immediate_frame.get_size() != bottom_reference.get_size():
		_fail("D3D12 shared-surface stress could not capture its stressed frame.")
		return
	if not _frame_has_complete_surface(immediate_frame):
		_fail("D3D12 shared-surface stress exposed an incomplete shared surface.")
		return

	var completed_frame := await _wait_for_settled_frame(view, generation_before_stress)
	if completed_frame == null or _frame_hash(completed_frame) != bottom_hash:
		_fail("D3D12 shared presentation did not converge to the final completed scroll commit.")
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

func _wait_for_settled_frame(view: HTMLView, generation_before_change: int) -> Image:
	var deadline := Time.get_ticks_msec() + 2000
	var last_generation := 0
	var stable_generation_count := 0
	while Time.get_ticks_msec() < deadline:
		await process_frame
		var generation := view.get_generation()
		if generation <= generation_before_change:
			stable_generation_count = 0
			continue
		if generation == last_generation:
			stable_generation_count += 1
			if stable_generation_count >= 3:
				await RenderingServer.frame_post_draw
				var frame := _capture_frame()
				if frame != null:
					return frame
		else:
			last_generation = generation
			stable_generation_count = 0
	return null

func _capture_frame() -> Image:
	var frame := root.get_texture().get_image()
	if frame == null or frame.get_width() < WIDTH or frame.get_height() < HEIGHT:
		return null
	if frame.get_size() == Vector2i(WIDTH, HEIGHT):
		return frame
	return frame.get_region(Rect2i(0, 0, WIDTH, HEIGHT))

func _frame_hash(frame: Image) -> PackedByteArray:
	if frame.get_format() != Image.FORMAT_RGBA8:
		frame.convert(Image.FORMAT_RGBA8)
	var hashing := HashingContext.new()
	hashing.start(HashingContext.HASH_SHA256)
	hashing.update(frame.get_data())
	return hashing.finish()

func _frame_has_complete_surface(frame: Image) -> bool:
	if frame.get_size() != Vector2i(WIDTH, HEIGHT):
		return false
	if frame.get_format() != Image.FORMAT_RGBA8:
		frame.convert(Image.FORMAT_RGBA8)
	var pixels := frame.get_data()
	for y in range(HEIGHT):
		var black_run := 0
		var row_offset := y * WIDTH * 4
		for x in range(WIDTH):
			var pixel_offset := row_offset + x * 4
			if pixels[pixel_offset] < 5 and pixels[pixel_offset + 1] < 5 and pixels[pixel_offset + 2] < 5:
				black_run += 1
				if black_run >= 40:
					return false
			else:
				black_run = 0
	return true

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

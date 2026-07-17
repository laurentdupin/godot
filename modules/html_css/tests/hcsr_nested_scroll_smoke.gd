extends SceneTree

const WIDTH := 400
const HEIGHT := 180
const HTML := """<!DOCTYPE html><html><head><style>
*{box-sizing:border-box}html,body{margin:0;width:100vw;height:100vh;overflow:hidden;background:#e2e8f0;color:#1e293b}
main{position:absolute;inset:0;display:flex;flex-direction:column;min-height:0;background:#cbd5e1}
header{height:40px;flex-shrink:0;background:#94a3b8}
.columns{flex:1;min-height:0;display:flex;flex-direction:row}
section{width:50%;min-height:0;display:flex;flex-direction:column;background:#dbeafe}
section+section{background:#dcfce7}
h2{height:40px;flex-shrink:0;margin:0;background:#bfdbfe}
section+section h2{background:#bbf7d0}
.scroll{flex:1;min-height:0;overflow-y:auto;background:#f8fafc}
button{display:block;margin:0;width:100%;height:100px;border:0;background:#f1f5f9;color:#1e293b}
button+button{background:#e0f2fe}
</style></head><body><main><header>Fixed header</header><div class="columns">
<section><h2>Left title</h2><div class="scroll">
<button data-godot-action="left-first">Left first</button><button data-godot-action="left-second">Left second</button><button data-godot-action="left-third">Left third</button>
</div></section><section><h2>Right title</h2><div class="scroll">
<button data-godot-action="right-first">Right first</button><button data-godot-action="right-second">Right second</button><button data-godot-action="right-third">Right third</button>
</div></section></div></main></body></html>"""

var actions: Array[StringName] = []

func _initialize() -> void:
	var use_d3d12 := OS.get_cmdline_user_args().has("--d3d12")
	var use_vulkan := OS.get_cmdline_user_args().has("--vulkan")
	var backend := HTMLView.BACKEND_D3D12 if use_d3d12 else (HTMLView.BACKEND_VULKAN if use_vulkan else HTMLView.BACKEND_CPU)
	var backend_name := "D3D12" if use_d3d12 else ("Vulkan" if use_vulkan else "CPU")
	var document := HTMLDocument.new()
	document.html = HTML
	var view := HTMLView.new()
	view.backend_preference = backend
	view.size = Vector2(WIDTH, HEIGHT)
	view.document = document
	view.action_requested.connect(func(action: StringName, _payload: Dictionary) -> void: actions.append(action))
	root.size = Vector2i(WIDTH, HEIGHT)
	root.add_child(view)
	for _frame in range(4):
		await process_frame

	for _step in range(3):
		_send_wheel(Vector2(10, 100), MOUSE_BUTTON_WHEEL_DOWN)
		await process_frame
		await RenderingServer.frame_post_draw
		if not _frame_has_complete_surface():
			_fail("%s nested-scroll frame exposed cleared/black retained tiles." % backend_name)
			return
	var animation_deadline := Time.get_ticks_msec() + 250
	while Time.get_ticks_msec() < animation_deadline:
		await process_frame
		await RenderingServer.frame_post_draw
		if not _frame_has_complete_surface():
			_fail("%s nested-scroll animation exposed cleared/black retained tiles." % backend_name)
			return

	_send_click(Vector2(10, 100))
	await process_frame
	_send_click(Vector2(210, 100))
	await process_frame
	if actions != [&"left-second", &"right-first"]:
		_fail("%s nested scrolling did not isolate the hovered left flex scroller; actions=%s" % [backend_name, actions])
		return

	print("HCSR Godot %s independent flex nested-scroll smoke passed." % backend_name)
	quit()

func _frame_has_complete_surface() -> bool:
	var image := root.get_texture().get_image()
	if image == null or image.get_width() < WIDTH or image.get_height() < HEIGHT:
		return false
	for y in range(HEIGHT):
		var black_run := 0
		for x in range(WIDTH):
			var color := image.get_pixel(x, y)
			if color.r < 0.02 and color.g < 0.02 and color.b < 0.02:
				black_run += 1
				if black_run >= 40:
					return false
			else:
				black_run = 0
	return true

func _send_wheel(position: Vector2, button: MouseButton) -> void:
	var wheel := InputEventMouseButton.new()
	wheel.button_index = button
	wheel.position = position
	wheel.global_position = position
	wheel.pressed = true
	root.push_input(wheel, true)

func _send_click(position: Vector2) -> void:
	var down := InputEventMouseButton.new()
	down.button_index = MOUSE_BUTTON_LEFT
	down.position = position
	down.global_position = position
	down.pressed = true
	root.push_input(down, true)
	var up := InputEventMouseButton.new()
	up.button_index = MOUSE_BUTTON_LEFT
	up.position = position
	up.global_position = position
	up.pressed = false
	root.push_input(up, true)

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

extends SceneTree

const HTML := """<!DOCTYPE html><html><body style="margin:0;overflow:hidden">
<div style="position:absolute;left:0;top:0;width:180px;height:100px;overflow-y:auto">
<button style="display:block;margin:0;width:100%;height:100px" data-godot-action="left-first">Left first</button>
<button style="display:block;margin:0;width:100%;height:100px" data-godot-action="left-second">Left second</button>
</div>
<div style="position:absolute;left:200px;top:0;width:180px;height:100px;overflow-y:auto">
<button style="display:block;margin:0;width:100%;height:100px" data-godot-action="right-first">Right first</button>
<button style="display:block;margin:0;width:100%;height:100px" data-godot-action="right-second">Right second</button>
</div></body></html>"""

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
	view.size = Vector2(400, 120)
	view.document = document
	view.action_requested.connect(func(action: StringName, _payload: Dictionary) -> void: actions.append(action))
	root.size = Vector2i(400, 120)
	root.add_child(view)
	for _frame in range(4):
		await process_frame

	for _step in range(3):
		_send_wheel(Vector2(10, 20), MOUSE_BUTTON_WHEEL_DOWN)
		await process_frame
	for _frame in range(3):
		await process_frame

	_send_click(Vector2(10, 20))
	await process_frame
	_send_click(Vector2(210, 20))
	await process_frame
	if actions != [&"left-second", &"right-first"]:
		_fail("%s nested scrolling did not isolate the hovered left scroller; actions=%s" % [backend_name, actions])
		return

	print("HCSR Godot %s independent nested-scroll smoke passed." % backend_name)
	quit()

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

extends SceneTree

const WIDTH := 240
const HEIGHT := 140
const HTML := """<!DOCTYPE html><html><head><style>
html,body{margin:0;width:100vw;height:100vh;overflow:hidden}
.scroll{width:200px;height:100px;overflow-y:scroll}
button{display:block;width:180px;height:100px;margin:0;border:0}
</style></head><body><div class="scroll"><button data-godot-action="first">First</button><button data-godot-action="second">Second</button><button data-godot-action="third">Third</button></div></body></html>"""

var actions: Array[StringName] = []

func _initialize() -> void:
	var use_d3d12 := OS.get_cmdline_user_args().has("--d3d12")
	var use_vulkan := OS.get_cmdline_user_args().has("--vulkan")
	var use_metal := OS.get_cmdline_user_args().has("--metal")
	var backend := HTMLView.BACKEND_D3D12 if use_d3d12 else (HTMLView.BACKEND_VULKAN if use_vulkan else (HTMLView.BACKEND_METAL if use_metal else HTMLView.BACKEND_CPU))
	var backend_name := "D3D12" if use_d3d12 else ("Vulkan" if use_vulkan else ("Metal" if use_metal else "CPU"))
	var document := HTMLDocument.new()
	document.html = HTML
	var view := HTMLView.new()
	view.backend_preference = backend
	view.size = Vector2(WIDTH, HEIGHT)
	view.document = document
	view.action_requested.connect(func(action: StringName, _payload: Dictionary) -> void: actions.append(action))
	root.size = Vector2i(WIDTH, HEIGHT)
	root.add_child(view)
	for _frame in range(6):
		await process_frame

	_send_drag(Vector2(194, 16), Vector2(194, 88))
	for _frame in range(4):
		await process_frame
	_send_click(Vector2(60, 50))
	await process_frame
	if actions != [&"third"]:
		_fail("%s scrollbar thumb drag did not expose the final item; actions=%s" % [backend_name, actions])
		return

	print("HCSR Godot %s scrollbar drag smoke passed." % backend_name)
	quit()

func _send_drag(start: Vector2, finish: Vector2) -> void:
	var down := InputEventMouseButton.new()
	down.button_index = MOUSE_BUTTON_LEFT
	down.position = start
	down.global_position = start
	down.pressed = true
	root.push_input(down, true)
	var motion := InputEventMouseMotion.new()
	motion.position = finish
	motion.global_position = finish
	motion.button_mask = MOUSE_BUTTON_MASK_LEFT
	root.push_input(motion, true)
	var up := InputEventMouseButton.new()
	up.button_index = MOUSE_BUTTON_LEFT
	up.position = finish
	up.global_position = finish
	up.pressed = false
	root.push_input(up, true)

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

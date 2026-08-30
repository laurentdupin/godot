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
	call_deferred("_run")


func _run() -> void:
	var use_vulkan := OS.get_cmdline_user_args().has("--vulkan")
	var use_metal := OS.get_cmdline_user_args().has("--metal")
	var backend := HTMLView.BACKEND_VULKAN if use_vulkan else (
		HTMLView.BACKEND_METAL if use_metal else HTMLView.BACKEND_CPU)
	var backend_name := "Vulkan" if use_vulkan else ("Metal" if use_metal else "CPU")
	var document := HTMLDocument.new()
	document.html = HTML
	var view := HTMLView.new()
	view.backend_preference = backend
	view.size = Vector2(WIDTH, HEIGHT)
	view.document = document
	view.action_requested.connect(
		func(action: StringName, _payload: Dictionary) -> void: actions.append(action))
	root.size = Vector2i(WIDTH, HEIGHT)
	root.add_child(view)
	for _frame in range(8):
		await process_frame

	for _step in range(3):
		_send_pan(Vector2(10, 100), Vector2(0, 2))
		await process_frame
		await RenderingServer.frame_post_draw

	_send_click(Vector2(10, 100))
	await process_frame
	_send_click(Vector2(210, 100))
	await process_frame
	if actions != [&"left-second", &"right-first"]:
		_fail("%s pan gesture did not isolate and scroll the hovered left region; actions=%s" % [
			backend_name, actions])
		return

	print("HCSR Godot %s pan-gesture nested-scroll smoke passed." % backend_name)
	quit(0)


func _send_pan(position: Vector2, delta: Vector2) -> void:
	var pan := InputEventPanGesture.new()
	pan.position = position
	pan.delta = delta
	root.push_input(pan, true)


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

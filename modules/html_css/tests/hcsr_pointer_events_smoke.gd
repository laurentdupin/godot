extends SceneTree

var actions: Array[StringName] = []
var activated_generation := 0

func _initialize() -> void:
	var document := HTMLDocument.new()
	document.html = """<!DOCTYPE html><html><head><style>
		body { margin: 0; }
		#back { position: absolute; z-index: 10; left: 20px; top: 20px; width: 80px; height: 80px; }
		.cover { position: absolute; z-index: 20; left: 0; top: 0; width: 220px; height: 120px; }
		.pass-through { pointer-events: none; }
		.blocking { pointer-events: auto; }
		#dead-child { position: absolute; left: 20px; top: 20px; width: 80px; height: 80px; }
		#override { pointer-events: auto; position: absolute; left: 120px; top: 20px; width: 80px; height: 80px; }
	</style></head><body>
		<button id='back' data-godot-action='back'><span>Back</span></button>
		<header id='cover' class='cover pass-through'>
			<div id='dead-child'></div>
			<button id='override' data-godot-action='override'>Override</button>
		</header>
	</body></html>"""
	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_CPU
	view.size = Vector2(240, 140)
	view.document = document
	view.action_requested.connect(func(action: StringName, _payload: Dictionary) -> void: actions.append(action))
	view.frame_activated.connect(func(generation: int) -> void: activated_generation = generation)
	root.add_child(view)
	if not await _wait_for_activation_after(0):
		return

	_send_click(Vector2(60, 60))
	await process_frame
	if actions != [&"back"]:
		_fail("pointer-events:none cover did not pass through to the Back action: %s" % [actions])
		return

	actions.clear()
	_send_click(Vector2(160, 60))
	await process_frame
	if actions != [&"override"]:
		_fail("Explicit pointer-events:auto descendant was not targetable: %s" % [actions])
		return
	for _frame in range(4):
		await process_frame

	if view.set_element_attribute(&"cover", &"class", "cover blocking") != OK:
		_fail("Could not mutate the cover to pointer-events:auto")
		return
	var previous_generation := activated_generation
	if not await _wait_for_activation_after(previous_generation):
		return
	actions.clear()
	_send_click(Vector2(60, 60))
	await process_frame
	if not actions.is_empty():
		_fail("Retained pointer-events:auto mutation still activated the covered Back button: %s" % [actions])
		return
	for _frame in range(4):
		await process_frame

	if view.set_element_attribute(&"cover", &"class", "cover pass-through") != OK:
		_fail("Could not restore the cover to pointer-events:none")
		return
	previous_generation = activated_generation
	if not await _wait_for_activation_after(previous_generation):
		return
	actions.clear()
	_send_click(Vector2(60, 60))
	await process_frame
	if actions != [&"back"]:
		_fail("Restoring pointer-events:none left stale retained targeting: %s" % [actions])
		return

	print("HCSR HTMLView pointer-events smoke passed.")
	quit()

func _wait_for_activation_after(generation: int) -> bool:
	for _frame in range(120):
		await process_frame
		if activated_generation > generation:
			return true
	_fail("HCSR pointer-events smoke timed out waiting for activation after generation %d." % generation)
	return false

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

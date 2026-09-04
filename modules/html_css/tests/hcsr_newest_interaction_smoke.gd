extends SceneTree

var actions: Array[StringName] = []

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var document := HTMLDocument.new()
	document.html = """<!doctype html><html><head><style>
		html,body{margin:0;width:100%;height:100%}
		#action{position:absolute;left:10px;top:10px;width:120px;height:40px}
		#action-label{display:block;width:100%;height:100%}
		#action:active #action-label{transform:translateX(100px)}
		#choice{position:absolute;left:10px;top:70px;width:120px;height:30px}
		#host{position:absolute;left:10px;top:110px;width:120px;height:35px}
		#next{width:120px;height:35px}
	</style></head><body>
		<button id='action' data-godot-action='activate:action'><span id='action-label' data-godot-action='activate:action'>Activate</span></button>
		<select id='choice' data-godot-action='form:choice'><option value='a'>Alpha</option><option value='b'>Beta</option></select>
		<div id='host'></div>
	</body></html>"""
	document.resource_root = "res://"
	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_CPU
	view.size = Vector2(180, 160)
	view.document = document
	view.action_requested.connect(func(action: StringName, _payload: Dictionary) -> void: actions.append(action))
	root.add_child(view)
	for _frame in range(8):
		await process_frame

	_send_button(Vector2(70, 30), true)
	for _frame in range(3):
		await process_frame
	_send_button(Vector2(70, 30), false)
	await process_frame
	if actions != [&"activate:action"]:
		_fail("Nested button content did not resolve to its host action: %s" % [actions])
		return
	for click in range(9):
		_send_button(Vector2(70, 30), true)
		for _frame in range(1 + click % 3):
			await process_frame
		_send_button(Vector2(70, 30), false)
		await process_frame
	if actions.count(&"activate:action") != 10:
		_fail("Repeated press/release cycles lost button actions: %s" % [actions])
		return

	var before: Dictionary = view.get_form_control_state(&"choice")
	if before.get("value", "") != "a" or before.get("selected_index", -1) != 0:
		_fail("Initial select state did not cross the newest ABI: %s" % before)
		return
	_send_click(Vector2(70, 85))
	for _frame in range(4):
		await process_frame
	_send_click(Vector2(70, 145))
	for _frame in range(2):
		await process_frame
	var after: Dictionary = view.get_form_control_state(&"choice")
	if after.get("value", "") != "b" or after.get("selected_index", -1) != 1:
		_fail("Select activation did not update the host-visible state: %s" % after)
		return
	if view.set_element_inner_html(&"host", "<button id='next' data-godot-action='navigate:next'><span>Next</span></button>") != OK:
		_fail("The newest backend rejected a structural page mutation.")
		return
	for _frame in range(4):
		await process_frame
	_send_click(Vector2(70, 127))
	await process_frame
	# The authored select action is dispatched for the click that opens the popup
	# and for the option click that commits the new value.
	if actions.count(&"activate:action") != 10 or actions.count(&"form:choice") != 2 or actions.back() != &"navigate:next":
		_fail("An action inserted by a DeepDesktop-style page rebuild was not interactive: %s" % [actions])
		return
	print("HCSR newest interaction and dropdown smoke passed.")
	quit(0)

func _send_click(position: Vector2) -> void:
	_send_button(position, true)
	_send_button(position, false)

func _send_button(position: Vector2, pressed: bool) -> void:
	var down := InputEventMouseButton.new()
	down.button_index = MOUSE_BUTTON_LEFT
	down.position = position
	down.global_position = position
	down.pressed = pressed
	root.push_input(down, true)

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

extends SceneTree

var phases: Array[StringName] = []
var actions: Array[StringName] = []

func _initialize() -> void:
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><body style='margin:0'><button id='hold' data-godot-action='delete-model' style='width:200px;height:80px'>Hold</button></body></html>"
	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_CPU
	view.size = Vector2(320, 160)
	view.document = document
	view.element_pointer_event.connect(_on_pointer_event)
	view.action_requested.connect(func(action: StringName, _payload: Dictionary) -> void: actions.append(action))
	root.add_child(view)
	await process_frame
	await process_frame

	_send_button(true)
	await process_frame
	view.cancel_pointer_interaction()
	await process_frame
	if phases != [&"down", &"cancel"] or not actions.is_empty():
		_fail("Cancelled hold emitted unexpected phases/actions: %s / %s" % [phases, actions])
		return

	phases.clear()
	_send_button(true)
	await process_frame
	_send_button(false)
	await process_frame
	if phases != [&"down", &"up"] or actions != [&"delete-model"]:
		_fail("Completed click emitted unexpected phases/actions: %s / %s" % [phases, actions])
		return

	print("HCSR HTMLView pointer-phase smoke passed.")
	quit()

func _send_button(pressed: bool) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = MOUSE_BUTTON_LEFT
	event.position = Vector2(40, 40)
	event.global_position = event.position
	event.pressed = pressed
	root.push_input(event, true)

func _on_pointer_event(phase: StringName, element_id: StringName, action: StringName, button: int, payload: Dictionary) -> void:
	if element_id != &"hold" or action != &"delete-model" or button != MOUSE_BUTTON_LEFT or payload.get("phase") != phase:
		_fail("Pointer phase payload lost authored element/action/button metadata.")
		return
	phases.append(phase)

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

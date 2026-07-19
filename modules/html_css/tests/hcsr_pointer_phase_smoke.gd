extends SceneTree

var phases: Array[StringName] = []
var slider_phases: Array[StringName] = []
var actions: Array[StringName] = []

func _initialize() -> void:
	var use_d3d12 := OS.get_cmdline_user_args().has("--d3d12")
	var use_vulkan := OS.get_cmdline_user_args().has("--vulkan")
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><body style='margin:0'><button id='hold' data-godot-action='delete-model' style='width:200px;height:80px'><span id='hold-label'>Hold</span></button><input id='slider' type='range' style='display:block;width:200px;height:32px;margin-top:16px'></body></html>"
	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_D3D12 if use_d3d12 else (HTMLView.BACKEND_VULKAN if use_vulkan else HTMLView.BACKEND_CPU)
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
	if phases != [&"enter", &"down", &"cancel"] or not actions.is_empty():
		_fail("Cancelled hold emitted unexpected phases/actions: %s / %s" % [phases, actions])
		return

	phases.clear()
	_send_button(true)
	await process_frame
	_send_button(false)
	await process_frame
	if phases != [&"down", &"up", &"click"] or actions != [&"delete-model"]:
		_fail("Completed click emitted unexpected phases/actions: %s / %s" % [phases, actions])
		return

	var range_before_crossing := view.get_form_control_state("slider")
	_send_pointer(Vector2(280, 40), true)
	await process_frame
	_send_motion(Vector2(180, 112), MOUSE_BUTTON_MASK_LEFT)
	await process_frame
	var range_after_crossing := view.get_form_control_state("slider")
	_send_pointer(Vector2(280, 40), false)
	await process_frame
	if range_after_crossing.get("focused") != false or range_after_crossing.get("value") != range_before_crossing.get("value"):
		_fail("A range acquired focus or changed value when the primary press began outside it: before=%s after=%s" % [range_before_crossing, range_after_crossing])
		return

	slider_phases.clear()
	_send_pointer(Vector2(100, 112), true)
	await process_frame
	view.cancel_pointer_interaction()
	await process_frame
	if slider_phases != [&"enter", &"down", &"capture", &"cancel", &"capture_loss"]:
		_fail("Range pointer capture emitted unexpected phases: %s" % [slider_phases])
		return

	print("HCSR HTMLView pointer-phase smoke passed.")
	quit()

func _send_button(pressed: bool) -> void:
	_send_pointer(Vector2(100, 40), pressed)

func _send_pointer(position: Vector2, pressed: bool) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = MOUSE_BUTTON_LEFT
	event.position = position
	event.global_position = event.position
	event.pressed = pressed
	root.push_input(event, true)

func _send_motion(position: Vector2, button_mask: int) -> void:
	var event := InputEventMouseMotion.new()
	event.position = position
	event.global_position = position
	event.button_mask = button_mask
	root.push_input(event, true)

func _on_pointer_event(phase: StringName, element_id: StringName, action: StringName, button: int, payload: Dictionary) -> void:
	if element_id.is_empty():
		return
	if element_id == &"slider":
		if action != &"slider" or payload.get("action_element_id") != &"slider":
			_fail("Range pointer capture lost its native actionable metadata: %s" % [payload])
			return
		slider_phases.append(phase)
		return
	var expected_button := MOUSE_BUTTON_LEFT if phase in [&"down", &"up", &"cancel", &"click"] else MOUSE_BUTTON_NONE
	if element_id != &"hold" or action != &"delete-model" or button != expected_button or payload.get("phase") != phase or payload.get("action_element_id") != &"hold":
		_fail("Pointer phase payload lost authored element/action/button metadata: phase=%s id=%s action=%s button=%s payload=%s" % [phase, element_id, action, button, payload])
		return
	phases.append(phase)

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

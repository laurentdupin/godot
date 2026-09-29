extends SceneTree

var pointer_phases: Array[StringName] = []
var pointer_ids: Array[StringName] = []


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var document := HTMLDocument.new()
	document.html = """<html><head><style>
		html,body{margin:0;width:100%;height:100%}
		#target{position:absolute;left:50px;top:35px;width:100px;height:70px}
	</style></head><body><button id='target' data-godot-action='hover-target'>Hover</button></body></html>"""
	var view := HTMLView.new()
	var arguments := OS.get_cmdline_user_args()
	view.backend_preference = HTMLView.BACKEND_VULKAN if arguments.has("--vulkan") \
			else HTMLView.BACKEND_D3D12
	view.size = Vector2(220, 140)
	view.logical_size = Vector2i(220, 140)
	view.document = document
	view.element_pointer_event.connect(_on_pointer_event)
	root.add_child(view)
	for _frame in range(12):
		await process_frame
	var motion := InputEventMouseMotion.new()
	motion.position = Vector2(100, 70)
	motion.global_position = motion.position
	view.dispatch_input_event(motion)
	await process_frame
	view.notification(Control.NOTIFICATION_MOUSE_EXIT_SELF)
	await process_frame
	if not pointer_phases.has(&"enter") or not pointer_phases.has(&"leave"):
		_fail("Pure mouse motion did not emit host enter/leave phases: %s" % [pointer_phases])
		return
	if not pointer_ids.has(&"target"):
		_fail("Hover event did not preserve the hit element id: %s" % [pointer_ids])
		return
	print("HCSR_NEWEST_HOST_HOVER_OK phases=%s ids=%s" % [pointer_phases, pointer_ids])
	quit(0)


func _on_pointer_event(phase: StringName, element_id: StringName,
		action: StringName, _button: int, _payload: Dictionary) -> void:
	if action == &"hover-target":
		pointer_phases.append(phase)
		pointer_ids.append(element_id)


func _fail(message: String) -> void:
	push_error(message)
	quit(1)

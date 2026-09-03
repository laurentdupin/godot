extends SceneTree

const OUTPUT_SIZE := Vector2i(1280, 720)
const LOGICAL_SIZE := Vector2i(2560, 1440)

var actions: Array[StringName] = []
var forwarding_container: ForwardingContainer

class ForwardingContainer extends SubViewportContainer:
	var output_size := Vector2(OUTPUT_SIZE)
	var last_position := Vector2.ZERO

	func _propagate_input_event(event: InputEvent) -> bool:
		if event is not InputEventMouse:
			return true
		var forwarded := event.duplicate()
		# The SubViewport remains in presentation pixels. Godot's Control transform
		# converts this to the HTMLView's larger logical coordinate space once.
		var mapped: Vector2 = event.position
		forwarded.position = mapped
		forwarded.global_position = mapped
		if forwarded is InputEventMouseMotion:
			forwarded.relative = mapped - last_position
		last_position = mapped
		for child: Node in get_children():
			if child is SubViewport:
				(child as SubViewport).push_input(forwarded, true)
		return false


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	root.size = OUTPUT_SIZE
	root.content_scale_mode = Window.CONTENT_SCALE_MODE_DISABLED

	var container := ForwardingContainer.new()
	forwarding_container = container
	container.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	container.mouse_filter = Control.MOUSE_FILTER_STOP
	root.add_child(container)

	var viewport := SubViewport.new()
	viewport.size = OUTPUT_SIZE
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	container.add_child(viewport)
	viewport.notification(Node.NOTIFICATION_VP_MOUSE_ENTER)

	var document := HTMLDocument.new()
	document.html = """<!doctype html><html><head><style>
		html,body{margin:0;width:100%;height:100%}
		.cards{position:absolute;left:11.8%;right:11.8%;top:29.85%;bottom:29.85%;display:flex;gap:2.05vw}
		.slot{flex:0 0 23%;position:relative}
		.slot:hover{z-index:2}.slot:hover article{transform:translateY(-4px) scale(1.035)}
		.slot:active article{transform:translateY(-1px) scale(1.015)}
		article{position:absolute;inset:0;display:flex;transition:transform .18s ease-out}
		button{width:100%;flex:1;border:0}.copy{pointer-events:none}
	</style></head><body><section class='cards'>
		<div class='slot'><article><button data-godot-action='select-mode:first'><span class='copy'>First</span></button></article></div>
		<div class='slot'><article><button data-godot-action='select-mode:second'><span class='copy'>Second</span></button></article></div>
		<div class='slot'><article><button data-godot-action='select-mode:third'><span class='copy'>Third</span></button></article></div>
		<div class='slot'><article><button data-godot-action='select-mode:fourth'><span class='copy'>Fourth</span></button></article></div>
	</section></body></html>"""
	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_VULKAN if OS.get_cmdline_user_args().has("--vulkan") else HTMLView.BACKEND_D3D12
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_CONTROL_PHYSICAL_ADJUSTED
	view.logical_size = LOGICAL_SIZE
	view.size = LOGICAL_SIZE
	view.scale = Vector2(0.5, 0.5)
	view.document = document
	view.action_requested.connect(func(action: StringName, _payload: Dictionary) -> void: actions.append(action))
	viewport.add_child(view)
	for _frame in range(10):
		await process_frame

	# Center of the fourth card in output/window coordinates.
	var click := Vector2(1040, 360)
	for index in range(10):
		_send_motion(click)
		_send_button(click, true)
		for _frame in range(1 + index % 3):
			await process_frame
		_send_button(click, false)
		await process_frame
	if actions.size() != 10 or actions.any(func(action: StringName) -> bool: return action != &"select-mode:fourth"):
		_fail("Scaled SubViewport input lost or misrouted actions: %s" % [actions])
		return
	print("HCSR newest scaled SubViewport click smoke passed.")
	# Let both the HTML renderer and Godot retire their last shared-texture work
	# before tearing down the Vulkan device.
	for _frame in range(6):
		await process_frame
	quit(0)


func _send_motion(position: Vector2) -> void:
	var motion := InputEventMouseMotion.new()
	motion.position = position
	motion.global_position = position
	forwarding_container._propagate_input_event(motion)


func _send_button(position: Vector2, pressed: bool) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = MOUSE_BUTTON_LEFT
	event.position = position
	event.global_position = position
	event.pressed = pressed
	forwarding_container._propagate_input_event(event)


func _fail(message: String) -> void:
	push_error(message)
	quit(1)

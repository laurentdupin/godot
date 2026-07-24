extends SceneTree

var backend_preference := HTMLView.BACKEND_CPU
var backend_name := "CPU"


func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--d3d12"):
		backend_preference = HTMLView.BACKEND_D3D12
		backend_name = "D3D12"
	elif OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	elif OS.get_cmdline_user_args().has("--metal"):
		backend_preference = HTMLView.BACKEND_METAL
		backend_name = "Metal"

	var retained := _create_view(false)
	var clean := _create_view(true)
	for _frame in range(12):
		await process_frame

	var retained_view: HTMLView = retained["view"]
	var clean_view: HTMLView = clean["view"]
	_push_click(retained["viewport"], Vector2(30, 24))
	for _frame in range(3):
		await process_frame
	_push_click(retained["viewport"], Vector2(30, 98))
	if retained_view.set_form_control_value(&"choice", "alternate") != OK:
		push_error("The retained select rejected its alternate value.")
		quit(1)
		return
	var state: Dictionary = retained_view.get_form_control_state(&"choice")
	if state.get("selected_index", -1) != 1 or state.get("value", "") != "alternate":
		push_error("The retained select did not expose its committed alternate state.")
		quit(1)
		return

	# Host form synchronization is deliberately idempotent. Repeating the
	# already-committed value must not create an unbounded GPU frame backlog or
	# leave the selected generation behind older prepared frames.
	for _frame in range(120):
		if retained_view.set_form_control_value(&"choice", "alternate") != OK:
			push_error("The retained select rejected an idempotent synchronized value.")
			quit(1)
			return
		await process_frame
	var retained_image: Image = retained_view.get_texture().get_image()
	var clean_image: Image = clean_view.get_texture().get_image()
	if retained_image.get_data() != clean_image.get_data():
		push_error("The %s HTMLView retained the previous collapsed select label after its value changed." % backend_name)
		quit(1)
		return

	print("HCSR collapsed-select value repaint smoke passed on %s." % backend_name)
	quit()


func _create_view(selected: bool) -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(220, 160)
	viewport.disable_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(viewport)
	var selected_attribute := " selected" if selected else ""
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><head><style>html,body{margin:0;background:#18212b;color:#edf2f7;font:16px sans-serif}select{margin:8px;width:180px;height:36px}</style></head><body><select id='choice'><option value='default'>Default GPU</option><option value='alternate'%s>Alternate model</option></select></body></html>" % selected_attribute
	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.fixed_viewport_size = Vector2i(220, 160)
	view.size = Vector2(220, 160)
	view.document = document
	viewport.add_child(view)
	return { "viewport": viewport, "view": view }


func _push_click(viewport: SubViewport, position: Vector2) -> void:
	for pressed in [true, false]:
		var event := InputEventMouseButton.new()
		event.position = position
		event.global_position = position
		event.button_index = MOUSE_BUTTON_LEFT
		event.pressed = pressed
		viewport.push_input(event, true)

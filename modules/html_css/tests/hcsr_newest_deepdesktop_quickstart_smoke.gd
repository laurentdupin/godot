extends SceneTree

var actions: Array[StringName] = []

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var fixture_root := ProjectSettings.globalize_path("res://../../../thirdparty/hcsr_newest/Examples/DeepDesktopQuickStart")
	var html_path := fixture_root.path_join("DeepDesktopQuickStart.html")
	if not FileAccess.file_exists(html_path):
		_fail("DeepDesktopQuickStart fixture was not found at %s" % html_path)
		return
	var document := HTMLDocument.new()
	var links := RegEx.new()
	links.compile("(?i)<link\\b[^>]*>")
	document.html = links.sub(FileAccess.get_file_as_string(html_path), "", true)
	document.css = FileAccess.get_file_as_string(fixture_root.path_join("DeepDesktopFonts.css")) + "\n" \
			+ FileAccess.get_file_as_string(fixture_root.path_join("DeepDesktop.css"))
	var view := HTMLView.new()
	var arguments := OS.get_cmdline_user_args()
	view.backend_preference = HTMLView.BACKEND_VULKAN if arguments.has("--vulkan") else \
			(HTMLView.BACKEND_D3D12 if arguments.has("--d3d12") else HTMLView.BACKEND_CPU)
	view.size = Vector2(1280, 720)
	view.logical_size = Vector2i(1280, 720)
	view.document = document
	view.action_requested.connect(func(action: StringName, _payload: Dictionary) -> void: actions.append(action))
	root.add_child(view)
	for _frame in range(10):
		await process_frame

	# The five-row GPU menu opens below its control. Its second row overlaps
	# page content and must still win hit testing as a scene-owned popup.
	_send_click(Vector2(950, 138))
	for _frame in range(3): await process_frame
	if actions.has(&"form:quick-gpu"):
		_fail("Opening the QuickStart GPU dropdown incorrectly emitted a committed form action.")
		return
	_send_click(Vector2(950, 229))
	for _frame in range(3): await process_frame
	var gpu: Dictionary = view.get_form_control_state(&"quick-gpu")
	if gpu.get("value", "") != "1" or gpu.get("selected_index", -1) != 1 \
			or actions.count(&"form:quick-gpu") != 1:
		_fail("The QuickStart GPU dropdown did not commit option 1: state=%s actions=%s" % [gpu, actions])
		return

	# The eleven-row model menu is viewport-clamped. The last visible row must
	# remain interactive even though it is far from the collapsed select.
	_send_click(Vector2(820, 267))
	for _frame in range(3): await process_frame
	if actions.has(&"form:quick-model"):
		_fail("Opening the QuickStart model dropdown incorrectly emitted a committed form action.")
		return
	_send_click(Vector2(820, 691))
	for _frame in range(3): await process_frame
	var model: Dictionary = view.get_form_control_state(&"quick-model")
	if model.get("value", "") != "10" or model.get("selected_index", -1) != 10 \
			or actions.count(&"form:quick-model") != 1:
		_fail("The QuickStart model dropdown did not commit its last option: state=%s actions=%s" % [model, actions])
		return

	print("HCSR newest DeepDesktopQuickStart dropdown smoke passed.")
	quit(0)

func _send_click(position: Vector2) -> void:
	for pressed in [true, false]:
		var event := InputEventMouseButton.new()
		event.button_index = MOUSE_BUTTON_LEFT
		event.position = position
		event.global_position = position
		event.pressed = pressed
		root.push_input(event, true)

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

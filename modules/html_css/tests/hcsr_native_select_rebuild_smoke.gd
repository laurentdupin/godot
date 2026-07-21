extends SceneTree

const WIDTH := 1152
const HEIGHT := 680

var backend_preference := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"
var test_viewport: SubViewport

func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	elif OS.get_cmdline_user_args().has("--metal"):
		backend_preference = HTMLView.BACKEND_METAL
		backend_name = "Metal"
	elif OS.get_cmdline_user_args().has("--cpu"):
		backend_preference = HTMLView.BACKEND_CPU
		backend_name = "CPU"

	var fixture_root := "res://../../../thirdparty/hcsr/Examples/DeepDesktopOptions"
	var css := FileAccess.get_file_as_string("res://../../../thirdparty/hcsr/Examples/DeepDesktopModels/DeepDesktop.css")
	var initial_fragment := FileAccess.get_file_as_string(fixture_root + "/OptionsLocalized.fragment.html")
	var localized_fragment := FileAccess.get_file_as_string(fixture_root + "/OptionsInitial.fragment.html")
	if css.is_empty() or initial_fragment.is_empty() or localized_fragment.is_empty():
		_fail("Could not load the native-select rebuild fixtures.")
		return

	var container := SubViewportContainer.new()
	container.size = Vector2(WIDTH * 2, HEIGHT)
	container.stretch = true
	root.add_child(container)
	test_viewport = SubViewport.new()
	test_viewport.size = Vector2i(WIDTH * 2, HEIGHT)
	test_viewport.disable_3d = true
	test_viewport.transparent_bg = true
	test_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	container.add_child(test_viewport)
	var rebuilt := _make_view(css, initial_fragment, "en")
	test_viewport.add_child(rebuilt)
	for _frame in range(5):
		await process_frame

	_click(Vector2(340, 112))
	for _frame in range(8):
		await process_frame
		RenderingServer.force_draw(false)
	await RenderingServer.frame_post_draw
	_click(Vector2(250, 342))
	if rebuilt.set_element_attribute(&"document-body", &"class", "runtime") != OK \
			or rebuilt.set_element_attribute(&"document-body", &"lang", "ja") != OK \
			or rebuilt.set_element_inner_html(&"app", localized_fragment) != OK:
		_fail("%s native-select rebuild mutation was rejected." % backend_name)
		return
	if rebuilt.set_form_control_value(&"control-77007424029", "7") != OK:
		_fail("%s native-select rebuild form synchronization was rejected." % backend_name)
		return
	for _frame in range(12):
		await process_frame
		RenderingServer.force_draw(false)
	await RenderingServer.frame_post_draw

	var rebuilt_raw := rebuilt.get_texture().get_image()
	var fresh := _make_view(css, localized_fragment, "ja")
	fresh.position = Vector2(WIDTH, 0)
	test_viewport.add_child(fresh)
	for _frame in range(5):
		await process_frame
		RenderingServer.force_draw(false)
	await RenderingServer.frame_post_draw
	var fresh_raw := fresh.get_texture().get_image()
	var changed := _count_image_differences(rebuilt_raw, fresh_raw)
	if changed != 0:
		_fail("%s native-select rebuild retained %d stale pixels." % [backend_name, changed])
		return

	print("HCSR Godot %s native-select rebuild smoke passed." % backend_name)
	quit()

func _make_view(css: String, fragment: String, language: String) -> HTMLView:
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><head><style>%s</style><style>.option-button:hover{opacity:1}</style></head><body id='document-body' class='runtime' lang='%s'><div id='app'>%s</div></body></html>" % [css, language, fragment]
	document.resource_root = "res://../../../thirdparty/hcsr/Examples/DeepDesktopModels"
	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.fixed_viewport_size = Vector2i(roundi(float(WIDTH) * 1440.0 / float(HEIGHT)), 1440)
	view.size = Vector2(WIDTH, HEIGHT)
	view.document = document
	return view

func _click(position: Vector2) -> void:
	var event := InputEventMouseButton.new()
	event.position = position
	event.global_position = position
	event.button_index = MOUSE_BUTTON_LEFT
	event.pressed = true
	root.push_input(event, true)
	event = event.duplicate()
	event.pressed = false
	root.push_input(event, true)

func _count_image_differences(first: Image, second: Image) -> int:
	if first == null or second == null or first.get_size() != second.get_size():
		return WIDTH * HEIGHT
	var changed := 0
	for y in range(first.get_height()):
		for x in range(first.get_width()):
			if first.get_pixel(x, y) != second.get_pixel(x, y):
				changed += 1
	return changed

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

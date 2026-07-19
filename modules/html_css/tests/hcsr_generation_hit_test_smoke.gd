extends SceneTree

const WIDTH := 420
const HEIGHT := 180
const OLD_POSITION := Vector2(70, 70)
const NEW_POSITION := Vector2(310, 70)
const STYLE := "body{margin:0;background:#111827}.target{position:absolute;top:30px;width:100px;height:80px;border:0}.old{left:20px;background:#2563eb}.new{left:260px;background:#dc2626}"

var backend_preference := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"
var actions: Array[StringName] = []

func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	elif OS.get_cmdline_user_args().has("--metal"):
		backend_preference = HTMLView.BACKEND_METAL
		backend_name = "Metal"

	root.size = Vector2i(WIDTH, HEIGHT)
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><head><style>%s</style></head><body><button id='target' class='target old' data-godot-action='old'>Target</button></body></html>" % STYLE
	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.size = Vector2(WIDTH, HEIGHT)
	view.document = document
	view.action_requested.connect(func(action: StringName, _payload: Dictionary) -> void: actions.append(action))
	root.add_child(view)

	for _frame in range(4):
		await process_frame
	RenderingServer.force_draw(false)
	await RenderingServer.frame_post_draw
	if not _click_and_expect(OLD_POSITION, &"old"):
		return

	if view.set_element_attribute(&"target", &"class", "target new") != OK:
		_fail("%s generation hit-test smoke could not queue the geometry mutation." % backend_name)
		return
	if view.set_element_attribute(&"target", &"data-godot-action", "new") != OK:
		_fail("%s generation hit-test smoke could not queue the action mutation." % backend_name)
		return

	var saw_new_generation := false
	for _frame in range(12):
		await process_frame
		RenderingServer.force_draw(false)
		await RenderingServer.frame_post_draw
		var image := root.get_texture().get_image()
		if image == null:
			_fail("%s generation hit-test smoke could not inspect the active presentation." % backend_name)
			return
		var old_pixel := image.get_pixelv(Vector2i(OLD_POSITION))
		var new_pixel := image.get_pixelv(Vector2i(NEW_POSITION))
		var old_visible := old_pixel.b > 0.4 and old_pixel.b > old_pixel.r * 1.3
		var new_visible := new_pixel.r > 0.3 and new_pixel.r > new_pixel.b * 1.3
		if old_visible == new_visible:
			_fail("%s presentation did not expose exactly one complete geometry generation (old=%s, new=%s)." % [backend_name, old_pixel, new_pixel])
			return
		if old_visible:
			if not _click_and_expect(OLD_POSITION, &"old"):
				return
		else:
			saw_new_generation = true
			if not _click_and_expect(NEW_POSITION, &"new"):
				return

	if not saw_new_generation:
		_fail("%s generation hit-test smoke never presented the mutated generation." % backend_name)
		return

	print("HCSR Godot %s generation-bound hit-test smoke passed." % backend_name)
	quit()

func _click_and_expect(position: Vector2, expected_action: StringName) -> bool:
	actions.clear()
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
	if actions != [expected_action]:
		_fail("%s active pixels and hit-test metadata disagreed: expected %s, got %s." % [backend_name, expected_action, actions])
		return false
	return true

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

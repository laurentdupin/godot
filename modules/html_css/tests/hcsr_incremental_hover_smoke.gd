extends SceneTree

const WIDTH := 480
const HEIGHT := 160
const BASE := Color("202020")
const BACK_HOVER := Color("464646")
const VIEW_HOVER := Color("4678a0")

var backend_preference := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"

func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--cpu"):
		backend_preference = HTMLView.BACKEND_CPU
		backend_name = "CPU"
	elif OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	elif OS.get_cmdline_user_args().has("--metal"):
		backend_preference = HTMLView.BACKEND_METAL
		backend_name = "Metal"

	root.size = Vector2i(WIDTH, HEIGHT)
	var view := _make_view()
	root.add_child(view)
	await _settle(6)

	if view.set_element_text(&"status", "updated") != OK:
		_fail("%s text mutation was rejected." % backend_name)
		return
	if view.set_element_attribute(&"status", &"class", "status active") != OK:
		_fail("%s class mutation was rejected." % backend_name)
		return
	if view.set_element_attribute(&"field", &"disabled", "disabled") != OK:
		_fail("%s disabled mutation was rejected." % backend_name)
		return

	# Dispatch immediately: the input boundary must order these queued mutations
	# before it evaluates the unchanged sibling's :hover dependencies.
	_send_pointer(Vector2(40, 35))
	await _settle(5)
	if not await _expect_pixel(Vector2i(40, 35), BACK_HOVER, "back enter"):
		return

	_send_pointer(Vector2(150, 35))
	await _settle(5)
	if not await _expect_pixel(Vector2i(40, 35), BASE, "back leave"):
		return
	if not await _expect_pixel(Vector2i(150, 35), VIEW_HOVER, "view enter"):
		return

	_send_pointer(Vector2(420, 120))
	await _settle(5)
	if not await _expect_pixel(Vector2i(150, 35), BASE, "view leave"):
		return

	print("HCSR Godot %s incremental-mutation hover smoke passed." % backend_name)
	quit()

func _make_view() -> HTMLView:
	var rows := ""
	for index in range(600):
		rows += "<div class='row'><span>row %d</span></div>" % index
	var html := """<!DOCTYPE html><html><head><style>
html,body{margin:0;width:100%%;height:100%%;overflow:hidden;background:#101010}
.toolbar{display:flex;gap:10px;padding:10px}
button{width:100px;height:50px;padding:0;border:0;background:#202020}
.back-button:hover{background:#464646}
.view-button:hover{background:#4678a0}
.content{display:none}.status.active{color:#ffffff}
</style></head><body><div class='toolbar'><button id='back' class='back-button'></button><button id='view' class='view-button'></button><span id='status' class='status'>idle</span><input id='field' value='field'/></div><div class='content'>%s</div></body></html>""" % rows
	var document := HTMLDocument.new()
	document.html = html
	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.size = Vector2(WIDTH, HEIGHT)
	view.document = document
	return view

func _send_pointer(position: Vector2) -> void:
	var motion := InputEventMouseMotion.new()
	motion.position = position
	motion.global_position = position
	root.push_input(motion, true)

func _settle(frame_count: int) -> void:
	for _frame in range(frame_count):
		await process_frame
		RenderingServer.force_draw(false)
	await RenderingServer.frame_post_draw

func _expect_pixel(position: Vector2i, expected: Color, phase: String) -> bool:
	await RenderingServer.frame_post_draw
	var image := root.get_texture().get_image()
	if image == null or image.get_width() <= position.x or image.get_height() <= position.y:
		_fail("%s %s could not read the composed viewport." % [backend_name, phase])
		return false
	var actual := image.get_pixelv(position)
	if abs(actual.r - expected.r) > 0.02 or abs(actual.g - expected.g) > 0.02 or abs(actual.b - expected.b) > 0.02:
		_fail("%s %s published %s at %s instead of %s." % [backend_name, phase, actual, position, expected])
		return false
	return true

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

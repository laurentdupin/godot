extends SceneTree

const WIDTH := 320
const HEIGHT := 160
const STYLE := "body{margin:0;background:#e5e7eb}.back{position:absolute;right:18px;top:16px;width:72px;height:72px;border:2px solid #111827;font-size:34px}.old{background:#111;color:#111}.interim{background:#7f1d1d;color:#fff}.final{background:rgba(50,50,50,.8);color:#fff}.caption{position:absolute;left:20px;top:60px;font-size:20px;color:#111827}"
const INITIAL_BODY := "<button id='back' class='back old'>Old</button><div class='caption'>Initial</div>"
const FINAL_BODY := "<button id='back' class='back final'>X</button><div class='caption'>Replacement</div>"

var backend_preference := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"

func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	elif OS.get_cmdline_user_args().has("--metal"):
		backend_preference = HTMLView.BACKEND_METAL
		backend_name = "Metal"

	root.size = Vector2i(WIDTH * 2, HEIGHT)
	var rebuilt := _make_view(INITIAL_BODY)
	var fresh := _make_view(FINAL_BODY)
	fresh.position = Vector2(WIDTH, 0)
	root.add_child(rebuilt)
	root.add_child(fresh)

	for _frame in range(3):
		await process_frame

	# Queue one steady-state GPU packet, then replace the page on the next game
	# frame while that packet may still be owned by the render thread. The final
	# generation must not require a later pointer event to become visible.
	if rebuilt.set_element_attribute(&"back", &"class", "back interim") != OK:
		_fail("%s follow-up smoke could not queue the interim mutation." % backend_name)
		return
	await process_frame
	if rebuilt.set_body_inner_html(FINAL_BODY) != OK:
		_fail("%s follow-up smoke could not replace the body." % backend_name)
		return

	for _frame in range(8):
		await process_frame
		RenderingServer.force_draw(false)

	var viewport_image := root.get_texture().get_image()
	if viewport_image == null or viewport_image.get_width() < WIDTH * 2 or viewport_image.get_height() < HEIGHT:
		_fail("%s follow-up smoke could not read the composed viewport." % backend_name)
		return

	var changed := _count_different_pixels(viewport_image)
	if changed != 0:
		_fail("%s dropped or partially presented the queued replacement frame; %d pixels differ from a fresh final render." % [backend_name, changed])
		return

	# A stable document must remain internally consistent on every published
	# pool slot, not merely converge to a correct final slot after corruption.
	for sample_index in range(16):
		await process_frame
		RenderingServer.force_draw(false)
		viewport_image = root.get_texture().get_image()
		changed = _count_different_pixels(viewport_image)
		if changed != 0:
			_fail("%s published an inconsistent steady frame at sample %d; %d pixels differ from the clean peer." % [backend_name, sample_index, changed])
			return

	print("HCSR Godot %s queued follow-up frame smoke passed." % backend_name)
	quit()

func _make_view(body: String) -> HTMLView:
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><head><style>%s</style></head><body>%s</body></html>" % [STYLE, body]
	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.size = Vector2(WIDTH, HEIGHT)
	view.document = document
	return view

func _count_different_pixels(image: Image) -> int:
	if image == null or image.get_width() < WIDTH * 2 or image.get_height() < HEIGHT:
		return WIDTH * HEIGHT
	var changed := 0
	for y in range(HEIGHT):
		for x in range(WIDTH):
			if image.get_pixel(x, y) != image.get_pixel(x + WIDTH, y):
				changed += 1
	return changed

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

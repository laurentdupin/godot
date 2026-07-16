extends SceneTree

const WIDTH := 160
const HEIGHT := 80
const HTML := """<!DOCTYPE html><html><head><style>
html,body{margin:0;width:100vw;height:100vh;background:#172554;overflow:hidden}
.card{width:72px;height:36px;margin:12px;background:#f97316;transform:translateX(11px);opacity:.625}
</style></head><body><div class="card"></div></body></html>"""

func _initialize() -> void:
	var backend := HTMLView.BACKEND_VULKAN if OS.get_cmdline_user_args().has("--vulkan") else HTMLView.BACKEND_D3D12
	var backend_name := "Vulkan" if backend == HTMLView.BACKEND_VULKAN else "D3D12"
	root.size = Vector2i(WIDTH * 2, HEIGHT)
	root.add_child(_make_view(HTMLView.BACKEND_CPU, Vector2.ZERO))
	root.add_child(_make_view(backend, Vector2(WIDTH, 0)))

	for _frame in range(8):
		await process_frame
	await RenderingServer.frame_post_draw

	var image := root.get_texture().get_image()
	if image == null or image.get_width() < WIDTH * 2 or image.get_height() < HEIGHT:
		_fail("%s property-tree smoke could not read the composed viewport." % backend_name)
		return

	var changed := 0
	for y in range(HEIGHT):
		for x in range(WIDTH):
			var cpu := image.get_pixel(x, y)
			var gpu := image.get_pixel(x + WIDTH, y)
			if abs(cpu.r8 - gpu.r8) > 1 or abs(cpu.g8 - gpu.g8) > 1 or abs(cpu.b8 - gpu.b8) > 1 or abs(cpu.a8 - gpu.a8) > 1:
				changed += 1

	if changed != 0:
		_fail("%s property-tree presentation differs from CPU in %d pixels." % [backend_name, changed])
		return

	print("HCSR Godot %s transform/effect property-tree smoke passed." % backend_name)
	quit()

func _make_view(backend: int, position: Vector2) -> HTMLView:
	var document := HTMLDocument.new()
	document.html = HTML
	var view := HTMLView.new()
	view.backend_preference = backend
	view.position = position
	view.size = Vector2(WIDTH, HEIGHT)
	view.document = document
	return view

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

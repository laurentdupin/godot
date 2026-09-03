extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var document := HTMLDocument.new()
	document.html = """<!doctype html><html><head><style>
		html,body{margin:0;width:100%;height:100%;background:#222}
		.top{position:absolute;left:7px;top:5px;width:101px;height:17px;background:#f00}
		.bottom{position:absolute;left:19px;top:63px;width:43px;height:29px;background:#0f0}
	</style></head><body><div class='top'></div><div class='bottom'></div></body></html>"""
	document.resource_root = "res://"
	var cpu := HTMLRenderTarget.new()
	root.add_child(cpu)
	cpu.backend_preference = HTMLView.BACKEND_CPU
	cpu.size = Vector2i(120, 100)
	cpu.document = document
	var vulkan := HTMLRenderTarget.new()
	root.add_child(vulkan)
	vulkan.backend_preference = HTMLView.BACKEND_VULKAN
	vulkan.size = Vector2i(120, 100)
	vulkan.document = document
	cpu.render_now()
	vulkan.render_now()
	var cpu_rect := TextureRect.new()
	cpu_rect.position = Vector2(0, 0)
	cpu_rect.size = Vector2(120, 100)
	root.add_child(cpu_rect)
	var gpu_rect := TextureRect.new()
	gpu_rect.position = Vector2(140, 0)
	gpu_rect.size = Vector2(120, 100)
	root.add_child(gpu_rect)
	var canvas: Image
	var cpu_image: Image
	for _frame in range(30):
		await process_frame
		cpu_rect.texture = cpu.get_texture()
		gpu_rect.texture = vulkan.get_texture()
		RenderingServer.force_draw(true)
		await RenderingServer.frame_post_draw
		canvas = root.get_texture().get_image()
		cpu_image = cpu.get_image()
	if canvas == null or canvas.is_empty() or cpu_image == null or cpu_image.is_empty():
		_fail("CPU/Vulkan comparison images were unavailable.")
		return
	var direct := 0.0
	var flipped := 0.0
	var variation := 0.0
	var samples := 0
	for y in range(0, 100, 4):
		for x in range(0, 120, 4):
			var gpu_pixel := canvas.get_pixel(140 + x, y)
			direct += _difference(cpu_image.get_pixel(x, y), gpu_pixel)
			flipped += _difference(cpu_image.get_pixel(x, 99 - y), gpu_pixel)
			variation += abs(cpu_image.get_pixel(x, y).r - cpu_image.get_pixel(0, 0).r)
			samples += 1
	direct /= samples
	flipped /= samples
	variation /= samples
	if variation < 0.01:
		_fail("CPU reference image had no asymmetric scene detail.")
		return
	if direct > 0.03 or direct >= flipped:
		_fail("Vulkan orientation differs from CPU: direct=%.6f flipped=%.6f" % [direct, flipped])
		return
	print("HCSR newest Vulkan orientation smoke passed: direct=%.6f flipped=%.6f" % [direct, flipped])
	quit(0)

func _difference(left: Color, right: Color) -> float:
	return max(abs(left.r - right.r), abs(left.g - right.g), abs(left.b - right.b), abs(left.a - right.a))

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

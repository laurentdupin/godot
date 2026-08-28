extends SceneTree


func _initialize() -> void:
	var use_d3d12 := OS.get_cmdline_user_args().has("--d3d12")
	var use_vulkan := OS.get_cmdline_user_args().has("--vulkan")
	var use_metal := OS.get_cmdline_user_args().has("--metal")
	var use_gpu := use_d3d12 or use_vulkan or use_metal
	var document := HTMLDocument.new()
	document.html = FileAccess.get_file_as_string("res://PackedFont.html")
	document.resource_root = "res://"
	document.background_color = Color("9a1724")

	var target := HTMLRenderTarget.new()
	root.add_child(target)
	target.backend_preference = HTMLView.BACKEND_D3D12 if use_d3d12 else (HTMLView.BACKEND_VULKAN if use_vulkan else (HTMLView.BACKEND_METAL if use_metal else HTMLView.BACKEND_CPU))
	target.size = Vector2i(320, 180)
	target.document = document
	target.render_now()

	var image: Image
	if use_gpu:
		for _frame in range(60):
			if target.get_texture() != null:
				break
			await process_frame
		if target.get_texture() != null:
			image = target.get_texture().get_image()
	else:
		image = target.get_image()
	if image == null or image.get_size() != Vector2i(320, 180):
		push_error("Packed @font-face resource smoke did not produce a frame.")
		target.free()
		quit(1)
		return
	var background := image.get_pixel(300, 160)
	if background.r < 0.45 or background.g > 0.20 or background.b > 0.25:
		push_error("The stylesheet adjacent to the packed font was not loaded through Godot's resource bridge.")
		target.free()
		quit(1)
		return
	var red_advantage := 0.0
	for y in range(10, 120):
		for x in range(0, 120):
			var pixel := image.get_pixel(x, y)
			red_advantage = maxf(red_advantage, pixel.r - maxf(pixel.g, pixel.b))
	if red_advantage < 0.65:
		push_error("The packed @font-face glyph was not rendered from Godot's virtual filesystem.")
		target.free()
		quit(1)
		return

	print("HCSR packed stylesheet and @font-face resource smoke passed.")
	target.free()
	quit()

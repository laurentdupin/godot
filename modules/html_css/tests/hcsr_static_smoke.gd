extends SceneTree

func _initialize() -> void:
	var use_d3d12 := OS.get_cmdline_user_args().has("--d3d12")
	var use_vulkan := OS.get_cmdline_user_args().has("--vulkan")
	var use_gpu := use_d3d12 or use_vulkan
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><head><style>body { margin: 0; background: #123456; color: white; }</style></head><body><p>Hello from static HCSR</p></body></html>"
	document.resource_root = "res://"
	document.background_color = Color("123456")

	var target := HTMLRenderTarget.new()
	target.backend_preference = 4 if use_d3d12 else (3 if use_vulkan else 1)
	target.size = Vector2i(320, 180)
	target.document = document
	target.render_now()
	if use_gpu:
		var texture := target.get_texture()
		if texture == null or texture.get_width() != 320 or texture.get_height() != 180:
			push_error("Static HCSR did not return the expected host-device GPU texture.")
			target.free()
			quit(1)
			return

		var texture_rect := TextureRect.new()
		texture_rect.texture = texture
		texture_rect.size = Vector2(320, 180)
		root.add_child(texture_rect)
		await process_frame
		await RenderingServer.frame_post_draw
		var viewport_image := root.get_texture().get_image()
		var gpu_sample := viewport_image.get_pixel(300, 160)
		if gpu_sample.a < 0.9 or gpu_sample.r < 0.03 or gpu_sample.b < 0.20:
			push_error("Static HCSR GPU texture did not present the expected document color.")
			texture_rect.queue_free()
			target.free()
			quit(1)
			return

		print("Static HCSR Godot %s smoke passed." % ("D3D12" if use_d3d12 else "Vulkan"))
		texture_rect.queue_free()
		target.free()
		quit()
		return

	var image := target.get_image()
	if image == null or image.get_width() != 320 or image.get_height() != 180:
		push_error("Static HCSR did not return the expected Godot image.")
		target.free()
		quit(1)
		return

	var sample := image.get_pixel(300, 160)
	if sample.a < 0.9 or sample.b < 0.20:
		push_error("Static HCSR image did not contain the expected document background.")
		target.free()
		quit(1)
		return

	print("Static HCSR Godot smoke passed.")
	target.free()
	quit()

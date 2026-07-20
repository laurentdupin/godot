extends SceneTree

var actions: Array[StringName] = []

func _initialize() -> void:
	var use_d3d12 := OS.get_cmdline_user_args().has("--d3d12")
	var use_vulkan := OS.get_cmdline_user_args().has("--vulkan")
	var use_metal := OS.get_cmdline_user_args().has("--metal")
	var use_gpu := use_d3d12 or use_vulkan or use_metal
	var backend := HTMLView.BACKEND_D3D12 if use_d3d12 else (HTMLView.BACKEND_VULKAN if use_vulkan else (HTMLView.BACKEND_METAL if use_metal else HTMLView.BACKEND_CPU))
	var document := HTMLDocument.new()
	document.html = """<!DOCTYPE html><html><head><style>
		body { margin: 0; background: white; }
		#topbar { position: absolute; z-index: 100; left: 40px; top: 20px; width: 80px; height: 40px; }
		#dismiss { position: fixed; z-index: 105; inset: 0; margin: 0; padding: 0; background: transparent; border: 0; }
		#popup { position: absolute; z-index: 110; left: 0; top: 100%; width: 80px; height: 40px; margin: 0; }
		#arrow { position: absolute; left: 200px; top: 100px; width: 0; height: 0; border-left: 5px solid transparent; border-right: 5px solid transparent; border-top: 7px solid rgb(180, 180, 180); }
	</style></head><body>
		<div id='topbar'>
			<button id='dismiss' data-godot-action='dismiss'></button>
			<button id='popup' data-godot-action='popup'>Option</button>
		</div>
		<div id='arrow'></div>
	</body></html>"""

	var target := HTMLRenderTarget.new()
	target.backend_preference = backend
	target.size = Vector2i(320, 180)
	target.document = document
	target.render_now()
	var image: Image
	var texture_rect: TextureRect
	if use_gpu:
		var texture := target.get_texture()
		if texture == null:
			_fail("CSS geometry GPU smoke did not produce a host-device texture")
			return
		texture_rect = TextureRect.new()
		texture_rect.texture = texture
		texture_rect.size = Vector2(320, 180)
		root.add_child(texture_rect)
		await process_frame
		await RenderingServer.frame_post_draw
		image = root.get_texture().get_image()
	else:
		image = target.get_image()
	if image == null:
		_fail("CSS geometry smoke did not produce an image")
		return
	var arrow_center := image.get_pixel(205, 100)
	var arrow_outside := image.get_pixel(199, 100)
	if arrow_center.r < 0.55 or arrow_center.r > 0.90 or arrow_outside.r < 0.98:
		_fail("Zero-content asymmetric borders did not paint a CSS triangle: center=%s outside=%s" % [arrow_center, arrow_outside])
		return
	for y in range(70, 96):
		for x in range(125, 190):
			var empty_button_pixel := image.get_pixel(x, y)
			if empty_button_pixel.r < 0.98 or empty_button_pixel.g < 0.98 or empty_button_pixel.b < 0.98:
				_fail("Empty fixed dismiss button painted visible fallback content at %d,%d: %s" % [x, y, empty_button_pixel])
				return
	if texture_rect != null:
		texture_rect.queue_free()
	target.free()
	await process_frame

	var view := HTMLView.new()
	view.backend_preference = backend
	view.size = Vector2(320, 180)
	view.document = document
	view.action_requested.connect(func(action: StringName, _payload: Dictionary) -> void: actions.append(action))
	root.add_child(view)
	await process_frame
	await process_frame

	_send_click(Vector2(250, 140))
	await process_frame
	if actions != [&"dismiss"]:
		_fail("Nested fixed inset dismiss layer did not receive an outside click: %s" % [actions])
		return

	actions.clear()
	_send_click(Vector2(60, 75))
	await process_frame
	if actions != [&"popup"]:
		_fail("Higher z-index popup did not target above its fixed dismiss layer: %s" % [actions])
		return

	print("HCSR CSS border/inset geometry smoke passed (%s)." % ("D3D12" if use_d3d12 else ("Vulkan" if use_vulkan else ("Metal" if use_metal else "CPU"))))
	quit()

func _send_click(position: Vector2) -> void:
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

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

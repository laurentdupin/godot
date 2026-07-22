extends SceneTree

var backend := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"
var last_3d_sample := Color(0, 0, 0, 0)

func _initialize() -> void:
	if "--cpu" in OS.get_cmdline_user_args():
		backend = HTMLView.BACKEND_CPU
		backend_name = "CPU"
	elif "--vulkan" in OS.get_cmdline_user_args():
		backend = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	call_deferred("_run")

func _run() -> void:
	var root := Control.new()
	root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	get_root().add_child(root)

	var view := HTMLView.new()
	view.backend_preference = backend
	view.logical_size = Vector2i(320, 180)
	view.size = Vector2(320, 180)
	view.html = "<html><head><style>html,body{margin:0;background:#123456}.card{position:absolute;left:40px;top:30px;width:80px;height:50px;background:#20c060;border:2px solid #8fe0b0;border-radius:9px}.card:hover{background:#e03030}.label{position:absolute;left:145px;top:42px;color:white;font:20px 'Segoe UI Emoji','Segoe UI',sans-serif}</style></head><body><div id='card' class='card'></div><span class='label'>Output 🎬</span></body></html>"
	root.add_child(view)

	var same_size_output: HTMLViewOutput = view.create_output(Vector2i(320, 180))
	var output: HTMLViewOutput = view.create_output(Vector2i(640, 360))
	if same_size_output == null or not same_size_output.is_valid() or output == null or not output.is_valid():
		_fail("Could not create the secondary output.")
		return
	var stable_texture := output.texture
	if stable_texture == null:
		_fail("Secondary output did not expose its stable texture proxy.")
		return

	if not await _wait_for_generations(view, output, 0, 0):
		return
	if view.get_generation() != output.generation:
		_fail("Automatic and secondary outputs activated different shared generations: %d versus %d." % [view.get_generation(), output.generation])
		return
	if not await _wait_for_matching_generation(view, same_size_output):
		return
	if not _validate_exact_images(view.get_texture(), same_size_output.texture, "initial same-size primary/secondary"):
		return
	if not _validate_frame(view.get_texture(), Vector2i(320, 180), Vector2i(80, 55), Color8(32, 192, 96), "primary initial"):
		return
	if not _validate_frame(output.texture, Vector2i(640, 360), Vector2i(160, 110), Color8(32, 192, 96), "secondary initial"):
		return

	var material_viewport := _create_3d_material_viewport(stable_texture)
	if not await _wait_for_3d_color(material_viewport, func(color: Color) -> bool: return color.b > 0.30 and color.g > 0.15 and color.r < 0.40):
		_fail("%s secondary output was not sampled by a 3D material; center=%s." % [backend_name, last_3d_sample])
		return

	var output_generation := output.generation
	var motion := InputEventMouseMotion.new()
	motion.position = Vector2(80, 55)
	motion.global_position = motion.position
	get_root().push_input(motion, true)
	if not await _wait_for_output_generation(output, output_generation):
		return
	if not _validate_frame(view.get_texture(), Vector2i(320, 180), Vector2i(80, 55), Color8(224, 48, 48), "primary hover"):
		return
	if not _validate_frame(output.texture, Vector2i(640, 360), Vector2i(160, 110), Color8(224, 48, 48), "secondary hover"):
		return
	if not await _wait_for_matching_generation(view, same_size_output):
		return
	if not _validate_exact_images(view.get_texture(), same_size_output.texture, "hover same-size primary/secondary"):
		return

	output.size = Vector2i(960, 540)
	if not await _wait_for_output_size(output, Vector2i(960, 540)):
		return
	if output.texture != stable_texture:
		_fail("Secondary output replaced its Texture2D proxy during resize.")
		return
	if not _validate_frame(output.texture, Vector2i(960, 540), Vector2i(240, 165), Color8(224, 48, 48), "secondary resized hover"):
		return
	if not await _wait_for_3d_color(material_viewport, func(color: Color) -> bool: return color.b > 0.30 and color.g > 0.15 and color.r < 0.40):
		_fail("%s stable secondary Texture2D stopped sampling after resize; center=%s." % [backend_name, last_3d_sample])
		return

	output.release()
	if output.is_valid() or output.texture != null:
		_fail("Released output remained attached to its HTMLView.")
		return
	var owner_destroyed_output := view.create_output(Vector2i(640, 360))
	if owner_destroyed_output == null or not await _wait_for_output_generation(owner_destroyed_output, 0):
		return
	view.queue_free()
	await process_frame
	if owner_destroyed_output.is_valid() or owner_destroyed_output.texture != null:
		_fail("Destroying HTMLView did not detach its secondary outputs.")
		return

	print("HCSR shared logical frame multi-output smoke passed on %s." % backend_name)
	quit(0)

func _wait_for_generations(view: HTMLView, output: HTMLViewOutput, primary_after: int, output_after: int) -> bool:
	for _frame in range(240):
		await process_frame
		if view.get_texture() != null and view.get_texture().get_width() == 320 and output.generation > output_after:
			return true
	_fail("Timed out waiting for primary and secondary output activation.")
	return false

func _wait_for_output_generation(output: HTMLViewOutput, after: int) -> bool:
	for _frame in range(240):
		await process_frame
		if output.generation > after:
			return true
	_fail("Timed out waiting for a secondary output generation after %d." % after)
	return false

func _wait_for_matching_generation(view: HTMLView, output: HTMLViewOutput) -> bool:
	for _frame in range(240):
		await process_frame
		if output.generation == view.get_generation() and output.generation > 0:
			return true
	_fail("Timed out waiting for matching primary and secondary generations: %d versus %d." % [view.get_generation(), output.generation])
	return false

func _wait_for_output_size(output: HTMLViewOutput, expected_size: Vector2i) -> bool:
	for _frame in range(240):
		await process_frame
		if output.texture != null and Vector2i(output.texture.get_width(), output.texture.get_height()) == expected_size:
			return true
	_fail("Timed out waiting for secondary output size %s." % expected_size)
	return false

func _validate_frame(texture: Texture2D, expected_size: Vector2i, sample: Vector2i, expected: Color, phase: String) -> bool:
	if texture == null or Vector2i(texture.get_width(), texture.get_height()) != expected_size:
		_fail("%s texture size mismatch: expected %s, got %s." % [phase, expected_size, Vector2i(texture.get_width(), texture.get_height()) if texture != null else Vector2i()])
		return false
	var image := texture.get_image()
	if image == null or image.is_empty():
		_fail("%s texture could not be captured." % phase)
		return false
	var actual := image.get_pixelv(sample)
	if _maximum_channel_difference(actual, expected) > 0.04:
		_fail("%s sample mismatch: expected %s, got %s." % [phase, expected, actual])
		return false
	return true

func _validate_exact_images(primary_texture: Texture2D, secondary_texture: Texture2D, phase: String) -> bool:
	var primary := primary_texture.get_image() if primary_texture != null else null
	var secondary := secondary_texture.get_image() if secondary_texture != null else null
	if primary == null or secondary == null or primary.is_empty() or secondary.is_empty():
		_fail("%s images could not be captured." % phase)
		return false
	if primary.get_size() != secondary.get_size() or primary.get_data() != secondary.get_data():
		_fail("%s pixels differ despite identical logical and physical metrics." % phase)
		return false
	return true

func _maximum_channel_difference(left: Color, right: Color) -> float:
	return max(abs(left.r - right.r), abs(left.g - right.g), abs(left.b - right.b), abs(left.a - right.a))

func _create_3d_material_viewport(texture: Texture2D) -> SubViewport:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	get_root().add_child(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0, 0, 2)
	camera.current = true
	viewport.add_child(camera)
	var quad := QuadMesh.new()
	quad.size = Vector2(2.0, 1.125)
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.albedo_texture = texture
	var mesh := MeshInstance3D.new()
	mesh.mesh = quad
	mesh.material_override = material
	viewport.add_child(mesh)
	return viewport

func _wait_for_3d_color(viewport: SubViewport, predicate: Callable) -> bool:
	for _frame in range(120):
		await process_frame
		await RenderingServer.frame_post_draw
		var image := viewport.get_texture().get_image()
		if image != null and not image.is_empty():
			last_3d_sample = image.get_pixel(image.get_width() / 2, image.get_height() / 2)
			if predicate.call(last_3d_sample):
				return true
	return false

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

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
	elif "--metal" in OS.get_cmdline_user_args():
		backend = HTMLView.BACKEND_METAL
		backend_name = "Metal"
	call_deferred("_run")

func _run() -> void:
	var root := Control.new()
	root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	get_root().add_child(root)

	var view := HTMLView.new()
	view.backend_preference = backend
	view.logical_size = Vector2i(320, 180)
	view.size = Vector2(320, 180)
	view.html = "<html><head><style>html,body{margin:0;background:transparent}.card{position:absolute;left:40px;top:30px;width:80px;height:50px;background:#20c060;border:2px solid #8fe0b0;border-radius:9px}.card:hover{background:#e03030}.alpha{position:absolute;left:120px;top:65px;width:80px;height:50px;background:rgba(50,50,50,.7)}.label{position:absolute;left:145px;top:42px;color:white;font:20px 'Segoe UI Emoji','Segoe UI',sans-serif}</style></head><body><div id='card' class='card'></div><div class='alpha'></div><span class='label'>Output 🎬</span></body></html>"
	root.add_child(view)

	var same_size_output: HTMLViewOutput = view.create_output(Vector2i(320, 180))
	var output: HTMLViewOutput = view.create_output(Vector2i(640, 360))
	var rounded_output: HTMLViewOutput = view.create_output(Vector2i(853, 480))
	var mipmapped_output: HTMLViewOutput = view.create_output(Vector2i(640, 360), true)
	if same_size_output == null or not same_size_output.is_valid() or output == null or not output.is_valid() or rounded_output == null or not rounded_output.is_valid() or mipmapped_output == null or not mipmapped_output.is_valid() or not mipmapped_output.mipmaps:
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
	if not await _wait_for_output_generation(rounded_output, 0):
		return
	if not _validate_frame(rounded_output.texture, Vector2i(853, 480), Vector2i(213, 147), Color8(32, 192, 96), "rounded-aspect secondary initial"):
		return
	if not await _wait_for_output_generation(mipmapped_output, 0):
		return
	if not _validate_mipmapped_frame(mipmapped_output, Vector2i(640, 360), "mipmapped secondary initial"):
		return
	if not _validate_exact_images(output.texture, mipmapped_output.texture, "initial regular/mipmapped secondary level zero"):
		return
	var stable_mipmapped_texture := mipmapped_output.texture

	var reference_image := output.get_texture().get_image()
	var reference_texture := ImageTexture.create_from_image(reference_image)
	var material_viewport := _create_3d_material_viewport(stable_texture)
	var reference_viewport := _create_3d_material_viewport(reference_texture)
	if not await _wait_for_3d_match(material_viewport, reference_viewport):
		_fail("%s secondary output did not match normal sRGB ImageTexture sampling; secondary=%s." % [backend_name, last_3d_sample])
		return
	if not await _capture_combined_frame_evidence(view, same_size_output, material_viewport):
		return

	var output_generation := output.generation
	var motion := InputEventMouseMotion.new()
	motion.position = Vector2(80, 55)
	motion.global_position = motion.position
	get_root().push_input(motion, true)
	if not await _wait_for_synchronized_generation(view, [same_size_output, output, rounded_output, mipmapped_output], output_generation):
		return
	if not _validate_frame(view.get_texture(), Vector2i(320, 180), Vector2i(80, 55), Color8(224, 48, 48), "primary hover"):
		return
	if not _validate_frame(output.texture, Vector2i(640, 360), Vector2i(160, 110), Color8(224, 48, 48), "secondary hover"):
		return
	if not await _wait_for_output_generation(mipmapped_output, output_generation):
		return
	if not _validate_mipmapped_frame(mipmapped_output, Vector2i(640, 360), "mipmapped secondary hover"):
		return
	if not _validate_exact_images(output.texture, mipmapped_output.texture, "hover regular/mipmapped secondary level zero"):
		return
	if not await _wait_for_matching_generation(view, same_size_output):
		return
	if not _validate_exact_images(view.get_texture(), same_size_output.texture, "hover same-size primary/secondary"):
		return

	for transition in range(8):
		var previous_generation := output.generation
		motion.position = Vector2(250, 150) if transition % 2 == 0 else Vector2(80, 55)
		motion.global_position = motion.position
		get_root().push_input(motion, true)
		if not await _wait_for_synchronized_generation(view, [same_size_output, output, rounded_output, mipmapped_output], previous_generation):
			return
		if not _validate_exact_images(output.texture, mipmapped_output.texture, "retained transition %d regular/mipmapped secondary level zero" % transition):
			return

	output.size = Vector2i(960, 540)
	if not await _wait_for_output_size(output, Vector2i(960, 540)):
		return
	if output.texture != stable_texture:
		_fail("Secondary output replaced its Texture2D proxy during resize.")
		return
	if not _validate_frame(output.texture, Vector2i(960, 540), Vector2i(240, 165), Color8(224, 48, 48), "secondary resized hover"):
		return
	if not await _wait_for_3d_color(material_viewport, func(color: Color) -> bool: return color.a > 0.65 and color.a < 0.75 and color.r > 0.12 and color.r < 0.22):
		_fail("%s stable secondary Texture2D lost its straight-alpha sRGB sample after resize; center=%s." % [backend_name, last_3d_sample])
		return

	mipmapped_output.size = Vector2i(960, 540)
	if not await _wait_for_output_size(mipmapped_output, Vector2i(960, 540)):
		return
	if mipmapped_output.texture != stable_mipmapped_texture or not _validate_mipmapped_frame(mipmapped_output, Vector2i(960, 540), "mipmapped secondary resized"):
		_fail("Mipmapped secondary output did not preserve its stable texture identity and mip chain across resize.")
		return
	if not _validate_exact_images(output.texture, mipmapped_output.texture, "resized regular/mipmapped secondary level zero"):
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

func _wait_for_synchronized_generation(view: HTMLView, outputs: Array, after: int) -> bool:
	for _frame in range(240):
		await process_frame
		var primary_generation := view.get_generation()
		var all_advanced := primary_generation > after
		for output: HTMLViewOutput in outputs:
			if output.generation != primary_generation:
				_fail("A synchronized output group exposed mixed generations during one engine frame: primary=%d, secondary=%d." % [primary_generation, output.generation])
				return false
			all_advanced = all_advanced and output.generation > after
		if all_advanced:
			return true
	_fail("Timed out waiting for a synchronized output group generation after %d." % after)
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
	if primary.get_size() != secondary.get_size():
		_fail("%s image sizes differ." % phase)
		return false
	var level_zero_rect := Rect2i(Vector2i.ZERO, primary.get_size())
	var primary_level_zero := primary.get_region(level_zero_rect)
	var secondary_level_zero := secondary.get_region(level_zero_rect)
	if primary_level_zero.get_data() != secondary_level_zero.get_data():
		var differing_pixels := 0
		var maximum_difference := 0.0
		for y in range(primary.get_height()):
			for x in range(primary.get_width()):
				var difference := _maximum_channel_difference(primary.get_pixel(x, y), secondary.get_pixel(x, y))
				if difference > 0.0:
					differing_pixels += 1
					maximum_difference = max(maximum_difference, difference)
		if differing_pixels > 0:
			_fail("%s pixels differ despite identical logical and physical metrics: %d pixels, maximum channel difference %.6f." % [phase, differing_pixels, maximum_difference])
			return false
	return true

func _validate_mipmapped_frame(output: HTMLViewOutput, expected_size: Vector2i, phase: String) -> bool:
	var texture := output.texture
	var image := texture.get_image() if texture != null else null
	if image == null or image.is_empty() or image.get_size() != expected_size or not image.has_mipmaps():
		_fail("%s did not expose a complete mipmapped texture at %s." % [phase, expected_size])
		return false
	if not _validate_mipmap_alpha_areas(image, phase):
		return false
	return true

func _validate_mipmap_alpha_areas(chain: Image, phase: String) -> bool:
	var previous: Image = _extract_mipmap(chain, 0)
	for level in range(1, chain.get_mipmap_count() + 1):
		var current: Image = _extract_mipmap(chain, level)
		for y in range(current.get_height()):
			var source_top := float(y * previous.get_height()) / current.get_height()
			var source_bottom := float((y + 1) * previous.get_height()) / current.get_height()
			for x in range(current.get_width()):
				var source_left := float(x * previous.get_width()) / current.get_width()
				var source_right := float((x + 1) * previous.get_width()) / current.get_width()
				var weighted_alpha := 0.0
				var total_area := 0.0
				for source_y in range(int(floor(source_top)), int(ceil(source_bottom))):
					var overlap_y: float = maxf(0.0, minf(source_bottom, float(source_y + 1)) - maxf(source_top, float(source_y)))
					for source_x in range(int(floor(source_left)), int(ceil(source_right))):
						var overlap_x: float = maxf(0.0, minf(source_right, float(source_x + 1)) - maxf(source_left, float(source_x)))
						var area: float = overlap_x * overlap_y
						weighted_alpha += previous.get_pixel(source_x, source_y).a * area
						total_area += area
				var expected_alpha := weighted_alpha / total_area if total_area > 0.0 else 0.0
				var actual_alpha := current.get_pixel(x, y).a
				if abs(actual_alpha - expected_alpha) > (2.0 / 255.0):
					_fail("%s mip %d alpha footprint diverged at %s: expected %.6f, got %.6f." % [phase, level, Vector2i(x, y), expected_alpha, actual_alpha])
					return false
		previous = current
	return true

func _extract_mipmap(chain: Image, level: int) -> Image:
	var width: int = maxi(1, chain.get_width() >> level)
	var height: int = maxi(1, chain.get_height() >> level)
	var offset: int = chain.get_mipmap_offset(level)
	var byte_count: int = width * height * 4
	return Image.create_from_data(width, height, false, Image.FORMAT_RGBA8, chain.get_data().slice(offset, offset + byte_count))

func _maximum_channel_difference(left: Color, right: Color) -> float:
	return max(abs(left.r - right.r), abs(left.g - right.g), abs(left.b - right.b), abs(left.a - right.a))

func _create_3d_material_viewport(texture: Texture2D) -> SubViewport:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	viewport.own_world_3d = true
	viewport.transparent_bg = true
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
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.albedo_texture = texture
	material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC
	var mesh := MeshInstance3D.new()
	mesh.mesh = quad
	mesh.material_override = material
	viewport.add_child(mesh)
	return viewport

func _wait_for_3d_match(actual_viewport: SubViewport, reference_viewport: SubViewport) -> bool:
	for _frame in range(120):
		await process_frame
		await RenderingServer.frame_post_draw
		var actual_image := actual_viewport.get_texture().get_image()
		var reference_image := reference_viewport.get_texture().get_image()
		if actual_image != null and reference_image != null and not actual_image.is_empty() and not reference_image.is_empty():
			last_3d_sample = actual_image.get_pixel(actual_image.get_width() / 2, actual_image.get_height() / 2)
			var reference := reference_image.get_pixel(reference_image.get_width() / 2, reference_image.get_height() / 2)
			if _maximum_channel_difference(last_3d_sample, reference) <= 0.02 and last_3d_sample.a > 0.65 and last_3d_sample.a < 0.75:
				return true
	return false

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

func _capture_combined_frame_evidence(view: HTMLView, output: HTMLViewOutput, material_viewport: SubViewport) -> bool:
	for _frame in range(3):
		await process_frame
	await RenderingServer.frame_post_draw
	var generation_before := view.get_generation()
	if generation_before <= 0 or output.generation != generation_before:
		_fail("Combined capture did not begin on one primary/secondary generation.")
		return false
	var raw_primary := view.get_texture().get_image() if view.get_texture() != null else null
	var raw_secondary := output.texture.get_image() if output.texture != null else null
	var canvas_full := get_root().get_texture().get_image()
	var composed_3d := material_viewport.get_texture().get_image()
	var generation_after := view.get_generation()
	if generation_after != generation_before or output.generation != generation_before:
		_fail("A logical generation changed while capturing combined frame evidence.")
		return false
	if raw_primary == null or raw_secondary == null or canvas_full == null or composed_3d == null:
		_fail("One or more combined frame evidence images could not be captured.")
		return false
	if raw_primary.is_empty() or raw_secondary.is_empty() or canvas_full.is_empty() or composed_3d.is_empty():
		_fail("One or more combined frame evidence images were empty.")
		return false
	if raw_primary.get_size() != Vector2i(320, 180) or raw_secondary.get_size() != Vector2i(320, 180):
		_fail("Combined raw primary/secondary evidence used unexpected dimensions.")
		return false
	var canvas := canvas_full.get_region(Rect2i(Vector2i.ZERO, Vector2i(320, 180)))
	var output_directory := ProjectSettings.globalize_path("user://hcsr_combined_frame_evidence/%s" % backend_name.to_lower())
	if DirAccess.make_dir_recursive_absolute(output_directory) != OK:
		_fail("Could not create the combined frame evidence directory.")
		return false
	var paths := {
		"raw_primary": output_directory.path_join("raw-primary.png"),
		"raw_secondary": output_directory.path_join("raw-secondary.png"),
		"canvas": output_directory.path_join("canvas.png"),
		"composed_3d": output_directory.path_join("composed-3d.png"),
	}
	if raw_primary.save_png(paths.raw_primary) != OK \
			or raw_secondary.save_png(paths.raw_secondary) != OK \
			or canvas.save_png(paths.canvas) != OK \
			or composed_3d.save_png(paths.composed_3d) != OK:
		_fail("Could not save combined frame evidence images.")
		return false
	var evidence := {
		"backend": backend_name,
		"logical_generation": generation_before,
		"secondary_generation": output.generation,
		"host_frame_number": view.get_host_frame_number(),
		"timeline_time_seconds": view.get_timeline_time_seconds(),
		"raw_primary_sha256": FileAccess.get_sha256(paths.raw_primary),
		"raw_secondary_sha256": FileAccess.get_sha256(paths.raw_secondary),
		"canvas_sha256": FileAccess.get_sha256(paths.canvas),
		"composed_3d_sha256": FileAccess.get_sha256(paths.composed_3d),
	}
	var evidence_file := FileAccess.open(output_directory.path_join("frame.json"), FileAccess.WRITE)
	if evidence_file == null:
		_fail("Could not write the combined frame evidence record.")
		return false
	evidence_file.store_string(JSON.stringify(evidence, "\t"))
	evidence_file.close()
	print("HCSR_COMBINED_FRAME_EVIDENCE %s" % JSON.stringify(evidence))
	return true

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

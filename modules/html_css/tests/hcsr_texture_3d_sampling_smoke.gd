extends SceneTree

const SIZE := Vector2i(256, 144)
var last_sample := Color(0, 0, 0, 0)
var last_reference_sample := Color(0, 0, 0, 0)

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var use_d3d12 := OS.get_cmdline_user_args().has("--d3d12")
	var use_vulkan := OS.get_cmdline_user_args().has("--vulkan")
	var use_metal := OS.get_cmdline_user_args().has("--metal")
	var backend := HTMLView.BACKEND_D3D12 if use_d3d12 else (HTMLView.BACKEND_VULKAN if use_vulkan else (HTMLView.BACKEND_METAL if use_metal else HTMLView.BACKEND_CPU))
	var backend_name := "D3D12" if use_d3d12 else ("Vulkan" if use_vulkan else ("Metal" if use_metal else "CPU"))

	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><body style='margin:0'><div id='surface' style='position:fixed;inset:0;background:#123456'></div></body></html>"
	var host := Control.new()
	host.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	root.add_child(host)
	var target := HTMLRenderTarget.new()
	host.add_child(target)
	target.backend_preference = backend
	target.size = SIZE
	target.document = document
	target.render_now()

	var texture: Texture2D = null
	for _frame in range(120):
		await process_frame
		texture = target.get_texture()
		if texture != null:
			break
	if texture == null:
		_fail("%s did not expose the HTML presentation as a Texture2D." % backend_name, target)
		return
	for _frame in range(3):
		await process_frame
	await RenderingServer.frame_post_draw

	var reference_image := texture.get_image()
	var reference_texture := ImageTexture.create_from_image(reference_image)
	var viewport := _create_material_viewport(texture)
	var reference_viewport := _create_material_viewport(reference_texture)
	var texture_rid := texture.get_rid()

	if not await _wait_for_match(viewport, reference_viewport):
		_fail("%s Texture2D was not sampled by a 3D material before mutation; center=%s reference=%s." % [backend_name, last_sample, last_reference_sample], target)
		return

	if target.set_element_style(&"surface", "position:fixed;inset:0;background:#c2410c") != OK:
		_fail("%s rejected the 3D texture mutation." % backend_name, target)
		return
	target.render_now()
	var retained_texture: Texture2D = null
	for _frame in range(120):
		await process_frame
		retained_texture = target.get_texture()
		if retained_texture == texture:
			break
	if retained_texture != texture or texture.get_rid() != texture_rid:
		_fail("%s replaced the Texture2D wrapper or its public RID after mutation." % backend_name, target)
		return
	reference_texture.update(texture.get_image())
	if not await _wait_for_match(viewport, reference_viewport):
		_fail("%s did not atomically update the Texture2D sampled by a 3D material; center=%s." % [backend_name, last_sample], target)
		return

	print("HCSR Godot %s Texture2DRD 3D material sampling smoke passed." % backend_name)
	target.free()
	viewport.free()
	reference_viewport.free()
	host.free()
	texture = null
	retained_texture = null
	reference_texture = null
	reference_image = null
	document = null
	await process_frame
	RenderingServer.force_draw(false)
	await RenderingServer.frame_post_draw
	quit()

func _create_material_viewport(texture: Texture2D) -> SubViewport:
	var viewport := SubViewport.new()
	viewport.size = SIZE
	viewport.disable_3d = false
	viewport.own_world_3d = true
	viewport.transparent_bg = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(viewport)
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

func _wait_for_match(viewport: SubViewport, reference_viewport: SubViewport) -> bool:
	for _frame in range(120):
		await process_frame
		RenderingServer.force_draw(false)
		await RenderingServer.frame_post_draw
		var image := viewport.get_texture().get_image()
		var reference := reference_viewport.get_texture().get_image()
		if image != null and reference != null and image.get_size() == SIZE and reference.get_size() == SIZE:
			last_sample = image.get_pixel(SIZE.x / 2, SIZE.y / 2)
			last_reference_sample = reference.get_pixel(SIZE.x / 2, SIZE.y / 2)
			if last_reference_sample.a > 0.9 and _maximum_channel_difference(last_sample, last_reference_sample) <= 0.02:
				return true
	return false

func _maximum_channel_difference(left: Color, right: Color) -> float:
	return max(abs(left.r - right.r), abs(left.g - right.g), abs(left.b - right.b), abs(left.a - right.a))

func _fail(message: String, target: HTMLRenderTarget) -> void:
	push_error(message)
	target.free()
	quit(1)

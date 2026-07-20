extends SceneTree

const SIZE := Vector2i(256, 144)
var last_sample := Color(0, 0, 0, 0)

func _initialize() -> void:
	var use_d3d12 := OS.get_cmdline_user_args().has("--d3d12")
	var use_vulkan := OS.get_cmdline_user_args().has("--vulkan")
	var use_metal := OS.get_cmdline_user_args().has("--metal")
	var backend := HTMLView.BACKEND_D3D12 if use_d3d12 else (HTMLView.BACKEND_VULKAN if use_vulkan else (HTMLView.BACKEND_METAL if use_metal else HTMLView.BACKEND_CPU))
	var backend_name := "D3D12" if use_d3d12 else ("Vulkan" if use_vulkan else ("Metal" if use_metal else "CPU"))

	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><body id='surface' style='margin:0;background:#123456'></body></html>"
	var target := HTMLRenderTarget.new()
	target.backend_preference = backend
	target.size = SIZE
	target.document = document
	target.render_now()

	var texture := target.get_texture()
	if texture == null:
		_fail("%s did not expose the HTML presentation as a Texture2D." % backend_name, target)
		return

	var viewport := SubViewport.new()
	viewport.size = SIZE
	viewport.own_world_3d = true
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
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.albedo_texture = texture
	var texture_rid := texture.get_rid()
	var mesh := MeshInstance3D.new()
	mesh.mesh = quad
	mesh.material_override = material
	viewport.add_child(mesh)

	if not await _wait_for_color(viewport, func(color: Color) -> bool: return color.b > 0.55 and color.g > 0.40 and color.r < 0.40):
		_fail("%s Texture2D was not sampled by a 3D material before mutation; center=%s." % [backend_name, last_sample], target)
		return

	if target.set_element_style(&"surface", "margin:0;background:#c2410c") != OK:
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
	if not await _wait_for_color(viewport, func(color: Color) -> bool: return color.r > 0.80 and color.g > 0.40 and color.b < 0.35):
		_fail("%s did not atomically update the Texture2D sampled by a 3D material; center=%s." % [backend_name, last_sample], target)
		return

	print("HCSR Godot %s Texture2DRD 3D material sampling smoke passed." % backend_name)
	target.free()
	quit()

func _wait_for_color(viewport: SubViewport, predicate: Callable) -> bool:
	for _frame in range(120):
		await process_frame
		await RenderingServer.frame_post_draw
		var image := viewport.get_texture().get_image()
		if image != null and image.get_size() == SIZE:
			last_sample = image.get_pixel(SIZE.x / 2, SIZE.y / 2)
			if predicate.call(last_sample):
				return true
	return false

func _fail(message: String, target: HTMLRenderTarget) -> void:
	push_error(message)
	target.free()
	quit(1)

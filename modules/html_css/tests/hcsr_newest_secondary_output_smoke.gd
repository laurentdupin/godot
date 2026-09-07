extends SceneTree

var failed := false

func _require(condition: bool) -> void:
	if not condition:
		failed = true
		push_error("Secondary output invariant failed")

func _initialize() -> void:
	_run.call_deferred()

func _check_output(output: HTMLViewOutput, view: HTMLView, wide := false) -> void:
	_require(output.is_valid())
	_require(output.generation == view.get_generation())
	_require(output.output_to_logical(output.logical_to_output(Vector2(40, 60))).is_equal_approx(Vector2(40, 60)))
	_require(int(view.get_frame_scheduler_diagnostics()["frame_synchronization"]["failures"]) == 0)
	var image := output.texture.get_image()
	_require(image != null and image.get_size() == output.size)
	# The source occupies its left half. The camera scales that layout to each target.
	_require(image.get_pixel(image.get_width() / 4, image.get_height() / 2).a > .9)
	_require(image.get_pixel(image.get_width()*7/8,image.get_height()/2).a < .1)
	_require((image.get_pixel(image.get_width()*5/8,image.get_height()/2).a > .9) == wide)

func _run() -> void:
	var view := HTMLView.new()
	view.size = Vector2(320, 180)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.logical_size = Vector2i(320,180)
	view.backend_preference = HTMLView.BACKEND_CPU if "--cpu" in OS.get_cmdline_user_args() else HTMLView.BACKEND_GPU_AUTO
	var doc := HTMLDocument.new()
	doc.html = "<style>html,body{margin:0;width:100%;height:100%}#box{background:white;position:absolute;left:0;top:0;width:50%;height:100%}</style><div id='box'></div>"
	view.document = doc
	root.add_child(view)
	var a := view.create_output(Vector2i(640, 360), true)
	var b := view.create_output(Vector2i(160, 90), false)
	_require(a != null and b != null)
	for i in 5:
		await process_frame
		await RenderingServer.frame_post_draw
	_check_output(a, view)
	_check_output(b, view)
	for i in 8:
		await process_frame
		a.size = Vector2i((30+i)*16, (30+i)*9)
		_require(view.set_element_style("box", "background:white;position:absolute;left:0;top:0;width:%d%%;height:100%%;opacity:1" % (75 if i % 2 else 50)) == OK)
		await RenderingServer.frame_post_draw
		_check_output(a, view, i % 2 == 1)
		_check_output(b, view, i % 2 == 1)
		_require(view.logical_size == Vector2i(320,180))
	var transient := view.create_output(Vector2i(160,90), true)
	transient.release()
	b.release()
	await process_frame
	await RenderingServer.frame_post_draw
	_check_output(a, view, true)
	# The same target is usable on two independently viewed 3D quads (SBS).
	view.hide()
	var world := Node3D.new()
	root.add_child(world)
	var camera := Camera3D.new()
	camera.position.z = 4
	world.add_child(camera)
	camera.current = true
	for x in [-1.05, 1.05]:
		var mesh := MeshInstance3D.new()
		var quad := QuadMesh.new()
		quad.size = Vector2(2,1.2)
		mesh.mesh = quad
		mesh.position.x = x
		var material := StandardMaterial3D.new()
		material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		material.albedo_texture = a.texture
		mesh.material_override = material
		world.add_child(mesh)
	for i in 4:
		await process_frame
		await RenderingServer.frame_post_draw
	root.get_texture().get_image().save_png("user://secondary-output-quads.png")
	a.release()
	view.queue_free()
	await process_frame
	if failed:
		quit(1)
		return
	print("SECONDARY_OUTPUT_OK shared_generation=true scaled_layout=true alpha=true resize=true release=true quads=2")
	quit()

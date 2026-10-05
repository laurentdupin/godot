extends SceneTree

func _initialize() -> void:
	run.call_deferred()
	create_timer(30).timeout.connect(func(): quit(2))

func run() -> void:
	var view := HTMLView.new()
	view.size = Vector2(160, 120)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.logical_size = Vector2i(160, 120)
	view.backend_preference = HTMLView.BACKEND_GPU_AUTO
	var document := HTMLDocument.new()
	document.html = "<style>html,body{margin:0}#box{width:80px;height:60px;background:red}</style><div id='box'></div>"
	view.document = document
	root.add_child(view)
	var output := view.create_output(Vector2i(160, 120), false)
	for i in 10:
		await process_frame
		await RenderingServer.frame_post_draw
	view.set_element_attribute("box", "style", "width:90px")
	for i in 30:
		await process_frame
		await RenderingServer.frame_post_draw
	var status: Dictionary = view.get_frame_scheduler_diagnostics().get("frame_synchronization", {})
	if status.get("terminal", true) or status.get("recoverable_render_failures", 0) != 2:
		push_error("Recovery state: " + str(status))
		quit(1)
		return
	if status.get("recorded_generation", 0) < 2 or not status.get("last_render_failure", "").is_empty():
		push_error("Renderer did not recover: " + str(status))
		quit(1)
		return
	var image: Image = output.texture.get_image()
	if image.get_pixel(20, 20).r < 0.9:
		push_error("Recovery lost the scene pixels")
		quit(1)
		return
	print("HCSR_RENDER_RECOVERY_OK failures=2 terminal=false scene_pixels=true")
	quit()

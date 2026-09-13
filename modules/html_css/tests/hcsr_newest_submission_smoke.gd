extends SceneTree

var failed := false

func require(condition: bool, message: String) -> void:
	if not condition:
		failed = true
		push_error("SUBMISSION_FAILED " + message)

func _initialize() -> void:
	run.call_deferred()
	create_timer(30).timeout.connect(func(): quit(2))

func settle() -> void:
	for i in 5:
		await process_frame
		await RenderingServer.frame_post_draw

func run() -> void:
	var cpu := "--cpu" in OS.get_cmdline_user_args()
	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_CPU if cpu else HTMLView.BACKEND_GPU_AUTO
	view.size = Vector2(160, 120)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.logical_size = Vector2i(160, 120)
	var document := HTMLDocument.new()
	document.html = "<style>html,body{margin:0;background:transparent}</style><div id='content'></div>"
	view.document = document
	root.add_child(view)
	var small := view.create_output(Vector2i(160, 120), false)
	var large := view.create_output(Vector2i(320, 240), true)
	for markup in ["", "<div style='position:absolute;left:20px;top:20px;width:80px;height:60px;background:rgba(255,0,0,.5)'></div>", ""]:
		require(view.set_element_inner_html("content", markup) == OK, "mutation accepted")
		await settle()
		var sync: Dictionary = view.get_frame_scheduler_diagnostics().frame_synchronization
		require(not sync.terminal and int(sync.failures) == 0, "frame synchronization: " + str(sync))
		require(sync.render_path == ("cpu_reference" if cpu else "rendering_device"), "one submission path")
		if not cpu:
			require(int(sync.gpu_recordings) > 0, "GPU submission")
		for output in [small, large]:
			var image: Image = output.texture.get_image()
			var scale := image.get_width() / 160.0
			var pixel := image.get_pixel(int(40 * scale), int(40 * scale))
			require(abs(pixel.a - (0.0 if markup.is_empty() else 0.5)) < 0.03, "clear/alpha on each output")
			require(image.get_pixel(0, 0).a < 0.01, "transparent exterior")
			require(output.generation == view.get_generation(), "matching output generation")
		large.size = Vector2i(240, 180)
	small.release()
	large.release()
	view.queue_free()
	await settle()
	print("SUBMISSION_", "FAILED" if failed else "OK", " cpu=", cpu)
	quit(1 if failed else 0)

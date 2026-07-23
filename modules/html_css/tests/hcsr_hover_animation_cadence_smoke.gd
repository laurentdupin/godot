extends SceneTree

var backend := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"

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
	view.html = "<html><head><style>html,body{margin:0;background:#101010}.card{position:absolute;left:40px;top:30px;width:180px;height:100px;background:rgb(50,50,50);border:4px solid rgb(80,80,80);transition:background-color .2s,border-color .2s,transform .2s}.card:hover{background:rgb(110,110,110);border-color:rgb(160,160,160);transform:scale(1.02)}</style></head><body><button class='card'>Hover</button></body></html>"
	root.add_child(view)
	var secondary_output := view.create_output(Vector2i(640, 360))
	print("HCSR cadence fixture waiting for initial frame on %s." % backend_name)
	for _frame in range(240):
		await process_frame
		if view.get_generation() > 0 and view.get_texture() != null:
			break
	if view.get_generation() <= 0:
		push_error("%s cadence fixture did not produce an initial generation." % backend_name)
		quit(1)
		return
	print("HCSR cadence fixture dispatching hover at generation %d." % view.get_generation())
	await process_frame
	var outside_motion := InputEventMouseMotion.new()
	outside_motion.position = Vector2(300, 160)
	outside_motion.global_position = outside_motion.position
	get_root().push_input(outside_motion, true)
	Input.warp_mouse(Vector2(300, 160))
	await process_frame
	var motion := InputEventMouseMotion.new()
	motion.position = Vector2(100, 80)
	motion.global_position = motion.position
	get_root().push_input(motion, true)
	Input.warp_mouse(Vector2(100, 80))
	var generations: Array[int] = []
	var colors: Array[Color] = []
	var secondary_colors: Array[Color] = []
	var started := Time.get_ticks_msec()
	var next_sample := started
	var last_generation := -1
	var observed_frames := 0
	while Time.get_ticks_msec() - started < 350 and observed_frames < 240:
		await process_frame
		observed_frames += 1
		if Time.get_ticks_msec() >= next_sample and view.get_texture() != null:
			colors.append(view.get_texture().get_image().get_pixel(60, 60))
			if secondary_output.texture != null:
				secondary_colors.append(secondary_output.texture.get_image().get_pixel(120, 120))
			next_sample += 20
		if view.get_generation() == last_generation or view.get_texture() == null:
			continue
		last_generation = view.get_generation()
		generations.append(last_generation)
	var distinct_colors := 0
	var previous := Color(-1, -1, -1, -1)
	for color in colors:
		if previous.r < 0.0 or not color.is_equal_approx(previous):
			distinct_colors += 1
			previous = color
	if distinct_colors < 6:
		push_error("%s hover transition published only %d distinct colors: %s" % [backend_name, distinct_colors, colors])
		quit(1)
		return
	var secondary_distinct_colors := 0
	previous = Color(-1, -1, -1, -1)
	for color in secondary_colors:
		if previous.r < 0.0 or not color.is_equal_approx(previous):
			secondary_distinct_colors += 1
			previous = color
	if secondary_distinct_colors < 6:
		push_error("%s secondary-output hover transition published only %d distinct colors: %s" % [backend_name, secondary_distinct_colors, secondary_colors])
		quit(1)
		return
	print("HCSR hover transition cadence passed on %s with %d primary generations and %d secondary colors." % [backend_name, generations.size(), secondary_distinct_colors])
	quit(0)

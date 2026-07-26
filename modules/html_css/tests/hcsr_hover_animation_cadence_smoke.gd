extends SceneTree

const AUTHORED_TRANSITION_MILLISECONDS := 200
const MINIMUM_TRANSITION_GENERATIONS := 6
const MINIMUM_PROCESS_FRAME_COVERAGE := 0.8

var backend := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"

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
	view.html = "<html><head><style>@font-face{font-family:'HCSR Test Emoji';src:url('NotoColorEmoji-X.ttf') format('truetype')}html,body{margin:0;background:#101010}.card{position:absolute;left:40px;top:20px;width:180px;height:140px;display:flex;flex-direction:column;justify-content:center;align-items:center;background:rgb(50,50,50);border:4px solid rgb(80,80,80);transition:background-color .2s,border-color .2s,transform .2s}.card:hover{background:rgb(110,110,110);border-color:rgb(160,160,160);transform:scale(1.02)}.marker{display:flex;justify-content:center;align-items:center;flex:1 1 auto;width:100%;font-family:'HCSR Test Emoji';font-size:72px}.label{flex:0 0 auto}</style></head><body><button class='card'><span class='marker'>❌</span><span class='label'>Hover</span></button></body></html>"
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
	for _frame in range(240):
		await process_frame
		if secondary_output.generation > 0:
			break
	if secondary_output.generation <= 0:
		push_error("%s cadence fixture did not publish its initial secondary output." % backend_name)
		quit(1)
		return
	var initial_marker_image := view.get_texture().get_image()
	if initial_marker_image == null or _maximum_red_advantage(initial_marker_image, Rect2i(75, 20, 110, 105)) < 0.25:
		push_error("%s cadence fixture did not produce the expected static red color glyph." % backend_name)
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
	var transition_process_frames := 0
	while Time.get_ticks_msec() - started < 350 and observed_frames < 240:
		if Time.get_ticks_msec() - started < AUTHORED_TRANSITION_MILLISECONDS:
			transition_process_frames += 1
		var moving_hover := InputEventMouseMotion.new()
		moving_hover.position = Vector2(100 + observed_frames % 8, 80)
		moving_hover.global_position = moving_hover.position
		get_root().push_input(moving_hover, true)
		Input.warp_mouse(moving_hover.position)
		await process_frame
		observed_frames += 1
		if secondary_output.generation != view.get_generation():
			push_error("%s hover enter exposed mixed primary/secondary generations %d/%d." % [backend_name, view.get_generation(), secondary_output.generation])
			quit(1)
			return
		if Time.get_ticks_msec() >= next_sample and view.get_texture() != null:
			var primary_image := view.get_texture().get_image()
			if primary_image != null:
				colors.append(primary_image.get_pixel(60, 130))
			if secondary_output.texture != null and secondary_output.generation > 0:
				var secondary_image := secondary_output.texture.get_image()
				if secondary_image != null:
					secondary_colors.append(secondary_image.get_pixel(120, 260))
			next_sample += 20
		if view.get_generation() == last_generation or view.get_texture() == null:
			continue
		last_generation = view.get_generation()
		generations.append(last_generation)
	var minimum_generation_count := maxi(MINIMUM_TRANSITION_GENERATIONS, ceili(float(transition_process_frames) * MINIMUM_PROCESS_FRAME_COVERAGE))
	if generations.size() < minimum_generation_count:
		push_error("%s hover transition published only %d primary generations for %d process frames during the authored transition; required %d." % [backend_name, generations.size(), transition_process_frames, minimum_generation_count])
		quit(1)
		return
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
	if colors.is_empty() or secondary_colors.is_empty() \
			or _maximum_channel_difference(colors[-1], Color8(110, 110, 110)) > 0.04 \
			or _maximum_channel_difference(secondary_colors[-1], Color8(110, 110, 110)) > 0.04:
		push_error("%s hover transition did not reach its authored primary/secondary enter endpoint." % backend_name)
		quit(1)
		return
	var transformed_primary := view.get_texture().get_image()
	var transformed_secondary := secondary_output.texture.get_image() if secondary_output.texture != null else null
	var transformed_composed := get_root().get_texture().get_image()
	var primary_red_advantage := _maximum_red_advantage(transformed_primary, Rect2i(75, 20, 110, 105))
	var secondary_red_advantage := _maximum_red_advantage(transformed_secondary, Rect2i(150, 40, 220, 210))
	var composed_red_advantage := _maximum_red_advantage(transformed_composed, Rect2i(75, 20, 110, 105))
	if primary_red_advantage < 0.25 or secondary_red_advantage < 0.25 or composed_red_advantage < 0.25:
		push_error("%s transformed hover layer changed color-glyph channels; red advantage primary=%.3f secondary=%.3f composed=%.3f." % [backend_name, primary_red_advantage, secondary_red_advantage, composed_red_advantage])
		quit(1)
		return

	get_root().push_input(outside_motion, true)
	Input.warp_mouse(Vector2(300, 160))
	var leave_colors: Array[Color] = []
	var leave_secondary_colors: Array[Color] = []
	started = Time.get_ticks_msec()
	next_sample = started
	observed_frames = 0
	while Time.get_ticks_msec() - started < 350 and observed_frames < 240:
		var moving_outside := InputEventMouseMotion.new()
		moving_outside.position = Vector2(300 + observed_frames % 8, 160)
		moving_outside.global_position = moving_outside.position
		get_root().push_input(moving_outside, true)
		Input.warp_mouse(moving_outside.position)
		await process_frame
		observed_frames += 1
		if secondary_output.generation != view.get_generation():
			push_error("%s hover leave exposed mixed primary/secondary generations %d/%d." % [backend_name, view.get_generation(), secondary_output.generation])
			quit(1)
			return
		if Time.get_ticks_msec() < next_sample:
			continue
		if view.get_texture() != null:
			var leave_primary_image := view.get_texture().get_image()
			if leave_primary_image != null:
				leave_colors.append(leave_primary_image.get_pixel(60, 130))
		if secondary_output.texture != null and secondary_output.generation > 0:
			var leave_secondary_image := secondary_output.texture.get_image()
			if leave_secondary_image != null:
				leave_secondary_colors.append(leave_secondary_image.get_pixel(120, 260))
		next_sample += 20
	if _count_distinct_colors(leave_colors) < 6 or _count_distinct_colors(leave_secondary_colors) < 6:
		push_error("%s hover leave transition did not publish enough distinct primary/secondary frames." % backend_name)
		quit(1)
		return
	if leave_colors.is_empty() or leave_secondary_colors.is_empty() \
			or _maximum_channel_difference(leave_colors[-1], Color8(50, 50, 50)) > 0.04 \
			or _maximum_channel_difference(leave_secondary_colors[-1], Color8(50, 50, 50)) > 0.04:
		push_error("%s hover leave transition did not return primary/secondary output to its authored endpoint." % backend_name)
		quit(1)
		return
	print("HCSR hover enter/leave cadence passed on %s with %d primary generations for %d process frames during the authored transition (required %d)." % [backend_name, generations.size(), transition_process_frames, minimum_generation_count])
	quit(0)

func _count_distinct_colors(colors: Array[Color]) -> int:
	var count := 0
	var previous := Color(-1, -1, -1, -1)
	for color in colors:
		if previous.r < 0.0 or not color.is_equal_approx(previous):
			count += 1
			previous = color
	return count

func _maximum_channel_difference(left: Color, right: Color) -> float:
	return max(abs(left.r - right.r), abs(left.g - right.g), abs(left.b - right.b), abs(left.a - right.a))

func _maximum_red_advantage(image: Image, bounds: Rect2i) -> float:
	if image == null or image.is_empty():
		return -1.0
	var clipped := bounds.intersection(Rect2i(Vector2i.ZERO, image.get_size()))
	var result := -1.0
	for y in range(clipped.position.y, clipped.end.y):
		for x in range(clipped.position.x, clipped.end.x):
			var color := image.get_pixel(x, y)
			if color.a > 0.1:
				result = maxf(result, color.r - color.b)
	return result

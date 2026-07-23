extends SceneTree

const BUTTON_COUNT := 6
const BUTTON_WIDTH := 100
const BUTTON_HEIGHT := 100
const BUTTON_GAP := 12
const BUTTON_LEFT := 20
const BUTTON_TOP := 40

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
	view.logical_size = Vector2i(720, 180)
	view.size = Vector2(720, 180)
	view.html = _fixture_html()
	root.add_child(view)
	var secondary_output := view.create_output(Vector2i(1440, 360))

	for _frame in range(300):
		await process_frame
		if view.get_generation() > 0 and secondary_output.generation > 0:
			break
	if view.get_generation() <= 0 or secondary_output.generation <= 0:
		push_error("HCSR rapid-hover fixture did not publish both outputs.")
		quit(1)
		return

	var initial_generation := view.get_generation()
	var visited := PackedInt32Array()
	for frame in range(180):
		var target := frame % BUTTON_COUNT
		visited.append(target)
		var position := _button_center(target)
		get_root().push_input(_motion_for_button(target), true)
		Input.warp_mouse(position)
		await process_frame

	# Finish on the final button and allow its authored transition to settle.
	get_root().push_input(_motion_for_button(BUTTON_COUNT - 1), true)
	Input.warp_mouse(_button_center(BUTTON_COUNT - 1))
	for _frame in range(120):
		await process_frame

	var primary := view.get_texture().get_image()
	var secondary := secondary_output.texture.get_image()
	if primary == null or secondary == null:
		push_error("HCSR rapid-hover fixture could not read the completed outputs.")
		quit(1)
		return

	var pixel_failure := false
	for index in range(BUTTON_COUNT):
		var expected := Color8(130, 130, 130) if index == BUTTON_COUNT - 1 else Color8(45, 45, 45)
		var primary_color := primary.get_pixel(_button_center(index).x, BUTTON_TOP + 20)
		var secondary_color := secondary.get_pixel(_button_center(index).x * 2, (BUTTON_TOP + 20) * 2)
		if _maximum_channel_difference(primary_color, expected) > 0.04:
			push_error("Primary button %d retained the wrong hover state: %s expected %s." % [index, primary_color, expected])
			pixel_failure = true
		if _maximum_channel_difference(secondary_color, expected) > 0.04:
			push_error("Secondary button %d retained the wrong hover state: %s expected %s." % [index, secondary_color, expected])
			pixel_failure = true

	var published_generations := view.get_generation() - initial_generation
	print("HCSR rapid-hover sweep published %d primary generations." % published_generations)
	if pixel_failure:
		quit(1)
		return
	if published_generations < 30:
		push_error("Rapid hover published only %d primary generations for 180 pointer transitions." % published_generations)
		quit(1)
		return
	print("HCSR rapid-hover sweep passed on %s with %d published primary generations." % [backend_name, published_generations])
	quit(0)

func _fixture_html() -> String:
	var buttons := ""
	for index in range(BUTTON_COUNT):
		buttons += "<button class='card' style='left:%dpx'>%d</button>" % [BUTTON_LEFT + index * (BUTTON_WIDTH + BUTTON_GAP), index]
	return "<html><head><style>html,body{margin:0;background:#101010}.card{position:absolute;top:%dpx;width:%dpx;height:%dpx;background:rgb(45,45,45);border:2px solid rgb(80,80,80);transition:background-color .15s,border-color .15s}.card:hover{background:rgb(130,130,130);border-color:rgb(190,190,190)}</style></head><body>%s</body></html>" % [BUTTON_TOP, BUTTON_WIDTH, BUTTON_HEIGHT, buttons]

func _motion_for_button(index: int) -> InputEventMouseMotion:
	var motion := InputEventMouseMotion.new()
	motion.position = _button_center(index)
	motion.global_position = motion.position
	return motion

func _button_center(index: int) -> Vector2i:
	return Vector2i(BUTTON_LEFT + index * (BUTTON_WIDTH + BUTTON_GAP) + BUTTON_WIDTH / 2, BUTTON_TOP + BUTTON_HEIGHT / 2)

func _maximum_channel_difference(left: Color, right: Color) -> float:
	return max(abs(left.r - right.r), abs(left.g - right.g), abs(left.b - right.b), abs(left.a - right.a))

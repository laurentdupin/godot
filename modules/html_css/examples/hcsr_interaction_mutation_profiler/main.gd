extends Control

const SURFACE_LEFT := 24.0
const SURFACE_TOP := 150.0
const SURFACE_RIGHT := -24.0
const SURFACE_BOTTOM := -24.0
const WARMUP_USEC := 1_500_000
const PHASE_RAMP_USEC := 250_000
const PHASE_QUIESCE_USEC := 250_000
const PHASE_MEASURE_USEC := 2_000_000
const MAX_FRAME_SAMPLES := 200_000
const CARD_COUNT := 30
const CARD_COLUMNS := 5
const PHASE_NAMES := [
	"animation_only",
	"hover",
	"form_click",
	"scroll",
	"localized_mutation",
	"subtree_stylesheet",
	"mixed",
]
enum MutationOperation {
	FORM_VALUE,
	CHECKED,
	ATTRIBUTE,
	TEXT,
	STYLE,
	INNER_HTML,
	STYLESHEET,
}
const MUTATION_OPERATION_NAMES := [
	"form_value",
	"checked",
	"attribute",
	"text",
	"style",
	"inner_html",
	"stylesheet",
]

var html_view: HTMLView
var secondary_output: HTMLViewOutput
var secondary_preview: TextureRect
var surface_frame: ColorRect
var hud_label: Label
var motion_event := InputEventMouseMotion.new()
var click_down_event := InputEventMouseButton.new()
var click_up_event := InputEventMouseButton.new()
var wheel_down_event := InputEventMouseButton.new()
var wheel_up_event := InputEventMouseButton.new()
var text_key_event := InputEventKey.new()
var backend_preference := HTMLView.BACKEND_GPU_AUTO
var backend_name := "gpu_auto"
var automated := false
var use_secondary_output := false
var secondary_resize_pending := false
var secondary_logical_size := Vector2i.ZERO

var phase_index := -1
var phase_start_usec := 0
var phase_ramp_end_usec := 0
var phase_measure_start_usec := 0
var phase_end_usec := 0
var phase_measurement_started := false
var phase_drain_frame_pending := false
var warmup_start_usec := 0
var last_process_usec := 0
var latest_frame_milliseconds := 0.0
var click_action_index := 0
var wheel_action_index := 0
var form_mutation_index := 0
var local_mutation_index := 0
var subtree_mutation_index := 0
var stylesheet_mutation_index := 0
var checkbox_state := true
var next_click_usec := 0
var next_form_usec := 0
var next_wheel_usec := 0
var next_local_mutation_usec := 0
var next_topology_mutation_usec := 0
var next_stylesheet_mutation_usec := 0
var next_hud_update_usec := 0

var frame_samples := PackedFloat64Array()
var frame_phase_ids := PackedByteArray()
var frame_sample_count := 0
var dropped_frame_samples := 0
var phase_sample_counts := PackedInt32Array()
var phase_below_240_counts := PackedInt32Array()
var phase_below_120_counts := PackedInt32Array()
var phase_below_100_counts := PackedInt32Array()
var phase_motion_counts := PackedInt32Array()
var phase_click_counts := PackedInt32Array()
var phase_key_counts := PackedInt32Array()
var phase_wheel_counts := PackedInt32Array()
var phase_form_batch_counts := PackedInt32Array()
var phase_local_batch_counts := PackedInt32Array()
var phase_subtree_batch_counts := PackedInt32Array()
var phase_stylesheet_batch_counts := PackedInt32Array()

var subtree_variants: Array[String] = []
var stylesheet_variants: Array[String] = []
var form_value_variants: Array[String] = []
var status_text_variants: Array[String] = []
var floating_style_variants: Array[String] = []
var mutation_attempts := 0
var mutation_successes := 0
var mutation_failures := 0
var mutation_operation_attempts := PackedInt32Array()
var mutation_operation_failures := PackedInt32Array()
var mutation_operation_last_errors := PackedInt32Array()
var mutation_batches := 0
var mutation_issue_usec := 0
var maximum_mutation_issue_usec := 0
var queued_generations := 0
var activated_generations := 0
var last_queued_generation := 0
var last_activated_generation := 0
var frame_budget_misses := 0
var click_count := 0
var synthetic_key_count := 0
var pointer_event_count := 0
var render_errors := 0
var initial_generation := 0
var final_generation := 0
var finishing := false


func _ready() -> void:
	set_process(false)
	_parse_arguments()
	_prepare_input_events()
	Engine.max_fps = 0
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	_build_godot_ui()
	await get_tree().process_frame
	if use_secondary_output:
		_recreate_secondary_output_for_current_size()
	if use_secondary_output and (secondary_output == null or not secondary_output.is_valid()):
		_fail("The requested secondary HTMLView output could not be created.")
		return
	_build_mutation_variants()
	if DisplayServer.get_name() != "headless" and DisplayServer.window_get_vsync_mode() != DisplayServer.VSYNC_DISABLED:
		_fail("Godot did not retain the requested disabled VSync mode.")
		return
	if not await _wait_for_initial_generation():
		_fail("HTMLView did not publish its initial generation.")
		return
	initial_generation = html_view.get_generation()
	hud_label.text = "HCSR surface is live. Warming the interaction and mutation workload..."
	frame_samples.resize(MAX_FRAME_SAMPLES)
	frame_phase_ids.resize(MAX_FRAME_SAMPLES)
	phase_sample_counts.resize(PHASE_NAMES.size())
	phase_below_240_counts.resize(PHASE_NAMES.size())
	phase_below_120_counts.resize(PHASE_NAMES.size())
	phase_below_100_counts.resize(PHASE_NAMES.size())
	phase_motion_counts.resize(PHASE_NAMES.size())
	phase_click_counts.resize(PHASE_NAMES.size())
	phase_key_counts.resize(PHASE_NAMES.size())
	phase_wheel_counts.resize(PHASE_NAMES.size())
	phase_form_batch_counts.resize(PHASE_NAMES.size())
	phase_local_batch_counts.resize(PHASE_NAMES.size())
	phase_subtree_batch_counts.resize(PHASE_NAMES.size())
	phase_stylesheet_batch_counts.resize(PHASE_NAMES.size())
	mutation_operation_attempts.resize(MUTATION_OPERATION_NAMES.size())
	mutation_operation_failures.resize(MUTATION_OPERATION_NAMES.size())
	mutation_operation_last_errors.resize(MUTATION_OPERATION_NAMES.size())
	warmup_start_usec = Time.get_ticks_usec()
	last_process_usec = warmup_start_usec
	next_hud_update_usec = warmup_start_usec
	set_process(true)
	print(
		"HCSR_STRESS_START backend=%s driver=%s display=%s vsync_mode=%d max_fps=%d secondary=%s automated=%s logical_size=%s control_size=%s" % [
			backend_name,
			RenderingServer.get_current_rendering_driver_name(),
			DisplayServer.get_name(),
			DisplayServer.window_get_vsync_mode(),
			Engine.max_fps,
			use_secondary_output,
			automated,
			html_view.get_logical_size(),
			html_view.size,
		]
	)


func _process(_delta: float) -> void:
	var now_usec := Time.get_ticks_usec()
	if phase_index >= 0 and phase_measurement_started and last_process_usec > 0:
		latest_frame_milliseconds = float(now_usec - last_process_usec) / 1000.0
		_record_frame_sample(latest_frame_milliseconds)
	last_process_usec = now_usec

	if phase_index < 0:
		_drive_hover(now_usec, true)
		if now_usec - warmup_start_usec >= WARMUP_USEC:
			_start_phase(0, now_usec)
	else:
		if phase_drain_frame_pending:
			phase_drain_frame_pending = false
			if phase_index + 1 >= PHASE_NAMES.size():
				_begin_finish()
			else:
				_start_phase(phase_index + 1, now_usec)
		elif now_usec < phase_ramp_end_usec:
			_drive_phase(now_usec)
		elif now_usec < phase_measure_start_usec:
			pass
		elif now_usec < phase_end_usec:
			if not phase_measurement_started:
				_begin_phase_measurement(now_usec)
			_drive_phase(now_usec)
		else:
			if not phase_measurement_started:
				_begin_phase_measurement(now_usec)
			_drive_phase(phase_end_usec - 1)
			phase_drain_frame_pending = true

	if not automated and now_usec >= next_hud_update_usec:
		next_hud_update_usec = now_usec + 500_000
		_update_hud(now_usec)


func _parse_arguments() -> void:
	for argument in OS.get_cmdline_user_args():
		match argument:
			"--vulkan":
				backend_preference = HTMLView.BACKEND_VULKAN
				backend_name = "vulkan"
			"--cpu":
				backend_preference = HTMLView.BACKEND_CPU
				backend_name = "cpu"
			"--d3d12":
				backend_preference = HTMLView.BACKEND_D3D12
				backend_name = "d3d12"
			"--automated":
				automated = true
			"--secondary-output":
				use_secondary_output = true


func _prepare_input_events() -> void:
	click_down_event.button_index = MOUSE_BUTTON_LEFT
	click_down_event.button_mask = MOUSE_BUTTON_MASK_LEFT
	click_down_event.pressed = true
	click_up_event.button_index = MOUSE_BUTTON_LEFT
	click_up_event.button_mask = 0
	click_up_event.pressed = false
	wheel_down_event.button_index = MOUSE_BUTTON_WHEEL_DOWN
	wheel_down_event.factor = 1.0
	wheel_down_event.pressed = true
	wheel_up_event.button_index = MOUSE_BUTTON_WHEEL_UP
	wheel_up_event.factor = 1.0
	wheel_up_event.pressed = true
	text_key_event.pressed = true


func _build_godot_ui() -> void:
	var background := ColorRect.new()
	background.color = Color("090d18")
	background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	background.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(background)

	var hud_background := ColorRect.new()
	hud_background.set_anchors_preset(Control.PRESET_TOP_WIDE)
	hud_background.offset_left = 18.0
	hud_background.offset_top = 16.0
	hud_background.offset_right = -18.0
	hud_background.offset_bottom = 132.0
	hud_background.color = Color(0.025, 0.04, 0.075, 0.94)
	hud_background.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(hud_background)

	hud_label = Label.new()
	hud_label.set_anchors_preset(Control.PRESET_TOP_WIDE)
	hud_label.offset_left = 34.0
	hud_label.offset_top = 26.0
	hud_label.offset_right = -34.0
	hud_label.offset_bottom = 122.0
	hud_label.add_theme_font_size_override("font_size", 18)
	hud_label.add_theme_color_override("font_color", Color("d8e7ff"))
	hud_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	hud_label.text = "Preparing HCSR stress fixture..."
	add_child(hud_label)

	surface_frame = ColorRect.new()
	surface_frame.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	surface_frame.offset_left = SURFACE_LEFT - 6.0
	surface_frame.offset_top = SURFACE_TOP - 6.0
	surface_frame.offset_right = SURFACE_RIGHT + 6.0
	surface_frame.offset_bottom = SURFACE_BOTTOM + 6.0
	surface_frame.color = Color("27466f")
	surface_frame.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(surface_frame)

	html_view = HTMLView.new()
	html_view.name = "StressHTMLView"
	html_view.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	html_view.offset_left = SURFACE_LEFT
	html_view.offset_top = SURFACE_TOP
	html_view.offset_right = SURFACE_RIGHT
	html_view.offset_bottom = SURFACE_BOTTOM
	html_view.logical_size = Vector2i.ZERO
	html_view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_CONTROL_PHYSICAL_ADJUSTED
	html_view.backend_preference = backend_preference
	html_view.frame_budget_milliseconds = 1000.0 / 240.0
	html_view.z_index = 1
	html_view.html = _build_document_html()
	html_view.frame_queued.connect(_on_frame_queued)
	html_view.frame_activated.connect(_on_frame_activated)
	html_view.frame_budget_missed.connect(_on_frame_budget_missed)
	html_view.element_clicked.connect(_on_element_clicked)
	html_view.element_pointer_event.connect(_on_element_pointer_event)
	html_view.render_error.connect(_on_render_error)
	add_child(html_view)
	resized.connect(_on_root_resized)

	if use_secondary_output:
		secondary_preview = TextureRect.new()
		secondary_preview.name = "SecondaryOutputPreview"
		secondary_preview.anchor_left = 0.72
		secondary_preview.anchor_top = 0.70
		secondary_preview.anchor_right = 0.98
		secondary_preview.anchor_bottom = 0.96
		secondary_preview.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
		secondary_preview.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
		secondary_preview.mouse_filter = Control.MOUSE_FILTER_IGNORE
		secondary_preview.z_index = 20
		add_child(secondary_preview)


func _on_root_resized() -> void:
	if not use_secondary_output or secondary_resize_pending:
		return
	secondary_resize_pending = true
	call_deferred("_apply_pending_secondary_resize")


func _apply_pending_secondary_resize() -> void:
	secondary_resize_pending = false
	if use_secondary_output:
		_recreate_secondary_output_for_current_size()


func _recreate_secondary_output_for_current_size() -> void:
	var logical_size := Vector2i(max(1, int(ceil(html_view.size.x))), max(1, int(ceil(html_view.size.y))))
	if logical_size == secondary_logical_size and secondary_output != null and secondary_output.is_valid():
		return
	if secondary_output != null:
		secondary_output.release()
		secondary_output = null
	secondary_logical_size = logical_size
	html_view.logical_size = logical_size
	var output_width: int = mini(640, logical_size.x)
	var output_height: int = maxi(1, int(round(float(output_width) * float(logical_size.y) / float(logical_size.x))))
	secondary_output = html_view.create_output(Vector2i(output_width, output_height))
	if secondary_output == null or not secondary_output.is_valid():
		_fail("The responsive secondary HTMLView output could not be created.")
		return
	secondary_preview.texture = secondary_output.texture


func _wait_for_initial_generation() -> bool:
	var deadline_usec := Time.get_ticks_usec() + 15_000_000
	while Time.get_ticks_usec() < deadline_usec:
		await get_tree().process_frame
		if html_view.get_generation() > 0:
			if secondary_output == null or secondary_output.generation > 0:
				return true
	return false


func _start_phase(new_phase_index: int, now_usec: int) -> void:
	phase_index = new_phase_index
	phase_start_usec = now_usec
	phase_ramp_end_usec = now_usec + PHASE_RAMP_USEC
	phase_measure_start_usec = phase_ramp_end_usec + PHASE_QUIESCE_USEC
	phase_end_usec = phase_measure_start_usec + PHASE_MEASURE_USEC
	phase_measurement_started = false
	phase_drain_frame_pending = false
	_reset_phase_schedule(now_usec, true)
	print("HCSR_STRESS_PHASE_START backend=%s phase=%s generation=%d" % [
		backend_name,
		PHASE_NAMES[phase_index],
		html_view.get_generation(),
	])
	_update_hud(now_usec)


func _reset_phase_schedule(now_usec: int, reset_payload_sequence: bool) -> void:
	if reset_payload_sequence:
		click_action_index = 0
		wheel_action_index = 0
		form_mutation_index = 0
		local_mutation_index = 0
		subtree_mutation_index = 0
		stylesheet_mutation_index = 0
	next_click_usec = now_usec
	next_form_usec = now_usec
	next_wheel_usec = now_usec
	next_local_mutation_usec = now_usec
	next_topology_mutation_usec = now_usec
	next_stylesheet_mutation_usec = now_usec


func _reset_current_phase_work_counts() -> void:
	phase_motion_counts[phase_index] = 0
	phase_click_counts[phase_index] = 0
	phase_key_counts[phase_index] = 0
	phase_wheel_counts[phase_index] = 0
	phase_form_batch_counts[phase_index] = 0
	phase_local_batch_counts[phase_index] = 0
	phase_subtree_batch_counts[phase_index] = 0
	phase_stylesheet_batch_counts[phase_index] = 0


func _begin_phase_measurement(now_usec: int) -> void:
	phase_measurement_started = true
	last_process_usec = now_usec
	_reset_phase_schedule(phase_measure_start_usec, false)
	_reset_current_phase_work_counts()


func _drive_phase(now_usec: int) -> void:
	match phase_index:
		0:
			pass
		1:
			_drive_hover(now_usec, false)
		2:
			_drive_hover(now_usec, false)
			while now_usec >= next_click_usec:
				next_click_usec += 100_000
				_perform_click_interaction()
			while now_usec >= next_form_usec:
				next_form_usec += 50_000
				_mutate_form_state()
		3:
			var scroll_position := _scroll_position()
			_send_motion(scroll_position)
			while now_usec >= next_wheel_usec:
				next_wheel_usec += 8_333
				_send_wheel(scroll_position, int(wheel_action_index / 40) % 2 == 0)
				wheel_action_index += 1
		4:
			_drive_hover(now_usec, false)
			while now_usec >= next_local_mutation_usec:
				next_local_mutation_usec += 16_667
				_mutate_local_state()
		5:
			_drive_hover(now_usec, false)
			while now_usec >= next_topology_mutation_usec:
				next_topology_mutation_usec += 66_667
				_mutate_subtree()
			while now_usec >= next_stylesheet_mutation_usec:
				next_stylesheet_mutation_usec += 100_000
				_mutate_stylesheet()
		6:
			_drive_hover(now_usec, false)
			var mixed_scroll_position := _scroll_position()
			while now_usec >= next_wheel_usec:
				next_wheel_usec += 8_333
				_send_wheel(mixed_scroll_position, int(wheel_action_index / 32) % 2 == 0)
				wheel_action_index += 1
			while now_usec >= next_local_mutation_usec:
				next_local_mutation_usec += 16_667
				_mutate_local_state()
			while now_usec >= next_click_usec:
				next_click_usec += 100_000
				_perform_click_interaction()
			while now_usec >= next_form_usec:
				next_form_usec += 50_000
				_mutate_form_state()
			while now_usec >= next_topology_mutation_usec:
				next_topology_mutation_usec += 100_000
				_mutate_subtree()
			while now_usec >= next_stylesheet_mutation_usec:
				next_stylesheet_mutation_usec += 250_000
				_mutate_stylesheet()


func _perform_click_interaction() -> void:
	if click_action_index % 5 == 0:
		_send_click(_search_position())
		_send_text_character(click_action_index)
	else:
		_send_click(_card_center(click_action_index % CARD_COUNT))
	click_action_index += 1


func _drive_hover(now_usec: int, warmup: bool) -> void:
	var origin_usec := warmup_start_usec if warmup else phase_start_usec
	var elapsed_seconds := float(max(0, now_usec - origin_usec)) / 1_000_000.0
	var horizontal_phase := 0.5 + 0.5 * sin(elapsed_seconds * 4.2)
	var row := int(floor(elapsed_seconds * 0.85)) % 4
	var left_position := _card_center(row * CARD_COLUMNS)
	var right_position := _card_center(row * CARD_COLUMNS + CARD_COLUMNS - 1)
	var local_position := left_position.lerp(right_position, horizontal_phase)
	_send_motion(local_position)


func _card_center(index: int) -> Vector2:
	var logical_size := _current_logical_size()
	var workspace_left := logical_size.x * 0.17
	var workspace_width := logical_size.x * 0.83
	var grid_left := workspace_left + workspace_width * 0.017
	var grid_top := logical_size.y * 0.103
	var grid_width := workspace_width * 0.69
	var grid_height := logical_size.y * 0.66
	var card_width := grid_width * 0.18
	var card_height := grid_height * 0.14
	var gap := grid_width * 0.01
	var column := index % CARD_COLUMNS
	var row := int(index / CARD_COLUMNS) % 6
	return Vector2(
		grid_left + card_width * 0.5 + float(column) * (card_width + gap),
		grid_top + card_height * 0.5 + float(row) * (card_height + gap)
	)


func _search_position() -> Vector2:
	var logical_size := _current_logical_size()
	var workspace_left := logical_size.x * 0.17
	var workspace_width := logical_size.x * 0.83
	return Vector2(
		workspace_left + workspace_width * (0.017 + 0.125),
		logical_size.y * (0.02 + 0.0325)
	)


func _scroll_position() -> Vector2:
	var logical_size := _current_logical_size()
	var workspace_left := logical_size.x * 0.17
	var workspace_width := logical_size.x * 0.83
	return Vector2(
		workspace_left + workspace_width * (1.0 - 0.017 - 0.1275),
		logical_size.y * (0.103 + 0.33)
	)


func _current_logical_size() -> Vector2:
	var logical_size := html_view.get_logical_size()
	return Vector2(max(1, logical_size.x), max(1, logical_size.y))


func _send_motion(local_position: Vector2) -> void:
	var viewport_position := html_view.get_global_rect().position + local_position
	motion_event.position = viewport_position
	motion_event.global_position = viewport_position
	get_viewport().push_input(motion_event, true)
	if phase_measurement_started:
		phase_motion_counts[phase_index] += 1


func _send_click(local_position: Vector2) -> void:
	var viewport_position := html_view.get_global_rect().position + local_position
	click_down_event.position = viewport_position
	click_down_event.global_position = viewport_position
	get_viewport().push_input(click_down_event, true)
	click_up_event.position = viewport_position
	click_up_event.global_position = viewport_position
	get_viewport().push_input(click_up_event, true)
	if phase_measurement_started:
		phase_click_counts[phase_index] += 1


func _send_text_character(sequence: int) -> void:
	text_key_event.unicode = 97 + (sequence % 26)
	get_viewport().push_input(text_key_event, true)
	synthetic_key_count += 1
	if phase_measurement_started:
		phase_key_counts[phase_index] += 1


func _send_wheel(local_position: Vector2, downward: bool) -> void:
	var viewport_position := html_view.get_global_rect().position + local_position
	var wheel_event := wheel_down_event if downward else wheel_up_event
	wheel_event.position = viewport_position
	wheel_event.global_position = viewport_position
	get_viewport().push_input(wheel_event, true)
	if phase_measurement_started:
		phase_wheel_counts[phase_index] += 1


func _mutate_form_state() -> void:
	var variant := form_mutation_index % 2
	var start_usec := Time.get_ticks_usec()
	match variant:
		0:
			_track_mutation(html_view.set_form_control_value(
				&"search",
				form_value_variants[int(form_mutation_index / 2) % form_value_variants.size()]
			), MutationOperation.FORM_VALUE)
		1:
			checkbox_state = not checkbox_state
			_track_mutation(
				html_view.set_form_control_checked(&"enabled", checkbox_state),
				MutationOperation.CHECKED
			)
	form_mutation_index += 1
	_complete_mutation_batch(start_usec)
	if phase_measurement_started:
		phase_form_batch_counts[phase_index] += 1


func _mutate_local_state() -> void:
	var variant := local_mutation_index % 2
	var start_usec := Time.get_ticks_usec()
	_track_mutation(html_view.set_element_attribute(
		&"app",
		&"class",
		"app theme-a" if variant == 0 else "app theme-b"
	), MutationOperation.ATTRIBUTE)
	_track_mutation(html_view.set_element_text(
		&"status",
		status_text_variants[local_mutation_index % status_text_variants.size()]
	), MutationOperation.TEXT)
	_track_mutation(html_view.set_element_style(
		&"floating-panel",
		floating_style_variants[variant]
	), MutationOperation.STYLE)
	local_mutation_index += 1
	_complete_mutation_batch(start_usec)
	if phase_measurement_started:
		phase_local_batch_counts[phase_index] += 1


func _mutate_subtree() -> void:
	var start_usec := Time.get_ticks_usec()
	var variant := subtree_mutation_index % subtree_variants.size()
	_track_mutation(html_view.set_element_inner_html(
		&"mutation-zone",
		 subtree_variants[variant]
	), MutationOperation.INNER_HTML)
	subtree_mutation_index += 1
	_complete_mutation_batch(start_usec)
	if phase_measurement_started:
		phase_subtree_batch_counts[phase_index] += 1


func _mutate_stylesheet() -> void:
	var start_usec := Time.get_ticks_usec()
	var variant := stylesheet_mutation_index % stylesheet_variants.size()
	_track_mutation(html_view.replace_stylesheet_text(
		&"dynamic-style",
		stylesheet_variants[variant]
	), MutationOperation.STYLESHEET)
	stylesheet_mutation_index += 1
	_complete_mutation_batch(start_usec)
	if phase_measurement_started:
		phase_stylesheet_batch_counts[phase_index] += 1


func _track_mutation(result: int, operation: MutationOperation) -> void:
	mutation_attempts += 1
	mutation_operation_attempts[operation] += 1
	if result == OK:
		mutation_successes += 1
	else:
		mutation_failures += 1
		mutation_operation_failures[operation] += 1
		mutation_operation_last_errors[operation] = result


func _complete_mutation_batch(start_usec: int) -> void:
	var elapsed_usec := Time.get_ticks_usec() - start_usec
	mutation_batches += 1
	mutation_issue_usec += elapsed_usec
	maximum_mutation_issue_usec = max(maximum_mutation_issue_usec, elapsed_usec)


func _record_frame_sample(frame_milliseconds: float) -> void:
	if frame_sample_count >= MAX_FRAME_SAMPLES:
		dropped_frame_samples += 1
		return
	frame_samples[frame_sample_count] = frame_milliseconds
	frame_phase_ids[frame_sample_count] = phase_index
	frame_sample_count += 1
	phase_sample_counts[phase_index] += 1
	if frame_milliseconds > 1000.0 / 240.0:
		phase_below_240_counts[phase_index] += 1
	if frame_milliseconds > 1000.0 / 120.0:
		phase_below_120_counts[phase_index] += 1
	if frame_milliseconds > 10.0:
		phase_below_100_counts[phase_index] += 1


func _update_hud(_now_usec: int) -> void:
	var phase_name: String = "warmup" if phase_index < 0 else PHASE_NAMES[phase_index]
	hud_label.text = (
		"HCSR interaction + mutation profiler | backend=%s driver=%s VSync=%d max_fps=%d secondary=%s\n"
		+ "phase=%s frame=%.3f ms (%.1f FPS) samples=%d generation=%d queued=%d activated=%d\n"
		+ "mutations=%d/%d failed=%d budget_misses=%d | automated=%s"
	) % [
		backend_name,
		RenderingServer.get_current_rendering_driver_name(),
		DisplayServer.window_get_vsync_mode(),
		Engine.max_fps,
		use_secondary_output,
		phase_name,
		latest_frame_milliseconds,
		0.0 if latest_frame_milliseconds <= 0.0 else 1000.0 / latest_frame_milliseconds,
		frame_sample_count,
		html_view.get_generation(),
		queued_generations,
		activated_generations,
		mutation_successes,
		mutation_attempts,
		mutation_failures,
		frame_budget_misses,
		automated,
	]


func _begin_finish() -> void:
	if finishing:
		return
	finishing = true
	set_process(false)
	call_deferred("_finish_after_settle")


func _finish_after_settle() -> void:
	var generation_before_sentinel := html_view.get_generation()
	if html_view.set_element_attribute(&"app", &"class", "app theme-a freeze") != OK \
			or html_view.set_element_text(&"status", "final stable state") != OK:
		_fail("Stress run could not publish its post-measure convergence sentinel.")
		return
	var converged := await _wait_for_final_convergence(generation_before_sentinel)
	final_generation = html_view.get_generation()
	_print_results()
	if not converged:
		_fail("Stress run did not drain to one stable primary/output generation.")
		return
	var scheduler := html_view.get_frame_scheduler_diagnostics()
	if mutation_failures != 0 or render_errors != 0 or final_generation <= initial_generation:
		_fail("Stress run completed with mutation/render/generation failures.")
		return
	if int(scheduler.get(&"preparation_failed", 0)) != 0 or int(scheduler.get(&"submission_failed", 0)) != 0:
		_fail("Stress run completed with renderer scheduler failures.")
		return
	if not _has_complete_workload_coverage():
		_fail("Stress run did not execute the complete fixed-rate interaction and mutation schedule.")
		return
	if secondary_output != null and secondary_output.generation != final_generation:
		_fail("Secondary output did not converge to the final primary generation.")
		return
	var success_marker := (
		"HCSR_INTERACTION_MUTATION_STRESS_OK backend=%s driver=%s vsync_mode=%d secondary=%s samples=%d below_100=%d generations=%d mutations=%d" % [
			backend_name,
			RenderingServer.get_current_rendering_driver_name(),
			DisplayServer.window_get_vsync_mode(),
			use_secondary_output,
			frame_sample_count,
			_total_int_array(phase_below_100_counts),
			final_generation - initial_generation,
			mutation_successes,
		]
	)
	if automated:
		if secondary_output != null:
			secondary_output.release()
			secondary_output = null
		if secondary_preview != null:
			secondary_preview.queue_free()
			secondary_preview = null
		html_view.queue_free()
		await get_tree().create_timer(0.1).timeout
		print(success_marker)
		get_tree().quit(0)
	else:
		print(success_marker)
		_reset_for_interactive_loop()


func _wait_for_final_convergence(generation_before_sentinel: int) -> bool:
	var deadline_usec := Time.get_ticks_usec() + 10_000_000
	var stable_frames := 0
	var previous_generation := -1
	var stable_start_usec := 0
	while Time.get_ticks_usec() < deadline_usec:
		await get_tree().process_frame
		var current_generation := html_view.get_generation()
		var output_matches := secondary_output == null or secondary_output.generation == current_generation
		if (
			current_generation > generation_before_sentinel
			and last_queued_generation == current_generation
			and last_activated_generation == current_generation
			and output_matches
		):
			if previous_generation == current_generation:
				stable_frames += 1
			else:
				stable_frames = 1
				stable_start_usec = Time.get_ticks_usec()
			if stable_frames >= 8 and Time.get_ticks_usec() - stable_start_usec >= 150_000:
				return true
		else:
			stable_frames = 0
			stable_start_usec = 0
		previous_generation = current_generation
	return false


func _print_results() -> void:
	for current_phase in range(PHASE_NAMES.size()):
		var values := _sorted_phase_samples(current_phase)
		if values.is_empty():
			continue
		var average_ms := _average(values)
		print(
			"HCSR_STRESS_PHASE backend=%s phase=%s samples=%d avg_fps=%.3f frame_ms_p50=%.3f frame_ms_p95=%.3f frame_ms_p99=%.3f frame_ms_max=%.3f below_240=%d below_120=%d below_100=%d motions=%d clicks=%d keys=%d wheels=%d form_batches=%d local_batches=%d subtree_batches=%d stylesheet_batches=%d" % [
				backend_name,
				PHASE_NAMES[current_phase],
				values.size(),
				0.0 if average_ms <= 0.0 else 1000.0 / average_ms,
				_percentile(values, 0.50),
				_percentile(values, 0.95),
				_percentile(values, 0.99),
				values[values.size() - 1],
				phase_below_240_counts[current_phase],
				phase_below_120_counts[current_phase],
				phase_below_100_counts[current_phase],
				phase_motion_counts[current_phase],
				phase_click_counts[current_phase],
				phase_key_counts[current_phase],
				phase_wheel_counts[current_phase],
				phase_form_batch_counts[current_phase],
				phase_local_batch_counts[current_phase],
				phase_subtree_batch_counts[current_phase],
				phase_stylesheet_batch_counts[current_phase],
			]
		)
	var scheduler := html_view.get_frame_scheduler_diagnostics()
	var required_monitor_names: Array[StringName] = [
		&"HCSR/Frame Time",
		&"HCSR/Core Pipeline Time",
		&"HCSR/Layout Time",
		&"HCSR/GPU Submit Time",
		&"HCSR/Runtime GC Pause Time",
		&"HCSR/Runtime Allocated Bytes",
	]
	var monitors_available := true
	for monitor_name in required_monitor_names:
		if not Performance.has_custom_monitor(monitor_name):
			monitors_available = false
			break
	var monitor_frame_ms := -1.0
	var monitor_core_ms := -1.0
	var monitor_layout_ms := -1.0
	var monitor_submit_ms := -1.0
	var monitor_gc_pause_ms := -1.0
	var monitor_runtime_bytes := -1.0
	if monitors_available:
		monitor_frame_ms = float(Performance.get_custom_monitor(&"HCSR/Frame Time")) * 1000.0
		monitor_core_ms = float(Performance.get_custom_monitor(&"HCSR/Core Pipeline Time")) * 1000.0
		monitor_layout_ms = float(Performance.get_custom_monitor(&"HCSR/Layout Time")) * 1000.0
		monitor_submit_ms = float(Performance.get_custom_monitor(&"HCSR/GPU Submit Time")) * 1000.0
		monitor_gc_pause_ms = float(Performance.get_custom_monitor(&"HCSR/Runtime GC Pause Time")) * 1000.0
		monitor_runtime_bytes = float(Performance.get_custom_monitor(&"HCSR/Runtime Allocated Bytes"))
	print(
		"HCSR_STRESS_SUMMARY backend=%s samples=%d dropped_samples=%d generation_delta=%d queued=%d activated=%d budget_misses=%d clicks=%d keys=%d pointer_events=%d mutation_batches=%d mutation_calls=%d mutation_failures=%d mutation_issue_avg_ms=%.4f mutation_issue_max_ms=%.4f scheduler=%s" % [
			backend_name,
			frame_sample_count,
			dropped_frame_samples,
			final_generation - initial_generation,
			queued_generations,
			activated_generations,
			frame_budget_misses,
			click_count,
			synthetic_key_count,
			pointer_event_count,
			mutation_batches,
			mutation_attempts,
			mutation_failures,
			0.0 if mutation_batches == 0 else float(mutation_issue_usec) / 1000.0 / float(mutation_batches),
			float(maximum_mutation_issue_usec) / 1000.0,
			scheduler,
		]
	)
	for operation in range(MUTATION_OPERATION_NAMES.size()):
		print(
			"HCSR_STRESS_MUTATION backend=%s operation=%s attempts=%d failures=%d last_error=%d" % [
				backend_name,
				MUTATION_OPERATION_NAMES[operation],
				mutation_operation_attempts[operation],
				mutation_operation_failures[operation],
				mutation_operation_last_errors[operation],
			]
		)
	print(
		"HCSR_STRESS_FINAL_MONITOR backend=%s available=%s frame_ms=%.4f core_ms=%.4f layout_ms=%.4f submit_ms=%.4f gc_pause_ms=%.4f runtime_bytes=%.0f" % [
			backend_name,
			monitors_available,
			monitor_frame_ms,
			monitor_core_ms,
			monitor_layout_ms,
			monitor_submit_ms,
			monitor_gc_pause_ms,
			monitor_runtime_bytes,
		]
	)


func _sorted_phase_samples(requested_phase: int) -> Array[float]:
	var values: Array[float] = []
	values.resize(phase_sample_counts[requested_phase])
	var write_index := 0
	for sample_index in range(frame_sample_count):
		if frame_phase_ids[sample_index] == requested_phase:
			values[write_index] = frame_samples[sample_index]
			write_index += 1
	values.sort()
	return values


func _average(values: Array[float]) -> float:
	var total := 0.0
	for value in values:
		total += value
	return 0.0 if values.is_empty() else total / float(values.size())


func _percentile(sorted_values: Array[float], percentile: float) -> float:
	if sorted_values.is_empty():
		return 0.0
	var index := int(round(percentile * float(sorted_values.size() - 1)))
	return sorted_values[clamp(index, 0, sorted_values.size() - 1)]


func _total_int_array(values: PackedInt32Array) -> int:
	var total := 0
	for value in values:
		total += value
	return total


func _has_complete_workload_coverage() -> bool:
	if dropped_frame_samples != 0 or click_count == 0 or pointer_event_count == 0:
		return false
	for current_phase in range(PHASE_NAMES.size()):
		if phase_sample_counts[current_phase] <= 0:
			return false
	if phase_motion_counts[1] <= 0:
		return false
	if phase_click_counts[2] != 20 or phase_key_counts[2] != 4 or phase_form_batch_counts[2] != 40:
		return false
	if phase_wheel_counts[3] != 241:
		return false
	if phase_local_batch_counts[4] != 120:
		return false
	if phase_subtree_batch_counts[5] != 30 or phase_stylesheet_batch_counts[5] != 20:
		return false
	return (
		phase_wheel_counts[6] == 241
		and phase_local_batch_counts[6] == 120
		and phase_click_counts[6] == 20
		and phase_key_counts[6] == 4
		and phase_form_batch_counts[6] == 40
		and phase_subtree_batch_counts[6] == 20
		and phase_stylesheet_batch_counts[6] == 8
	)


func _reset_for_interactive_loop() -> void:
	if html_view.set_element_attribute(&"app", &"class", "app theme-a") != OK \
			or html_view.set_element_text(&"status", "interactive loop restarted") != OK:
		_fail("The interactive workload could not restart its animated state.")
		return
	frame_sample_count = 0
	dropped_frame_samples = 0
	phase_sample_counts.fill(0)
	phase_below_240_counts.fill(0)
	phase_below_120_counts.fill(0)
	phase_below_100_counts.fill(0)
	phase_index = -1
	warmup_start_usec = Time.get_ticks_usec()
	last_process_usec = warmup_start_usec
	finishing = false
	set_process(true)


func _build_mutation_variants() -> void:
	for variant in range(256):
		form_value_variants.append("query-%03d" % variant)
		status_text_variants.append("localized update %03d" % variant)
	floating_style_variants.append(
		"position:absolute;right:23.5%;bottom:2.5%;width:18%;height:6.7%;"
		+ "transform:translateX(12px);background:#17345f;border:2px solid #69a7ff;"
		+ "border-radius:10px;opacity:.94"
	)
	floating_style_variants.append(
		"position:absolute;right:23.5%;bottom:2.5%;width:18%;height:6.7%;"
		+ "transform:translateX(-12px);background:#4b235f;border:2px solid #df85ff;"
		+ "border-radius:10px;opacity:.94"
	)
	for variant in range(3):
		var rows := ""
		var row_count := 72 + variant * 12
		for row in range(row_count):
			rows += (
				"<div class='mutation-row variant-%d'><span class='row-title'>Task %03d</span>"
				+ "<span class='row-owner'>owner-%d</span><span class='row-meter'><span style='width:%d%%'></span></span></div>"
			) % [variant, row, (row + variant) % 9, 12 + ((row * 17 + variant * 11) % 86)]
		subtree_variants.append(rows)
	stylesheet_variants.append(
		".theme-a .card{border-color:#31517c}.theme-b .card{border-color:#774493}"
		+ ".mutation-row:nth-child(3n){background:#15253c}.row-meter span{background:#4ea1ff}"
	)
	stylesheet_variants.append(
		".theme-a .card{border-color:#8a5c32}.theme-b .card{border-color:#3c8478}"
		+ ".card-grid{gap:.8%}.card{width:18.2%}.mutation-row:nth-child(2n){background:#261c35}"
		+ ".row-meter span{background:#df7cff}"
	)
	stylesheet_variants.append(
		".theme-a .card{border-color:#39734c}.theme-b .card{border-color:#8d354d}"
		+ ".card-grid{gap:1.2%}.card{width:17.8%}.mutation-row:nth-child(4n){background:#183128}"
		+ ".row-meter span{background:#67d68b}"
	)


func _build_document_html() -> String:
	var graph_bars := ""
	for bar_index in range(16):
		graph_bars += (
			"<span class='graph-bar %s' style='animation-duration:%dms;animation-delay:-%dms'></span>"
		) % [
			"graph-alt" if bar_index % 3 == 0 else "graph-main",
			520 + (bar_index % 7) * 83,
			bar_index * 71,
		]
	var cards := ""
	for card_index in range(CARD_COUNT):
		cards += (
			"<button id='card-%d' class='card' data-godot-action='select-card' data-card='%d'>"
			+ "<span class='card-badge'>%02d</span><span class='card-title'>Interactive card %d</span>"
			+ "<span class='card-copy'>Hover, press and retarget continuously</span></button>"
		) % [card_index, card_index, card_index, card_index]
	var initial_rows := ""
	for row in range(72):
		initial_rows += (
			"<div class='mutation-row'><span class='row-title'>Task %03d</span>"
			+ "<span class='row-owner'>owner-%d</span><span class='row-meter'><span style='width:%d%%'></span></span></div>"
		) % [row, row % 9, 12 + ((row * 17) % 86)]
	return """<!DOCTYPE html><html><head><style>
*{box-sizing:border-box}html,body{margin:0;width:100%%;height:100%%;overflow:hidden;background:#0b1120;color:#dce8ff;font-family:Arial,sans-serif}
body{position:relative}.app{position:relative;width:100%%;height:100%%;background:linear-gradient(145deg,#0b1120,#111c32)}
.sidebar{position:absolute;left:0;top:0;width:17%%;height:100%%;padding:1.4%%;background:#0d1728;border-right:1px solid #263a5a}
.brand{font-size:22px;font-weight:700;margin-bottom:2.2%%}.nav{display:flex;flex-direction:column;gap:7px}.nav button{height:38px;background:#14233a;color:#cfe0ff;border:1px solid #294568;border-radius:8px;text-align:left;padding:0 12px;transition:background-color .12s,transform .12s}.nav button:hover{background:#254a78;transform:translateX(4px)}
.graph-title{position:absolute;left:8%%;top:43%%;color:#8eb8ec;font-size:11px}.live-graph{position:absolute;left:8%%;right:8%%;top:48%%;height:30%%;display:flex;align-items:flex-end;gap:3px;padding:8px;background:#091321;border:1px solid #294568;border-radius:9px;overflow:hidden}.graph-bar{display:block;flex:1 1 auto;height:20%%;min-width:3px;background:#4ea1ff;border-radius:3px 3px 0 0;animation-name:graph-rise;animation-timing-function:linear;animation-iteration-count:infinite;animation-direction:alternate}.graph-bar.graph-alt{animation-name:graph-rise-alt}.graph-scan{position:absolute;left:5px;top:4px;width:3px;height:92%%;background:rgba(120,210,255,.65);animation:graph-scan 1300ms linear infinite}.graph-legend{position:absolute;left:8%%;right:8%%;top:81%%;font-size:10px;color:#7899bd}.activity-dot{display:inline-block;width:7px;height:7px;margin-right:5px;border-radius:50%%;background:#67d68b;animation:dot-pulse 700ms linear infinite alternate}.activity-dot.alt{background:#df7cff;animation-duration:940ms}
@keyframes graph-rise{from{height:14%%;background:#315b8f;transform:translateY(2px)}to{height:96%%;background:#6bb9ff;transform:translateY(-2px)}}
@keyframes graph-rise-alt{from{height:88%%;background:#763d92;transform:translateY(-1px)}to{height:22%%;background:#e58cff;transform:translateY(3px)}}
@keyframes graph-scan{from{transform:translateX(0);opacity:.25}to{transform:translateX(4800%%);opacity:.9}}
@keyframes dot-pulse{from{transform:scale(.65);opacity:.4}to{transform:scale(1.25);opacity:1}}
.freeze .graph-bar,.freeze .graph-scan,.freeze .activity-dot{animation:none!important}.freeze .card,.freeze .nav button{transition:none!important}
.workspace{position:absolute;left:17%%;top:0;width:83%%;height:100%%}.toolbar{position:absolute;left:1.7%%;right:1.7%%;top:2%%;height:6.5%%;display:flex;align-items:center;gap:10px}.toolbar input{width:25%%;height:80%%;background:#0d1728;color:#e9f2ff;border:1px solid #36577e;border-radius:8px;padding:0 12px}.toolbar label{display:flex;align-items:center;gap:6px}.status{margin-left:auto;color:#91b7e8}
.card-grid{position:absolute;left:1.7%%;top:10.3%%;width:69%%;height:66%%;display:flex;flex-wrap:wrap;align-content:flex-start;gap:1%%}.card{position:relative;width:18%%;height:14%%;padding:1%%;background:#13233a;color:#e8f2ff;border:2px solid #31517c;border-radius:10px;transform:translateY(0) scale(1);transition:background-color .12s,border-color .12s,transform .12s,box-shadow .12s;overflow:hidden}.card:hover{background:#264d7d;border-color:#75b7ff;transform:translateY(-4px) scale(1.02);box-shadow:0 7px 18px rgba(0,0,0,.35)}.card:active{background:#386ba8;transform:translateY(-1px) scale(.99)}.card-badge{position:absolute;right:7px;top:6px;color:#7fabe0;font-size:11px}.card-title{display:block;font-weight:700;font-size:13px}.card-copy{display:block;margin-top:5px;color:#9fb4d1;font-size:10px}
.mutation-shell{position:absolute;right:1.7%%;top:10.3%%;width:25.5%%;height:66%%;background:#0c1627;border:1px solid #2a4364;border-radius:10px;overflow:hidden}.mutation-heading{height:9%%;padding:2.6%% 4%%;background:#13233a;font-weight:700}.mutation-zone{height:91%%;overflow:auto;padding:2.5%%}.mutation-row{height:40px;margin-bottom:6px;padding:7px;background:#111e32;border:1px solid #203b5c;border-radius:6px}.row-title{display:block;font-size:11px}.row-owner{display:inline-block;width:30%%;color:#829aba;font-size:9px}.row-meter{display:inline-block;width:64%%;height:6px;background:#26364e;border-radius:4px;overflow:hidden}.row-meter span{display:block;height:100%%;background:#4ea1ff}
.form-strip{position:absolute;left:1.7%%;right:1.7%%;bottom:2.5%%;height:16%%;padding:1.2%%;background:#0d1728;border:1px solid #294568;border-radius:10px}.form-strip textarea{width:40%%;height:78%%;background:#111f34;color:#e5efff;border:1px solid #36577e;border-radius:7px;padding:8px}.form-strip select{width:15%%;height:42%%;margin-left:1%%;background:#111f34;color:#e5efff}.form-strip .form-label{margin-left:1.2%%}.floating-panel{position:absolute;right:23.5%%;bottom:2.5%%;width:18%%;height:6.7%%;transform:translateX(0);background:#17345f;border:2px solid #69a7ff;border-radius:10px;opacity:.94}
.theme-b{background:linear-gradient(145deg,#171024,#17243a)}
</style><style id='dynamic-style'>.theme-a .card{border-color:#31517c}.theme-b .card{border-color:#774493}.mutation-row:nth-child(3n){background:#15253c}.row-meter span{background:#4ea1ff}</style></head>
<body><div id='app' class='app theme-a'><aside class='sidebar'><div class='brand'>HCSR stress</div><div class='nav'><button id='nav-a' data-godot-action='nav'>Dashboard</button><button id='nav-b' data-godot-action='nav'>Inventory</button><button id='nav-c' data-godot-action='nav'>Activity</button><button id='nav-d' data-godot-action='nav'>Settings</button></div><div class='graph-title'>Live frame traffic</div><div class='live-graph'>%s<span class='graph-scan'></span></div><div class='graph-legend'><span class='activity-dot'></span><span class='activity-dot alt'></span> continuously animated</div></aside>
<main class='workspace'><div class='toolbar'><input id='search' value='query-0'><label><input id='enabled' type='checkbox' checked> enabled</label><span id='status' class='status'>ready</span></div><div class='card-grid'>%s</div><section class='mutation-shell'><div class='mutation-heading'>Live topology</div><div id='mutation-zone' class='mutation-zone'>%s</div></section><div class='form-strip'><textarea id='notes'>Live form state and keyboard updates</textarea><select id='choice'><option value='a'>Alpha</option><option value='b'>Beta</option><option value='c'>Gamma</option></select><span class='form-label'>Pointer, wheel, form and DOM work run in separate lanes.</span></div><div id='floating-panel' class='floating-panel'></div></main></div></body></html>""" % [graph_bars, cards, initial_rows]


func _on_frame_queued(_generation: int) -> void:
	queued_generations += 1
	last_queued_generation = _generation


func _on_frame_activated(_generation: int) -> void:
	activated_generations += 1
	last_activated_generation = _generation


func _on_frame_budget_missed(_generation: int, _elapsed_milliseconds: float, _budget_milliseconds: float, _stage: StringName) -> void:
	frame_budget_misses += 1


func _on_element_clicked(_element_id: StringName, _button: int) -> void:
	click_count += 1


func _on_element_pointer_event(_phase: StringName, _element_id: StringName, _action: StringName, _button: int, _payload: Dictionary) -> void:
	pointer_event_count += 1


func _on_render_error(message: String) -> void:
	render_errors += 1
	push_error(message)


func _fail(message: String) -> void:
	if hud_label != null:
		hud_label.text = "HCSR stress fixture failed:\n" + message
	push_error(message)
	if automated:
		get_tree().quit(1)
	else:
		set_process(false)

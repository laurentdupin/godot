extends Control

const LOGICAL_SIZE := Vector2i(560, 360)
const INITIAL_VALUE := 86

var html_view: HTMLView
var secondary_output: HTMLViewOutput
var secondary_preview: TextureRect
var diagnostics: Label
var repeated_writes: CheckButton
var requested_value := INITIAL_VALUE
var last_written_value := -1
var write_count := 0
var write_failures := 0
var automated_pointer_targets: Array[StringName] = []


func _ready() -> void:
	# This is the configuration used by Deep Desktop. It must be set before the
	# HTMLView creates its backend.
	OS.set_environment("HCSR_SEMANTIC_WORKER", "1")
	_build_native_ui()
	await get_tree().process_frame
	secondary_output = html_view.create_output(LOGICAL_SIZE)
	if secondary_output == null or not secondary_output.is_valid():
		diagnostics.text = "ERROR: HCSR could not create the secondary output."
		set_process(false)
		return
	secondary_preview.texture = secondary_output.texture
	set_process(true)
	if OS.get_cmdline_user_args().has("--automated"):
		_run_automated_slow_drag.call_deferred()


func _process(_delta: float) -> void:
	var state: Dictionary = html_view.get_form_control_state(&"value-slider")
	if not state.is_empty():
		requested_value = roundi(float(state.get("value", requested_value)))

	if repeated_writes.button_pressed or requested_value != last_written_value:
		var result := html_view.set_element_text(&"value-output", "%d°" % requested_value)
		write_count += 1
		if result == OK:
			last_written_value = requested_value
		else:
			write_failures += 1

	var secondary_generation := 0
	if secondary_output != null and secondary_output.is_valid():
		secondary_generation = secondary_output.generation
	diagnostics.text = (
		"Polled slider: %d°    Last text requested: %d°    Writes: %d    Failures: %d\n"
		+ "Primary generation: %d    Secondary generation: %d    Driver: %s"
	) % [
		requested_value,
		last_written_value,
		write_count,
		write_failures,
		html_view.get_generation(),
		secondary_generation,
		RenderingServer.get_current_rendering_driver_name(),
	]


func _build_native_ui() -> void:
	var background := ColorRect.new()
	background.color = Color("071416")
	background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	background.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(background)

	var root_box := VBoxContainer.new()
	root_box.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	root_box.add_theme_constant_override("separation", 12)
	root_box.offset_left = 20.0
	root_box.offset_top = 16.0
	root_box.offset_right = -20.0
	root_box.offset_bottom = -16.0
	add_child(root_box)

	var title := Label.new()
	title.text = "HCSR same-width slider text repaint reproduction"
	title.add_theme_font_size_override("font_size", 24)
	root_box.add_child(title)

	var instructions := Label.new()
	instructions.text = "Drag the left slider slowly, then quickly. The value printed inside both HCSR surfaces must follow every movement."
	instructions.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root_box.add_child(instructions)

	var surfaces := HBoxContainer.new()
	surfaces.size_flags_vertical = Control.SIZE_EXPAND_FILL
	surfaces.add_theme_constant_override("separation", 16)
	root_box.add_child(surfaces)

	var primary_column := _make_surface_column("Primary HTMLView (interactive)")
	surfaces.add_child(primary_column)
	html_view = HTMLView.new()
	html_view.name = "InteractiveHTMLView"
	html_view.custom_minimum_size = Vector2(LOGICAL_SIZE)
	html_view.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	html_view.size_flags_vertical = Control.SIZE_EXPAND_FILL
	html_view.backend_preference = HTMLView.BACKEND_D3D12
	html_view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	html_view.fixed_viewport_size = LOGICAL_SIZE
	html_view.logical_size = LOGICAL_SIZE
	html_view.html = _build_document_html()
	html_view.element_pointer_event.connect(_on_element_pointer_event)
	primary_column.add_child(html_view)

	var secondary_column := _make_surface_column("Secondary HTMLViewOutput (Deep Desktop-style consumer)")
	surfaces.add_child(secondary_column)
	secondary_preview = TextureRect.new()
	secondary_preview.name = "SecondaryOutputPreview"
	secondary_preview.custom_minimum_size = Vector2(LOGICAL_SIZE)
	secondary_preview.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	secondary_preview.size_flags_vertical = Control.SIZE_EXPAND_FILL
	secondary_preview.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	secondary_preview.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	secondary_preview.mouse_filter = Control.MOUSE_FILTER_IGNORE
	secondary_column.add_child(secondary_preview)

	var controls := HBoxContainer.new()
	controls.add_theme_constant_override("separation", 14)
	root_box.add_child(controls)
	repeated_writes = CheckButton.new()
	repeated_writes.text = "Repeat set_element_text every frame (Deep Desktop pattern)"
	repeated_writes.button_pressed = not OS.get_cmdline_user_args().has("--changed-only")
	controls.add_child(repeated_writes)

	var reset := Button.new()
	reset.text = "Reset to 86°"
	reset.pressed.connect(_reset_value)
	controls.add_child(reset)

	diagnostics = Label.new()
	diagnostics.text = "Waiting for the first HCSR frame..."
	diagnostics.add_theme_font_size_override("font_size", 16)
	root_box.add_child(diagnostics)
	set_process(false)


func _make_surface_column(caption: String) -> VBoxContainer:
	var column := VBoxContainer.new()
	column.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	column.size_flags_vertical = Control.SIZE_EXPAND_FILL
	column.add_theme_constant_override("separation", 6)
	var label := Label.new()
	label.text = caption
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	column.add_child(label)
	return column


func _reset_value() -> void:
	html_view.set_form_control_value(&"value-slider", str(INITIAL_VALUE))
	requested_value = INITIAL_VALUE
	last_written_value = -1


func _run_automated_slow_drag() -> void:
	# Wait for the primary and secondary surfaces to become coherent before
	# injecting a real pointer drag through Godot's viewport input routing.
	for _frame in range(240):
		await get_tree().process_frame
		if html_view.get_generation() > 0 and secondary_output.generation == html_view.get_generation():
			break
	if html_view.get_generation() == 0:
		_automation_failed("HCSR did not publish an initial frame.")
		return

	DisplayServer.window_move_to_foreground()
	await get_tree().process_frame
	var start_local := _html_to_local(Vector2(90.0, 166.0))
	var end_local := _html_to_local(Vector2(380.0, 166.0))
	_send_automated_button(start_local, true)
	await get_tree().process_frame

	var observed_values: Array[int] = []
	var observed_generations: Array[int] = []
	for step in range(86):
		var local_position := start_local.lerp(end_local, float(step + 1) / 86.0)
		_send_automated_motion(local_position)
		# Six frames per two logical pixels deliberately models the slow drag that
		# reproduces the stale text while keeping repeated writes active.
		for _settle_frame in range(6):
			await get_tree().process_frame
		var state := html_view.get_form_control_state(&"value-slider")
		var value := roundi(float(state.get("value", -1)))
		if observed_values.is_empty() or observed_values.back() != value:
			observed_values.append(value)
			observed_generations.append(html_view.get_generation())

	_send_automated_button(end_local, false)
	for _frame in range(30):
		await get_tree().process_frame

	print("HCSR_SLOW_DRAG values=%s generations=%s targets=%s final_primary=%d final_secondary=%d writes=%d failures=%d" % [
		observed_values,
		observed_generations,
		automated_pointer_targets,
		html_view.get_generation(),
		secondary_output.generation,
		write_count,
		write_failures,
	])
	if observed_values.size() < 4:
		_automation_failed("The automated pointer drag did not move the HCSR range through enough values.")
		return
	if secondary_output.generation != html_view.get_generation():
		_automation_failed("The primary and secondary HCSR outputs ended on different generations.")
		return
	await RenderingServer.frame_post_draw
	var screenshot_path := OS.get_temp_dir().path_join("hcsr_slider_text_repaint_automated.png")
	var screenshot_error := get_viewport().get_texture().get_image().save_png(screenshot_path)
	print("HCSR_SLOW_DRAG_SCREENSHOT path=%s result=%d" % [screenshot_path, screenshot_error])
	get_tree().quit()


func _html_to_local(html_position: Vector2) -> Vector2:
	return Vector2(
		html_position.x * html_view.size.x / float(LOGICAL_SIZE.x),
		html_position.y * html_view.size.y / float(LOGICAL_SIZE.y)
	)


func _send_automated_motion(local_position: Vector2) -> void:
	var event := InputEventMouseMotion.new()
	event.position = html_view.get_global_transform_with_canvas() * local_position
	event.global_position = event.position
	event.button_mask = MOUSE_BUTTON_MASK_LEFT
	Input.parse_input_event(event)


func _send_automated_button(local_position: Vector2, pressed: bool) -> void:
	var event := InputEventMouseButton.new()
	event.position = html_view.get_global_transform_with_canvas() * local_position
	event.global_position = event.position
	event.button_index = MOUSE_BUTTON_LEFT
	event.button_mask = MOUSE_BUTTON_MASK_LEFT if pressed else 0
	event.pressed = pressed
	Input.parse_input_event(event)


func _on_element_pointer_event(_phase: StringName, element_id: StringName, _action: StringName, _button: int, _payload: Dictionary) -> void:
	if not element_id.is_empty() and (automated_pointer_targets.is_empty() or automated_pointer_targets.back() != element_id):
		automated_pointer_targets.append(element_id)


func _automation_failed(message: String) -> void:
	push_error(message)
	get_tree().quit(1)


func _build_document_html() -> String:
	return """<!DOCTYPE html>
<html>
<head>
<style>
html, body {
  width: 100%; height: 100%; margin: 0; overflow: hidden;
  background: #102523; color: #edf8f6; font-family: Arial, sans-serif;
}
main { box-sizing: border-box; height: 100%; padding: 36px; }
.card {
  box-sizing: border-box; height: 100%; padding: 34px;
  border: 2px solid #58706c; border-radius: 18px; background: #172d2a;
}
h1 { margin: 0 0 42px; font-size: 28px; font-weight: 500; }
.row { display: flex; align-items: center; gap: 24px; }
input[type=range] { flex: 1; height: 42px; }
output {
  display: block; width: 92px; text-align: right;
  color: #83e6df; font-size: 32px; font-variant-numeric: tabular-nums;
}
.hint { margin-top: 38px; color: #a9bbb8; font-size: 17px; line-height: 1.45; }
</style>
</head>
<body>
<main>
  <section class="card">
    <h1>Slow-drag repaint test</h1>
    <div class="row">
      <input id="value-slider" type="range" min="0" max="360" step="1" value="86">
      <output id="value-output">86°</output>
    </div>
    <p class="hint">The slider thumb and this value must remain synchronized, including during slow movement.</p>
  </section>
</main>
</body>
</html>"""

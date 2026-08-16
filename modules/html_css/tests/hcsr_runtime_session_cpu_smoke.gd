extends SceneTree

const HTML := "<root><item id=\"panel\" class=\"panel\">hello</item></root>"
const CSS := ".panel { width: 64px; height: 64px; color: #0000ff; } .red { color: #ff0000; }"
const OUTPUT_SIZE := Vector2i(128, 128)

func _initialize() -> void:
	call_deferred("_run")

func _make_document(initially_red: bool) -> HTMLDocument:
	var result := HTMLDocument.new()
	result.html = HTML.replace("class=\"panel\"", "class=\"panel red\"") if initially_red else HTML
	result.css = CSS
	return result

func _make_target(document: HTMLDocument) -> HTMLRenderTarget:
	var result := HTMLRenderTarget.new()
	result.backend_preference = HTMLView.BACKEND_CPU
	result.size = OUTPUT_SIZE
	result.document = document
	root.add_child(result)
	return result

func _wait_for_target(target: HTMLRenderTarget, expected_sample: Color) -> Image:
	for _frame in range(300):
		var image := target.get_image()
		if image != null and image.get_size() == OUTPUT_SIZE:
			var sample := image.get_pixel(32, 32)
			if sample.is_equal_approx(expected_sample):
				return image
		await process_frame
	return null

func _run() -> void:
	var retained_document := _make_document(false)
	var retained_target := _make_target(retained_document)

	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_CPU
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.fixed_viewport_size = OUTPUT_SIZE
	view.size = Vector2(OUTPUT_SIZE)
	view.document = retained_document
	root.add_child(view)

	var initial_image := await _wait_for_target(retained_target, Color("0000ff"))
	if initial_image == null:
		_fail("RuntimeSession HTMLRenderTarget did not publish the initial blue frame.")
		return
	for _frame in range(300):
		if view.get_generation() > 0 and view.get_texture() != null:
			break
		await process_frame
	if view.get_generation() == 0 or view.get_texture() == null:
		_fail("RuntimeSession HTMLView did not activate a coherent initial publication.")
		return

	var before_generation: int = view.get_generation()
	var submission_frame: int = Engine.get_process_frames()
	var journal := [{
		"operation": "set_attribute",
		"id": "panel",
		"name": "class",
		"value": "panel red",
	}]
	if retained_target.apply_element_mutations(journal) != OK or view.apply_element_mutations(journal) != OK:
		_fail("RuntimeSession rejected the bounded Godot attribute journal.")
		return

	var retained_red := await _wait_for_target(retained_target, Color("ff0000"))
	if retained_red == null:
		_fail("RuntimeSession changed-tile publication did not make the retained target red.")
		return
	for _frame in range(300):
		if view.get_generation() > before_generation:
			break
		await process_frame
	if view.get_generation() <= before_generation:
		_fail("RuntimeSession HTMLView mutation did not advance its active generation.")
		return
	if view.get_host_frame_number() != submission_frame:
		_fail("RuntimeSession interactive mutation missed same-frame host activation: submitted=%d activated=%d." % [submission_frame, view.get_host_frame_number()])
		return
	# Activation is atomic, while the hidden double buffer is synchronized in
	# later bounded slices. Let both independent surfaces finish that work before
	# reading aggregate upload telemetry.
	for _frame in range(16):
		await process_frame
	if DisplayServer.get_name() != "headless":
		await RenderingServer.frame_post_draw
		var viewport_image := root.get_texture().get_image()
		if viewport_image == null or !viewport_image.get_pixel(32, 32).is_equal_approx(Color("ff0000")):
			_fail("RuntimeSession HTMLView publication was not composed into the Godot viewport.")
			return

	var changed_tile_bytes: float = Performance.get_custom_monitor("HCSR/RuntimeSession Changed Tile Bytes")
	var texture_upload_bytes: float = Performance.get_custom_monitor("HCSR/RuntimeSession Texture Upload Bytes")
	var step_seconds: float = Performance.get_custom_monitor("HCSR/RuntimeSession Step Time")
	var presentation_slice_seconds: float = Performance.get_custom_monitor("HCSR/RuntimeSession Presentation Slice Time")
	if changed_tile_bytes <= 0.0 or texture_upload_bytes != changed_tile_bytes * 2.0:
		_fail("RuntimeSession double-buffer upload did not remain tile-local: changed=%d B, uploaded=%d B." % [int(changed_tile_bytes), int(texture_upload_bytes)])
		return
	if step_seconds > 0.004:
		_fail("RuntimeSession exceeded the 4 ms aggregate Godot stepping gate: %.3f ms." % (step_seconds * 1000.0))
		return
	if presentation_slice_seconds > 0.004:
		_fail("RuntimeSession exceeded the 4 ms Godot presentation-slice gate: %.3f ms." % (presentation_slice_seconds * 1000.0))
		return

	var clean_target := _make_target(_make_document(true))
	var clean_red := await _wait_for_target(clean_target, Color("ff0000"))
	if clean_red == null or retained_red.get_data() != clean_red.get_data():
		_fail("Retained RuntimeSession mutation pixels differ from a clean target publication.")
		return

	print("HCSR RuntimeSession Godot CPU same-frame smoke passed: generation %d -> %d on host frame %d, changed/upload=%d/%d B, step/presentation=%.3f/%.3f ms." % [before_generation, view.get_generation(), submission_frame, int(changed_tile_bytes), int(texture_upload_bytes), step_seconds * 1000.0, presentation_slice_seconds * 1000.0])
	retained_target.queue_free()
	clean_target.queue_free()
	view.queue_free()
	quit()

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

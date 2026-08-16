extends SceneTree

const PRIMARY_SIZE := Vector2i(2560, 1440)
const SECONDARY_SIZE := Vector2i(1600, 900)
const RESIZED_SECONDARY_SIZE := Vector2i(1280, 720)

func _initialize() -> void:
	call_deferred("_run")

func _document(red: bool) -> HTMLDocument:
	var result := HTMLDocument.new()
	result.html = "<root><item id=\"panel\" class=\"panel%s\">frame</item></root>" % (" red" if red else "")
	result.css = ".panel { width: 2560px; height: 1440px; color: #0000ff; } .red { color: #ff0000; }"
	return result

func _make_view(red: bool = false) -> HTMLView:
	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_CPU
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.logical_size = PRIMARY_SIZE
	view.fixed_viewport_size = PRIMARY_SIZE
	view.size = Vector2(PRIMARY_SIZE)
	view.document = _document(red)
	root.add_child(view)
	return view

func _set_red(view: HTMLView, red: bool) -> Error:
	return view.apply_element_mutations([{
		"operation": "set_attribute",
		"id": "panel",
		"name": "class",
		"value": "panel red" if red else "panel",
	}])

func _wait_for_atomic(view: HTMLView, secondary: HTMLViewOutput, color: Color, minimum_generation: int = 0, maximum_frames: int = 3000) -> bool:
	for _frame in range(maximum_frames):
		var primary_image := view.get_texture().get_image()
		var secondary_image := secondary.texture.get_image()
		if primary_image != null and secondary_image != null \
				and primary_image.get_size() == PRIMARY_SIZE \
				and secondary_image.get_size() == secondary.size \
				and view.get_generation() > minimum_generation \
				and secondary.generation == view.get_generation() \
				and primary_image.get_pixel(16, 16).is_equal_approx(color) \
				and secondary_image.get_pixel(10, 10).is_equal_approx(color):
			return true
		await process_frame
	var primary_image := view.get_texture().get_image()
	var secondary_image := secondary.texture.get_image()
	print("atomic wait timeout: visible=%d minimum=%d secondary=%d primary_size=%s secondary_size=%s primary_sample=%s secondary_sample=%s" % [view.get_generation(), minimum_generation, secondary.generation, primary_image.get_size() if primary_image != null else Vector2i(), secondary_image.get_size() if secondary_image != null else Vector2i(), primary_image.get_pixel(16, 16) if primary_image != null else Color(), secondary_image.get_pixel(10, 10) if secondary_image != null else Color()])
	return false

func _wait_for_partial_staging(view: HTMLView, visible_generation: int, visible_color: Color) -> bool:
	var observed_candidate_reset := false
	for _frame in range(3000):
		var image := view.get_texture().get_image()
		if view.get_generation() != visible_generation or image == null or !image.get_pixel(16, 16).is_equal_approx(visible_color):
			print("partial staging lost visible authority: expected_gen=%d actual_gen=%d image=%s sample=%s upload=%d" % [visible_generation, view.get_generation(), image != null, image.get_pixel(16, 16) if image != null else Color(), int(Performance.get_custom_monitor("HCSR/RuntimeSession Texture Upload Bytes"))])
			return false
		var staged_tiles: float = Performance.get_custom_monitor("HCSR/RuntimeSession Staged Tiles")
		if staged_tiles < 4:
			observed_candidate_reset = true
		if observed_candidate_reset and staged_tiles >= 4:
			return true
		await process_frame
	print("partial staging timeout: process_frame=%d generation=%d staged=%d upload=%d work=%d step_us=%d" % [Engine.get_process_frames(), view.get_generation(), int(Performance.get_custom_monitor("HCSR/RuntimeSession Staged Tiles")), int(Performance.get_custom_monitor("HCSR/RuntimeSession Texture Upload Bytes")), int(Performance.get_custom_monitor("HCSR/RuntimeSession Work Units")), int(Performance.get_custom_monitor("HCSR/RuntimeSession Step Time") * 1000000.0)])
	return false

func _wait_for_initial_standby_sync() -> bool:
	var expected_upload_bytes := (PRIMARY_SIZE.x * PRIMARY_SIZE.y + SECONDARY_SIZE.x * SECONDARY_SIZE.y) * 4 * 2
	for _frame in range(1200):
		if Performance.get_custom_monitor("HCSR/RuntimeSession Texture Upload Bytes") >= expected_upload_bytes:
			return true
		await process_frame
	return false

func _run() -> void:
	OS.set_environment("HCSR_RUNTIME_TEST_PRESENTATION_UNIT_LIMIT", "")
	var retained := _make_view(false)
	var retained_secondary: HTMLViewOutput = retained.create_output(SECONDARY_SIZE)
	var peer := _make_view(false)
	var peer_secondary: HTMLViewOutput = peer.create_output(Vector2i(640, 360))
	if !await _wait_for_atomic(retained, retained_secondary, Color("0000ff")) \
			or !await _wait_for_atomic(peer, peer_secondary, Color("0000ff")):
		_fail("Multiple RuntimeSession views did not converge under the shared frame budget.")
		return
	peer.queue_free()
	for _frame in range(4):
		await process_frame
	if !await _wait_for_initial_standby_sync():
		_fail("The initial hidden standby did not finish bounded synchronization.")
		return
	for _frame in range(32):
		await process_frame

	var blue_generation := retained.get_generation()
	OS.set_environment("HCSR_RUNTIME_TEST_PRESENTATION_UNIT_LIMIT", "1")
	if _set_red(retained, true) != OK or !await _wait_for_partial_staging(retained, blue_generation, Color("0000ff")):
		_fail("The obsolete red endpoint did not stage several physical tiles while blue remained visible.")
		return
	if _set_red(retained, false) != OK:
		_fail("The newest blue endpoint mutation was rejected.")
		return
	for _frame in range(12):
		var image := retained.get_texture().get_image()
		if image == null or image.get_pixel(16, 16).is_equal_approx(Color("ff0000")):
			_fail("An obsolete physically staged red endpoint activated after a newer author mutation.")
			return
		if retained.get_generation() > blue_generation:
			break
		await process_frame
	OS.set_environment("HCSR_RUNTIME_TEST_PRESENTATION_UNIT_LIMIT", "")
	if !await _wait_for_atomic(retained, retained_secondary, Color("0000ff"), blue_generation):
		_fail("The newest blue endpoint did not publish atomically after physical supersession.")
		return

	var pre_resize_generation := retained.get_generation()
	OS.set_environment("HCSR_RUNTIME_TEST_PRESENTATION_UNIT_LIMIT", "1")
	if _set_red(retained, true) != OK or !await _wait_for_partial_staging(retained, pre_resize_generation, Color("0000ff")):
		_fail("The pre-resize candidate did not enter partial physical staging.")
		return
	retained_secondary.size = RESIZED_SECONDARY_SIZE
	OS.set_environment("HCSR_RUNTIME_TEST_PRESENTATION_UNIT_LIMIT", "")
	if !await _wait_for_atomic(retained, retained_secondary, Color("ff0000"), pre_resize_generation):
		_fail("A resize did not cancel the old configuration and publish a clean-sized endpoint.")
		return
	if retained.get_generation() <= pre_resize_generation or retained_secondary.generation != retained.get_generation():
		_fail("Configuration replacement did not preserve monotonic atomic visible generations.")
		return

	var clean := _make_view(true)
	var clean_secondary: HTMLViewOutput = clean.create_output(RESIZED_SECONDARY_SIZE)
	if !await _wait_for_atomic(clean, clean_secondary, Color("ff0000")):
		_fail("The independent clean reference did not publish.")
		return
	var retained_primary_image := retained.get_texture().get_image()
	var retained_secondary_image := retained_secondary.texture.get_image()
	if retained_primary_image.get_data() != clean.get_texture().get_image().get_data() \
			or retained_secondary_image.get_data() != clean_secondary.texture.get_image().get_data():
		_fail("Superseded/resized primary or secondary output differs from a clean publication.")
		return

	OS.set_environment("HCSR_RUNTIME_TEST_PRESENTATION_UNIT_LIMIT", "")
	print("HCSR RuntimeSession 2560x1440 physical supersession/resize smoke passed: visible generation %d." % retained.get_generation())
	retained.queue_free()
	clean.queue_free()
	quit()

func _fail(message: String) -> void:
	OS.set_environment("HCSR_RUNTIME_TEST_PRESENTATION_UNIT_LIMIT", "")
	push_error(message)
	quit(1)

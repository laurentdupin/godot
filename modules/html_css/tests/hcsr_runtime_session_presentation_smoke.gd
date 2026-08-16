extends SceneTree

const SMALL_SIZE := Vector2i(128, 128)
const LARGE_SIZE := Vector2i(1024, 1024)

func _initialize() -> void:
	call_deferred("_run")

func _document(size: Vector2i, red: bool = false) -> HTMLDocument:
	var result := HTMLDocument.new()
	result.html = "<root><item id=\"panel\" class=\"panel%s\">frame</item></root>" % (" red" if red else "")
	result.css = ".panel { width: %dpx; height: %dpx; color: #0000ff; } .red { color: #ff0000; }" % [size.x, size.y]
	return result

func _make_view(size: Vector2i) -> HTMLView:
	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_CPU
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.logical_size = size
	view.fixed_viewport_size = size
	view.size = Vector2(size)
	view.document = _document(size)
	root.add_child(view)
	return view

func _wait_for_view(view: HTMLView, color: Color, maximum_frames: int = 1200) -> Image:
	for _frame in range(maximum_frames):
		var texture := view.get_texture()
		if texture != null:
			var image := texture.get_image()
			if image != null and image.get_size() == Vector2i(view.logical_size) and image.get_pixel(16, 16).is_equal_approx(color):
				return image
		await process_frame
	return null

func _set_red(view: HTMLView, red: bool) -> Error:
	return view.apply_element_mutations([{
		"operation": "set_attribute",
		"id": "panel",
		"name": "class",
		"value": "panel red" if red else "panel",
	}])

func _run() -> void:
	var large := _make_view(LARGE_SIZE)
	var large_blue := await _wait_for_view(large, Color("0000ff"))
	if large_blue == null:
		_fail("Large RuntimeSession output did not finish bounded initial presentation.")
		return
	var initial_slice: float = Performance.get_custom_monitor("HCSR/RuntimeSession Presentation Slice Time")
	if initial_slice > 0.004:
		_fail("Large initial presentation exceeded one 4 ms host slice: %.3f ms." % (initial_slice * 1000.0))
		return
	if _set_red(large, true) != OK:
		_fail("Large RuntimeSession mutation journal was rejected.")
		return
	var large_red := await _wait_for_view(large, Color("ff0000"))
	if large_red == null:
		_fail("Large RuntimeSession broad-damage publication did not complete.")
		return
	for _frame in range(600):
		await process_frame
	var broad_slice: float = Performance.get_custom_monitor("HCSR/RuntimeSession Presentation Slice Time")
	if broad_slice > 0.004:
		_fail("Large broad-damage presentation exceeded one 4 ms host slice: %.3f ms." % (broad_slice * 1000.0))
		return
	var clean_large := _make_view(LARGE_SIZE)
	clean_large.document = _document(LARGE_SIZE, true)
	var clean_red := await _wait_for_view(clean_large, Color("ff0000"))
	if clean_red == null or clean_red.get_data() != large_red.get_data():
		_fail("Large retained presentation differs from an independent clean publication.")
		return
	large.queue_free()
	clean_large.queue_free()
	await process_frame

	var multi := _make_view(SMALL_SIZE)
	var secondary: HTMLViewOutput = multi.create_output(Vector2i(256, 256))
	if secondary == null:
		_fail("RuntimeSession could not create the differently sized secondary output.")
		return
	var stable_primary_texture := multi.get_texture()
	var stable_secondary_texture := secondary.texture
	if await _wait_for_view(multi, Color("0000ff")) == null:
		_fail("RuntimeSession multi-output initial publication did not complete.")
		return
	for _frame in range(300):
		if secondary.generation == multi.get_generation() and secondary.generation > 0:
			break
		await process_frame
	if secondary.generation != multi.get_generation():
		_fail("Primary and secondary outputs did not activate one generation atomically.")
		return
	var multi_generation := multi.get_generation()
	if _set_red(multi, true) != OK or await _wait_for_view(multi, Color("ff0000")) == null:
		_fail("RuntimeSession multi-output mutation did not complete.")
		return
	if secondary.generation != multi.get_generation() or multi.get_generation() <= multi_generation:
		_fail("Primary and secondary mutation outputs exposed mixed generations.")
		return
	if multi.get_texture() != stable_primary_texture or secondary.texture != stable_secondary_texture:
		_fail("Atomic publication replaced a stable Godot texture proxy.")
		return
	var secondary_image := secondary.texture.get_image()
	if secondary_image == null or !secondary_image.get_pixel(32, 32).is_equal_approx(Color("ff0000")):
		_fail("Differently sized secondary output did not receive the atomic publication.")
		return
	var clean_multi := _make_view(SMALL_SIZE)
	clean_multi.document = _document(SMALL_SIZE, true)
	var clean_secondary: HTMLViewOutput = clean_multi.create_output(Vector2i(256, 256))
	if await _wait_for_view(clean_multi, Color("ff0000")) == null:
		_fail("Clean multi-output reference did not publish.")
		return
	for _frame in range(300):
		if clean_secondary.generation == clean_multi.get_generation() and clean_multi.get_generation() > 0:
			break
		await process_frame
	if clean_secondary.texture.get_image().get_data() != secondary_image.get_data():
		_fail("Retained differently sized output differs from an independent clean output.")
		return

	var superseded := _make_view(SMALL_SIZE)
	if await _wait_for_view(superseded, Color("0000ff")) == null:
		_fail("Supersession fixture did not publish its initial frame.")
		return
	if _set_red(superseded, true) != OK or _set_red(superseded, false) != OK:
		_fail("Rapid supersession journals were rejected.")
		return
	var observed_obsolete_red := false
	var superseded_generation := superseded.get_generation()
	for _frame in range(300):
		var superseded_image := superseded.get_texture().get_image()
		if superseded_image != null and superseded_image.get_pixel(16, 16).is_equal_approx(Color("ff0000")):
			observed_obsolete_red = true
		if superseded.get_generation() > superseded_generation and superseded_image != null and superseded_image.get_pixel(16, 16).is_equal_approx(Color("0000ff")):
			break
		await process_frame
	if observed_obsolete_red or superseded.get_generation() <= superseded_generation:
		_fail("An obsolete superseded endpoint became visible.")
		return

	var failure := _make_view(SMALL_SIZE)
	var failure_secondary: HTMLViewOutput = failure.create_output(Vector2i(256, 256))
	var failure_blue := await _wait_for_view(failure, Color("0000ff"))
	if failure_blue == null:
		_fail("Failure-injection fixture did not publish its initial frame.")
		return
	for _frame in range(300):
		if failure_secondary.generation == failure.get_generation() and failure.get_generation() > 0:
			break
		await process_frame
	for _frame in range(32):
		await process_frame
	var failure_generation := failure.get_generation()
	var failure_pixels := failure.get_texture().get_image().get_data()
	OS.set_environment("HCSR_RUNTIME_TEST_FAIL_PRESENTATION_OUTPUT", "1")
	if _set_red(failure, true) != OK:
		OS.set_environment("HCSR_RUNTIME_TEST_FAIL_PRESENTATION_OUTPUT", "")
		_fail("Failure-injection mutation journal was rejected before presentation.")
		return
	for _frame in range(32):
		await process_frame
	OS.set_environment("HCSR_RUNTIME_TEST_FAIL_PRESENTATION_OUTPUT", "")
	if failure.get_generation() != failure_generation or failure_secondary.generation != failure_generation:
		_fail("A failed multi-output candidate partially activated a generation.")
		return
	if failure.get_texture().get_image().get_data() != failure_pixels:
		_fail("A failed multi-output candidate modified the visible primary texture.")
		return

	multi.queue_free()
	clean_multi.queue_free()
	superseded.queue_free()
	failure.queue_free()
	await process_frame
	for _frame in range(300):
		if Performance.get_custom_monitor("HCSR/RuntimeSession Retiring Sessions") == 0.0:
			break
		await process_frame
	if Performance.get_custom_monitor("HCSR/RuntimeSession Retiring Sessions") != 0.0:
		_fail("Module-owned RuntimeSession retirement did not converge after backend destruction: %d pending." % int(Performance.get_custom_monitor("HCSR/RuntimeSession Retiring Sessions")))
		return
	print("HCSR RuntimeSession bounded/atomic presentation smoke passed: initial/broad max %.3f/%.3f ms." % [initial_slice * 1000.0, broad_slice * 1000.0])
	quit()

func _fail(message: String) -> void:
	OS.set_environment("HCSR_RUNTIME_TEST_FAIL_PRESENTATION_OUTPUT", "")
	push_error(message)
	quit(1)

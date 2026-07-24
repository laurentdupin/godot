extends SceneTree

var backend := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"

func _initialize() -> void:
	if "--vulkan" in OS.get_cmdline_user_args():
		backend = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	ProjectSettings.set_setting(
			"rendering/html_css/hcsr/testing/capacity_block_after_submissions", 3)
	ProjectSettings.set_setting(
			"rendering/html_css/hcsr/testing/capacity_block_frames", 4)
	ProjectSettings.set_setting(
			"rendering/html_css/hcsr/testing/capacity_block_arm", 0)
	call_deferred("_run")

func _run() -> void:
	var root := Control.new()
	root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	get_root().add_child(root)

	var view := HTMLView.new()
	view.backend_preference = backend
	view.logical_size = Vector2i(64, 64)
	view.size = Vector2(64, 64)
	view.frame_budget_milliseconds = 10000.0
	view.html = (
			"<html><style>html,body{margin:0}.card{width:64px;height:64px}"
			+ ".initial{background:#102030}.one{background:#204060}"
			+ ".two{background:#306090}.three{background:#4080c0}.blocked{background:#900000}"
			+ ".newer{background:#009000}.latest{background:#f0c020}</style>"
			+ "<body><div id='card' class='card initial'></div></body></html>")
	root.add_child(view)
	var output := view.create_output(Vector2i(64, 64))
	if output == null:
		_fail("Could not create the required synchronized output.")
		return

	if not await _wait_for_new_generation(view, output, 0):
		return
	ProjectSettings.set_setting(
			"rendering/html_css/hcsr/testing/capacity_block_arm", 1)
	if not await _mutate_and_wait(view, output, "card one"):
		return
	if not await _mutate_and_wait(view, output, "card two"):
		return
	if not await _mutate_and_wait(view, output, "card three"):
		return

	var generation_before_block := view.get_generation()
	var creates_before: float = Performance.get_custom_monitor("HCSR/Texture Resource Creates")
	var frees_before: float = Performance.get_custom_monitor("HCSR/Texture Resource Frees")
	var cancellations_before: float = Performance.get_custom_monitor("HCSR/Capacity Probe Cancellations")
	var lock_busy_before: float = Performance.get_custom_monitor("HCSR/Presentation Lock Busy")
	var scheduler_before := view.get_frame_scheduler_diagnostics()
	if view.set_element_attribute("card", "class", "card blocked") != OK:
		_fail("Could not queue the capacity-probe mutation.")
		return
	await process_frame
	if view.set_element_attribute("card", "class", "card newer") != OK:
		_fail("Could not queue the superseding mutation.")
		return
	if view.set_element_attribute("card", "class", "card latest") != OK:
		_fail("Could not queue the latest mutation.")
		return

	var unchanged_frames := 0
	for _frame in range(3):
		await process_frame
		if view.get_generation() != output.generation:
			_fail("Required outputs diverged while the injected capacity block was active.")
			return
		if view.get_generation() == generation_before_block:
			unchanged_frames += 1
	if unchanged_frames < 2:
		_fail("The deterministic capacity block did not retain the active generation.")
		return

	if not await _wait_for_new_generation(view, output, generation_before_block):
		return
	if view.get_generation() <= generation_before_block + 1:
		_fail("The obsolete capacity-probe generation was submitted instead of being canceled.")
		return

	var image := view.get_texture().get_image()
	if image == null or image.is_empty():
		_fail("Could not read the final synchronized texture.")
		return
	var pixel := image.get_pixel(32, 32)
	if abs(pixel.r - 240.0 / 255.0) > 0.02 \
			or abs(pixel.g - 192.0 / 255.0) > 0.02 \
			or abs(pixel.b - 32.0 / 255.0) > 0.02:
		_fail("The resumed frame did not contain the latest semantic state: %s." % pixel)
		return

	var budget := view.get_last_frame_budget_result()
	if budget.is_empty() or StringName(budget.stage) == &"no_visual_output":
		_fail("The coalesced request did not receive an activation outcome: %s." % budget)
		return
	if StringName(budget.stage) != &"physical_pool_blocked":
		_fail("The coalesced request did not preserve its producer-backpressure outcome: %s." % budget)
		return
	var creates_after: float = Performance.get_custom_monitor("HCSR/Texture Resource Creates")
	var frees_after: float = Performance.get_custom_monitor("HCSR/Texture Resource Frees")
	var cancellations_after: float = Performance.get_custom_monitor("HCSR/Capacity Probe Cancellations")
	var lock_busy_after: float = Performance.get_custom_monitor("HCSR/Presentation Lock Busy")
	var scheduler_after := view.get_frame_scheduler_diagnostics()
	if creates_after != creates_before or frees_after != frees_before:
		_fail("Ordinary slot rotation created or freed texture resources: creates %s -> %s, frees %s -> %s." % [
			creates_before,
			creates_after,
			frees_before,
			frees_after,
		])
		return
	if cancellations_after != cancellations_before + 1.0:
		_fail("The obsolete capacity probe was not accounted for exactly once: %s -> %s." % [
			cancellations_before,
			cancellations_after,
		])
		return
	if lock_busy_after != lock_busy_before:
		_fail("The render-thread presentation owner unexpectedly encountered lock contention.")
		return
	if int(scheduler_after.submitted) <= int(scheduler_before.submitted) \
			or int(scheduler_after.physical_pool_blocked) <= int(scheduler_before.physical_pool_blocked) \
			or int(scheduler_after.superseded_by_newer_revision) <= int(scheduler_before.superseded_by_newer_revision):
		_fail("The scheduler did not account for submission, pool blocking, and supersession: %s -> %s." % [
			scheduler_before,
			scheduler_after,
		])
		return
	if int(scheduler_after.preparation_failed) != int(scheduler_before.preparation_failed) \
			or int(scheduler_after.submission_failed) != int(scheduler_before.submission_failed):
		_fail("The successful fault-injection path reported a renderer failure outcome.")
		return

	ProjectSettings.set_setting(
			"rendering/html_css/hcsr/testing/capacity_block_after_submissions", null)
	ProjectSettings.set_setting(
			"rendering/html_css/hcsr/testing/capacity_block_frames", null)
	ProjectSettings.set_setting(
			"rendering/html_css/hcsr/testing/capacity_block_arm", null)
	print("HCSR delayed-capacity scheduler smoke passed on %s: generation %d -> %d." % [
		backend_name,
		generation_before_block,
		view.get_generation(),
	])
	quit(0)

func _mutate_and_wait(view: HTMLView, output: HTMLViewOutput, css_class: String) -> bool:
	var previous := view.get_generation()
	if view.set_element_attribute("card", "class", css_class) != OK:
		_fail("Could not queue mutation '%s'." % css_class)
		return false
	return await _wait_for_new_generation(view, output, previous)

func _wait_for_new_generation(view: HTMLView, output: HTMLViewOutput, after_generation: int) -> bool:
	for _frame in range(240):
		await process_frame
		if view.get_generation() != output.generation:
			_fail("Primary and required secondary outputs exposed mixed generations.")
			return false
		if view.get_generation() > after_generation:
			return true
	_fail("Timed out waiting for a generation after %d." % after_generation)
	return false

func _fail(message: String) -> void:
	ProjectSettings.set_setting(
			"rendering/html_css/hcsr/testing/capacity_block_after_submissions", null)
	ProjectSettings.set_setting(
			"rendering/html_css/hcsr/testing/capacity_block_frames", null)
	ProjectSettings.set_setting(
			"rendering/html_css/hcsr/testing/capacity_block_arm", null)
	push_error(message)
	quit(1)

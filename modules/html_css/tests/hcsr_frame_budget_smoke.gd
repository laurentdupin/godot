extends SceneTree

var backend := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"
var missed_events: Array[Dictionary] = []

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
	view.frame_budget_milliseconds = 10000.0
	view.html = "<html><style>html,body{margin:0}.card{width:120px;height:80px;background:#204060}.hot{background:#d03030}</style><body><div id='card' class='card'></div></body></html>"
	view.frame_budget_missed.connect(_on_frame_budget_missed)
	root.add_child(view)
	var output := view.create_output(Vector2i(640, 360))
	if output == null:
		_fail("Could not create synchronized secondary output.")
		return

	if not await _wait_for_generation(view, output, 0):
		return
	var within_budget := view.get_last_frame_budget_result()
	if within_budget.is_empty() or within_budget.missed or float(within_budget.elapsed_milliseconds) > float(within_budget.budget_milliseconds):
		_fail("A frame inside its explicit budget was reported as missed: %s." % within_budget)
		return
	if not missed_events.is_empty():
		_fail("The generous-budget frame emitted a missed-budget event.")
		return

	var previous_generation := view.get_generation()
	view.frame_budget_milliseconds = 0.001
	if view.set_element_attribute("card", "class", "card hot") != OK:
		_fail("Could not queue the overload mutation.")
		return
	if not await _wait_for_generation(view, output, previous_generation):
		return
	if missed_events.size() != 1:
		_fail("The explicit overload did not emit exactly one missed-budget event: %s." % missed_events)
		return
	var missed := view.get_last_frame_budget_result()
	if not missed.missed or int(missed.generation) != view.get_generation() or int(missed.generation) != output.generation:
		_fail("The overload result did not identify the synchronized active generation: %s." % missed)
		return
	if float(missed.elapsed_milliseconds) <= float(missed.budget_milliseconds):
		_fail("The overload result did not exceed its declared budget: %s." % missed)
		return
	if StringName(missed.stage) != &"prepare_submit" and StringName(missed.stage) != &"activation":
		_fail("The overload result did not classify its missed stage: %s." % missed)
		return
	var semantic_preparation: float = Performance.get_custom_monitor("HCSR/Semantic Preparation Time")
	var semantic_validation: float = Performance.get_custom_monitor("HCSR/Semantic Snapshot Validation Time")
	var translated_commands: float = Performance.get_custom_monitor("HCSR/Translated Commands")
	var translation_bytes: float = Performance.get_custom_monitor("HCSR/Translation Allocated Bytes")
	if semantic_preparation <= 0.0 or semantic_validation <= 0.0:
		_fail("Separated semantic stage telemetry was not published: preparation=%f validation=%f." % [semantic_preparation, semantic_validation])
		return
	if translated_commands != 0.0 or translation_bytes != 0.0:
		_fail("A non-scroll mutation unexpectedly materialized translated commands: commands=%f bytes=%f." % [translated_commands, translation_bytes])
		return
	if backend != HTMLView.BACKEND_CPU:
		var physical_compilation: float = Performance.get_custom_monitor("HCSR/Physical Compilation Time")
		var record_and_submit: float = Performance.get_custom_monitor("HCSR/Record And Submit Time")
		if physical_compilation <= 0.0 or record_and_submit <= 0.0:
			_fail("Separated GPU stage telemetry was not published: physical=%f record_submit=%f." % [physical_compilation, record_and_submit])
			return

	var stable_generation := view.get_generation()
	view.frame_budget_milliseconds = 10000.0
	if view.set_element_attribute("card", "class", "card hot") != OK:
		_fail("Could not queue the unchanged-state budget sample.")
		return
	for _frame in range(4):
		await process_frame
	var no_output := view.get_last_frame_budget_result()
	if view.get_generation() != stable_generation or output.generation != stable_generation:
		_fail("An unchanged-state budget sample invented a visual generation.")
		return
	if no_output.missed or StringName(no_output.stage) != &"no_visual_output":
		_fail("An unchanged-state request did not finish explicitly without visual output: %s." % no_output)
		return
	if missed_events.size() != 1:
		_fail("An unchanged-state request emitted a false missed-budget event.")
		return

	print("HCSR explicit frame budget smoke passed on %s: %s." % [backend_name, missed])
	quit(0)

func _wait_for_generation(view: HTMLView, output: HTMLViewOutput, after: int) -> bool:
	for _frame in range(240):
		await process_frame
		if view.get_generation() != output.generation:
			_fail("Primary and secondary outputs exposed mixed generations while measuring a frame budget.")
			return false
		if view.get_generation() > after:
			return true
	_fail("Timed out waiting for a synchronized frame after generation %d." % after)
	return false

func _on_frame_budget_missed(generation: int, elapsed_milliseconds: float, budget_milliseconds: float, stage: StringName) -> void:
	missed_events.append({
		"generation": generation,
		"elapsed_milliseconds": elapsed_milliseconds,
		"budget_milliseconds": budget_milliseconds,
		"stage": stage,
	})

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

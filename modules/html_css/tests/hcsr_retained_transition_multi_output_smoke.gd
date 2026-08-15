extends SceneTree

const LOGICAL_SIZE := Vector2i(320, 180)
const SECONDARY_SIZE := Vector2i(640, 360)
const SAMPLE_POSITION := Vector2i(100, 106)
const WAIT_TIMEOUT_MSEC := 5000
const BACKGROUND := Color("111827")
const OPTIONS_COLOR := Color("2563eb")

var backend_preference := HTMLView.BACKEND_CPU
var backend_name := "CPU"


func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--d3d12"):
		backend_preference = HTMLView.BACKEND_D3D12
		backend_name = "D3D12"
	elif OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"

	root.size = LOGICAL_SIZE
	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_CONTROL
	view.logical_size = LOGICAL_SIZE
	view.size = Vector2(LOGICAL_SIZE)
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><head><style>html,body{margin:0;width:100%;height:100%;background:#111827}.dropdown{position:absolute;left:20px;top:16px;width:180px;height:120px;z-index:2}.dropdown.open{z-index:7}.options{position:absolute;left:0;top:36px;width:160px;height:70px;background:#2563eb;opacity:0;transform:translateY(-8px);pointer-events:none;transition:opacity .18s ease-out,transform .18s ease-out}.dropdown.open .options{opacity:1;transform:translateY(0);pointer-events:auto}</style></head><body><div id='dropdown' class='dropdown'><button id='trigger' aria-expanded='false'></button><div id='options' class='options'></div></div></body></html>"
	view.document = document
	root.add_child(view)
	await process_frame
	var secondary := view.create_output(SECONDARY_SIZE)
	if secondary == null or not secondary.is_valid():
		_fail("%s could not create the differently sized transition output." % backend_name)
		return
	if not await _wait_for_generation_pair(view, secondary, 0):
		return

	var baseline_generation := view.get_generation()
	var baseline_image := view.get_texture().get_image()
	if baseline_image == null or not _approximately(baseline_image.get_pixelv(SAMPLE_POSITION), BACKGROUND, 0.02):
		_fail("%s closed transition endpoint was not dormant." % backend_name)
		return
	var mutations: Array[Dictionary] = [
		{
			"operation": "set_attribute",
			"id": "dropdown",
			"name": "class",
			"value": "dropdown open",
		},
		{
			"operation": "set_attribute",
			"id": "trigger",
			"name": "aria-expanded",
			"value": "true",
		},
	]
	if view.apply_element_mutations(mutations) != OK:
		_fail("%s could not submit the retained transition endpoint journal." % backend_name)
		return

	var saw_intermediate_sample := false
	var final_generation := baseline_generation
	var deadline_msec := Time.get_ticks_msec() + WAIT_TIMEOUT_MSEC
	while Time.get_ticks_msec() < deadline_msec:
		await process_frame
		var generation := view.get_generation()
		if generation <= baseline_generation or secondary.generation != generation:
			continue
		var primary_image := view.get_texture().get_image()
		var secondary_image := secondary.texture.get_image()
		if primary_image == null or secondary_image == null:
			continue
		var primary_color := primary_image.get_pixelv(SAMPLE_POSITION)
		var secondary_color := secondary_image.get_pixel(
			SAMPLE_POSITION.x * 2,
			SAMPLE_POSITION.y * 2)
		if not _approximately(primary_color, secondary_color, 2.0 / 255.0):
			_fail("%s primary and differently sized output sampled different overlay generations: primary=%s secondary=%s generation=%d/%d." % [backend_name, primary_color, secondary_color, generation, secondary.generation])
			return
		var differs_from_closed := not _approximately(primary_color, BACKGROUND, 0.02)
		var differs_from_open := not _approximately(primary_color, OPTIONS_COLOR, 0.02)
		if differs_from_closed and differs_from_open:
			saw_intermediate_sample = true
		if _approximately(primary_color, OPTIONS_COLOR, 0.02):
			final_generation = generation
			break

	if final_generation <= baseline_generation or not saw_intermediate_sample:
		_fail("%s did not expose both a coherent dynamic sample and the final retained transition endpoint." % backend_name)
		return
	if secondary.generation != final_generation:
		_fail("%s finished with mixed primary/secondary generations: %d/%d." % [backend_name, final_generation, secondary.generation])
		return

	secondary.release()
	print("HCSR Godot %s retained transition multi-output smoke passed at generation %d." % [backend_name, final_generation])
	quit()


func _wait_for_generation_pair(view: HTMLView, secondary: HTMLViewOutput, after_generation: int) -> bool:
	var deadline_msec := Time.get_ticks_msec() + WAIT_TIMEOUT_MSEC
	while Time.get_ticks_msec() < deadline_msec:
		await process_frame
		if view.get_generation() > after_generation and secondary.generation == view.get_generation() and view.get_texture() != null and secondary.texture != null:
			return true
	_fail("%s timed out waiting for one coherent primary/secondary transition generation." % backend_name)
	return false


func _approximately(actual: Color, expected: Color, tolerance: float) -> bool:
	return absf(actual.r - expected.r) <= tolerance \
			and absf(actual.g - expected.g) <= tolerance \
			and absf(actual.b - expected.b) <= tolerance \
			and absf(actual.a - expected.a) <= tolerance


func _fail(message: String) -> void:
	push_error(message)
	quit(1)

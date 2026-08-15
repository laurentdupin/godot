extends SceneTree

const WIDTH := 320
const HEIGHT := 180
const STYLE := "body{margin:0;background:#111827}.trigger{position:absolute;left:20px;top:20px;width:120px;height:48px;background:#2563eb}.trigger[aria-expanded=true]{background:#dc2626}.popup{position:absolute;left:20px;top:82px;width:220px;height:70px;background:#2563eb;opacity:0;pointer-events:none}.popup.open{opacity:1;pointer-events:auto}"

var backend_preference := HTMLView.BACKEND_CPU
var backend_name := "CPU"
var queued_generations: Array[int] = []
var activated_generations: Array[int] = []
var fresh_activated_generations: Array[int] = []


func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--d3d12"):
		backend_preference = HTMLView.BACKEND_D3D12
		backend_name = "D3D12"
	elif OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"

	var retained := _create_target(false, true)
	if not await _wait_for_target(retained, 0):
		return

	var baseline_generation := activated_generations[-1]
	queued_generations.clear()
	activated_generations.clear()
	var mutations: Array[Dictionary] = [
		{
			"operation": "set_attribute",
			"id": "popup",
			"name": "class",
			"value": "popup open",
		},
		{
			"operation": "set_attribute",
			"id": "trigger",
			"name": "aria-expanded",
			"value": "true",
		},
	]
	if retained.apply_element_mutations(mutations) != OK:
		_fail("%s could not submit one atomic element-mutation journal." % backend_name)
		return
	if not queued_generations.is_empty() or not activated_generations.is_empty():
		_fail("%s published the journal before engine-frame processing." % backend_name)
		return
	if not await _wait_for_target(retained, baseline_generation):
		return
	if queued_generations.size() != 1 or activated_generations.size() != 1 or queued_generations[0] != activated_generations[0]:
		_fail("%s did not publish exactly one coherent generation for the journal: queued=%s active=%s." % [backend_name, queued_generations, activated_generations])
		return

	var fresh := _create_target(true, false)
	if not await _wait_for_target(fresh, 0, false):
		return
	var retained_image := _get_target_image(retained)
	var fresh_image := _get_target_image(fresh)
	if retained_image == null or fresh_image == null or not _images_equal(retained_image, fresh_image):
		_fail("%s atomic journal output differed from the independently constructed final document." % backend_name)
		return

	queued_generations.clear()
	activated_generations.clear()
	var invalid_mutations: Array[Dictionary] = [
		{
			"operation": "set_attribute",
			"id": "popup",
			"name": "class",
			"value": "popup",
		},
		{
			"operation": "set_attribute",
			"id": "missing",
			"name": "aria-expanded",
			"value": "false",
		},
	]
	if retained.apply_element_mutations(invalid_mutations) != ERR_DOES_NOT_EXIST:
		_fail("%s did not reject a journal with a missing target." % backend_name)
		return
	await process_frame
	if not queued_generations.is_empty() or not activated_generations.is_empty():
		_fail("%s scheduled output for a rejected journal." % backend_name)
		return
	if not _images_equal(_get_target_image(retained), fresh_image):
		_fail("%s partially applied an operation before rejecting its journal." % backend_name)
		return

	print("HCSR Godot %s atomic DOM mutation journal smoke passed." % backend_name)
	quit()


func _create_target(final_state: bool, observe_signals: bool) -> HTMLRenderTarget:
	var target := HTMLRenderTarget.new()
	target.size = Vector2i(WIDTH, HEIGHT)
	target.backend_preference = backend_preference
	if observe_signals:
		target.frame_queued.connect(_on_frame_queued)
		target.frame_activated.connect(_on_frame_activated)
	else:
		target.frame_activated.connect(_on_fresh_frame_activated)
	root.add_child(target)
	var document := HTMLDocument.new()
	var popup_class := "popup open" if final_state else "popup"
	var expanded := "true" if final_state else "false"
	document.html = "<!DOCTYPE html><html><head><style>%s</style></head><body><button id='trigger' class='trigger' aria-expanded='%s'></button><div id='popup' class='%s'></div></body></html>" % [STYLE, expanded, popup_class]
	target.document = document
	return target


func _wait_for_target(target: HTMLRenderTarget, after_generation: int, observe_signals := true) -> bool:
	var deadline_msec := Time.get_ticks_msec() + 5000
	while Time.get_ticks_msec() < deadline_msec:
		await process_frame
		if observe_signals:
			if not activated_generations.is_empty() and activated_generations[-1] > after_generation:
				return true
		elif not fresh_activated_generations.is_empty() and fresh_activated_generations[-1] > after_generation and _get_target_image(target) != null:
			return true
	_fail("%s timed out waiting for a rendered journal generation (after=%d observe=%s queued=%s active=%s fresh=%s image=%s)." % [backend_name, after_generation, observe_signals, queued_generations, activated_generations, fresh_activated_generations, _get_target_image(target) != null])
	return false


func _get_target_image(target: HTMLRenderTarget) -> Image:
	if backend_preference == HTMLView.BACKEND_CPU:
		return target.get_image()
	var texture := target.get_texture()
	return texture.get_image() if texture != null else null


func _images_equal(left: Image, right: Image) -> bool:
	if left == null or right == null or left.get_size() != right.get_size():
		return false
	for y in range(left.get_height()):
		for x in range(left.get_width()):
			var difference := left.get_pixel(x, y) - right.get_pixel(x, y)
			if maxf(maxf(absf(difference.r), absf(difference.g)), maxf(absf(difference.b), absf(difference.a))) > 1.0 / 255.0:
				return false
	return true


func _on_frame_queued(generation: int) -> void:
	queued_generations.append(generation)


func _on_frame_activated(generation: int) -> void:
	activated_generations.append(generation)


func _on_fresh_frame_activated(generation: int) -> void:
	fresh_activated_generations.append(generation)


func _fail(message: String) -> void:
	push_error(message)
	quit(1)

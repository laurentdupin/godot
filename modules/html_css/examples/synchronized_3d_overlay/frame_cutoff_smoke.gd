extends Node

const WIDTH := 240
const HEIGHT := 100

var view: HTMLView
var activated_generations: Array[int] = []


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	get_tree().root.size = Vector2i(WIDTH, HEIGHT)
	var host := Control.new()
	host.size = Vector2(WIDTH, HEIGHT)
	get_tree().root.add_child(host)

	view = HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_GPU_AUTO
	view.logical_size = Vector2i(WIDTH, HEIGHT)
	view.size = Vector2(WIDTH, HEIGHT)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_CONTROL_PHYSICAL_ADJUSTED
	view.html = """
		<html>
			<head><style>html { display:block; position:relative; width:240px; height:100px; background:#111827; }</style></head>
			<div id='target' style='position:absolute;left:20px;top:20px;width:100px;height:50px;background:#374151'></div>
		</html>
	"""
	view.frame_activated.connect(_on_frame_activated)
	host.add_child(view)

	if not await _wait_for_initial_activation():
		return
	var baseline_generation := view.get_generation()
	activated_generations.clear()
	var separately_derived_generations: Array[int] = []

	await get_tree().process_frame
	var request_process_frame := Engine.get_process_frames()
	for color in ["#16a34a", "#2563eb", "#dc2626"]:
		var error := view.apply_element_mutations([
			{
				"operation": "set_style",
				"id": "target",
				"value": "position:absolute;left:20px;top:20px;width:100px;height:50px;background:%s" % color,
			},
		])
		if error != OK:
			_fail("The frame-cutoff smoke could not submit endpoint %s: %s." % [color, error_string(error)])
			return
		RenderingServer.force_sync()
		var queued_generation := view.get_queued_generation()
		if queued_generation <= (separately_derived_generations[-1] if not separately_derived_generations.is_empty() else baseline_generation):
			_fail("Endpoint %s was not derived as a distinct newer publication: baseline=%d derived=%s queued=%d." % [
				color,
				baseline_generation,
				separately_derived_generations,
				queued_generation,
			])
			return
		if view.get_generation() != baseline_generation or not activated_generations.is_empty():
			_fail("Endpoint %s activated before the shared frame cutoff: active=%d activations=%s." % [
				color,
				view.get_generation(),
				activated_generations,
			])
			return
		separately_derived_generations.append(queued_generation)

	await RenderingServer.frame_post_draw
	if view.get_generation() <= baseline_generation:
		_fail("The newest endpoint was not activated before the submission frame was presented.")
		return
	if view.get_host_frame_number() != request_process_frame:
		_fail("The activated endpoint did not retain the exact request frame (request=%d host=%d)." % [
			request_process_frame,
			view.get_host_frame_number(),
		])
		return
	if activated_generations.size() != 1 or activated_generations[0] != view.get_generation():
		_fail("The cutoff activated intermediate endpoints instead of exactly one newest endpoint: %s." % [activated_generations])
		return

	var composed := get_tree().root.get_texture().get_image()
	if composed == null:
		_fail("The frame-cutoff smoke could not read the composed viewport.")
		return
	var red_pixels := _count_pixels(composed, func(pixel: Color) -> bool: return pixel.r > 0.65 and pixel.g < 0.35 and pixel.b < 0.35)
	var green_pixels := _count_pixels(composed, func(pixel: Color) -> bool: return pixel.g > 0.45 and pixel.r < 0.35 and pixel.b < 0.35)
	var blue_pixels := _count_pixels(composed, func(pixel: Color) -> bool: return pixel.b > 0.45 and pixel.r < 0.35 and pixel.g < 0.45)
	if red_pixels == 0 or green_pixels != 0 or blue_pixels != 0:
		_fail("The submission frame did not contain only endpoint C (red=%d green=%d blue=%d)." % [red_pixels, green_pixels, blue_pixels])
		return

	for _frame in range(3):
		await get_tree().process_frame
	if activated_generations.size() != 1:
		_fail("An obsolete A/B endpoint activated after endpoint C: %s." % [activated_generations])
		return

	print("HCSR_RUNTIME_FRAME_CUTOFF_OK generation=%d request_frame=%d derived=%s activations=%d red_pixels=%d" % [
		view.get_generation(),
		request_process_frame,
		separately_derived_generations,
		activated_generations.size(),
		red_pixels,
	])
	get_tree().quit(0)


func _wait_for_initial_activation() -> bool:
	for _frame in range(120):
		await get_tree().process_frame
		RenderingServer.force_draw(false)
		await RenderingServer.frame_post_draw
		if view.get_generation() > 0:
			return true
	_fail("The frame-cutoff smoke did not receive its bootstrap frame.")
	return false


func _on_frame_activated(generation: int) -> void:
	activated_generations.append(generation)


func _count_pixels(image: Image, predicate: Callable) -> int:
	var count := 0
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			if predicate.call(image.get_pixel(x, y)):
				count += 1
	return count


func _fail(message: String) -> void:
	push_error(message)
	get_tree().quit(1)

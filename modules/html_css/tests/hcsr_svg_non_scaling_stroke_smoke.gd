extends SceneTree

const WIDTH := 2560
const HEIGHT := 1392
const SVG_DATA := "PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAzMDAgMjAwIiBwcmVzZXJ2ZUFzcGVjdFJhdGlvPSJub25lIj48ZGVmcz48bGluZWFyR3JhZGllbnQgaWQ9ImciIHgxPSIwIiB5MT0iMCIgeDI9IjAiIHkyPSIxIj48c3RvcCBvZmZzZXQ9IjAiIHN0b3AtY29sb3I9IiNmZjgwMDAiLz48c3RvcCBvZmZzZXQ9IjEiIHN0b3AtY29sb3I9IiMwMGMwZmYiLz48L2xpbmVhckdyYWRpZW50PjwvZGVmcz48cmVjdCB4PSIxIiB5PSIxIiB3aWR0aD0iMjk4IiBoZWlnaHQ9IjE5OCIgcng9IjE4IiBmaWxsPSJub25lIiBzdHJva2U9InVybCgjZykiIHN0cm9rZS13aWR0aD0iMSIgdmVjdG9yLWVmZmVjdD0ibm9uLXNjYWxpbmctc3Ryb2tlIi8+PC9zdmc+"
var tested_view: HTMLView


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var use_vulkan := OS.get_cmdline_user_args().has("--vulkan")
	var backend := HTMLView.BACKEND_VULKAN if use_vulkan else HTMLView.BACKEND_D3D12
	var backend_name := "Vulkan" if use_vulkan else "D3D12"
	root.size = Vector2i(WIDTH, HEIGHT)
	Input.warp_mouse(Vector2(2500, 1340))
	var viewport := SubViewport.new()
	viewport.size = Vector2i(WIDTH, HEIGHT)
	viewport.disable_3d = true
	viewport.transparent_bg = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(viewport)

	var view := HTMLView.new()
	tested_view = view
	view.backend_preference = backend
	view.size = Vector2(WIDTH, HEIGHT)
	view.html = ("""<!DOCTYPE html><html><head><style>
html,body{margin:0;width:100vw;height:100vh;background:transparent;overflow:hidden}
.slot{position:absolute;left:1055px;top:416px;width:450px;height:560px}
.content{position:absolute;inset:4px;background:#304860;border-radius:16px;transform-origin:center;transition:transform 200ms linear}
.border{position:absolute;left:0;top:0;width:450px;height:560px;object-fit:fill;transition:left 200ms linear,top 200ms linear,width 200ms linear,height 200ms linear}
.slot.hot .content{transform:scale(1.1)}
.slot.hot .border{left:-22px;top:-28px;width:495px;height:616px}
</style></head><body><div id="slot" class="slot"><div class="content"></div><img class="border" src="data:image/svg+xml;base64,%s" alt=""></div></body></html>""" % SVG_DATA)
	viewport.add_child(view)
	for _frame in range(16):
		await process_frame
		RenderingServer.force_draw(false)
	var baseline := view.get_texture().get_image()
	var baseline_top := _find_top_stroke(baseline, 1280, 380, 440)
	var baseline_thickness := _vertical_stroke_thickness(baseline, 1280, baseline_top)

	if view.set_element_attribute("slot", "class", "slot hot") != OK:
		_fail("%s could not activate the SVG resize transition." % backend_name)
		return
	var maximum_record_seconds := 0.0
	var maximum_frame_seconds := 0.0
	var maximum_core_seconds := 0.0
	var maximum_layout_seconds := 0.0
	for _frame in range(36):
		await process_frame
		RenderingServer.force_draw(false)
		maximum_record_seconds = max(maximum_record_seconds,
			Performance.get_custom_monitor("HCSR/Record And Submit Time"))
		maximum_frame_seconds = max(maximum_frame_seconds,
			Performance.get_custom_monitor("HCSR/Frame Time"))
		maximum_core_seconds = max(maximum_core_seconds,
			Performance.get_custom_monitor("HCSR/Core Pipeline Time"))
		maximum_layout_seconds = max(maximum_layout_seconds,
			Performance.get_custom_monitor("HCSR/Layout Time"))
	var hovered := view.get_texture().get_image()
	var hovered_top := _find_top_stroke(hovered, 1280, 370, 440)
	var hovered_thickness := _vertical_stroke_thickness(hovered, 1280, hovered_top)

	if baseline_top < 0 or hovered_top < 0 or hovered_top > baseline_top - 20:
		_fail("%s SVG border did not grow upward with its resized viewport: %d -> %d." % [backend_name, baseline_top, hovered_top])
		return
	if baseline_thickness <= 0 or abs(baseline_thickness - hovered_thickness) > 1:
		_fail("%s authored 1px SVG vector-effect stroke changed coverage: %d rows -> %d rows." % [backend_name, baseline_thickness, hovered_thickness])
		return
	print("HCSR Godot %s non-scaling SVG stroke smoke passed: authored_stroke=1px coverage_rows=%d->%d max_frame=%.3fms max_core=%.3fms max_layout=%.3fms max_record=%.3fms." % [backend_name, baseline_thickness, hovered_thickness, maximum_frame_seconds * 1000.0, maximum_core_seconds * 1000.0, maximum_layout_seconds * 1000.0, maximum_record_seconds * 1000.0])
	viewport.remove_child(view)
	view.free()
	tested_view = null
	root.remove_child(viewport)
	viewport.free()
	quit(0)


func _is_stroke(pixel: Color) -> bool:
	return pixel.a > 0.35 and ((pixel.r > 0.55 and pixel.b < 0.35) or (pixel.b > 0.55 and pixel.r < 0.35))


func _vertical_stroke_thickness(image: Image, x: int, center_y: int) -> int:
	if center_y < 0:
		return 0
	var count := 0
	for y in range(max(0, center_y - 5), min(image.get_height(), center_y + 6)):
		if _is_stroke(image.get_pixel(x, y)):
			count += 1
	return count


func _find_top_stroke(image: Image, x: int, start_y: int, end_y: int) -> int:
	for y in range(start_y, end_y + 1):
		if _is_stroke(image.get_pixel(x, y)):
			return y
	return -1


func _fail(message: String) -> void:
	push_error(message)
	if tested_view != null:
		tested_view.free()
		tested_view = null
	quit(1)

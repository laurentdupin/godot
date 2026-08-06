extends Node

const FIXTURE_DOCUMENT_PATH := "res://hcsr_android_svg_fixture.html"


func _ready() -> void:
	_run.call_deferred()


func _run() -> void:
	for backend in [HTMLView.BACKEND_CPU, HTMLView.BACKEND_VULKAN]:
		if not await _verify_backend(backend):
			get_tree().quit(1)
			return
	print("HCSR_ANDROID_SVG_OK cpu=vulkan")
	get_tree().quit(0)


func _verify_backend(backend: int) -> bool:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.disable_3d = true
	viewport.transparent_bg = false
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(viewport)
	var document := HTMLDocument.new()
	document.html_file = FIXTURE_DOCUMENT_PATH
	document.resource_root = "res://"
	var view := HTMLView.new()
	view.backend_preference = backend
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.fixed_viewport_size = Vector2i(64, 64)
	view.size = Vector2(64, 64)
	view.document = document
	viewport.add_child(view)
	for _frame in 5:
		await get_tree().process_frame
	if view.set_element_inner_html(&"app", '<img src="hcsr_android_svg_fixture.svg" alt="">') != OK:
		push_error("HCSR_ANDROID_SVG_FAILED backend=%d mutation rejected" % backend)
		return false
	for _frame in 30:
		await get_tree().process_frame
		RenderingServer.force_draw(false)
	await RenderingServer.frame_post_draw
	var image := view.get_texture().get_image()
	var backend_name := "CPU" if backend == HTMLView.BACKEND_CPU else "Vulkan"
	if image == null or image.is_empty():
		push_error("HCSR_ANDROID_SVG_FAILED backend=%s empty texture" % backend_name)
		return false
	var corner := image.get_pixel(2, 2)
	var cyan_pixels := 0
	for y in image.get_height():
		for x in image.get_width():
			var pixel := image.get_pixel(x, y)
			if pixel.b > 0.55 and pixel.g > 0.40 and pixel.r < 0.40:
				cyan_pixels += 1
	if cyan_pixels < 40 or corner.r > 0.10 or corner.g > 0.10 or corner.b > 0.10:
		push_error("HCSR_ANDROID_SVG_FAILED backend=%s cyan_pixels=%d corner=%s generation=%d" % [
			backend_name, cyan_pixels, corner, view.get_generation()])
		return false
	var generation := view.get_generation()
	viewport.remove_child(view)
	view.free()
	remove_child(viewport)
	viewport.free()
	print("HCSR_ANDROID_SVG_BACKEND_OK backend=%s generation=%d cyan_pixels=%d" % [
		backend_name, generation, cyan_pixels])
	return true

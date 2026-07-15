extends SceneTree

const WINDOW_SIZE := Vector2i(320, 240)
const OVERLAY_RECT := Rect2i(48, 52, 112, 84)
const RESIZED_OVERLAY_RECT := Rect2i(28, 34, 144, 104)
const OPAQUE_RECT := Rect2i(72, 68, 32, 24)
const TRANSLUCENT_RECT := Rect2i(112, 100, 40, 24)
const TRANSPARENT_SAMPLE := Vector2i(144, 68)

func _initialize() -> void:
	DisplayServer.window_set_size(WINDOW_SIZE)
	DisplayServer.window_set_position(Vector2i(80, 80))

	var background := ColorRect.new()
	background.color = Color(0.0, 0.0, 1.0, 1.0)
	background.position = Vector2.ZERO
	background.size = Vector2(WINDOW_SIZE)
	root.add_child(background)

	var overlay := SubViewport.new()
	overlay.size = OVERLAY_RECT.size
	overlay.transparent_bg = true
	overlay.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	var overlay_color := ColorRect.new()
	overlay_color.color = Color(1.0, 0.0, 0.0, 0.5)
	overlay_color.position = Vector2(TRANSLUCENT_RECT.position - OVERLAY_RECT.position)
	overlay_color.size = Vector2(TRANSLUCENT_RECT.size)
	overlay.add_child(overlay_color)
	var opaque_color := ColorRect.new()
	opaque_color.color = Color(0.0, 1.0, 0.0, 1.0)
	opaque_color.position = Vector2(OPAQUE_RECT.position - OVERLAY_RECT.position)
	opaque_color.size = Vector2(OPAQUE_RECT.size)
	overlay.add_child(opaque_color)
	root.add_child(overlay)

	var viewport_rid := overlay.get_viewport_rid()
	var window_id := DisplayServer.MAIN_WINDOW_ID
	RenderingServer.viewport_attach_to_screen(viewport_rid, Rect2(OVERLAY_RECT), window_id)
	# Exercise the documented last-setting-wins contract with direct-to-screen mode.
	RenderingServer.viewport_set_render_direct_to_screen(viewport_rid, true)
	RenderingServer.viewport_set_screen_composition(viewport_rid, RenderingServer.VIEWPORT_SCREEN_COMPOSITION_PREMULTIPLIED_ALPHA, 100)

	for frame in 8:
		await process_frame

	var client_origin := DisplayServer.window_get_position(window_id)
	var outside := _capture_pixel(client_origin + Vector2i(16, 16))
	var transparent := _capture_pixel(client_origin + TRANSPARENT_SAMPLE)
	var translucent := _capture_pixel(client_origin + TRANSLUCENT_RECT.position + TRANSLUCENT_RECT.size / 2)
	var opaque := _capture_pixel(client_origin + OPAQUE_RECT.position + OPAQUE_RECT.size / 2)
	overlay.size = RESIZED_OVERLAY_RECT.size
	RenderingServer.viewport_attach_to_screen(viewport_rid, Rect2(RESIZED_OVERLAY_RECT), window_id)
	for frame in 4:
		await process_frame
	var resized_translucent_position := RESIZED_OVERLAY_RECT.position + TRANSLUCENT_RECT.position - OVERLAY_RECT.position + TRANSLUCENT_RECT.size / 2
	var resized_opaque_position := RESIZED_OVERLAY_RECT.position + OPAQUE_RECT.position - OVERLAY_RECT.position + OPAQUE_RECT.size / 2
	var resized_translucent := _capture_pixel(client_origin + resized_translucent_position)
	var resized_opaque := _capture_pixel(client_origin + resized_opaque_position)
	RenderingServer.viewport_attach_to_screen(viewport_rid, Rect2(), DisplayServer.INVALID_WINDOW_ID)

	if outside.b < 0.7 or outside.r > 0.2 or outside.g > 0.2:
		_fail("Base viewport presentation was not blue: %s" % [outside])
		return
	if transparent.b < 0.7 or transparent.r > 0.2 or transparent.g > 0.2:
		_fail("Transparent screen-composition content did not preserve the base: %s" % [transparent])
		return
	if translucent.r < 0.4 or translucent.b < 0.4 or translucent.g > 0.2:
		_fail("Screen composition did not blend translucent red over blue: %s" % [translucent])
		return
	if opaque.g < 0.8 or opaque.r > 0.2 or opaque.b > 0.2:
		_fail("Opaque screen-composition content did not replace the base: %s" % [opaque])
		return
	if resized_translucent.r < 0.4 or resized_translucent.b < 0.4 or resized_translucent.g > 0.2:
		_fail("Resized screen composition did not preserve translucent blending: %s" % [resized_translucent])
		return
	if resized_opaque.g < 0.8 or resized_opaque.r > 0.2 or resized_opaque.b > 0.2:
		_fail("Resized screen composition did not preserve opaque content: %s" % [resized_opaque])
		return

	print("RenderingServer screen-composition smoke passed: outside=%s transparent=%s translucent=%s opaque=%s resized_translucent=%s resized_opaque=%s" % [outside, transparent, translucent, opaque, resized_translucent, resized_opaque])
	quit()

func _capture_pixel(screen_position: Vector2i) -> Color:
	var image := DisplayServer.screen_get_image_rect(Rect2i(screen_position, Vector2i.ONE))
	if image == null or image.is_empty():
		_fail("DisplayServer could not capture the screen-composition test window")
		return Color.BLACK
	return image.get_pixel(0, 0)

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

extends SceneTree

func _initialize() -> void:
	var background := ColorRect.new()
	background.color = Color(0.1, 0.8, 0.2, 1.0)
	background.size = Vector2(320, 180)
	root.add_child(background)

	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><head><style>html, body { margin: 0; background: transparent; } .glass { position: absolute; left: 40px; top: 30px; width: 160px; height: 90px; border-radius: 12px; backdrop-filter: invert(1); }</style></head><body><div class=\"glass\"></div></body></html>"
	document.resource_root = "res://"
	document.background_color = Color(0, 0, 0, 0)

	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_CPU
	view.size = Vector2(320, 180)
	view.backdrop_filter_enabled = true
	view.document = document
	root.add_child(view)

	if not await _validate_canvas_frame(view, "initial"):
		quit(1)
		return

	background.size = Vector2(240, 140)
	view.size = Vector2(240, 140)
	if not await _validate_canvas_frame(view, "resized"):
		quit(1)
		return

	print("HCSR Godot 2D backdrop canvas and resize smoke passed.")
	quit()

func _validate_canvas_frame(view: HTMLView, phase: String) -> bool:
	for frame in range(3):
		await process_frame
	await RenderingServer.frame_post_draw
	var regions := view.get_backdrop_filter_regions()
	if regions.size() != 1:
		push_error("HCSR %s canvas frame did not retain its backdrop metadata." % phase)
		return false
	var image := root.get_texture().get_image()
	var sample := image.get_pixel(100, 75)
	if sample.r < 0.75 or sample.g > 0.3 or sample.b < 0.65:
		push_error("HCSR %s canvas frame did not invert the scene backdrop (sample %s)." % [phase, sample])
		return false
	return true

extends SceneTree

func _initialize() -> void:
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><head><style>body { margin: 0; background: #123456; color: white; }</style></head><body><p>Hello from static HCSR</p></body></html>"
	document.resource_root = "res://"
	document.background_color = Color("123456")

	var target := HTMLRenderTarget.new()
	target.backend_preference = 1
	target.size = Vector2i(320, 180)
	target.document = document
	target.render_now()

	var image := target.get_image()
	if image == null or image.get_width() != 320 or image.get_height() != 180:
		push_error("Static HCSR did not return the expected Godot image.")
		target.free()
		quit(1)
		return

	var sample := image.get_pixel(300, 160)
	if sample.a < 0.9 or sample.b < 0.20:
		push_error("Static HCSR image did not contain the expected document background.")
		target.free()
		quit(1)
		return

	print("Static HCSR Godot smoke passed.")
	target.free()
	quit()

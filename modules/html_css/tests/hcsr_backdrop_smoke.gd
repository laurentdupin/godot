extends SceneTree

func _initialize() -> void:
	var use_d3d12 := OS.get_cmdline_user_args().has("--d3d12")
	var use_vulkan := OS.get_cmdline_user_args().has("--vulkan")
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><head><style>html, body { margin: 0; background: transparent; } .glass { position: absolute; left: 40px; top: 30px; width: 160px; height: 90px; border-radius: 12px; backdrop-filter: invert(1); }</style></head><body><div class=\"glass\"></div></body></html>"
	document.resource_root = "res://"
	document.background_color = Color(0, 0, 0, 0)

	var target := HTMLRenderTarget.new()
	target.backend_preference = 4 if use_d3d12 else (3 if use_vulkan else 1)
	target.size = Vector2i(320, 180)
	target.backdrop_filter_enabled = true
	target.document = document
	target.render_now()
	if not _validate_frame(target, Vector2i(320, 180), "initial"):
		target.free()
		quit(1)
		return

	# Recreate all size-dependent output while retaining the logical document and
	# backdrop metadata configuration. This guards the 2D viewport resize path.
	target.size = Vector2i(240, 140)
	target.render_now()
	if not _validate_frame(target, Vector2i(240, 140), "resized"):
		target.free()
		quit(1)
		return

	print("Static HCSR Godot %s backdrop metadata and resize smoke passed." % ("D3D12" if use_d3d12 else ("Vulkan" if use_vulkan else "CPU")))
	target.free()
	quit()

func _validate_frame(target: HTMLRenderTarget, expected_size: Vector2i, phase: String) -> bool:
	var regions := target.get_backdrop_filter_regions()
	if regions.size() != 1:
		push_error("HCSR %s frame did not publish exactly one backdrop-filter region (received %d: %s)." % [phase, regions.size(), regions])
		return false
	var region: Dictionary = regions[0]
	var operations: Array = region.get("filter_operations", [])
	if not region.get("supported", false) or operations.size() != 1 or operations[0].get("type", -1) != HTMLView.HTML_BACKDROP_FILTER_OPERATION_INVERT:
		push_error("HCSR %s frame did not preserve the supported invert operation." % phase)
		return false
	var bounds: Rect2 = region.get("bounds", Rect2())
	if not bounds.is_equal_approx(Rect2(40, 30, 160, 90)):
		push_error("HCSR %s frame reported incorrect backdrop bounds: %s." % [phase, bounds])
		return false
	var texture := target.get_texture()
	if texture == null or texture.get_width() != expected_size.x or texture.get_height() != expected_size.y:
		push_error("HCSR %s frame did not recreate the expected %s output texture." % [phase, expected_size])
		return false
	return true

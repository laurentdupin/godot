extends SceneTree

func _initialize() -> void:
	var use_d3d12 := OS.get_cmdline_user_args().has("--d3d12")
	var use_vulkan := OS.get_cmdline_user_args().has("--vulkan")
	var use_metal := OS.get_cmdline_user_args().has("--metal")
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><head><style>html, body { margin: 0; background: transparent; } .glass { position: absolute; left: 40px; top: 30px; width: 160px; height: 90px; border-radius: 12px; backdrop-filter: invert(1); }</style></head><body><div id=\"glass\" class=\"glass\"></div></body></html>"
	document.resource_root = "res://"
	document.background_color = Color(0, 0, 0, 0)

	var target := HTMLRenderTarget.new()
	root.add_child(target)
	target.backend_preference = HTMLView.BACKEND_D3D12 if use_d3d12 else (HTMLView.BACKEND_VULKAN if use_vulkan else (HTMLView.BACKEND_METAL if use_metal else HTMLView.BACKEND_CPU))
	target.size = Vector2i(320, 180)
	target.backdrop_filter_enabled = true
	target.document = document
	target.render_now()
	if not await _wait_for_initial_frame(target):
		target.free()
		quit(1)
		return
	if not _validate_frame(target, Vector2i(320, 180), "initial"):
		target.free()
		quit(1)
		return

	# Recreate all size-dependent output while retaining the logical document and
	# backdrop metadata configuration. The replacement is asynchronous: every
	# observable state must be either the complete old generation or the complete
	# new generation, never a new texture paired with stale metadata (or vice versa).
	if target.set_element_style("glass", "position:absolute;left:20px;top:15px;width:120px;height:70px;border-radius:8px;backdrop-filter:invert(1)") != OK:
		push_error("HCSR rejected the packet-metadata mutation.")
		target.free()
		quit(1)
		return
	target.size = Vector2i(240, 140)
	target.render_now()
	if not await _wait_for_resized_frame(target):
		target.free()
		quit(1)
		return

	print("Static HCSR Godot %s backdrop metadata and resize smoke passed." % ("D3D12" if use_d3d12 else ("Vulkan" if use_vulkan else ("Metal" if use_metal else "CPU"))))
	target.free()
	quit()

func _wait_for_initial_frame(target: HTMLRenderTarget) -> bool:
	for _frame in range(120):
		await process_frame
		if target.get_texture() != null and target.get_backdrop_filter_regions().size() == 1:
			return true
	push_error("HCSR did not asynchronously activate the initial backdrop packet within 120 frames.")
	return false

func _validate_frame(target: HTMLRenderTarget, expected_size: Vector2i, phase: String, expected_bounds := Rect2(40, 30, 160, 90)) -> bool:
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
	if not bounds.is_equal_approx(expected_bounds):
		push_error("HCSR %s frame reported incorrect backdrop bounds: %s." % [phase, bounds])
		return false
	var texture := target.get_texture()
	if texture == null or texture.get_width() != expected_size.x or texture.get_height() != expected_size.y:
		push_error("HCSR %s frame did not recreate the expected %s output texture." % [phase, expected_size])
		return false
	return true

func _wait_for_resized_frame(target: HTMLRenderTarget) -> bool:
	for attempt in 120:
		target.render_now()
		var texture := target.get_texture()
		var regions := target.get_backdrop_filter_regions()
		if texture == null or regions.size() != 1:
			push_error("HCSR exposed an incomplete texture/metadata generation while resizing.")
			return false
		var texture_size := Vector2i(texture.get_width(), texture.get_height())
		var bounds: Rect2 = regions[0].get("bounds", Rect2())
		if texture_size == Vector2i(320, 180):
			if not bounds.is_equal_approx(Rect2(40, 30, 160, 90)):
				push_error("HCSR mixed the old texture with new packet metadata: %s." % bounds)
				return false
		elif texture_size == Vector2i(240, 140):
			return _validate_frame(target, texture_size, "resized", Rect2(20, 15, 120, 70))
		else:
			push_error("HCSR exposed an unexpected intermediate texture size: %s." % texture_size)
			return false
		await process_frame
	push_error("HCSR did not asynchronously activate the resized packet within 120 frames.")
	return false

extends SceneTree

func _initialize() -> void:
	_run.call_deferred()

func _status(surface: Node) -> Dictionary:
	if surface is HTMLView:
		return surface.get_frame_scheduler_diagnostics()["frame_synchronization"]
	return surface.get_frame_synchronization()

func _run() -> void:
	var doc := HTMLDocument.new()
	doc.html = "<main id='app'></main>"
	for kind in range(2):
		var surface: Node = HTMLView.new() if kind == 0 else HTMLRenderTarget.new()
		surface.document = doc
		surface.size = Vector2(320, 180) if kind == 0 else Vector2i(320, 180)
		root.add_child(surface)
		for frame in range(6):
			await process_frame
		var original := "<input id='field' value='old'><div>Before</div>"
		var preload_id: int = surface.preload_page(original)
		if preload_id == 0:
			_fail("Preload returned zero")
			return
		for step in range(3):
			var current := original if step == 0 else original.replace("old", "new").replace("Before", "After")
			var error: Error = surface.set_element_inner_html_with_preload("app", current, preload_id)
			if step == 2 and surface.unload_page(preload_id) != OK:
				_fail("Unload failed")
				return
			if error != OK:
				_fail("Mutation failed")
				return
			await process_frame
			await RenderingServer.frame_post_draw
			var usage: Dictionary = _status(surface)["preload_usage"]
			var form: Dictionary = surface.get_form_control_state("field")
			if form.get("value", "") != ("old" if step == 0 else "new"):
				_fail("Current form value was not used")
				return
			if (step < 2 and int(usage["reused_token_count"]) <= 0) or (step == 2 and int(usage["reused_token_count"]) != 0):
				_fail("Unexpected reuse/fallback")
				return
			if bool(usage["exact_source"]) != (step == 0):
				_fail("Unexpected exact-source status")
				return
		# Exercise automatic release of a live preparation during surface teardown.
		if surface.preload_page(original) == 0:
			_fail("Second preload failed")
			return
		surface.free()
		await process_frame
	print("GODOT_PAGE_PRELOAD_OK surfaces=2 exact=true partial=true unload_before_step=true current_values=true")
	quit(0)

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

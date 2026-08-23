extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var host := Control.new()
	host.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	root.add_child(host)
	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_D3D12
	view.logical_size = Vector2i(320, 180)
	view.size = Vector2(320, 180)
	view.html = "<html><body style='margin:0'><div style='width:200px;height:100px;background:#20c060'></div></body></html>"
	host.add_child(view)
	var output := view.create_output(Vector2i(640, 360))
	if output == null:
		return _fail("Could not create the secondary output.")
	if not await _wait_pair(view, output, 0):
		return
	var stable_texture := output.texture
	output.size = Vector2i(960, 540)
	if not await _wait_pair(view, output, output.generation):
		return
	if output.texture != stable_texture or Vector2i(output.texture.get_width(), output.texture.get_height()) != Vector2i(960, 540):
		return _fail("The resized output did not preserve its stable texture proxy and requested size.")
	output.release()
	for _frame in range(120):
		await process_frame
	view.queue_free()
	host.queue_free()
	await process_frame
	print("HCSR runtime v9 multi-output resize probe passed.")
	quit(0)

func _wait_pair(view: HTMLView, output: HTMLViewOutput, after: int) -> bool:
	for _frame in range(480):
		await process_frame
		if view.get_generation() > after and output.generation == view.get_generation():
			return true
	_fail("Timed out waiting for a coherent resized output after generation %d; primary=%d secondary=%d size=%s." % [after, view.get_generation(), output.generation, output.size])
	return false

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

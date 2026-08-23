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
	view.html = "<html><head><style>html,body{margin:0}.card{width:180px;height:90px;background:#20c060}.card.hot{background:#e03030}</style></head><body><div id='card' class='card'></div></body></html>"
	host.add_child(view)
	var outputs: Array[HTMLViewOutput] = [
		view.create_output(Vector2i(320, 180)),
		view.create_output(Vector2i(640, 360)),
		view.create_output(Vector2i(853, 480)),
		view.create_output(Vector2i(640, 360), true),
	]
	for index in outputs.size():
		root.set_meta(StringName("hcsr_atomic_output_%d" % index), outputs[index])
	if not await _wait_group(view, outputs, 0):
		return
	var before := view.get_generation()
	if view.set_element_attribute(&"card", &"class", "card hot") != OK or not await _wait_group(view, outputs, before):
		return _fail("Retained mutation did not publish atomically to all output resolutions.")
	before = view.get_generation()
	outputs[1].size = Vector2i(960, 540)
	if not await _wait_group(view, outputs, before):
		return _fail("A differently sized output did not reconfigure atomically.")
	if outputs[1].texture == null or Vector2i(outputs[1].texture.get_width(), outputs[1].texture.get_height()) != Vector2i(960, 540):
		return _fail("The resized output did not expose its requested dimensions.")
	if outputs[3].texture == null or not outputs[3].texture.get_image().has_mipmaps():
		return _fail("The mipmapped output did not expose a generated mip chain.")
	print("HCSR runtime v9 atomic mixed-resolution output probe passed.")
	for output in outputs:
		output.release()
	for _frame in range(120):
		await process_frame
	view.queue_free()
	await process_frame
	quit(0)

func _wait_group(view: HTMLView, outputs: Array[HTMLViewOutput], after: int) -> bool:
	for _frame in range(600):
		await process_frame
		var generation := view.get_generation()
		var exact := generation > after
		for output in outputs:
			exact = exact and output != null and output.generation == generation
		if exact:
			return true
	var observed: Array[int] = []
	for output in outputs:
		observed.append(output.generation if output != null else -1)
	push_error("Generation wait expired: primary=%d outputs=%s after=%d" % [view.get_generation(), observed, after])
	return false

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

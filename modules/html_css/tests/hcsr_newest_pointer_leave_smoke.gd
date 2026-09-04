extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var document := HTMLDocument.new()
	document.html = """<html><head><style>
		html,body{margin:0;width:100%;height:100%}
		#target{position:absolute;left:50px;top:35px;width:100px;height:70px;background:black}
		#target:hover{transform:scale(1.35)}
	</style></head><body><button id='target'>Hover</button></body></html>"""
	var view := HTMLView.new()
	var arguments := OS.get_cmdline_user_args()
	view.backend_preference = HTMLView.BACKEND_VULKAN if arguments.has("--vulkan") \
			else HTMLView.BACKEND_D3D12
	view.size = Vector2(220, 140)
	view.logical_size = Vector2i(220, 140)
	view.document = document
	root.add_child(view)
	for _frame in range(8): await process_frame
	var baseline := await _capture()
	_send_motion(Vector2(100, 70))
	for _frame in range(4): await process_frame
	var hovered := await _capture()
	view.notification(Control.NOTIFICATION_MOUSE_EXIT_SELF)
	for _frame in range(4): await process_frame
	var left := await _capture()
	if baseline == null or hovered == null or left == null:
		_fail("Could not capture pointer-leave frames.")
		return
	if baseline.get_data() == hovered.get_data():
		_fail("Hover transform did not become visible.")
		return
	if baseline.get_data() != left.get_data():
		baseline.save_png("C:/Users/Frere/AppData/Local/Temp/hcsr-pointer-baseline.png")
		hovered.save_png("C:/Users/Frere/AppData/Local/Temp/hcsr-pointer-hovered.png")
		left.save_png("C:/Users/Frere/AppData/Local/Temp/hcsr-pointer-left.png")
		_fail("Pointer leave did not return to the exact baseline frame.")
		return
	print("HCSR newest pointer-leave hover reset passed on %s." % \
			RenderingServer.get_current_rendering_driver_name())
	quit(0)

func _send_motion(position: Vector2) -> void:
	var motion := InputEventMouseMotion.new()
	motion.position = position
	motion.global_position = position
	root.push_input(motion, true)

func _capture() -> Image:
	RenderingServer.force_draw(true)
	await RenderingServer.frame_post_draw
	return root.get_texture().get_image()

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

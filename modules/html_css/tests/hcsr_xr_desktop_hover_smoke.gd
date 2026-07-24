extends SceneTree

const DESKTOP_SIZE := Vector2i(800, 600)
const XR_RENDER_SIZE := Vector2i(480, 720)
const BUTTON_RECT := Rect2i(500, 220, 180, 80)

class MockXRInterface extends XRInterfaceExtension:
	var initialized := true

	func _get_name() -> StringName:
		return &"HTMLViewDesktopInputMockXR"

	func _get_capabilities() -> int:
		return XRInterface.XR_STEREO

	func _is_initialized() -> bool:
		return initialized

	func _initialize() -> bool:
		initialized = true
		return true

	func _uninitialize() -> void:
		initialized = false

	func _get_render_target_size() -> Vector2:
		return Vector2(XR_RENDER_SIZE)

	func _get_view_count() -> int:
		return 2

	func _get_camera_transform() -> Transform3D:
		return Transform3D.IDENTITY

	func _get_transform_for_view(_view: int, camera_transform: Transform3D) -> Transform3D:
		return camera_transform

	func _get_projection_for_view(_view: int, _aspect: float, _z_near: float, _z_far: float) -> PackedFloat64Array:
		return PackedFloat64Array([1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0])


func _initialize() -> void:
	root.size = DESKTOP_SIZE
	root.content_scale_mode = Window.CONTENT_SCALE_MODE_DISABLED

	var container := SubViewportContainer.new()
	container.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	container.stretch = false
	container.mouse_filter = Control.MOUSE_FILTER_STOP
	root.add_child(container)

	var html_viewport := SubViewport.new()
	html_viewport.size = DESKTOP_SIZE
	html_viewport.transparent_bg = false
	html_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	container.add_child(html_viewport)

	var document := HTMLDocument.new()
	document.html = """<!DOCTYPE html><html><head><style>
html,body{margin:0;width:100%;height:100%;background:#101820}
#target{position:absolute;left:500px;top:220px;width:180px;height:80px;border:0;background:#34506a}
#target:hover{background:#f07828}
</style></head><body><button id='target' data-godot-action='target'>Hover</button></body></html>"""
	var view := HTMLView.new()
	if OS.get_cmdline_user_args().has("--cpu"):
		view.backend_preference = HTMLView.BACKEND_CPU
	elif OS.get_cmdline_user_args().has("--vulkan"):
		view.backend_preference = HTMLView.BACKEND_VULKAN
	elif OS.get_cmdline_user_args().has("--metal"):
		view.backend_preference = HTMLView.BACKEND_METAL
	else:
		view.backend_preference = HTMLView.BACKEND_D3D12
	view.size = DESKTOP_SIZE
	view.document = document
	html_viewport.add_child(view)

	await _settle(5)
	var desktop_logical_size := root.get_visible_rect().size
	var desktop_container_size := container.size
	var sample := BUTTON_RECT.position + Vector2i(12, 12)
	_send_motion(html_viewport, sample)
	await _settle(4)
	var non_xr_image := html_viewport.get_texture().get_image()
	if non_xr_image == null or not _pixel_is_close(non_xr_image.get_pixelv(sample), Color("f07828")):
		_fail("Desktop mouse motion did not publish the non-XR HTML :hover frame (cursor=%s pixel=%s)." % [root.get_mouse_position(), non_xr_image.get_pixelv(sample) if non_xr_image != null else Color()])
		return
	_send_motion(html_viewport, Vector2(20, 20))
	await _settle(3)

	var mock_xr := MockXRInterface.new()
	XRServer.add_interface(mock_xr)
	XRServer.primary_interface = mock_xr
	root.use_xr = true
	await _settle(3)

	if root.get_visible_rect().size != desktop_logical_size:
		_fail("XR replaced desktop 2D layout metrics: expected %s, got %s." % [desktop_logical_size, root.get_visible_rect().size])
		return
	if container.size != desktop_container_size:
		_fail("XR resized the desktop SubViewportContainer to %s instead of %s." % [container.size, desktop_container_size])
		return

	_send_motion(html_viewport, sample)
	await _settle(5)

	var xr_image := html_viewport.get_texture().get_image()
	if xr_image == null or not _pixel_is_close(xr_image.get_pixelv(sample), Color("f07828")):
		_fail("Desktop mouse motion did not publish HTML :hover while the root viewport used XR rendering.")
		return

	root.size = Vector2i(900, 650)
	await _settle(3)
	if root.get_visible_rect().size == desktop_logical_size or root.get_visible_rect().size == Vector2(XR_RENDER_SIZE):
		_fail("Desktop 2D metrics did not follow a window resize while XR retained its independent render target: %s." % root.get_visible_rect().size)
		return
	if container.size != root.get_visible_rect().size:
		_fail("The desktop SubViewportContainer did not follow resized desktop 2D metrics while XR was active.")
		return

	root.use_xr = false
	XRServer.primary_interface = null
	XRServer.remove_interface(mock_xr)
	print("HCSR Godot XR/desktop mouse-motion hover smoke passed.")
	quit()


func _settle(frame_count: int) -> void:
	for _frame in range(frame_count):
		await process_frame
		RenderingServer.force_draw(true)


func _send_motion(viewport: SubViewport, position: Vector2) -> void:
	var motion := InputEventMouseMotion.new()
	motion.position = position
	motion.global_position = position
	viewport.notification(Node.NOTIFICATION_VP_MOUSE_ENTER)
	viewport.push_input(motion, true)


func _pixel_is_close(actual: Color, expected: Color) -> bool:
	return abs(actual.r - expected.r) < 0.04 and abs(actual.g - expected.g) < 0.04 and abs(actual.b - expected.b) < 0.04


func _fail(message: String) -> void:
	push_error(message)
	quit(1)

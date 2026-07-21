extends SceneTree

var backend_preference := HTMLView.BACKEND_CPU
var backend_name := "CPU"


func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--d3d12"):
		backend_preference = HTMLView.BACKEND_D3D12
		backend_name = "D3D12"
	elif OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	elif OS.get_cmdline_user_args().has("--metal"):
		backend_preference = HTMLView.BACKEND_METAL
		backend_name = "Metal"

	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	viewport.disable_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(viewport)

	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><style>html,body{margin:0;background:#102030;color:white;font:24px sans-serif}</style><body>Device scale</body></html>"
	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.fixed_viewport_size = Vector2i(320, 180)
	view.fixed_viewport_device_scale_factor = 2.0
	view.size = Vector2(320, 180)
	view.document = document
	viewport.add_child(view)
	if view.texture_filter != CanvasItem.TEXTURE_FILTER_LINEAR:
		push_error("HTMLView does not use linear filtering for non-1:1 physical composition.")
		quit(1)
		return

	for _frame in range(8):
		await process_frame
	var texture := view.get_texture()
	if texture == null or texture.get_size() != Vector2(640, 360):
		push_error("Fixed HTMLView 2x device scale did not produce a 640x360 physical surface.")
		quit(1)
		return
	if not view.local_to_html_position(Vector2(160, 90)).is_equal_approx(Vector2(160, 90)):
		push_error("Fixed HTMLView device scale changed logical input coordinates.")
		quit(1)
		return

	view.fixed_viewport_device_scale_factor = 1.5
	for _frame in range(8):
		await process_frame
	texture = view.get_texture()
	if texture == null or texture.get_size() != Vector2(480, 270):
		push_error("A later fixed HTMLView device-scale change did not reach HCSR viewport metrics.")
		quit(1)
		return

	print("HCSR fixed viewport device-scale smoke passed on %s." % backend_name)
	quit()

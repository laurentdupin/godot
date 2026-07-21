extends SceneTree

const PNG_BASE64 := "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl2nJ8AAAAASUVORK5CYII="

var backend_preference := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"

func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	elif OS.get_cmdline_user_args().has("--metal"):
		backend_preference = HTMLView.BACKEND_METAL
		backend_name = "Metal"
	elif OS.get_cmdline_user_args().has("--cpu"):
		backend_preference = HTMLView.BACKEND_CPU
		backend_name = "CPU"

	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.disable_3d = true
	viewport.transparent_bg = false
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(viewport)

	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><head><style>html,body{margin:0;background:#112233}img{display:block;width:32px;height:32px;object-fit:contain}</style></head><body><img src='data:image/png;base64,%s' alt=''></body></html>" % PNG_BASE64
	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.fixed_viewport_size = Vector2i(64, 64)
	view.size = Vector2(64, 64)
	view.document = document
	viewport.add_child(view)

	for _frame in range(12):
		await process_frame
		RenderingServer.force_draw(false)
	await RenderingServer.frame_post_draw
	var image := view.get_texture().get_image()
	if image == null or image.is_empty() or image.get_pixel(16, 16).is_equal_approx(Color("112233")):
		push_error("HCSR Godot %s data-URI PNG was not painted." % backend_name)
		quit(1)
		return

	print("HCSR Godot %s data-URI image smoke passed." % backend_name)
	quit()

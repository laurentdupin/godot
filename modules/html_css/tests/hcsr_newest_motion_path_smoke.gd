extends SceneTree

var failed := false

func require(value: bool, label: String) -> void:
	if not value:
		failed = true
		push_error("MOTION_PATH_FAILED " + label)

func _initialize() -> void:
	run.call_deferred()
	create_timer(45).timeout.connect(func(): quit(2))

func settle() -> void:
	for i in 5:
		await process_frame
		await RenderingServer.frame_post_draw

func run() -> void:
	var view := HTMLView.new()
	view.size = Vector2(200,120)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.logical_size = Vector2i(200,120)
	view.backend_preference = HTMLView.BACKEND_GPU_AUTO
	var doc := HTMLDocument.new()
	doc.html = "<!doctype html><style>body{margin:0}#clip{position:absolute;left:20px;top:20px;width:120px;height:80px;overflow:hidden}#target{position:absolute;left:10px;top:10px;width:20px;height:20px;background:red;offset-anchor:0 0;offset-rotate:0deg;offset-path:path('M0 0H200')}</style><div id='clip'><div id='target'></div></div>"
	view.document = doc
	root.add_child(view)
	var output := view.create_output(Vector2i(200,120),true)
	await settle()
	var image: Image = output.texture.get_image()
	require(image.get_pixel(35,35).r>.95 && image.get_pixel(135,35).a<.01,"initial path placement")
	var stats: Dictionary = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
	var uploaded := int(stats.uploaded_bytes)
	var surfaces := int(stats.raster_surfaces)
	require(view.set_element_attribute("target","style","offset-distance:100px")==OK,"move along path")
	await settle()
	image = output.texture.get_image()
	require(image.get_pixel(135,35).r>.95 && image.get_pixel(35,35).a<.01,"updated path placement")
	require(image.get_pixel(145,35).a<.01,"ancestor clips moved surface")
	stats = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
	require(int(stats.uploaded_bytes)==uploaded && int(stats.raster_surfaces)==surfaces,"path movement reuses atlas pixels")
	require(view.set_element_attribute("target","style","offset-path:none")==OK,"remove path")
	await settle()
	image = output.texture.get_image()
	require(image.get_pixel(35,35).r>.95 && image.get_pixel(135,35).a<.01,"path removal restores placement")
	require(int(view.get_frame_scheduler_diagnostics().frame_synchronization.failures)==0,"frame preparation")
	output.release()
	view.queue_free()
	await settle()
	print("MOTION_PATH_", "FAILED" if failed else "OK")
	quit(1 if failed else 0)

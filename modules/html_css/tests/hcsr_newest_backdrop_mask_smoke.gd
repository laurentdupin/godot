extends SceneTree

var failed := false
func require(value: bool, label: String) -> void:
	if not value:
		failed = true
		push_error("BACKDROP_MASK_FAILED " + label)

func _initialize() -> void:
	run.call_deferred()
	create_timer(30).timeout.connect(func(): quit(2))

func settle() -> void:
	for i in 8:
		await process_frame
		await RenderingServer.frame_post_draw

func run() -> void:
	root.size = Vector2i(320,180)
	var background := ColorRect.new()
	background.size = Vector2(320,180)
	background.color = Color(.1,.8,.2,1)
	root.add_child(background)
	var view := HTMLView.new()
	view.size = Vector2(320,180)
	view.backend_preference = HTMLView.BACKEND_CPU if "--cpu" in OS.get_cmdline_user_args() else HTMLView.BACKEND_GPU_AUTO
	view.backdrop_filter_enabled = true
	var document := HTMLDocument.new()
	document.background_color = Color.TRANSPARENT
	document.html = "<style>html,body{margin:0;background:transparent}#clip{position:absolute;left:40px;top:30px;width:160px;height:90px;overflow:hidden;border-radius:30px}#glass{width:160px;height:90px;backdrop-filter:invert(1);border-radius:20px}</style><div id='clip'><div id='glass'></div></div>"
	view.document = document
	root.add_child(view)
	await settle()
	var picture := root.get_texture().get_image()
	require(view.get_backdrop_filter_regions().size()==1,"effect metadata")
	var mask_frame := view.get_backdrop_filter_frame()
	require(mask_frame.valid && mask_frame.generation == mask_frame.main_target_generation,"coherent mask frame")
	var mask: Texture2D = mask_frame.mask_texture
	require(mask.get_image().get_pixel(100,75).g>.99 && mask.get_image().get_pixel(42,32).g<.01,"mask coverage readback")
	require(picture.get_pixel(100,75).r>.85 && picture.get_pixel(100,75).g<.25,"filtered interior")
	require(picture.get_pixel(42,32).g>.75,"rounded clipped corner remains backdrop")
	require(picture.get_pixel(30,75).g>.75,"outside mask")
	require(view.set_element_attribute("glass","style","background:rgba(0,0,255,.5)")==OK,"translucent foreground")
	await settle()
	require(view.get_backdrop_filter_frame().mask_texture == mask,"foreground-only mutation reuses mask")
	var foreground := root.get_texture().get_image().get_pixel(100,75)
	require(abs(foreground.r-.45)<.03 && abs(foreground.g-.1)<.03 && abs(foreground.b-.9)<.03,"premultiplied foreground over filtered backdrop: %s" % foreground)
	require(view.set_element_attribute("glass","style","border:8px solid rgba(0,0,255,.5)")==OK,"translucent border")
	await settle()
	var border := root.get_texture().get_image().get_pixel(100,34)
	require(abs(border.r-.45)<.03 && abs(border.g-.1)<.03 && abs(border.b-.9)<.03,"premultiplied border over filtered backdrop: %s" % border)
	require(view.set_element_attribute("glass","style","transform:translateX(80px)")==OK,"move filter")
	await settle()
	picture = root.get_texture().get_image()
	require(picture.get_pixel(70,75).g>.75 && picture.get_pixel(150,75).r>.85,"transformed mask")
	require(picture.get_pixel(220,75).g>.75,"ancestor clip after transform")
	view.size = Vector2(240,140)
	await settle()
	require(view.get_backdrop_filter_regions().size()==1,"resized mask metadata")
	require(view.set_element_attribute("glass","style","backdrop-filter:none")==OK,"remove filter")
	await settle()
	require(view.get_backdrop_filter_regions().is_empty(),"stale mask removed")
	require(not view.get_backdrop_filter_frame().valid,"removed mask not published")
	require(root.get_texture().get_image().get_pixel(100,75).g>.75,"original backdrop restored")
	print("BACKDROP_MASK_", "FAILED" if failed else "OK")
	quit(1 if failed else 0)

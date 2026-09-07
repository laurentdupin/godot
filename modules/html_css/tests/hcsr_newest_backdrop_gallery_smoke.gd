extends SceneTree

func _initialize() -> void:
	run.call_deferred()
	create_timer(30).timeout.connect(func(): quit(2))

func run() -> void:
	change_scene_to_file("res://main.tscn")
	for i in 20:
		await process_frame
		await RenderingServer.frame_post_draw
	var views := root.find_children("*","HTMLView",true,false)
	if views.size()!=1:
		push_error("BACKDROP_GALLERY missing HTMLView")
		quit(1)
		return
	var frame: Dictionary = views[0].get_backdrop_filter_frame()
	if not frame.valid or frame.effects.size()!=8:
		push_error("BACKDROP_GALLERY missing eight effect masks")
		quit(1)
		return
	var image: Image = frame.mask_texture.get_image()
	var ids := {}
	for y in range(0,image.get_height(),4):
		for x in range(0,image.get_width(),4):
			var pixel := image.get_pixel(x,y)
			if pixel.g>.5:
				ids[int(round(pixel.r*255))] = true
	if ids.size()!=8:
		push_error("BACKDROP_GALLERY coverage IDs incomplete: %s" % ids)
		quit(1)
		return
	print("BACKDROP_GALLERY_OK effects=8 mask_ids=8")
	quit()

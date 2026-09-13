extends SceneTree

var failed := false
func require(value: bool, label: String) -> void:
    if not value:
        failed = true
        push_error("RASTER_SURFACE_FAILED " + label)

func _initialize() -> void:
    run.call_deferred()
    create_timer(45).timeout.connect(func(): quit(2))

func settle() -> void:
    for i in 5:
        await process_frame
        await RenderingServer.frame_post_draw

func run() -> void:
    var view := HTMLView.new()
    view.size = Vector2(180,100)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.logical_size = Vector2i(180,100)
    view.backend_preference = HTMLView.BACKEND_CPU if "--cpu" in OS.get_cmdline_user_args() else HTMLView.BACKEND_GPU_AUTO
    var doc := HTMLDocument.new()
    doc.html = "<style>html,body{margin:0}#a,#b{position:absolute;top:10px;width:40px;height:30px;background:linear-gradient(90deg,red,blue);box-shadow:2px 3px 0 red}#a{left:10px}#b{left:80px}</style><div id='a'></div><div id='b'></div>"
    view.document = doc
    root.add_child(view)
    var output := view.create_output(Vector2i(180,100),true)
    await settle()
    var image: Image = output.texture.get_image()
    require(image.get_pixel(12,20).r > .85 && image.get_pixel(47,20).b > .85,"BGRA gradient orientation")
    require(image.get_pixel(30,42).r > .95,"outer shadow")
    var stats: Dictionary = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
    require(int(stats.decoded_images)==0 && int(stats.raster_surfaces)==2,"shared generated pixels bypass image decoding: " + str(stats))
    var uploaded := int(stats.uploaded_bytes)
    require(view.set_element_attribute("a","style","left:25px;opacity:.5")==OK,"move")
    await settle()
    image = output.texture.get_image()
    require(abs(image.get_pixel(30,20).a-.5)<.03 && image.get_pixel(12,20).a<.01,"move and group opacity")
    stats = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
    require(int(stats.raster_surfaces)==2 && int(stats.uploaded_bytes)==uploaded,"movement reuses atlas pixels")
    require(view.set_element_attribute("a","style","background:linear-gradient(90deg,lime,blue);box-shadow:none")==OK,"change appearance")
    await settle()
    image = output.texture.get_image()
    require(image.get_pixel(12,20).g>.85 && image.get_pixel(82,20).r>.85,"changed appearance leaves other instance unchanged")
    output.size = Vector2i(360,200)
    await settle()
    image = output.texture.get_image()
    require(image.get_pixel(24,40).g>.85 && image.get_pixel(164,40).r>.85,"secondary resolution")
    require(int(view.get_frame_scheduler_diagnostics().frame_synchronization.failures)==0,"frame preparation")
    output.release()
    view.queue_free()
    await settle()
    print("RASTER_SURFACE_", "FAILED" if failed else "OK")
    quit(1 if failed else 0)

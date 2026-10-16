extends SceneTree

var failed := false

func _initialize() -> void:
    _run.call_deferred()
    create_timer(45).timeout.connect(func(): quit(2))

func _check(value: bool, message: String) -> void:
    if not value:
        failed = true
        push_error(message)

func _settle() -> void:
    for frame in 10:
        await process_frame
        await RenderingServer.frame_post_draw

func _image(width: int, height: int, top: int) -> String:
    var svg := "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d'><rect width='100%%' height='100%%' fill='#e05030'/></svg>" % [width,height]
    return "<img style='position:absolute;left:300px;top:%dpx;width:%dpx;height:%dpx' src='data:image/svg+xml,%s'>" % [top,width,height,svg.uri_encode()]

func _run() -> void:
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    Engine.max_fps = 240
    var view := HTMLView.new()
    view.size = Vector2(800,600)
    view.logical_size = Vector2i(800,600)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.backend_preference = HTMLView.BACKEND_GPU_AUTO
    view.html = """<!doctype html><style>html,body{margin:0;overflow:hidden}
        #stable{position:absolute;left:12px;top:12px;width:200px;height:120px;background:linear-gradient(35deg,#12abef,#853912);border:2px solid white;border-radius:13px;color:white;font:18px Arial}
        </style><div id='stable'>Stable atlas pixels</div><div id='mutations'></div>"""
    root.add_child(view)
    var output := view.create_output(Vector2i(800,600),false)
    await _settle()
    var before := output.texture.get_image().get_region(Rect2i(0,0,240,160))
    var stats: Dictionary = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
    _check(int(stats.gpu_atlas_bytes) <= 1024*1024*4+4,"Small view starts with 4 MiB atlas")
    _check(int(stats.uploaded_bytes) < 1024*1024,"Initial upload contains live pixels, not a full page")
    for extent in [Vector2i(1600,256),Vector2i(3000,1800)]:
        _check(view.set_element_inner_html("mutations",_image(extent.x,extent.y,250)) == OK,"Grow atlas via new image")
        await _settle()
        var after := output.texture.get_image().get_region(Rect2i(0,0,240,160))
        _check(before.get_data() == after.get_data(),"Growth preserves existing atlas pixels and coordinates")
        stats = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
        var expected := 2048 if extent.x == 1600 else 4096
        _check(int(stats.gpu_atlas_bytes) >= expected*expected*4,"Page reached expected growth size")
        print("ATLAS_GROWTH size=",extent," gpu_atlas_bytes=",stats.gpu_atlas_bytes," uploaded_bytes=",stats.uploaded_bytes)
    output.release()
    view.queue_free()
    await _settle()
    print("ATLAS_GROWTH_", "FAILED" if failed else "PASS")
    quit(1 if failed else 0)

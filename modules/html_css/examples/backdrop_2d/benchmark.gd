extends SceneTree
func _initialize():
    call_deferred("run")
    create_timer(45).timeout.connect(func(): quit(2))
func run():
    Engine.max_fps = 0
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    var requested_size = OS.get_environment("GALLERY_RENDER_SIZE").split("x")
    if requested_size.size() == 2:
        DisplayServer.window_set_size(Vector2i(int(requested_size[0]), int(requested_size[1])))
    var scene = load("res://main.tscn").instantiate()
    root.add_child(scene)
    await process_frame
    var view = scene.get_node("BackdropHTML")
    var logical_size = OS.get_environment("GALLERY_FIXED_LOGICAL_SIZE").split("x")
    if logical_size.size() == 2:
        view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
        view.logical_size = Vector2i(int(logical_size[0]), int(logical_size[1]))
    var test_case = OS.get_environment("GALLERY_CASE")
    if test_case.is_empty(): test_case = "original"
    if test_case == "no_html":
        view.visible = false
    elif test_case == "no_filters":
        view.document.css = ".card {backdrop-filter:none !important;}"
    elif test_case == "colors_only":
        view.document.css = ".blur {backdrop-filter:none !important;} .combined {backdrop-filter:contrast(135%) saturate(180%) sepia(30%) !important;}"
    elif test_case == "blur_only":
        view.document.css = ".contrast,.sepia,.saturate,.brightness,.grayscale,.invert {backdrop-filter:none !important;}"
    for i in 90:
        await process_frame
        await RenderingServer.frame_post_draw
    var ready_state = view.get_frame_scheduler_diagnostics().get("frame_synchronization", {})
    if test_case != "no_html" and (ready_state.get("terminal", false) or view.get_generation() == 0):
        push_error("Gallery benchmark has no valid HTML rendering")
        quit(1)
        return
    if OS.get_environment("GALLERY_CAPTURE") == "1":
        for child in scene.get_children():
            if child is Control and child != view:
                child.set_process(false)
                child.elapsed = 1.25
                child.queue_redraw()
        for i in 10:
            await process_frame
            await RenderingServer.frame_post_draw
        root.get_texture().get_image().save_png(OS.get_environment("GALLERY_OUTPUT")+".png")
    var samples = []
    var begin = Time.get_ticks_usec()
    var previous = begin
    var initial = view.get_frame_scheduler_diagnostics()
    for i in 600:
        await process_frame
        await RenderingServer.frame_post_draw
        var now = Time.get_ticks_usec()
        samples.append((now-previous)/1000.0)
        previous=now
    var elapsed = (Time.get_ticks_usec()-begin)/1000.0
    samples.sort()
    var final_state = view.get_frame_scheduler_diagnostics()
    if final_state.get("frame_synchronization", {}).get("terminal", false):
        push_error("Gallery renderer failed during measurement")
        quit(1)
        return
    var result = {"case":test_case,"frames":600,"fps":600000.0/elapsed,"elapsed_ms":elapsed,"median_ms":samples[300],"p95_ms":samples[570],"max_ms":samples[599],"initial":initial,"final":final_state}
    print("GALLERY_RESULT|"+JSON.stringify(result))
    var output = OS.get_environment("GALLERY_OUTPUT")
    if not output.is_empty():
        var file = FileAccess.open(output,FileAccess.WRITE)
        file.store_string(JSON.stringify(result,"\t"))
    quit()

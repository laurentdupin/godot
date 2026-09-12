extends SceneTree
var failed := false
func _initialize() -> void:
    run.call_deferred()
    create_timer(40).timeout.connect(func(): quit(2))
func require(value: bool, message: String) -> void:
    if not value:
        failed = true
        push_error("GROUP_OPACITY_FAILED " + message)
func settle() -> void:
    for i in 8:
        await process_frame
        await RenderingServer.frame_post_draw
func run() -> void:
    var document := HTMLDocument.new()
    document.html = FileAccess.get_file_as_string(OS.get_environment("HCSR_GROUP_FIXTURE"))
    var views: Array[HTMLView] = []
    for backend in [HTMLView.BACKEND_CPU,HTMLView.BACKEND_GPU_AUTO]:
        var view := HTMLView.new()
        view.backend_preference = backend
        view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
        view.logical_size = Vector2i(480,240)
        view.size = Vector2(480,240)
        view.document = document
        root.add_child(view)
        views.append(view)
    await settle()
    var out := OS.get_environment("HCSR_GROUP_OUTPUT")
    if not out.is_empty(): DirAccess.make_dir_recursive_absolute(out)
    for i in views.size():
        var image := views[i].get_texture().get_image()
        if not out.is_empty(): image.save_png(out.path_join("cpu.png" if i==0 else "gpu.png"))
        var overlap := image.get_pixel(70,70)
        require(abs(overlap.r-.5)<.012 and abs(overlap.g-.5)<.012 and overlap.b>.99,"overlap " + str(overlap))
        var nested := image.get_pixel(230,70)
        require(abs(nested.r-.75)<.012 and abs(nested.g-.5)<.012 and abs(nested.b-.75)<.012,"nested " + str(nested))
        require(image.get_pixel(341,21).r>.99 and image.get_pixel(341,21).g>.99,"rounded group corner")
    var outputs: Array = []
    for view in views: outputs.append(view.create_output(Vector2i(960,480),false))
    await settle()
    for output in outputs:
        var pixel: Color = output.texture.get_image().get_pixel(140,140)
        require(abs(pixel.r-.5)<.012 and abs(pixel.g-.5)<.012 and pixel.b>.99,"secondary group output")
    var allocations: int = views[1].get_frame_scheduler_diagnostics().frame_synchronization.image_atlas.opacity_target_allocations
    for value in [.75,.5]:
        for view in views: require(view.set_element_attribute("overlap","style","opacity:"+str(value)) == OK,"opacity update")
        await settle()
    require(views[1].get_frame_scheduler_diagnostics().frame_synchronization.image_atlas.opacity_target_allocations == allocations,"different output sizes reuse group targets")
    for view in views: require(view.set_element_attribute("overlap","style","opacity:1") == OK,"opacity mutation")
    await settle()
    for view in views:
        var pixel := view.get_texture().get_image().get_pixel(70,70)
        require(pixel.b>.99 and pixel.r<.01,"group removal " + str(pixel))
        view.queue_free()
    await settle()
    print("GROUP_OPACITY_", "FAILED" if failed else "OK")
    quit(1 if failed else 0)

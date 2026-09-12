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
    var stats = views[1].get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
    if OS.get_environment("HCSR_SEQUENTIAL_OPACITY_GROUPS") == "1":
        require(not stats.disjoint_opacity_groups and stats.render_passes == 9,"sequential diagnostic path")
    else:
        require(stats.disjoint_opacity_groups and stats.render_passes == 3,"disjoint groups share depth passes")
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
    # Overlapping sibling groups must preserve source-over order via fallback.
    for view in views:
        var overlap_doc := HTMLDocument.new()
        overlap_doc.html = "<style>body{margin:0;background:white}.a,.b{position:absolute;top:20px;width:120px;height:120px;opacity:.5}.a{left:20px;background:red}.b{left:50px;background:blue}</style><div class='a'></div><div class='b'></div>"
        view.document = overlap_doc
    await settle()
    require(not views[1].get_frame_scheduler_diagnostics().frame_synchronization.image_atlas.disjoint_opacity_groups,"overlap uses sequential passes")
    for view in views:
        var image := view.get_texture().get_image()
        var pixel := image.get_pixel(80,70)
        require(abs(pixel.r-.5)<.012 and abs(pixel.g-.25)<.012 and abs(pixel.b-.75)<.012,"sibling painter order " + str(pixel))
        # Interior grid catches cracks and double blending along shared fan edges.
        for y in range(24,136):
            for x in range(54,136):
                var actual := image.get_pixel(x,y)
                require(abs(actual.r-.5)<.012 and abs(actual.g-.25)<.012 and abs(actual.b-.75)<.012,"triangle seam " + str(Vector2i(x,y)))
        view.queue_free()
    await settle()
    print("GROUP_OPACITY_", "FAILED" if failed else "OK")
    quit(1 if failed else 0)

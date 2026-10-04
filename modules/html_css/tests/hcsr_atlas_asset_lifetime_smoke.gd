extends SceneTree

var view: HTMLView
var failed := false

func _initialize() -> void:
    run.call_deferred()
    create_timer(180).timeout.connect(func(): quit(2))

func require(value: bool, label: String) -> void:
    if not value:
        failed = true
        push_error("ATLAS_ASSET_LIFETIME_FAILED " + label)

func settle() -> void:
    for frame in 6:
        await process_frame
        await RenderingServer.frame_post_draw

func stats() -> Dictionary:
    return view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas

func source(red: int, width := 64) -> String:
    return "data:image/svg+xml;base64," + Marshalls.utf8_to_base64("<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d'><rect width='%d' height='%d' fill='rgb(%d,0,0)'/></svg>" % [width,width,width,width,red])

func markup(cycle: int) -> String:
    return "<img id='art' src='%s'><span id='caption'>%s</span>" % [source(16+cycle*7),String.chr(65+cycle)]

func run() -> void:
    Engine.max_fps = 0
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    view = HTMLView.new()
    view.size = Vector2(128,100)
    view.logical_size = Vector2i(128,100)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.backend_preference = HTMLView.BACKEND_GPU_AUTO
    var document := HTMLDocument.new()
    document.html = "<style>html,body{margin:0;background:white;color:black;font:24px Arial}#art{position:absolute;left:8px;top:8px;width:64px;height:64px}#caption{position:absolute;left:80px;top:8px}</style><div id='app'>" + markup(0) + "</div>"
    document.background_color = Color.WHITE
    view.document = document
    root.add_child(view)
    var output := view.create_output(Vector2i(128,100),false)
    await settle()
    var original: PackedByteArray = output.texture.get_image().get_data()
    for cycle in 26:
        require(view.set_element_inner_html("app",markup(cycle)) == OK,"replace assets")
        await settle()
        var current := stats()
        require(int(current.image_entries) == 1 and int(current.glyph_entries) == 1,"one active image and glyph: " + str(current))
        require(absf(output.texture.get_image().get_pixel(32,32).r - (16+cycle*7)/255.0) < 0.01,"current image survives slot reuse")
    require(int(stats().reused_surface_slots) > 20,"retired asset slots reused")
    require(view.set_element_inner_html("app","") == OK,"remove assets")
    await settle()
    require(int(stats().image_entries) == 0 and int(stats().glyph_entries) == 0 and int(stats().pending_glyphs) == 0,"removed asset ownership released")
    require(view.set_element_inner_html("app",markup(0)) == OK,"restore assets")
    await settle()
    require(output.texture.get_image().get_data() == original,"restored assets reproduce exact initial pixels")
    if "--capacity" in OS.get_cmdline_user_args():
        # Eight full allocations exhaust the atlas without occupying the retry
        # image's visible region. Its failure must remain recoverable after
        # the large images leave the scene, while its source stays unchanged.
        var large := ""
        for index in 8:
            large += "<div style='position:absolute;left:0;top:0;width:1px;height:1px;overflow:hidden'><img style='width:4094px;height:4094px' src='%s'></div>" % source(20+index,4094)
        var retry := "<img id='retry' style='position:absolute;left:80px;top:8px;width:16px;height:16px' src='%s'>" % source(255,16)
        require(view.set_element_inner_html("app",large+retry) == OK,"capacity fixture")
        await settle()
        require(int(stats().failed_allocations) > 0,"fixture reaches allocator capacity")
        require(view.set_element_inner_html("app",retry) == OK,"reclaim capacity without changing retry asset")
        await settle()
        require(int(stats().failed_allocations) == 0,"capacity failures retry after reclamation")
        var pixel := output.texture.get_image().get_pixel(84,12)
        require(pixel.r > 0.99 and pixel.g < 0.01,"previously unavailable image publishes its pixels")
    require(int(view.get_frame_scheduler_diagnostics().frame_synchronization.failures) == 0,"frame synchronization")
    print("ATLAS_ASSET_LIFETIME_", "FAILED" if failed else "OK", " stats=",stats())
    output.release()
    view.queue_free()
    await settle()
    quit(1 if failed else 0)

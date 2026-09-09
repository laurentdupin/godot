extends SceneTree

func _initialize() -> void:
    run.call_deferred()
    create_timer(30).timeout.connect(func(): quit(2))

func check(condition: bool, message: String) -> void:
    if not condition:
        push_error(message)
        quit(1)
        assert(condition, message)

func settle() -> void:
    for i in 5:
        await process_frame
        await RenderingServer.frame_post_draw

func run() -> void:
    Engine.max_fps = 60
    root.size = Vector2i(200,160)
    for container in ["display:grid;min-height:0;align-content:center", "display:flex;flex-direction:column;min-height:0", "display:flex;flex-direction:column;min-height:40px", ""]:
        var document := HTMLDocument.new()
        document.html = ("<style>body{margin:0}#owner{width:120px;%s}#child{height:60px;min-height:0;background:lime}#after{height:10px;background:blue}</style>" % container
            + "<div id='owner'>" + ("<div style='width:100%;height:100%'>" if container.is_empty() else "")
            + "<div id='child'></div>" + ("</div>" if container.is_empty() else "") + "</div><div id='after'></div>")
        var view := HTMLView.new()
        view.backend_preference = HTMLView.BACKEND_VULKAN if OS.get_environment("HCSR_TEST_GPU") == "vulkan" else HTMLView.BACKEND_D3D12
        view.size = Vector2(120,140)
        view.logical_size = Vector2i(120,140)
        view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
        view.document = document
        root.add_child(view)
        await settle()
        var pixels := view.get_texture().get_image()
        check(pixels.get_pixel(20,50).g > .8, "Natural child height collapsed: " + container)
        check(pixels.get_pixel(20,65).b > .8, "Following sibling was misplaced: " + container)
        if container.is_empty():
            for height in [80,20,100]:
                check(view.set_element_attribute("child", "style", "height:%dpx" % height) == OK, "Mutation rejected")
                await settle()
                pixels = view.get_texture().get_image()
                check(pixels.get_pixel(20,height-5).g > .8, "Child resize did not render")
                check(pixels.get_pixel(20,height+5).b > .8, "Percentage ancestor left stale sibling geometry")
        check(not view.get_frame_scheduler_diagnostics()["frame_synchronization"].get("terminal", true), "Renderer failed")
        view.queue_free()
        await settle()
    print("DEFINITE_SIZE_RENDER_OK grid/flex minimums and percentage mutation propagation")
    quit(0)

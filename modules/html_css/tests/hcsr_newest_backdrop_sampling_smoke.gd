extends SceneTree

var failed := false

func _initialize() -> void:
    run.call_deferred()
    create_timer(45).timeout.connect(func(): quit(2))

func require(value: bool, label: String) -> void:
    if not value:
        failed = true
        push_error("BACKDROP_SAMPLING_FAILED " + label)

func settle() -> void:
    for i in 10:
        await process_frame
        await RenderingServer.frame_post_draw

func run() -> void:
    Engine.max_fps = 0
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    var background := ColorRect.new()
    background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
    background.color = Color(.1, .8, .2, 1)
    root.add_child(background)
    var view := HTMLView.new()
    view.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
    view.backend_preference = HTMLView.BACKEND_GPU_AUTO
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.logical_size = Vector2i(320, 180)
    view.backdrop_filter_enabled = true
    var document := HTMLDocument.new()
    document.background_color = Color.TRANSPARENT
    document.html = "<style>html,body{margin:0;background:transparent}#glass{position:absolute;left:80px;top:60px;width:120px;height:100px;backdrop-filter:invert(1)}</style><div id='glass'></div>"
    view.document = document
    root.add_child(view)
    for physical_size in [Vector2i(320, 180), Vector2i(640, 360), Vector2i(160, 90)]:
        DisplayServer.window_set_size(physical_size)
        await settle()
        for filter in ["invert(1)", "blur(3px) invert(1)", "blur(2px) blur(3px) invert(1)"]:
            require(view.set_element_attribute("glass", "style", "backdrop-filter:" + filter) == OK, "filter mutation")
            await settle()
            var image := root.get_texture().get_image()
            var scale := Vector2(image.get_size()) / Vector2(320, 180)
            # The last interior row must be filtered too. Smaller coverage quads
            # previously lost this entire row through interpolated UV rounding.
            var row := int(160 * scale.y) - 1
            for x in [80, 110, 150, 199]:
                var pixel := image.get_pixel(int((x + .5) * scale.x), row)
                # A magnified edge may have fractional raster coverage. Reject
                # the unchanged green backdrop, rather than requiring opacity.
                require(pixel.r > .4 and pixel.g < .6, "bottom row %s %s x=%s pixel=%s" % [physical_size, filter, x, pixel])
            var outside := image.get_pixel(int(70 * scale.x), int(100 * scale.y))
            require(outside.g > .75, "outside mask remains unfiltered")
            var sync: Dictionary = view.get_frame_scheduler_diagnostics().get("frame_synchronization", {})
            require(not sync.get("terminal", false), "renderer remains live")
    print("BACKDROP_SAMPLING_", "FAILED" if failed else "OK")
    quit(1 if failed else 0)

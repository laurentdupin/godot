extends SceneTree

const SIZE := Vector2i(640,220)
const TEXT := "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
var failed := false

func _initialize() -> void:
    _run.call_deferred()
    create_timer(45).timeout.connect(func(): quit(2))

func _check(value: bool, message: String) -> void:
    if not value:
        failed = true
        push_error(message)

func _step() -> void:
    await process_frame
    await RenderingServer.frame_post_draw

func _view(font_size: int) -> HTMLView:
    var view := HTMLView.new()
    view.size = Vector2(SIZE)
    view.logical_size = SIZE
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.backend_preference = HTMLView.BACKEND_GPU_AUTO
    view.html = "<style>html,body{margin:0;background:#182333;color:#eee}#label{position:absolute;left:12px;top:12px;font:%dpx Arial}</style><span id='label'>%s</span>" % [font_size,TEXT]
    root.add_child(view)
    return view

func _run() -> void:
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    Engine.max_fps = 240
    var view := _view(40)
    var output := view.create_output(SIZE,false)
    for frame in 12: await _step()
    for font_size in [14,16]:
        var old_generation := output.generation
        _check(view.set_element_attribute("label","style","font-size:%dpx" % font_size) == OK,"change font size")
        var trace: Array = []
        for frame in 12:
            await _step()
            var sync: Dictionary = view.get_frame_scheduler_diagnostics().frame_synchronization
            var atlas: Dictionary = sync.get("image_atlas",{})
            if output.generation > old_generation:
                if trace.is_empty() and font_size == 14:
                    _check(int(atlas.get("pending_glyphs",-1)) == 0,"distant prewarmed glyphs must not be used as pending fallbacks")
                trace.append({"frame":frame,"generation":output.generation,"pending":atlas.get("pending_glyphs",-1),"rasterized":atlas.get("rasterized_glyphs",-1)})
        _check(not trace.is_empty(),"mutation was presented")
        _check(int(view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas.pending_glyphs) == 0,"upgrades converge without another mutation")
        var warm := output.texture.get_image()
        var fresh := _view(font_size)
        var fresh_output := fresh.create_output(SIZE,false)
        for frame in 12: await _step()
        var expected := fresh_output.texture.get_image()
        var changed := 0
        for y in SIZE.y:
            for x in SIZE.x:
                var delta := warm.get_pixel(x,y) - expected.get_pixel(x,y)
                if maxf(maxf(absf(delta.r),absf(delta.g)),maxf(absf(delta.b),absf(delta.a))) > 1.01/255.0:
                    changed += 1
        _check(changed == 0,"warm/fresh glyph pixels differ: %d at %dpx" % [changed,font_size])
        print("GLYPH_FALLBACK_TRACE size=",font_size," changed_pixels=",changed," frames=",JSON.stringify(trace))
        fresh_output.release()
        fresh.queue_free()
        for frame in 3: await _step()
    print("GLYPH_FALLBACK_", "FAILED" if failed else "OK", " far_fallback_rejected=true nearby_upgrades_converge=true")
    output.release()
    view.queue_free()
    for frame in 3: await _step()
    quit(1 if failed else 0)

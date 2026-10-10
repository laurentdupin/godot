extends SceneTree

# HCSR_FONT_OPTICAL must name an explicit res:// or user:// font with an opsz axis.
var view: HTMLView
var failed := false

func _initialize() -> void:
    run.call_deferred()

func settle() -> void:
    for frame in 12:
        await process_frame
        await RenderingServer.frame_post_draw

func check(condition: bool, message: String) -> void:
    if not condition:
        failed = true
        push_error(message)
        quit(1)
        assert(condition, message)

func difference(a: Image, b: Image, tolerance: float = 0.0) -> int:
    var count := 0
    for y in a.get_height():
        for x in a.get_width():
            var delta := a.get_pixel(x,y) - b.get_pixel(x,y)
            if max(abs(delta.r), abs(delta.g), abs(delta.b), abs(delta.a)) > tolerance: count += 1
    return count

func markup(family: String, face: String = "") -> String:
    var css := "html,body{margin:0;background:white;color:black}#samples{font-family:%s;font-weight:400}span{position:absolute;width:160px;line-height:1}" % family
    if not face.is_empty():
        css += "@font-face{font-family:Optical;src:url('%s');font-weight:100 1000}" % face
    var source := "<style>" + css + "</style><div id='samples'>"
    for index in 120:
        source += "<span style='left:%dpx;top:%dpx;font-size:%.2fpx'>Hamburgefonts</span>" % [index % 6 * 160, index / 6 * 30, 8.0 + float(index)/10.0]
    return source + "</div>"

func run() -> void:
    var optical := OS.get_environment("HCSR_FONT_OPTICAL").replace("\\", "/")
    check((optical.begins_with("res://") or optical.begins_with("user://")) and FileAccess.file_exists(optical), "Provide an explicit font fixture with an opsz axis")
    root.size = Vector2i(960,640)
    view = HTMLView.new()
    view.backend_preference = HTMLView.BACKEND_GPU_AUTO
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.logical_size = Vector2i(960,640)
    view.size = Vector2(960,640)
    view.html = markup("'Segoe UI'")
    root.add_child(view)
    await settle()
    var before := view.get_texture().get_image()
    var before_entries: int = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas.glyph_entries
    # A few atlas raster-density buckets are legitimate; one face per size is not.
    check(before_entries < 64, "Static sizes created redundant glyph faces: " + str(before_entries))
    view.set_element_attribute("samples", "style", "font-optical-sizing:none")
    await settle()
    var after := view.get_texture().get_image()
    check(difference(before, after) == 0, "Static optical sizing changed pixels")
    check(view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas.glyph_entries == before_entries, "Static optical policy duplicated glyphs")
    view.html = markup("Optical", optical)
    await settle()
    before = view.get_texture().get_image()
    view.set_element_attribute("samples", "style", "font-optical-sizing:none")
    await settle()
    after = view.get_texture().get_image()
    check(difference(before, after) > 0, "Variable optical sizing was lost")
    view.set_element_attribute("samples", "style", "font-optical-sizing:auto")
    await settle()
    var restored := view.get_texture().get_image()
    # Rebuilt GPU interpolation may round to an adjacent BGRA8 code value.
    var changed := difference(before, restored, 1.01 / 255.0)
    check(changed == 0, "Optical sizing did not restore its original pixels")
    if failed: return
    print("FONT_RESOURCE_REUSE_OK static_sizes=120 static_glyph_entries=", before_entries, " static_pixels_identical=true variable_opsz=true")
    view.queue_free()
    await process_frame
    await RenderingServer.frame_post_draw
    quit(0)

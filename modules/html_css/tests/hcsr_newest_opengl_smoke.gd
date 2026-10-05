extends SceneTree

var failed := false
func _initialize():
    run.call_deferred()
    create_timer(90).timeout.connect(func(): quit(2))
func require(value: bool, label: String):
    if not value:
        failed = true
        push_error("OPENGL_SMOKE_FAILED " + label)
func settle():
    for i in 12:
        await process_frame
        await RenderingServer.frame_post_draw
func run():
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    var view := HTMLView.new()
    view.size = Vector2(320, 200)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.logical_size = Vector2i(320, 200)
    view.backend_preference = HTMLView.BACKEND_GPU_AUTO
    var doc := HTMLDocument.new()
    doc.background_color = Color.TRANSPARENT
    doc.html = "<style>html,body{margin:0}#scroll{position:absolute;left:15px;top:15px;width:90px;height:100px;overflow:auto;border-radius:12px;background:#234}#child{height:200px;background:linear-gradient(red,blue)}#moving{position:absolute;left:135px;top:15px;width:100px;height:65px;background:#33cc66;border:2px solid #ffaa00;border-radius:9px;transform:rotate(7deg);opacity:.6;filter:brightness(.8)}#check{position:absolute;left:160px;top:120px;width:25px;height:25px;accent-color:#00aaff}</style><div id='scroll'><div id='child'><div id='tail' style='padding-top:180px'>end</div></div></div><div id='moving'></div><input id='check' type='checkbox'>"
    view.document = doc
    root.add_child(view)
    var output := view.create_output(Vector2i(320, 200), true)
    await settle()
    var sync: Dictionary = view.get_frame_scheduler_diagnostics().get("frame_synchronization", {})
    require(sync.get("renderer") == "opengl3", "GPU adapter selected: " + str(sync))
    require(view.get_generation() > 0, "initial generation")
    var original: Image = output.texture.get_image()
    require(original.get_pixel(50, 30).a > .9, "scroll surface visible")
    require(original.get_pixel(180, 45).a > .5, "opacity group visible")
    var group_pixel := original.get_pixel(180,45)
    require(abs(group_pixel.r-.096)<.02 and abs(group_pixel.g-.384)<.02 and abs(group_pixel.b-.192)<.02, "straight shader output stored premultiplied")
    var stats: Dictionary = sync.image_atlas
    var geometry: int = stats.geometry_uploaded_bytes
    var atlas: int = stats.uploaded_bytes
    require(view.set_element_attribute("moving", "style", "transform:translate(20px,10px) rotate(12deg)") == OK, "transform mutation")
    require(view.scroll_element_into_view("tail", "start") == OK, "scroll mutation")
    await settle()
    stats = view.get_frame_scheduler_diagnostics().get("frame_synchronization", {}).image_atlas
    require(stats.geometry_uploaded_bytes == geometry, "movement reuses geometry")
    require(stats.uploaded_bytes == atlas, "movement reuses atlas")
    require(stats.cpu_compositing_bounds_evaluations == 0, "no CPU projected bounds")
    require(view.set_form_control_checked("check", true) == OK, "checkbox mutation")
    await settle()
    var checked: Image = output.texture.get_image()
    var changed := 0
    for y in range(120, 146):
        for x in range(160, 186):
            if original.get_pixel(x, y) != checked.get_pixel(x, y): changed += 1
    require(changed > 0, "native checkbox repaint")
    for pressed in [true,false]:
        var button := InputEventMouseButton.new()
        button.position = Vector2(170,130)
        button.global_position = button.position
        button.button_index = MOUSE_BUTTON_LEFT
        button.button_mask = MOUSE_BUTTON_MASK_LEFT if pressed else 0
        button.pressed = pressed
        view.dispatch_input_event(button)
        await settle()
    require(not view.get_form_control_state("check").checked,"real click toggles state")
    var clicked: Image = output.texture.get_image()
    changed = 0
    for y in range(120,146):
        for x in range(160,186):
            if checked.get_pixel(x,y)!=clicked.get_pixel(x,y):changed+=1
    require(changed>0,"real click repaints pixels")
    output.release()
    await settle()
    for size in [Vector2i(160,100),Vector2i(640,400),Vector2i(320,200)]:
        var resized := view.create_output(size, true)
        await settle()
        var image: Image = resized.texture.get_image()
        require(image.get_size() == size, "presentation resize")
        require(image.has_mipmaps(), "presentation mipmaps")
        resized.release()
        await settle()
    view.queue_free()
    await settle()
    print("OPENGL_SMOKE ", "FAIL" if failed else "OK")
    quit(1 if failed else 0)

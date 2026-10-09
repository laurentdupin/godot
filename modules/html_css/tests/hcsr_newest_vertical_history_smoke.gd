extends SceneTree
var failed := false
func _initialize():
    run.call_deferred()
    create_timer(45).timeout.connect(func(): quit(2))
func require(value, label):
    if not value:
        failed = true
        push_error("VERTICAL_HISTORY_FAILED " + label)
func settle():
    for i in 8:
        await process_frame
        await RenderingServer.frame_post_draw
func create_view(cpu: bool, wrapped: bool) -> HTMLView:
    var view := HTMLView.new()
    view.size = Vector2(320,240)
    view.logical_size = Vector2i(320,240)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.backend_preference = HTMLView.BACKEND_CPU if cpu else HTMLView.BACKEND_GPU_AUTO
    var doc := HTMLDocument.new()
    doc.background_color = Color.WHITE
    var text := "History ABC 123 wrapping text"
    doc.html = "<style>html,body{margin:0;background:white;color:black}#box{position:absolute;left:20px;top:20px;display:flex;width:100px;height:160px;font:20px/1.1 Arial;overflow-wrap:anywhere;white-space:pre-wrap}.rl{writing-mode:vertical-rl}.lr{writing-mode:vertical-lr}.upright{writing-mode:vertical-rl;text-orientation:upright}.center{writing-mode:vertical-rl;align-items:center;justify-content:center}.moved{writing-mode:vertical-rl;transform:translate(12px,18px)}.horizontal{writing-mode:horizontal-tb}</style><div id='box' class='rl'>" + ("<span>" + text + "</span>" if wrapped else text) + "</div>"
    view.document = doc
    root.add_child(view)
    return view
func run():
    var actual := create_view(false, false)
    var reference := create_view(true, true)
    var output := actual.create_output(Vector2i(320,240), true)
    var expected_output := reference.create_output(Vector2i(320,240), true)
    for state in ["rl", "lr", "upright", "center", "moved", "rl"]:
        require(actual.set_element_attribute("box", "class", state) == OK, "GPU mutation " + state)
        require(reference.set_element_attribute("box", "class", state) == OK, "CPU mutation " + state)
        await settle()
        var a := output.texture.get_image()
        var b := expected_output.texture.get_image()
        require(a != null and b != null, "output images " + state)
        if a == null or b == null: continue
        var ink := 0
        var mismatch := 0
        for y in 240:
            for x in 320:
                var aa := a.get_pixel(x,y).r < .5
                var bb := b.get_pixel(x,y).r < .5
                if bb: ink += 1
                if aa != bb: mismatch += 1
        require(ink > 150, "reference has text " + state)
        require(mismatch < max(40, ink * .15), "GPU/CPU text placement " + state + " mismatch=" + str(mismatch) + " ink=" + str(ink))
    require(int(actual.get_frame_scheduler_diagnostics().frame_synchronization.failures) == 0, "frame synchronization")
    output.release()
    expected_output.release()
    actual.queue_free()
    reference.queue_free()
    await settle()
    print("VERTICAL_HISTORY_", "FAILED" if failed else "OK")
    quit(1 if failed else 0)

extends SceneTree
var failed := false
const SIZE := Vector2i(640, 400)
const CSS := "html,body{margin:0;background:white;color:black;font:16px/20px Arial}#box{position:absolute;left:20px;top:20px;width:260px;height:180px}.sideways{writing-mode:sideways-lr}.rtl{direction:rtl}.upright{writing-mode:vertical-rl;text-orientation:upright}.vertical{writing-mode:vertical-rl}.grid{display:grid;grid-template-columns:70px 90px;grid-template-rows:60px 80px;gap:8px}.grid span{background:#cde;border:1px solid #369}.columns{columns:2;column-gap:20px;column-fill:auto;orphans:3;widows:2}.balance{text-wrap:balance}.pretty{text-wrap:pretty}#box.hyphen{hyphens:auto;width:70px}.moved{transform:translate(8px,12px)}td{border:1px solid #369;padding:4px}table{border-spacing:3px 7px}"
func _initialize():
    run.call_deferred()
    create_timer(90).timeout.connect(func(): quit(2))
func require(value, label):
    if not value:
        failed = true
        push_error("LAYOUT_FEATURES_FAILED " + label)
func settle():
    for i in 8:
        await process_frame
        await RenderingServer.frame_post_draw
func create_view(cpu: bool, body: String) -> HTMLView:
    var view := HTMLView.new()
    view.size = Vector2(SIZE)
    view.logical_size = SIZE
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.backend_preference = HTMLView.BACKEND_CPU if cpu else HTMLView.BACKEND_GPU_AUTO
    var doc := HTMLDocument.new()
    doc.background_color = Color.WHITE
    doc.html = "<style>" + CSS + "</style>" + body
    view.document = doc
    root.add_child(view)
    return view
func check_body(body: String, states: Array):
    var actual := create_view(false, body)
    var reference := create_view(true, body)
    var output := actual.create_output(SIZE, true)
    var expected_output := reference.create_output(SIZE, true)
    for state in states:
        require(actual.set_element_attribute("box", "class", state) == OK, "GPU mutation " + state)
        require(reference.set_element_attribute("box", "class", state) == OK, "CPU mutation " + state)
        await settle()
        var a := output.texture.get_image()
        var b := expected_output.texture.get_image()
        require(a != null and b != null, "output images " + state)
        if a == null or b == null: continue
        var ink := 0
        var mismatch := 0
        for y in SIZE.y:
            for x in SIZE.x:
                var aa := a.get_pixel(x,y).r < .5
                var bb := b.get_pixel(x,y).r < .5
                if bb: ink += 1
                if aa != bb: mismatch += 1
        print("LAYOUT_FEATURE_STATE ", state, " ink=", ink, " mismatch=", mismatch)
        require(ink > 150, "reference has text " + state)
        require(mismatch < max(50, ink * .15), "GPU/CPU placement " + state)
    require(int(actual.get_frame_scheduler_diagnostics().frame_synchronization.failures) == 0, "frame synchronization")
    output.release()
    expected_output.release()
    actual.queue_free()
    reference.queue_free()
    await settle()
func run():
    await check_body("<div id='box' lang='en-US'><span>Extraordinary internationalization and carefully chosen words</span></div>",
        ["balance", "pretty", "hyphen", "hyphen moved", "upright hyphen", "sideways", "sideways rtl", "vertical columns", "columns", ""])
    await check_body("<div id='box'><span>One</span><span>Two</span><span>Three</span><span>Four</span></div>",
        ["grid", "vertical grid", "sideways grid", "sideways rtl grid"])
    await check_body("<table id='box'><tr><td>One</td><td>Two</td></tr><tr><td>Three</td><td>Four</td></tr></table>",
        ["", "vertical", "sideways", "sideways rtl"])
    print("LAYOUT_FEATURES_", "FAILED" if failed else "OK")
    quit(1 if failed else 0)

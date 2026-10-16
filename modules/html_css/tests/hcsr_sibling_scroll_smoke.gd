extends SceneTree

const SIZE := Vector2i(800, 400)
var failed := false

func _initialize() -> void:
    _run.call_deferred()
    create_timer(40).timeout.connect(func(): quit(2))

func _check(value: bool, message: String) -> void:
    if not value:
        failed = true
        push_error(message)

func _settle() -> void:
    for frame in 8:
        await process_frame
        await RenderingServer.frame_post_draw

func _run() -> void:
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    Engine.max_fps = 240
    var view := HTMLView.new()
    view.size = Vector2(SIZE)
    view.logical_size = SIZE
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.backend_preference = HTMLView.BACKEND_GPU_AUTO
    var rows := ""
    for index in 30:
        rows += "<div id='row%d' class='row'></div>" % index
    view.html = """<!doctype html><style>html,body{margin:0}
        #panes{width:600px;height:200px;display:grid;grid-template-columns:1fr 1fr;grid-template-rows:minmax(0,1fr)}
        .pane{min-width:0;min-height:0;overflow:auto;overflow-anchor:none}
        .row{height:40px;background:#123456}#row10{background:#00ff00}.hidden{display:none}
        </style><div id='panes'><div id='list' class='pane'>%s</div>
        <div id='detail' class='pane'><div id='toggle' class='row'></div></div></div>""" % rows
    root.add_child(view)
    var output := view.create_output(SIZE, false)
    await _settle()
    _check(view.scroll_element_into_view("row10", "start") == OK, "Scroll request accepted")
    await _settle()
    var before := output.texture.get_image()
    _check(before.get_pixel(20,20).g > 0.95, "Initial scroll exposes green row 10")
    for operation in 5:
        if operation == 0:
            _check(view.set_element_inner_html("detail", "<div id='toggle' class='row'></div>") == OK, "Replace sibling content")
        else:
            _check(view.set_element_attribute("toggle", "class", "row hidden" if operation % 2 else "row") == OK, "Toggle sibling display")
        await _settle()
        var after := output.texture.get_image()
        var changed := 0
        for y in 190:
            for x in 280:
                if before.get_pixel(x,y) != after.get_pixel(x,y):
                    changed += 1
        _check(changed == 0, "Sibling operation %d changed %d list pixels" % [operation,changed])
        print("SIBLING_SCROLL_GPU operation=", operation, " changed_list_pixels=", changed)
    output.release()
    view.queue_free()
    await _settle()
    print("SIBLING_SCROLL_GPU_", "FAILED" if failed else "PASS")
    quit(1 if failed else 0)

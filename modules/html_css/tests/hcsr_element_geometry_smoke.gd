extends SceneTree

var failed := false
var actions: Array[StringName] = []
var event_bounds := Rect2()
var pointer_count := 0

const HTML := """<!doctype html><style>
html,body{margin:0}button{box-sizing:border-box;border:2px solid black;padding:0}
#absolute{position:absolute;left:40px;top:25px;width:100px;height:40px}
#flex{position:absolute;left:20px;top:90px;width:240px;display:flex;gap:10px}
#flex button{flex:none;width:100px;height:40px}
#none,.hidden{display:none}
#covered{position:absolute;left:280px;top:25px;width:70px;height:40px}
#cover{position:absolute;left:280px;top:25px;width:70px;height:40px;background:red;z-index:2}
</style><button id='absolute' data-godot-action='absolute'>Absolute</button>
<div id='flex'><button id='first' data-godot-action='first'>First</button><button id='second' data-godot-action='second'>Second</button></div>
<button id='none'>Hidden</button><div class='hidden'><div style='position:relative'><button id='descendant' style='position:absolute'>Descendant</button></div></div>
<button id='covered'>Covered</button><div id='cover'></div>"""

func _initialize():
    _run.call_deferred()
    create_timer(40).timeout.connect(func(): quit(2))

func _check(ok: bool, message: String):
    if not ok:
        failed = true
        push_error(message)

func _frames(count := 8):
    for i in count:
        await process_frame

func _pointer(_phase: StringName, _id: StringName, _action: StringName, _button: int, payload: Dictionary):
    pointer_count += 1
    if _id == &"absolute":
        event_bounds = Rect2(payload.bounds)

func _click(view: HTMLView, position: Vector2):
    var motion := InputEventMouseMotion.new()
    motion.position = position
    view.dispatch_input_event(motion)
    for pressed in [true, false]:
        var event := InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_LEFT
        event.position = position
        event.pressed = pressed
        view.dispatch_input_event(event)

func _run():
    root.size = Vector2i(1000, 700)
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    Engine.max_fps = 240
    var document := HTMLDocument.new()
    document.html = HTML
    for mode in [HTMLView.VIEWPORT_SIZE_PHYSICAL_SIZE, HTMLView.VIEWPORT_SIZE_CONTROL_PHYSICAL_ADJUSTED, HTMLView.VIEWPORT_SIZE_FIXED, HTMLView.VIEWPORT_SIZE_CONTROL]:
        var view := HTMLView.new()
        view.logical_size = Vector2i(640, 360)
        view.fixed_viewport_size = Vector2i(640, 360)
        view.fixed_viewport_device_scale_factor = 1.5
        view.viewport_size_mode = mode
        view.size = Vector2(400, 225)
        view.scale = Vector2(1.25, 1.25)
        view.position = Vector2(30, 20)
        view.document = document
        view.action_requested.connect(func(action: StringName, _payload: Dictionary): actions.append(action))
        view.element_pointer_event.connect(_pointer)
        _check(view.get_element_rect('absolute') == Rect2(), 'No layout before entering tree')
        root.add_child(view)
        await _frames()
        var prior_pointer_count := pointer_count
        var rect := view.get_element_rect('absolute')
        var html_rect := view.get_element_html_rect('absolute')
        _check(html_rect.is_equal_approx(Rect2(40, 25, 100, 40)), 'Absolute border box: mode %d got %s' % [mode, html_rect])
        _check(view.local_to_html_position(rect.position).is_equal_approx(html_rect.position), 'Inverse position conversion mode %d' % mode)
        _check(view.local_to_html_position(rect.end).is_equal_approx(html_rect.end), 'Inverse extent conversion mode %d' % mode)
        _check(view.get_element_html_rect('second').is_equal_approx(Rect2(130, 90, 100, 40)), 'Flex border box mode %d' % mode)
        _check(view.get_element_html_rect('covered').has_area(), 'Occluded element remains queryable')
        for id in ['', 'missing', 'none', 'descendant']:
            _check(view.get_element_rect(id) == Rect2(), 'Empty geometry for ' + id)
        if mode == HTMLView.VIEWPORT_SIZE_PHYSICAL_SIZE:
            var query_start := Time.get_ticks_usec()
            for i in 1000:
                view.get_element_rect('absolute')
            print('ELEMENT_GEOMETRY_QUERY average_us=', (Time.get_ticks_usec() - query_start) / 1000.0)
        _check(pointer_count == prior_pointer_count, 'Geometry query does not emit pointer events')
        event_bounds = Rect2()
        _click(view, rect.get_center())
        await _frames(2)
        _check(actions.back() == &"absolute" if not actions.is_empty() else false, 'Click queried absolute center mode %d' % mode)
        _check(event_bounds.is_equal_approx(html_rect), 'Pointer payload uses same HTML box')
        _click(view, view.get_element_rect('second').get_center())
        await _frames(2)
        _check(actions.back() == &"second", 'Click queried flex center mode %d' % mode)
        _check(view.set_element_style('absolute', 'left:60.5px;top:35.25px') == OK, 'Move mutation accepted')
        _check(view.get_element_html_rect('absolute').is_equal_approx(html_rect), 'Query does not commit queued writes itself')
        await _frames()
        _check(view.get_element_html_rect('absolute').position.is_equal_approx(Vector2(60.5, 35.25)), 'Fractional moved box after step')
        _check(view.apply_element_mutations([{'operation':'set_attribute', 'id':'absolute', 'name':'style', 'value':'left:80px;top:45px'}]) == OK, 'Batched move accepted')
        await _frames()
        _check(view.get_element_html_rect('absolute').position.is_equal_approx(Vector2(80, 45)), 'Batched move reflected')
        view.hide()
        await _frames(2)
        _check(view.get_element_rect('absolute').has_area(), 'Hidden source Control retains document geometry')
        view.queue_free()
        await _frames(2)
    var target := HTMLRenderTarget.new()
    target.size = Vector2i(640, 360)
    target.document = document
    root.add_child(target)
    await _frames()
    _check(target.get_element_rect('absolute').is_equal_approx(Rect2(40, 25, 100, 40)), 'RenderTarget geometry parity')
    _check(target.get_element_html_rect('absolute') == target.get_element_rect('absolute'), 'RenderTarget spaces coincide')
    target.queue_free()
    await _frames(2)
    print('ELEMENT_GEOMETRY_', 'FAIL' if failed else 'PASS')
    quit(1 if failed else 0)

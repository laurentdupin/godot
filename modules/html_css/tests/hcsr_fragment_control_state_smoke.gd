extends SceneTree
var failed := false
func _initialize():
    run.call_deferred()
    create_timer(30).timeout.connect(func(): quit(2))
func check(ok: bool, message: String):
    if not ok:
        failed = true
        push_error(message)
func frames():
    for i in 8:
        await process_frame
func key(view: HTMLView, code: Key):
    var event := InputEventKey.new()
    event.keycode = code
    event.ctrl_pressed = true
    event.pressed = true
    view.dispatch_input_event(event)
func run():
    var previous_clipboard := DisplayServer.clipboard_get()
    var view := HTMLView.new()
    view.size = Vector2(640, 360)
    view.logical_size = Vector2i(640, 360)
    view.html = "<select id='s'><option id='a' value='a'>A</option><option value='b'>B</option></select><input id='text' value='original'>"
    root.add_child(view)
    await frames()
    check(view.set_form_control_value('s', 'b') == OK, 'Select mutation accepted')
    await frames()
    check(view.apply_element_mutations([{'operation':'insert_html','id':'s','index':-1,'value':'<option value=c>C</option>'}]) == OK, 'Option insertion accepted')
    await frames()
    check(view.get_form_control_state('s').value == 'b', 'Insert preserves live selection')
    check(view.apply_element_mutations([{'operation':'remove_element','id':'a'}]) == OK, 'Option removal accepted')
    await frames()
    var state := view.get_form_control_state('s')
    check(state.value == 'b' and state.selected_index == 0, 'Removal updates selection index')
    var position := view.get_element_rect('text').get_center()
    for pressed in [true, false]:
        var click := InputEventMouseButton.new()
        click.button_index = MOUSE_BUTTON_LEFT
        click.position = position
        click.pressed = pressed
        view.dispatch_input_event(click)
    await frames()
    view.set_form_control_value('text', 'a🙂b')
    await frames()
    check(view.set_text_selection('text', 1, 2) == OK, 'Unicode selection accepted')
    await frames()
    state = view.get_form_control_state('text')
    check(state.selection_start == 1 and state.selection_end == 2, 'Selection offsets are Godot scalar offsets')
    key(view, KEY_C)
    check(DisplayServer.clipboard_get() == '🙂', 'Copy uses the complete supplementary character')
    await create_timer(0.2).timeout
    view.set_form_control_value('text', 'original')
    await frames()
    key(view, KEY_A)
    key(view, KEY_C)
    check(DisplayServer.clipboard_get() == 'original', 'Immediate select-all/copy sees queued input')
    # Windows clipboard observers can briefly own the clipboard after a write.
    await create_timer(0.2).timeout
    DisplayServer.clipboard_set('pasted')
    key(view, KEY_V)
    await frames()
    check(view.get_form_control_state('text').value == 'pasted', 'Queued clipboard paste')
    await create_timer(0.2).timeout
    DisplayServer.clipboard_set(previous_clipboard)
    view.queue_free()
    await frames()
    print('FRAGMENT_CONTROL_STATE_', 'FAIL' if failed else 'PASS')
    quit(1 if failed else 0)

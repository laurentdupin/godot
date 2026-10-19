extends SceneTree

# Run in an otherwise empty project, with a real rendering driver. Never wait
# for frame_post_draw: reaching that signal is the behavior under test.
const SIZE := Vector2i(320, 160)
var failed := false
var view: HTMLView
var output: HTMLViewOutput

func _initialize():
    _run.call_deferred()
    create_timer(30).timeout.connect(func(): quit(2))

func _check(condition: bool, message: String):
    if not condition:
        failed = true
        push_error(message)

func _loops(count: int):
    for i in count:
        await process_frame

func _idle(label: String):
    await _loops(24)
    var draws := Engine.get_frames_drawn()
    await create_timer(1.0).timeout
    var delta := Engine.get_frames_drawn() - draws
    print('LOW_PROCESSOR_IDLE ', label, ' draws=', delta)
    _check(delta == 0, label + ': idle must not request draws')

func _next_output(label: String, generation: int):
    var start := Time.get_ticks_usec()
    var loops := 0
    while output.generation <= generation and loops < 12:
        await process_frame
        loops += 1
    var elapsed := (Time.get_ticks_usec() - start) / 1000.0
    print('LOW_PROCESSOR_OUTPUT ', label, ' loops=', loops, ' ms=', elapsed)
    _check(output.generation > generation, label + ': output must complete while hidden')
    _check(loops <= 4, label + ': completion must not stall waiting for another canvas change')

func _run():
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    Engine.max_fps = 0
    view = HTMLView.new()
    view.size = SIZE
    view.logical_size = SIZE
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.fixed_viewport_size = SIZE
    view.release_targets_when_hidden = true
    view.hide()
    view.css = """
      html,body{margin:0;background:#102030;color:white;font:24px Arial}
      #label{display:block;width:260px;height:90px;background:#804020;transition:opacity .15s}
      #label.dim{opacity:.25}
      @keyframes pulse{from{transform:translateX(0px)}to{transform:translateX(30px)}}
      #label.pulse{animation-name:pulse;animation-duration:200ms;animation-timing-function:linear}
      """
    view.html = "<div id='label'>Before</div>"
    root.add_child(view)
    output = view.create_output(SIZE, false)
    await _loops(24)
    _check(output.generation > 0, 'Initial output completed')
    OS.low_processor_usage_mode_sleep_usec = 6900
    OS.low_processor_usage_mode = true
    # Readback may itself mark RenderingServer changed; do it before settling.
    var before := output.texture.get_image().get_data()
    await _idle('hidden output')
    var generation := output.generation
    _check(view.set_element_text('label', 'After mutation') == OK, 'Mutation accepted')
    await _next_output('hidden text mutation', generation)
    _check(output.texture.get_image().get_data() != before, 'Output pixels reflect text mutation')
    _check(view.get_texture().get_width() == 0, 'Hidden main presentation stays retired')
    await _idle('after mutation')
    output.size = SIZE * 2
    var resize_loops := 0
    while output.texture.get_size() != Vector2(SIZE * 2) and resize_loops < 12:
        await process_frame
        resize_loops += 1
    print('LOW_PROCESSOR_OUTPUT hidden output resize loops=', resize_loops)
    _check(resize_loops <= 4, 'Hidden output resize completes promptly')
    _check(output.texture.get_size() == Vector2(SIZE * 2), 'Resized output published')
    await _idle('after output resize')
    for state in ['dim', '', 'pulse']:
        generation = output.generation
        _check(view.set_element_attribute('label', 'class', state) == OK, 'Animation state accepted')
        await _next_output('hidden animation ' + state, generation)
        await create_timer(0.4).timeout
        print('LOW_PROCESSOR_ANIMATION ', state, ' generations=', output.generation - generation)
        _check(output.generation > generation + 1, 'Running transition/animation publishes multiple frames')
        await _idle('after animation ' + state)
    # Hiding an ancestor must take the same path as hiding HTMLView itself.
    var parent := Control.new()
    root.add_child(parent)
    view.reparent(parent)
    view.show()
    parent.hide()
    await _idle('hidden ancestor')
    generation = output.generation
    view.set_element_text('label', 'Hidden ancestor')
    await _next_output('hidden ancestor mutation', generation)
    await _idle('after hidden ancestor mutation')
    parent.show()
    await _idle('visible with output')
    generation = output.generation
    view.set_element_text('label', 'Visible with secondary output')
    await _next_output('visible secondary output mutation', generation)
    await _idle('after visible secondary output mutation')
    output.release()
    await _idle('visible without output')
    var draws := Engine.get_frames_drawn()
    var start := Time.get_ticks_usec()
    view.set_element_text('label', 'Visible mutation')
    var loops := 0
    while Engine.get_frames_drawn() == draws and loops < 12:
        await process_frame
        loops += 1
    print('LOW_PROCESSOR_VISIBLE loops=', loops, ' ms=', (Time.get_ticks_usec() - start) / 1000.0)
    _check(Engine.get_frames_drawn() > draws and loops <= 3, 'Visible canvas mutation still draws promptly')
    await _idle('visible after mutation')
    OS.low_processor_usage_mode = false
    parent.queue_free()
    await _loops(3)
    print('HIDDEN_OUTPUT_LOW_PROCESSOR_', 'FAIL' if failed else 'PASS')
    quit(1 if failed else 0)

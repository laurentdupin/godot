extends SceneTree
var view: HTMLView
var failed := false
func check(ok: bool, message: String):
    if not ok:
        failed = true
        push_error(message)
func _initialize():
    run.call_deferred()
    create_timer(90).timeout.connect(func(): quit(2))
func frames():
    for i in 20:
        await process_frame
        await RenderingServer.frame_post_draw
func mark(stage: String):
    await frames()
    var sync: Dictionary = view.get_frame_scheduler_diagnostics().frame_synchronization
    print('LONG_TEXT ', JSON.stringify({'stage':stage,'memory':sync.memory,'preparation_ms':sync.preparation_ms,'terminal':sync.terminal}))
func run():
    Engine.max_fps = 0
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    view = HTMLView.new()
    view.size = Vector2(640,360)
    view.logical_size = Vector2i(640,360)
    view.html = "<style>html,body{margin:0;font:16px Arial}#content{height:340px;overflow:auto}pre{white-space:pre-wrap}</style><div id='content'>small</div>"
    root.add_child(view)
    await mark('small')
    var text := 'This is a long notice paragraph with ordinary English words, punctuation and wrapping.\n'.repeat(1000).left(79375)
    var start := Time.get_ticks_usec()
    var result := view.set_element_inner_html('content', '<pre>' + text.xml_escape() + '</pre>')
    print('LONG_TEXT_ACCEPT ', result, ' us=', Time.get_ticks_usec()-start)
    await mark('large')
    var large: Dictionary = view.get_frame_scheduler_diagnostics().memory.get('census', {})
    check(not large.is_empty(), 'Set HCSR_MEMORY_CENSUS=1 before this test')
    if not large.is_empty():
        check(large.scratch_draws.capacity >= 32768, 'Large fixture exercises high-water scratch')
    print('LONG_TEXT_TRIM_LARGE ', JSON.stringify(view.trim_process_managed_memory()))
    view.set_element_inner_html('content', '<p>small again</p>')
    await mark('removed')
    var small: Dictionary = view.get_frame_scheduler_diagnostics().memory.get('census', {})
    if not small.is_empty():
        check(small.scratch_draws.capacity < 1024, 'Obsolete exceptional scratch retired')
        check(small.glyphs.items < 100, 'Removed shaping absent')
    var before := view.get_texture().get_image().get_data()
    var trim := view.trim_process_managed_memory()
    print('LONG_TEXT_TRIM_SMALL ', JSON.stringify(trim))
    check(not trim.is_empty(), 'Explicit trim supported')
    await frames()
    check(view.get_texture().get_image().get_data() == before, 'Compaction preserves pixels')
    for i in 3:
        view.set_element_inner_html('content', '<p>small %d</p>' % i)
        await mark('small_after_%d' % i)
    print('LONG_TEXT_', 'FAIL' if failed else 'PASS')
    quit(1 if failed else 0)

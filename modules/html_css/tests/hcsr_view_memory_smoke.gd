extends SceneTree

const SIZE := Vector2i(640, 360)
var failed := false
var view: HTMLView

func _initialize():
    _run.call_deferred()
    create_timer(40).timeout.connect(func(): quit(2))

func _check(condition: bool, message: String):
    if not condition:
        failed = true
        push_error(message)

func _frames(count: int):
    for i in count:
        await process_frame
        await RenderingServer.frame_post_draw

func _snapshot() -> Image:
    return view.get_texture().get_image()

func _run():
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    Engine.max_fps = 240
    view = HTMLView.new()
    view.size = SIZE
    view.logical_size = SIZE
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.release_targets_when_hidden = true
    view.html = """<style>html,body{margin:0;color:#f1e7da;background:linear-gradient(35deg,#123456,#3a6547);font:22px Arial}
      #card{position:absolute;left:25px;top:25px;width:420px;height:240px;background:#182333b0;border:1.5px solid #9abcde;border-radius:23px;box-shadow:4px 5px 12px #0008;opacity:.82;backdrop-filter:blur(5px)}
      span{display:block;padding:25px} </style><div id='card'><span>Memory / glyph / shadow parity 日本語 中文 한국어 office</span></div>"""
    if FileAccess.file_exists("res://memory-font.ttf"):
        view.html = "<style>@font-face{font-family:MemoryFont;src:url(memory-font.ttf)}span{font-family:MemoryFont,Arial}</style>" + view.html
    root.add_child(view)
    await _frames(16)
    var before := _snapshot()
    var sync: Dictionary = view.get_frame_scheduler_diagnostics().frame_synchronization
    var fonts: Dictionary = sync.fonts
    _check(int(sync.memory.process_managed_heap_bytes) > 0, 'Process GC counters reported')
    _check(int(sync.memory.cached_native_packet_payload_bytes) > 0, 'Per-scene native payload reported')
    var rows = sync.image_atlas.atlas_used_rows
    for cycle in 4:
        view.hide()
        await _frames(6)
        sync = view.get_frame_scheduler_diagnostics().frame_synchronization
        _check(sync.main_presentation_suspended, 'Hidden main presentation suspended')
        _check(view.get_texture().get_width() == 0, 'Hidden main target detached')
        _check(int(sync.image_atlas.opacity_target_bytes) == 0, 'Hidden compositing targets freed')
        _check(sync.image_atlas.atlas_used_rows == rows, 'Atlas retained when hidden')
        var start := Time.get_ticks_usec()
        view.show()
        await _frames(1)
        var first_ms := (Time.get_ticks_usec() - start) / 1000.0
        await _frames(5)
        var after := _snapshot()
        _check(after != null and before.get_data() == after.get_data(), 'Restored text, shadow and backdrop pixels match')
        print('VIEW_RESHOW cycle=', cycle, ' first_completed_frame_ms=', first_ms)
    # Hiding a source must never retire an independently consumed 3D/output target.
    var output := view.create_output(SIZE, false)
    await _frames(6)
    var generation := output.generation
    view.hide()
    await _frames(5)
    _check(output.texture.get_width() == SIZE.x, 'Independent output survives hide')
    _check(view.set_element_attribute('card', 'style', 'background:#804020') == OK, 'Hidden mutation accepted')
    await _frames(6)
    _check(output.generation > generation, 'Hidden independent output continues rendering')
    _check(view.get_texture().get_width() == 0, 'Mutation does not recreate hidden main target')
    output.release()
    view.show()
    await _frames(6)
    # Same base fonts are shared, but deleting one view must not invalidate another.
    var second := HTMLView.new()
    second.html = view.html
    second.size = SIZE
    root.add_child(second)
    await _frames(8)
    var shared: Dictionary = second.get_frame_scheduler_diagnostics().memory.fonts
    _check(shared.process_shared_font_resources == fonts.process_shared_font_resources, 'Same fonts do not create another base cache')
    var expected := second.get_texture().get_image()
    view.queue_free()
    await _frames(5)
    _check(second.get_texture().get_image().get_data() == expected.get_data(), 'Other view keeps shared fonts alive')
    second.queue_free()
    await _frames(5)
    var observer := HTMLView.new()
    observer.html = '<div style="width:20px;height:20px;background:red"></div>'
    root.add_child(observer)
    await _frames(5)
    var remaining: Dictionary = observer.get_frame_scheduler_diagnostics().memory.fonts
    _check(remaining.process_shared_font_resources == remaining.view_shared_font_references, 'Only observer-owned base fonts remain')
    observer.queue_free()
    await _frames(4)
    print('VIEW_MEMORY_SMOKE_', 'FAIL' if failed else 'PASS')
    quit(1 if failed else 0)

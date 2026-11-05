extends SceneTree

var failed := false
var directory := OS.get_environment("HCSR_FONT_PROBE_DIR")
func _initialize():
    run.call_deferred()
    create_timer(60).timeout.connect(func(): quit(2))
func check(ok: bool, message: String):
    if not ok:
        failed = true
        push_error(message)
func private_bytes() -> int:
    var result := []
    OS.execute('powershell.exe', ['-NoProfile', '-Command', '(Get-Process -Id %d).PrivateMemorySize64' % OS.get_process_id()], result)
    return str(result[0]).strip_edges().to_int() if not result.is_empty() else 0
func memory(stage: String):
    print('FONT_MEMORY ', JSON.stringify({'stage':stage, 'private_bytes':private_bytes(), 'godot_bytes':OS.get_static_memory_usage()}))
func frames(count: int):
    for i in count:
        await process_frame
        await RenderingServer.frame_post_draw
func run():
    Engine.max_fps = 0
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    var font := SystemFont.new()
    font.font_names = PackedStringArray(['Segoe UI'])
    memory('baseline')
    var samples := ['Latin office', '日本語の表示', '简体中文与繁體中文', '한국어 표시', 'Русский текст']
    var sizes := []
    for text in samples:
        var start := Time.get_ticks_usec()
        var size := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, 24)
        var elapsed := Time.get_ticks_usec() - start
        sizes.append(size)
        print('FONT_SHAPE ', JSON.stringify({'text':text,'size':[size.x,size.y],'first_us':elapsed}))
        memory(text)
    var timings := []
    for iteration in 300:
        var start := Time.get_ticks_usec()
        for i in samples.size():
            check(font.get_string_size(samples[i], HORIZONTAL_ALIGNMENT_LEFT, -1, 24) == sizes[i], 'Warm shaping metrics changed')
        timings.append(Time.get_ticks_usec() - start)
    timings.sort()
    print('FONT_WARM ', JSON.stringify({'median_us':timings[150], 'p95_us':timings[285], 'worst_us':timings[-1]}))
    font = null
    TextServerManager.get_primary_interface().font_clear_system_fallback_cache()
    await frames(4)
    memory('after_clear')
    for cycle in 3:
        var fresh := SystemFont.new()
        fresh.font_names = PackedStringArray(['Segoe UI'])
        for i in samples.size():
            check(fresh.get_string_size(samples[i], HORIZONTAL_ALIGNMENT_LEFT, -1, 24) == sizes[i], 'Reloaded fallback metrics changed')
        fresh = null
        TextServerManager.get_primary_interface().font_clear_system_fallback_cache()
    var view := HTMLView.new()
    view.size = Vector2(640, 360)
    view.logical_size = Vector2i(640, 360)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.html = "<style>body{margin:0;background:#123456;color:white;font:24px 'Segoe UI'}p{padding:6px}b{font-weight:700}</style><p>Latin office 日本語 简体中文 한국어 Русский</p><p><b>Bold 日本語 中文 한국어</b></p>"
    root.add_child(view)
    await frames(16)
    var before := view.get_texture().get_image()
    if not directory.is_empty():
        DirAccess.make_dir_recursive_absolute(directory)
        before.save_png(directory.path_join('multilingual.png'))
    var sync: Dictionary = view.get_frame_scheduler_diagnostics().get('frame_synchronization', {})
    check(not sync.get('terminal', false), 'HTML scene renders')
    if sync.get('memory', {}).has('census'):
        print('MEMORY_CENSUS ', JSON.stringify(sync.memory))
    var times := []
    for i in 120:
        var start := Time.get_ticks_usec()
        await process_frame
        times.append(Time.get_ticks_usec() - start)
    times.sort()
    print('FONT_IDLE ', JSON.stringify({'median_us':times[60], 'p95_us':times[114], 'worst_us':times[-1]}))
    check(view.get_texture().get_image().get_data() == before.get_data(), 'Idle pixels stable')
    print('FONT_MAPPING_', 'FAIL' if failed else 'PASS')
    quit(1 if failed else 0)

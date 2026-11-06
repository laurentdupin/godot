extends SceneTree

func _initialize():
    run.call_deferred()
    create_timer(90).timeout.connect(func(): quit(2))
func frames(count: int):
    for i in count:
        await process_frame
        await RenderingServer.frame_post_draw
func run():
    Engine.max_fps = 0
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    var view := HTMLView.new()
    view.size = Vector2(956, 1392)
    view.logical_size = Vector2i(956, 1392)
    view.fixed_viewport_size = Vector2i(956, 1392)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.html_file = OS.get_environment('HCSR_COMPONENT_PROBE_PAGE')
    if view.html_file.is_empty(): view.html_file = 'res://UI/settings.html'
    root.add_child(view)
    await frames(20)
    var directory := OS.get_environment('HCSR_COMPONENT_PROBE_DIR')
    if not directory.is_empty():
        DirAccess.make_dir_recursive_absolute(directory)
        view.get_texture().get_image().save_png(directory.path_join('initial.png'))
    var trim := view.trim_process_managed_memory()
    print('COMPONENT_MEMORY ', JSON.stringify(trim))
    var rng := RandomNumberGenerator.new()
    rng.seed = 8431
    var samples := []
    for i in 200:
        var position := Vector2(rng.randf_range(0, 956), rng.randf_range(0, 1392))
        var event := InputEventMouseMotion.new()
        event.position = position
        var start := Time.get_ticks_usec()
        view.dispatch_input_event(event)
        if i % 10 == 0:
            for pressed in [true, false]:
                var button := InputEventMouseButton.new()
                button.position = position
                button.button_index = MOUSE_BUTTON_LEFT
                button.pressed = pressed
                view.dispatch_input_event(button)
        await process_frame
        RenderingServer.force_draw(false)
        samples.append((Time.get_ticks_usec() - start) / 1000.0)
    samples.sort()
    var diagnostics := view.get_frame_scheduler_diagnostics()
    print('COMPONENT_INTERACTION ', JSON.stringify({'samples':samples.size(), 'median_ms':samples[100], 'p95_ms':samples[190], 'worst_ms':samples[-1], 'synchronization':diagnostics.frame_synchronization}))
    if diagnostics.frame_synchronization.get('terminal', false):
        push_error('Component probe renderer terminalized')
        quit(1)
        return
    print('COMPONENT_PROBE_PASS')
    quit()

extends SceneTree

func _initialize():
    run.call_deferred()
    create_timer(120).timeout.connect(func(): quit(2))

func frames(count: int):
    for i in count:
        await process_frame
        await RenderingServer.frame_post_draw

func summarize(samples: Array) -> Dictionary:
    samples.sort()
    return {'count':samples.size(), 'median_ms':samples[100], 'p95_ms':samples[190], 'worst_ms':samples[-1]}

func run():
    Engine.max_fps = 0
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    var views: Array[HTMLView] = []
    for page in ['settings','sidebar']:
        var view := HTMLView.new()
        view.size = Vector2(956,1392)
        view.logical_size = Vector2i(956,1392)
        view.fixed_viewport_size = Vector2i(956,1392)
        view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
        view.html_file = 'res://UI/%s.html' % page
        root.add_child(view)
        views.append(view)
    await frames(20)
    var rng := RandomNumberGenerator.new()
    rng.seed = 8431
    var pointer := []
    var clicks := []
    for i in 200:
        var view := views[i % views.size()]
        var position := Vector2(rng.randf_range(0,956),rng.randf_range(0,1392))
        var motion := InputEventMouseMotion.new()
        motion.position = position
        var started := Time.get_ticks_usec()
        view.dispatch_input_event(motion)
        await process_frame
        RenderingServer.force_draw(false)
        pointer.append((Time.get_ticks_usec()-started)/1000.0)
        await frames(10)
        started = Time.get_ticks_usec()
        for pressed in [true,false]:
            var button := InputEventMouseButton.new()
            button.position = position
            button.button_index = MOUSE_BUTTON_LEFT
            button.pressed = pressed
            view.dispatch_input_event(button)
        await process_frame
        RenderingServer.force_draw(false)
        clicks.append((Time.get_ticks_usec()-started)/1000.0)
    for view in views:
        assert(not view.get_frame_scheduler_diagnostics().frame_synchronization.get('terminal',false))
    print('SHARED_ATLAS_INTERACTION ', JSON.stringify({'pointer':summarize(pointer),'click':summarize(clicks),'positions':200,'settle_frames_per_position':10}))
    print('SHARED_ATLAS_INTERACTION_PASS')
    quit()

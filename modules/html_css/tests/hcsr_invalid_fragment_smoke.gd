extends SceneTree

var failed := false
const SIZE := Vector2i(320, 180)
const INVALID := ["<", "<space=-3em>", "<div>", "<div></span>", "<div a='1' A='2'></div>"]

func _initialize():
    run.call_deferred()
    create_timer(45).timeout.connect(func(): quit(2))

func check(ok: bool, label: String):
    if not ok:
        failed = true
        push_error("INVALID_FRAGMENT " + label)

func settle():
    for i in 8:
        await process_frame
        await RenderingServer.frame_post_draw

func run():
    var background := ColorRect.new()
    background.color = Color(.1, .8, .2)
    background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
    root.add_child(background)
    var view := HTMLView.new()
    view.size = SIZE
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.fixed_viewport_size = SIZE
    view.logical_size = SIZE
    view.backend_preference = HTMLView.BACKEND_GPU_AUTO
    var document := HTMLDocument.new()
    document.background_color = Color.TRANSPARENT
    document.html = "<style>html,body{margin:0;background:transparent}#slot{width:80px;height:50px;background:red}</style><div id='slot'></div><input id='field' value='original'>"
    view.document = document
    root.add_child(view)
    # No frame boundary yet: exercise the Godot startup queue too.
    for markup in INVALID:
        check(view.apply_element_mutations([
            {'operation':'set_value','id':'field','value':'must not apply'},
            {'operation':'set_inner_html','id':'slot','value':markup}
        ]) != OK, "startup batch rejected: " + markup)
    await settle()
    var output := view.create_output(SIZE, false)
    await settle()
    var before := output.texture.get_image().get_data()
    var generation := output.generation
    var outside := root.get_texture().get_image().get_pixel(250, 150)
    check(outside.g > .75, "transparent view preserves underlying green scene")
    for operation in ['set_inner_html', 'insert_html']:
        for markup in INVALID:
            check(view.apply_element_mutations([
                {'operation':'set_value','id':'field','value':'must not apply'},
                {'operation':operation,'id':'slot','value':markup,'index':-1}
            ]) != OK, "live batch rejected: " + markup)
            await settle()
            check(view.get_form_control_state('field').value == 'original', "rejection is atomic")
            check(output.texture.get_image().get_data() == before, "last output pixels preserved")
            check(output.generation == generation, "no replacement generation on rejection")
            var sync: Dictionary = view.get_frame_scheduler_diagnostics().get('frame_synchronization', {})
            check(not sync.get('terminal', false), "syntax rejection is nonterminal")
            outside = root.get_texture().get_image().get_pixel(250, 150)
            check(outside.g > .75, "underlying scene remains visible")
    check(view.set_element_inner_html('slot', "<div style='width:80px;height:50px;background:blue'>&lt;</div>") == OK, "escaped fragment accepted")
    await settle()
    check(output.generation > generation, "valid mutation after rejection publishes")
    check(output.texture.get_image().get_data() != before, "valid mutation changes pixels")
    print('INVALID_FRAGMENT_', 'FAIL' if failed else 'PASS')
    quit(1 if failed else 0)


extends SceneTree

# Exercise pooled document snapshots against a nonuniform host image. Moving a
# panel must not sample a previous frame's snapshot outside its new footprint.
var failed := false

func _initialize() -> void:
    run.call_deferred()
    create_timer(90).timeout.connect(func(): quit(2))

func settle() -> void:
    for i in 10:
        await process_frame
        await RenderingServer.frame_post_draw

func run() -> void:
    Engine.max_fps = 0
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    var background := ColorRect.new()
    background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
    var shader := Shader.new()
    shader.code = "shader_type canvas_item; uniform float phase; void fragment(){vec2 p=FRAGCOORD.xy+vec2(phase*13.0,phase*9.0); COLOR=vec4(.5+.4*sin(p.x*.13),.5+.4*cos(p.y*.17),.5+.4*sin((p.x+p.y)*.07),1);}"
    var material := ShaderMaterial.new()
    material.shader = shader
    background.material = material
    root.add_child(background)
    var view := HTMLView.new()
    view.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
    view.backend_preference = HTMLView.BACKEND_GPU_AUTO
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.logical_size = Vector2i(320, 180)
    view.backdrop_filter_enabled = true
    var document := HTMLDocument.new()
    document.background_color = Color.TRANSPARENT
    document.html = "<style>html,body{margin:0;background:transparent}#glass{position:absolute;left:24px;top:20px;width:90px;height:70px;border-radius:13px;backdrop-filter:blur(3px) invert(1)}#other{position:absolute;left:170px;top:40px;width:105px;height:105px;border-radius:20px;backdrop-filter:blur(2px) blur(3px) saturate(.5)}</style><div id='glass'></div><div id='other'></div>"
    view.document = document
    root.add_child(view)
    var output := OS.get_environment("HCSR_DOCUMENT_REGION_OUTPUT")
    var reference := OS.get_environment("HCSR_DOCUMENT_REGION_REFERENCE")
    if not output.is_empty():
        DirAccess.make_dir_recursive_absolute(output)
    var sample := 0
    for physical_size in [Vector2i(320, 180), Vector2i(640, 360), Vector2i(160, 90)]:
        DisplayServer.window_set_size(physical_size)
        await settle()
        for style in [
            "left:24px;top:20px;backdrop-filter:invert(1)",
            "left:150px;top:95px;backdrop-filter:blur(3px) invert(1)",
            "left:-30px;top:-20px;backdrop-filter:blur(2px) blur(3px) invert(1)",
            "left:32px;top:28px;transform:rotate(17deg) scale(.75,1.2);backdrop-filter:blur(4px) invert(1)",
            "left:55px;top:35px;transform:perspective(200px) rotateY(20deg);backdrop-filter:blur(3px) invert(1)",
            "left:150px;top:95px;backdrop-filter:invert(1)",
            "left:400px;top:300px;backdrop-filter:blur(3px) invert(1)",
            "left:24px;top:20px;backdrop-filter:blur(3px) invert(1)",
        ]:
            material.set_shader_parameter("phase",float(sample)*.45)
            if view.set_element_attribute("glass", "style", style) != OK:
                failed = true
            await settle()
            var sync: Dictionary = view.get_frame_scheduler_diagnostics().get("frame_synchronization", {})
            if sync.get("terminal", false):
                failed = true
            if not output.is_empty() or not reference.is_empty():
                var image := root.get_texture().get_image()
                if not output.is_empty() and image.save_png(output.path_join("%02d.png" % sample)) != OK:
                    failed = true
                if not reference.is_empty():
                    var before := Image.load_from_file(reference.path_join("%02d.png" % sample))
                    if before == null or before.get_size() != image.get_size():
                        failed = true
                        push_error("DOCUMENT_REGION missing reference %s" % sample)
                    else:
                        # Permit one 8-bit rounding level between equivalent GPU
                        # command sequences, but reject missing/stale filter pixels.
                        var a := before.get_data()
                        var b := image.get_data()
                        for i in a.size():
                            if absi(int(a[i]) - int(b[i])) > 1:
                                failed = true
                                push_error("DOCUMENT_REGION changed sample %s byte %s: %s -> %s" % [sample, i, a[i], b[i]])
                                break
            sample += 1
    print("DOCUMENT_REGION_", "FAILED" if failed else "OK", " samples=", sample)
    quit(1 if failed else 0)

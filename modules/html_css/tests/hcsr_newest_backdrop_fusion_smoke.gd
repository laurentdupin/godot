extends SceneTree

# Compare with HCSR_SEQUENTIAL_BACKDROP_COMPOSITES=1, using the same executable.
# Covers overlapping input, foreground outside the mask, nested scopes, group
# opacity, transformed blur, moving host pixels and fixed logical resolution.
var failed := false
func _initialize():
    run.call_deferred()
    create_timer(90).timeout.connect(func(): quit(2))
func settle():
    for i in 10:
        await process_frame
        await RenderingServer.frame_post_draw
func require(value, label):
    if not value:
        failed = true
        push_error("BACKDROP_FUSION_FAILED " + label)
func run():
    Engine.max_fps = 0
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    root.set_disable_input(true)
    var host := ColorRect.new()
    host.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
    var shader := Shader.new()
    shader.code = "shader_type canvas_item; uniform float phase; void fragment(){vec2 p=FRAGCOORD.xy; COLOR=vec4(.5+.4*sin(p.x*.13+phase),.5+.4*cos(p.y*.17+phase),.5+.4*sin((p.x+p.y)*.07),1);}"
    var material := ShaderMaterial.new()
    material.shader = shader
    host.material = material
    root.add_child(host)
    var view := HTMLView.new()
    view.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
    view.backend_preference = HTMLView.BACKEND_GPU_AUTO
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.logical_size = Vector2i(320,180)
    view.backdrop_filter_enabled = true
    var doc := HTMLDocument.new()
    doc.background_color = Color.TRANSPARENT
    doc.html = """<style>html,body{margin:0;background:transparent}.glass{position:absolute;width:105px;height:90px;border-radius:15px;background:#ffffff25;backdrop-filter:invert(1)}#a{left:15px;top:15px}#b{left:80px;top:45px;backdrop-filter:blur(3px) saturate(.5)}#c{left:210px;top:20px;opacity:.5;backdrop-filter:blur(2px) blur(3px) invert(1)}#nested{position:absolute;left:5px;top:40px;width:45px;height:35px;opacity:.6;background:#f002;backdrop-filter:contrast(2)}span{display:block;color:white;font:18px Arial}#escape{position:absolute;left:-12px;top:80px;width:100px;height:30px;background:#ff0080}#d,#e,#f{top:135px;width:35px;height:30px;border-radius:6px;backdrop-filter:invert(1)}#d{left:165px}#e{left:220px}#f{left:280px}#bleed{position:absolute;left:30px;top:5px;width:55px;height:8px;background:#e0505080}</style><div id='a' class='glass'><span>Alpha</span><div id='escape'></div></div><div id='b' class='glass'><span>Beta</span></div><div id='c' class='glass'><span>Gamma</span><div id='nested'>Inner</div></div><div id='d' class='glass'><div id='bleed'></div></div><div id='e' class='glass'></div><div id='f' class='glass'></div>"""
    view.document = doc
    root.add_child(view)
    var out := OS.get_environment("HCSR_FUSION_OUTPUT")
    var reference := OS.get_environment("HCSR_FUSION_REFERENCE")
    if not out.is_empty(): DirAccess.make_dir_recursive_absolute(out)
    var sample := 0
    for extent in [Vector2i(320,180),Vector2i(640,360),Vector2i(160,90)]:
        DisplayServer.window_set_size(extent)
        await settle()
        for state in 4:
            material.set_shader_parameter("phase",float(state))
            require(view.set_element_attribute("d","style","opacity:%s" % (0.0 if state==1 else .5 if state==2 else 1.0))==OK,"cohort head visibility" )
            require(view.set_element_attribute("b","style","transform:translateX(%spx) rotate(%sdeg);opacity:%s" % [state*9,state*7,1.0-state*.2])==OK,"mutation")
            await settle()
            var image := root.get_texture().get_image()
            if not out.is_empty(): image.save_png(out.path_join("%02d.png" % sample))
            if not reference.is_empty():
                var expected := Image.load_from_file(reference.path_join("%02d.png" % sample))
                require(expected != null and expected.get_size()==image.get_size(),"reference extent")
                if expected != null and expected.get_size()==image.get_size():
                    var a := image.get_data()
                    var b := expected.get_data()
                    var maximum := 0
                    for i in a.size(): maximum = maxi(maximum,absi(int(a[i])-int(b[i])))
                    require(maximum<=3,"pixel difference %s: %s" % [sample,maximum])
            sample += 1
    var sync: Dictionary = view.get_frame_scheduler_diagnostics().frame_synchronization
    require(not sync.get("terminal",false),"nonterminal")
    if OS.get_environment("HCSR_SEQUENTIAL_BACKDROP_COMPOSITES")!="1":
        require(sync.image_atlas.get("fused_backdrop_composites",0)>0,"fused path exercised")
        require(sync.image_atlas.get("document_snapshot_cohorts",0)>0,"cohort path exercised")
        require(sync.image_atlas.get("cpu_compositing_bounds_evaluations",0)==0,"no CPU projection")
    print("BACKDROP_FUSION_", "FAILED" if failed else "OK", " states=",sample," diagnostics=",sync.image_atlas)
    quit(1 if failed else 0)

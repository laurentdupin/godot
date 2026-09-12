extends SceneTree
var failed := false
func _initialize() -> void:
    run.call_deferred()
    create_timer(30).timeout.connect(func(): quit(2))
func require(value: bool, message: String) -> void:
    if not value:
        failed = true
        push_error("SHADER_SAMPLING_FAILED " + message)
func settle() -> void:
    for i in 8:
        await process_frame
        await RenderingServer.frame_post_draw
func run() -> void:
    var source := Image.create(2,2,false,Image.FORMAT_RGBA8)
    for y in 2:
        source.set_pixel(0,y,Color.RED)
        source.set_pixel(1,y,Color(0,1,0,0)) # Hidden green must never create a fringe.
    var uri := "data:image/png;base64," + Marshalls.raw_to_base64(source.save_png_to_buffer())
    var doc := HTMLDocument.new()
    doc.html = "<style>html,body{margin:0;width:128px;height:96px}img{position:absolute;left:8px;top:8px;width:64px;height:64px;opacity:.5}</style><img id='image' src='%s'>" % uri
    var views: Array[HTMLView] = []
    for backend in [HTMLView.BACKEND_CPU, HTMLView.BACKEND_GPU_AUTO]:
        var view := HTMLView.new()
        view.backend_preference = backend
        view.size = Vector2(128,96)
        view.logical_size = Vector2i(128,96)
        view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
        view.document = doc
        root.add_child(view)
        views.append(view)
    await settle()
    var images: Array[Image] = [views[0].get_texture().get_image(),views[1].get_texture().get_image()]
    var out := OS.get_environment("HCSR_SHADER_OUTPUT")
    if not out.is_empty():
        DirAccess.make_dir_recursive_absolute(out)
        FileAccess.open(out.path_join("sampling.html"),FileAccess.WRITE).store_string(doc.html)
        images[0].save_png(out.path_join("cpu.png"))
        images[1].save_png(out.path_join("gpu.png"))
    for image in images:
        var middle := image.get_pixel(39,24)
        require(abs(middle.r-.258) < .012 and abs(middle.a-.258) < .012 and middle.g < .004 and middle.b < .004,
                "transparent texel filtering, opacity and premultiplied output: " + str(middle))
        require(image.get_pixel(8,24).r > .49 and image.get_pixel(71,24).a < .004, "atlas edge clamp")
        require(image.get_pixel(7,24).a < .004 and image.get_pixel(72,24).a < .004, "image coverage")
    var maximum := 0.0
    for y in range(12,68):
        for x in range(12,68):
            var a := images[0].get_pixel(x,y)
            var b := images[1].get_pixel(x,y)
            maximum = maxf(maximum,maxf(abs(a.r-b.r),maxf(abs(a.g-b.g),abs(a.a-b.a))))
    require(maximum < .009, "CPU/GPU sampling mismatch: " + str(maximum))
    for view in views:
        require(view.set_element_attribute("image","style","opacity:.25") == OK,"opacity mutation")
    await settle()
    for view in views:
        var middle := view.get_texture().get_image().get_pixel(39,24)
        require(abs(middle.r-.129) < .012 and abs(middle.a-.129) < .012 and middle.g < .004,"updated tint/opacity")
        view.queue_free()
    await settle()
    print("SHADER_SAMPLING_", "FAILED" if failed else "OK", " max_cpu_gpu=",maximum)
    quit(1 if failed else 0)

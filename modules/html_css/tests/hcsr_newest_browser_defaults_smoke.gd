extends SceneTree

func _initialize():
    run.call_deferred()
    create_timer(45).timeout.connect(func(): quit(2))

func check(ok: bool, message: String):
    if not ok:
        push_error(message)
        quit(1)
        assert(ok, message)

func settle():
    for i in 8:
        await process_frame
        await RenderingServer.frame_post_draw

func ink(image: Image, y: int) -> int:
    var count = 0
    for row in range(y + 8, y + 24):
        for x in range(20, 140):
            var c = image.get_pixel(x,row)
            if c.r < .3 and c.g < .3 and c.b < .3:
                count += 1
    return count

func run():
    Engine.max_fps = 60
    root.size = Vector2i(200,220)
    var document = HTMLDocument.new()
    document.html = """<html><head><style>
        body{margin:0;background:white}input{position:absolute;left:10px;width:140px;height:32px}
        #submit{top:10px}#blank{top:52px}#value{top:94px}
        mark{position:absolute;left:10px;top:150px;width:100px;height:20px;display:block}
        </style></head><body><input id='submit' type='submit'>
        <input id='blank' type='submit' value=''><input id='value' type='button' value='Value'>
        <mark>Highlight</mark></body></html>"""
    var view = HTMLView.new()
    view.backend_preference = HTMLView.BACKEND_VULKAN if OS.get_environment("HCSR_TEST_GPU") == "vulkan" else HTMLView.BACKEND_D3D12
    view.size = Vector2(200,220)
    view.logical_size = Vector2i(200,220)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.document = document
    root.add_child(view)
    await settle()
    var image = view.get_texture().get_image()
    check(ink(image,10)>20, "Missing default submit caption")
    check(ink(image,52)==0, "Explicit empty value should not draw a caption")
    var mark_color = image.get_pixel(105,155)
    check(mark_color.r>.9 and mark_color.g>.9 and mark_color.b<.1, "Missing default mark highlight")
    for value in ["", "Updated", ""]:
        check(view.set_element_attribute("value", "value", value)==OK, "Value mutation rejected")
        await settle()
        image = view.get_texture().get_image()
        check(ink(image,94)==0 if value.is_empty() else ink(image,94)>20, "Stale caption after value mutation")
    print("BROWSER_DEFAULTS_RENDER_OK submit, empty value, highlight, repeated caption updates")
    quit(0)

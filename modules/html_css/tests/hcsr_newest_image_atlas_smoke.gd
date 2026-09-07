extends SceneTree

var failed := false
var cpu := false

func require(condition: bool, label: String) -> void:
    if not condition:
        failed = true
        push_error("ATLAS_CHECK_FAILED " + label)

func _initialize() -> void:
    run.call_deferred()
    create_timer(30).timeout.connect(func(): quit(2))

func settle() -> void:
    for i in 4:
        await process_frame
        await RenderingServer.frame_post_draw

func png(color: Color, width := 16, height := 8) -> String:
    var image := Image.create(width, height, false, Image.FORMAT_RGBA8)
    image.fill(color)
    return "data:image/png;base64," + Marshalls.raw_to_base64(image.save_png_to_buffer())

func check_pixels(image: Image, blue := false) -> void:
    var scale := Vector2(image.get_size()) / Vector2(320, 200)
    var pixel := func(x: int, y: int) -> Color: return image.get_pixel(int(x*scale.x), int(y*scale.y))
    var color: Color = pixel.call(20, 40)
    require((color.b > .9 and color.r < .1) if blue else (color.r > .9 and color.g < .1), "image source color " + str(color))
    require(pixel.call(20, 20).a < .1, "contain leaves transparent letterbox")
    color = pixel.call(20, 60)
    require(abs(color.r-color.g) < .01 and abs(color.g-color.b) < .01 and color.a > .9, "later grayscale overlay stays above image")
    color = pixel.call(120, 20)
    require(color.r > .9 and color.g < .1, "clipped image inside")
    require(pixel.call(160, 20).a < .1, "clipped image outside")
    color = pixel.call(220, 30)
    require(color.g > .9 and color.r < .1, "SVG color")
    color = pixel.call(20, 115)
    require(abs(color.a - .5) < .03, "straight alpha opacity " + str(color))

func run() -> void:
    cpu = "--cpu" in OS.get_cmdline_user_args()
    var red := png(Color.RED)
    var blue := png(Color.BLUE)
    var green := "data:image/svg+xml;base64," + Marshalls.utf8_to_base64("<svg xmlns='http://www.w3.org/2000/svg' width='16' height='8'><rect width='16' height='8' fill='#00ff00'/></svg>")
    var view := HTMLView.new()
    view.size = Vector2(320, 200)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.logical_size = Vector2i(320, 200)
    view.backend_preference = HTMLView.BACKEND_CPU if cpu else HTMLView.BACKEND_GPU_AUTO
    var doc := HTMLDocument.new()
    doc.html = """<style>html,body{margin:0;width:100%%;height:100%%}img{display:block;position:absolute}#first{left:10px;top:10px;width:80px;height:80px;object-fit:contain}#clip{position:absolute;left:110px;top:10px;width:40px;height:50px;overflow:hidden}#clip img{width:80px;height:80px}#svg{left:200px;top:10px;width:80px;height:80px;object-fit:cover}#alpha{left:10px;top:100px;width:40px;height:40px;opacity:.5;transform:rotate(30deg)}#overlay{position:absolute;left:10px;top:50px;width:80px;height:20px;background:red}</style><img id='first' src='%s'><div id='clip'><img src='%s'></div><img id='svg' src='%s'><img id='alpha' src='%s'><div id='overlay'></div>""" % [red, red, green, red]
    view.document = doc
    root.add_child(view)
    var large := view.create_output(Vector2i(640,400), true)
    var small := view.create_output(Vector2i(160,100), false)
    await settle()
    require(large != null and small != null, "outputs created")
    check_pixels(large.texture.get_image())
    check_pixels(small.texture.get_image())
    var stats: Dictionary = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
    if not stats.has("decoded_images"):
        quit(1)
        return
    require(int(stats.decoded_images) == 2 and int(stats.pages) == 1, "repeated PNG shares allocation: " + str(stats))
    var uploaded := int(stats.uploaded_bytes)
    require(view.set_element_attribute("first", "src", blue) == OK, "source mutation")
    await settle()
    check_pixels(large.texture.get_image(), true)
    check_pixels(small.texture.get_image(), true)
    stats = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
    require(int(stats.decoded_images) == 3 and int(stats.pages) == 1, "new source allocation")
    if not cpu:
        require(int(stats.uploaded_bytes)-uploaded == 18*10*4, "only new padded region uploaded: " + str(stats))
    large.size = Vector2i(960,600)
    await settle()
    check_pixels(large.texture.get_image(), true)
    require(large.generation == view.get_generation() and small.generation == large.generation, "shared packet generation")
    require(int(view.get_frame_scheduler_diagnostics().frame_synchronization.failures) == 0, "synchronization")
    # Files use the same explicit Godot resource resolver as stylesheets.
    var file_image := Image.create(16, 8, false, Image.FORMAT_RGBA8)
    file_image.fill(Color.GREEN)
    require(file_image.save_png("user://atlas-file.png") == OK, "write local fixture")
    require(view.set_element_attribute("svg", "src", "user://atlas-file.png") == OK, "local image reference")
    await settle()
    check_pixels(large.texture.get_image(), true)
    if "--pages" in OS.get_cmdline_user_args():
        require(view.set_element_attribute("first", "src", png(Color.YELLOW,4094,4094)) == OK, "second page")
        require(view.set_element_attribute("svg", "src", png(Color.MAGENTA,4094,4094)) == OK, "third page")
        await settle()
        var output := small.texture.get_image()
        var yellow := output.get_pixel(10,20)
        var magenta := output.get_pixel(110,15)
        require(yellow.r > .9 and yellow.g > .9 and yellow.b < .1, "second atlas page sampling")
        require(magenta.r > .9 and magenta.b > .9 and magenta.g < .1, "third atlas page sampling")
        stats = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
        require(int(stats.pages) == 3 and int(stats.draw_batches) >= 3, "page overflow and ordered batches: " + str(stats))
    stats = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
    large.texture.get_image().save_png("user://image-atlas-smoke.png")
    large.release()
    small.release()
    view.queue_free()
    await settle()
    print("IMAGE_ATLAS_", "FAILED" if failed else "OK", " cpu=", cpu, " stats=", stats)
    quit(1 if failed else 0)

extends SceneTree

var failed := false
func require(value: bool, label: String) -> void:
    if not value:
        failed = true
        push_error("DIRECT_PAINT_FAILED " + label)

func _initialize() -> void:
    run.call_deferred()
    create_timer(30).timeout.connect(func(): quit(2))

func settle() -> void:
    for i in 4:
        await process_frame
        await RenderingServer.frame_post_draw

func run() -> void:
    var cpu := "--cpu" in OS.get_cmdline_user_args()
    var view := HTMLView.new()
    view.size = Vector2(160,120)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.logical_size = Vector2i(160,120)
    view.backend_preference = HTMLView.BACKEND_CPU if cpu else HTMLView.BACKEND_GPU_AUTO
    var doc := HTMLDocument.new()
    doc.html = """<style>html,body{margin:0;width:100%;height:100%}#box{position:absolute;left:10px;top:10px;width:80px;height:80px;box-sizing:border-box;background:red;border:8px solid lime;border-radius:20px}#alpha{position:absolute;left:100px;top:10px;width:40px;height:80px;background:rgba(0,0,255,.5);border-radius:12px}</style><div id='box'></div><div id='alpha'></div><div id='extra'></div>"""
    view.document = doc
    root.add_child(view)
    var output := view.create_output(Vector2i(160,120), true)
    await settle()
    var image: Image = output.texture.get_image()
    require(image.get_pixel(12,40).g > .9, "solid border")
    require(image.get_pixel(40,40).r > .9, "background")
    require(image.get_pixel(10,10).a < .05, "rounded corner")
    require(abs(image.get_pixel(120,40).a-.5) < .03, "background alpha")
    var stats: Dictionary = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
    require(int(stats.raster_surfaces) == 2, "solid appearances have independent raster resources")
    var uploaded := int(stats.uploaded_bytes)
    require(view.set_element_attribute("box","style","width:90px;transform:translateX(2px)") == OK,"stretch and move")
    await settle()
    stats = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
    require(int(stats.uploaded_bytes) == uploaded, "stretch and move reuse uploaded pixels")
    image = output.texture.get_image()
    require(image.get_pixel(14,40).g > .9 && image.get_pixel(85,40).r > .9, "nine-slice placement after resize")
    require(view.set_element_attribute("box","style","background:blue;border-radius:0") == OK,"mutation")
    await settle()
    image = output.texture.get_image()
    require(image.get_pixel(40,40).b > .9 && image.get_pixel(10,10).a > .9,"mutated paint")
    var svg := "data:image/svg+xml;base64," + Marshalls.utf8_to_base64("<svg xmlns='http://www.w3.org/2000/svg' width='16' height='8'><rect width='16' height='8' fill='lime'/></svg>")
    require(view.set_element_inner_html("extra","<img style='position:absolute;left:100px;top:95px;width:16px;height:8px' src='%s'><span style='position:absolute;left:5px;top:95px;color:white;font-size:14px'>Atlas</span>" % svg) == OK,"add image")
    await settle()
    image = output.texture.get_image()
    require(image.get_pixel(105,98).g > .9 && image.get_pixel(40,40).b > .9,"mixed direct and atlas paint")
    stats = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
    require(int(stats.pages) == 1 && int(stats.glyph_pages) == 1 && int(stats.raster_surfaces) == 2, "images, glyphs and box surfaces share one page")
    output.size = Vector2i(320,240)
    await settle()
    image = output.texture.get_image()
    require(image.get_pixel(210,196).g > .9 && image.get_pixel(80,80).b > .9,"resized output")
    output.release()
    view.queue_free()
    await settle()
    print("DIRECT_PAINT_", "FAILED" if failed else "OK", " cpu=",cpu)
    quit(1 if failed else 0)

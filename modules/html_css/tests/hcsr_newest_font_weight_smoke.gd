extends SceneTree

# Explicit fixtures: static regular/bold and a variable font with a wght axis.
# Pass res:// or user:// font paths through HCSR_FONT_REGULAR/BOLD/VARIABLE.
var view: HTMLView

func _initialize() -> void:
    run.call_deferred()
    create_timer(60).timeout.connect(func(): quit(2))

func check(ok: bool, message: String) -> void:
    if not ok:
        push_error(message)
        quit(1)
        assert(ok, message)

func settle() -> void:
    for i in 8:
        await process_frame
        await RenderingServer.frame_post_draw

func run() -> void:
    Engine.max_fps = 60
    root.size = Vector2i(800,720)
    var regular = OS.get_environment("HCSR_FONT_REGULAR").replace("\\", "/")
    var bold = OS.get_environment("HCSR_FONT_BOLD").replace("\\", "/")
    var variable = OS.get_environment("HCSR_FONT_VARIABLE").replace("\\", "/")
    for path in [regular, bold, variable]:
        check(FileAccess.file_exists(path), "Missing explicit font fixture: " + path)
    var css = "body{margin:0;background:white;color:black}.row{height:60px;font-size:40px;line-height:60px}"
    for face in [["Regular", regular, "400"], ["Bold", bold, "700"],
        ["Keywords", regular, "normal"], ["Keywords", bold, "bold"],
        ["Matching", bold, "300"], ["Matching", regular, "500"],
        ["Range", bold, "100 300"], ["Range", variable, "400 900"],
        ["Variable", variable, "100 900"]]:
        css += "@font-face{font-family:'%s';src:url('%s');font-weight:%s}" % face
    var html = "<html><head><style>" + css + "</style></head><body>"
    for row in [["Regular",400], ["Keywords",400], ["Bold",700], ["Keywords",700],
        ["Matching",400], ["Regular",700], ["Variable",700], ["Range",700],
        ["Variable",400], ["Regular",400]]:
        html += "<div class='row' id='r%d' style='font-family:%s;font-weight:%d'>Hamburgefonts 0123</div>" % [html.count("class='row'"),row[0],row[1]]
    html += "</body></html>"
    var doc = HTMLDocument.new()
    doc.html = html
    view = HTMLView.new()
    view.backend_preference = HTMLView.BACKEND_VULKAN if OS.get_environment("HCSR_TEST_GPU") == "vulkan" else HTMLView.BACKEND_D3D12
    view.size = Vector2(800,720)
    view.logical_size = Vector2i(800,720)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.document = doc
    root.add_child(view)
    await settle()
    var pixels = view.get_texture().get_image()
    if not OS.get_environment("HCSR_FONT_OUTPUT").is_empty():
        pixels.save_png(OS.get_environment("HCSR_FONT_OUTPUT") + ".png")
    print("WEIGHT_DIFF ", row_difference(pixels,0,5), " ", row_difference(pixels,6,8))
    for pair in [[0,1],[2,3],[0,4],[6,7]]:
        check(row_difference(pixels,pair[0],pair[1]) < .001, "Incorrect font face/range selection: " + str(pair))
    check(row_difference(pixels,0,5) > .005, "Static regular face did not synthesize bold")
    check(row_difference(pixels,6,8) > .005, "Variable weight was ignored")
    for weight in [700,400,700,400]:
        check(view.set_element_attribute("r9", "style", "font-family:Regular;font-weight:%d" % weight) == OK, "Weight mutation failed")
        await settle()
        pixels = view.get_texture().get_image()
        check(row_difference(pixels,9,5 if weight == 700 else 0) < .001, "Stale glyphs after weight mutation")
    var out = OS.get_environment("HCSR_FONT_OUTPUT")
    if not out.is_empty():
        pixels.save_png(out + ".png")
        var file = FileAccess.open(out + ".html", FileAccess.WRITE)
        file.store_string(html)
    check(not view.get_frame_scheduler_diagnostics()["frame_synchronization"].get("terminal", true), "Renderer failed")
    print("FONT_WEIGHT_RENDER_OK keywords, CSS matching, ranges, synthetic bold, variable weight, repeated mutation")
    quit(0)

func row_difference(pixels: Image, a: int, b: int) -> float:
    var total = 0.0
    for y in 60:
        for x in 500:
            total += abs(pixels.get_pixel(x,a*60+y).r - pixels.get_pixel(x,b*60+y).r)
    return total / 30000.0

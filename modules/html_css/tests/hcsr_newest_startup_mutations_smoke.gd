extends SceneTree
const BASE := "<style>body{margin:0}#app{width:128px;height:64px;background:blue}section{width:128px;height:64px}</style><div id='app'></div>"
var errors: Array[String] = []
func _initialize() -> void:
    run.call_deferred()
func check(value: bool, message: String) -> void:
    if not value:
        push_error(message)
        quit(1)
        assert(value, message)
func document() -> HTMLDocument:
    var result := HTMLDocument.new()
    result.html = BASE
    return result
func markup(color: String) -> String:
    return "<section style='background:%s'></section>" % color
func new_view() -> HTMLView:
    var view := HTMLView.new()
    view.backend_preference = HTMLView.BACKEND_D3D12
    view.size = Vector2(128,64)
    view.logical_size = Vector2i(128,64)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.document = document()
    view.render_error.connect(func(message: String): errors.append(message))
    root.add_child(view)
    return view
func first_frame(view: HTMLView, label: String) -> void:
    var activated: Array[int] = []
    view.frame_activated.connect(func(generation: int): activated.append(generation))
    for i in 120:
        await process_frame
        await RenderingServer.frame_post_draw
        if not activated.is_empty():
            var pixel := view.get_texture().get_image().get_pixel(20,20)
            check(pixel.g > .8 and pixel.r < .2 and pixel.b < .2, label + ": first frame is not final green markup: " + str(pixel))
            check(errors.is_empty(), label + ": " + str(errors))
            print("STARTUP_FIRST_FRAME_OK ",label," generation=",activated[0])
            return
    check(false, label + ": no frame")
func dispose(view: HTMLView) -> void:
    view.queue_free()
    for i in 4: await process_frame
func run() -> void:
    root.size = Vector2i(256,128)
    Engine.max_fps = 60
    for mode in ["dependent targets", "dependent batch", "ordered singles", "batch", "replacement", "same document change", "clear", "unloaded preload"]:
        var view := new_view()
        check(view.get_generation() == 0, "test must precede the first frame")
        check(view.set_element_inner_html("app", markup("red")) == OK, "startup update rejected")
        if mode == "dependent targets":
            check(view.set_element_inner_html("app", "<section id='setup-page' style='background:red'></section>") == OK, "page insertion rejected")
            check(view.set_element_attribute("setup-page", "style", "background:yellow") == OK, "new descendant rejected")
            check(view.set_element_inner_html("app", "<section id='setup-page' style='background:red'></section>") == OK, "replacement rejected")
            check(view.set_element_attribute("setup-page", "id", "renamed-page") == OK, "rename rejected")
            check(view.set_element_attribute("renamed-page", "style", "background:lime") == OK, "renamed target rejected")
        elif mode == "dependent batch":
            check(view.apply_element_mutations([
                {"operation":"set_inner_html", "id":"app", "value":"<section id='child' style='background:red'></section>"},
                {"operation":"set_attribute", "id":"child", "name":"style", "value":"background:lime"}
            ]) == OK, "dependent batch rejected")
        elif mode == "batch":
            check(view.apply_element_mutations([
                {"operation":"set_inner_html", "id":"app", "value":markup("yellow")},
                {"operation":"set_inner_html", "id":"app", "value":markup("lime")}
            ]) == OK, "startup batch rejected")
        elif mode == "unloaded preload":
            var page := markup("lime")
            var preload_id := view.preload_page(page)
            check(preload_id != 0, "preload creation failed")
            check(view.set_element_inner_html_with_preload("app", page, preload_id) == OK, "preloaded update rejected")
            check(view.unload_page(preload_id) == OK, "preload unload failed")
        else:
            if mode in ["replacement", "same document change", "clear"]:
                # This pending request must be discarded with the old document,
                # rather than surfacing a late missing-target error on its replacement.
                check(view.set_element_inner_html("discarded-target", "obsolete") == OK, "deferred mutation rejected")
            if mode == "replacement":
                view.document = document()
            elif mode == "same document change":
                view.document.html = BASE + "<!--new document revision-->"
            elif mode == "clear":
                view.document = null
                check(view.set_element_inner_html("app", markup("red")) == ERR_UNCONFIGURED, "mutation accepted without a document")
                view.document = document()
            check(view.set_element_inner_html("app", markup("lime")) == OK, "final update rejected")
        await first_frame(view, mode)
        if mode == "dependent targets":
            check(view.set_element_inner_html("app", "<section id='live-child' style='background:red'></section>") == OK, "live insertion rejected")
            check(view.set_element_attribute("live-child", "style", "background:lime") == OK, "live dependent update rejected")
            for i in 8:
                await process_frame
                await RenderingServer.frame_post_draw
            var live_pixel := view.get_texture().get_image().get_pixel(20,20)
            check(live_pixel.g > .8 and live_pixel.r < .2, "live dependent update was lost")
            print("LIVE_DEPENDENT_MUTATION_OK")
        # Rebuilding a live document must not send updates to its previous scene.
        if mode == "replacement":
            view.document = document()
            check(view.set_element_inner_html("app", markup("yellow")) == OK, "live document update rejected")
            for i in 15:
                await process_frame
                await RenderingServer.frame_post_draw
            var pixel := view.get_texture().get_image().get_pixel(20,20)
            check(pixel.r > .8 and pixel.g > .8 and pixel.b < .2, "live document update was lost")
            print("LIVE_DOCUMENT_MUTATION_OK")
        await dispose(view)
    var invalid_view := new_view()
    check(invalid_view.set_element_inner_html("missing-target", "invalid") == OK, "deferred validation should accept input")
    for i in 20: await process_frame
    var failure: Dictionary = invalid_view.get_frame_scheduler_diagnostics()["frame_synchronization"]
    check(not failure.get("terminal", true) and "missing-target" in str(failure.get("last_mutation_rejection", "")), "rejected startup command must be diagnosed without freezing")
    check(invalid_view.get_generation() > 0, "rejected startup command prevented rendering")
    check(invalid_view.set_element_inner_html("app", "<section id='label'>old</section>") == OK, "setup insertion rejected")
    for i in 4: await process_frame
    check(invalid_view.set_element_inner_html("app", "<section id='replacement' style='background:red'></section>") == OK, "setup removal rejected")
    check(invalid_view.set_element_text("label", "stale") == OK, "dependent target must be validated at step")
    check(invalid_view.set_element_attribute("replacement", "style", "background:lime") == OK, "valid command after stale update rejected")
    for i in 8:
        await process_frame
        await RenderingServer.frame_post_draw
    failure = invalid_view.get_frame_scheduler_diagnostics()["frame_synchronization"]
    check(not failure.get("terminal", true), "stale update froze the view")
    check(failure.get("mutation_rejected_frames", 0) == 2, "rejected update was replayed or not diagnosed")
    check("label" in str(failure.get("last_mutation_rejection", "")), "missing stale target diagnostic")
    var recovered := invalid_view.get_texture().get_image().get_pixel(20,20)
    check(recovered.g > .8 and recovered.r < .2, "valid command after rejected update did not render")
    await dispose(invalid_view)
    print("STARTUP_MUTATION_ORDER_OK")
    quit()

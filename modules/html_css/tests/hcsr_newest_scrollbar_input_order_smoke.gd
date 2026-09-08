extends SceneTree
# Regression: an older queued pointer cancellation must not erase a later thumb
# press. Hold through several scene steps before moving, as a person would.
var view: HTMLView
func _initialize() -> void:
    run.call_deferred()
func settle() -> void:
    for _frame in range(15):
        await process_frame
    await RenderingServer.frame_post_draw
func button(pressed: bool, position: Vector2) -> void:
    var event := InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = pressed
    event.position = position
    view.dispatch_input_event(event)
func motion(position: Vector2) -> void:
    var event := InputEventMouseMotion.new()
    event.position = position
    event.button_mask = MOUSE_BUTTON_MASK_LEFT
    view.dispatch_input_event(event)
func sample() -> Color:
    return view.get_texture().get_image().get_pixel(60,50)
func require(value: bool, message: String) -> bool:
    if not value:
        push_error(message)
        quit(1)
    return value
func run() -> void:
    root.size = Vector2i(512,220)
    view = HTMLView.new()
    view.backend_preference = HTMLView.BACKEND_GPU_AUTO
    view.size = Vector2(512,220)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.logical_size = Vector2i(512,220)
    var document := HTMLDocument.new()
    document.html = """<style>body{margin:0}#scroll{position:absolute;left:40px;top:30px;width:260px;height:160px;overflow:auto}</style><div id='scroll'><div id='top' style='height:400px;background:red'></div><div style='height:400px;background:lime'></div></div>"""
    view.document = document
    root.add_child(view)
    await settle()
    for repeat in range(3):
        view.scroll_element_into_view("top", "start")
        await settle()
        if not require(sample().r > .8, "initial content must be red"): return
        view.cancel_pointer_interaction()
        button(true, Vector2(293,45))
        await settle()
        motion(Vector2(293,170))
        await settle()
        if not require(sample().g > .8 and sample().r < .2, "queued cancellation erased later thumb drag"): return
        button(false, Vector2(293,170))
        await settle()
        motion(Vector2(293,45))
        await settle()
        if not require(sample().g > .8, "released thumb still moves"): return
        button(true, Vector2(293,170))
        view.cancel_pointer_interaction()
        await settle()
        motion(Vector2(293,45))
        await settle()
        if not require(sample().g > .8, "later cancellation failed to end thumb drag"): return
    print("SCROLLBAR_INPUT_ORDER_OK: delayed drag, repeated grabs, release, later cancellation")
    quit()

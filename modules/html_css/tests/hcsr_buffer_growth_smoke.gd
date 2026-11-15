extends SceneTree

func _initialize():
    run.call_deferred()
    create_timer(40).timeout.connect(func(): quit(2))

func settle():
    for i in 8:
        await process_frame
        await RenderingServer.frame_post_draw

func run():
    var view := HTMLView.new()
    view.size = Vector2(480, 320)
    var doc := HTMLDocument.new()
    doc.html = "<style>body{margin:0} #effect{opacity:.5;background:red;width:200px;height:100px} #more div{position:absolute;left:250px;top:0;width:4px;height:4px;background:green}</style><div id='effect'><span>Stable group</span></div><div id='more'></div>"
    view.document = doc
    root.add_child(view)
    await settle()
    var first: Dictionary = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
    var before := view.get_texture().get_image().get_pixel(100, 70)
    assert(view.set_element_inner_html("more", "<div></div>".repeat(1800)) == OK)
    await settle()
    var sync: Dictionary = view.get_frame_scheduler_diagnostics().frame_synchronization
    var last: Dictionary = sync.image_atlas
    var after := view.get_texture().get_image().get_pixel(100, 70)
    print("BUFFER_GROWTH allocations=", first.opacity_target_allocations, "->", last.opacity_target_allocations,
        " instances=", first.instances, "->", last.instances, " color=", before, "->", after)
    quit(0 if not sync.terminal and last.instances > 1000 and first.opacity_target_allocations > 0 and
        first.opacity_target_allocations == last.opacity_target_allocations and before == after else 1)

extends SceneTree
var failed = false
func _initialize():
    run.call_deferred()
    create_timer(60).timeout.connect(func(): quit(2))
func require(value, message):
    if not value:
        failed = true
        push_error("GPU_GEOMETRY_FAILED " + message)
func settle():
    for i in 8:
        await process_frame
        await RenderingServer.frame_post_draw
func run():
    var doc = HTMLDocument.new()
    doc.html = "<style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:white}#panel{position:absolute;left:20px;top:20px;width:200px;height:100px;overflow:auto;border-radius:16px}#content{height:800px;width:150px;background:red}#blue{height:60px;background:blue}#fixed{position:fixed;left:260px;top:20px;width:50px;height:50px;background:lime}</style><div id='panel'><div id='content'><div id='blue'></div></div></div><div id='fixed'></div>"
    var views=[]
    for backend in [HTMLView.BACKEND_CPU,HTMLView.BACKEND_GPU_AUTO]:
        var view=HTMLView.new()
        view.backend_preference=backend
        view.viewport_size_mode=HTMLView.VIEWPORT_SIZE_FIXED
        view.logical_size=Vector2i(400,300)
        view.size=Vector2(400,300)
        view.document=doc
        root.add_child(view)
        views.append(view)
    await settle()
    var stats=views[1].get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
    require(stats.gpu_geometry,"GPU packet selected")
    require(stats.instances>0,"instanced drawing")
    var geometry=stats.geometry_generation
    var uploads=stats.geometry_uploaded_bytes
    var out=OS.get_environment("HCSR_GPU_SMOKE_OUTPUT")
    if not out.is_empty(): DirAccess.make_dir_recursive_absolute(out)
    for state in ["before","after"]:
        for i in views.size():
            var image=views[i].get_texture().get_image()
            if not out.is_empty(): image.save_png(out.path_join(state+"-"+str(i)+".png"))
            require(image.get_pixel(22,22).r>.95 and image.get_pixel(22,22).g>.95,"rounded clip corner "+state)
            require(image.get_pixel(50,140).r>.95 and image.get_pixel(50,140).g>.95,"outside panel clipped "+state)
            require(image.get_pixel(280,40).g>.95 and image.get_pixel(280,40).r<.05,"fixed geometry "+state)
            var inside=image.get_pixel(50,50)
            require(inside.b>.95 if state=="before" else inside.r>.95,"scroll exposes expected content "+state)
        if state=="before":
            for view in views:
                for pressed in [true,false]:
                    var e=InputEventMouseButton.new()
                    e.position=Vector2(212,105)
                    e.global_position=e.position
                    e.button_index=MOUSE_BUTTON_LEFT
                    e.button_mask=MOUSE_BUTTON_MASK_LEFT if pressed else 0
                    e.pressed=pressed
                    view.dispatch_input_event(e)
            await settle()
    stats=views[1].get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
    require(stats.geometry_generation==geometry,"scroll preserves geometry generation")
    require(stats.geometry_uploaded_bytes==uploads,"scroll uploads no geometry")
    for view in views: view.set_element_attribute("content","style","background:purple")
    await settle()
    stats=views[1].get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
    require(stats.geometry_generation!=geometry,"paint change updates geometry")
    require(stats.color_patch_updates > 0,"color edit patches the existing GPU drawing")
    for view in views:
        var pixel = view.get_texture().get_image().get_pixel(50,50)
        require(abs(pixel.r-.502)<.01 and pixel.g<.01 and abs(pixel.b-.502)<.01,
            "changed paint reaches cached geometry")
    var fractional = HTMLDocument.new()
    fractional.html = "<style>body{margin:0;background:white}#clip{position:absolute;left:330.5px;top:20.5px;width:30px;height:60px;overflow:hidden}#inside{position:relative;left:-5px;top:-5px;width:50px;height:100px;background:blue}</style><div id='clip'><div id='inside'></div></div>"
    for view in views: view.document = fractional
    await settle()
    for i in views.size():
        var picture = views[i].get_texture().get_image()
        require(picture.get_pixel(330,40).r<.01,"fractional left edge is inclusive " + str(i))
        require(picture.get_pixel(345,20).r<.01,"fractional top edge is inclusive " + str(i))
        require(picture.get_pixel(360,40).r>.99,"fractional right edge is exclusive " + str(i))
        require(picture.get_pixel(345,80).r>.99,"fractional bottom edge is exclusive " + str(i))
        var output = views[i].create_output(Vector2i(800,600),false)
        await settle()
        picture = output.texture.get_image()
        require(picture.get_pixel(660,80).r>.99 and picture.get_pixel(661,80).r<.01,"scaled fractional clip " + str(i))
        output.release()
    for view in views: view.queue_free()
    await process_frame
    print("GPU_GEOMETRY_SMOKE_OK" if not failed else "GPU_GEOMETRY_SMOKE_FAILED")
    quit(1 if failed else 0)

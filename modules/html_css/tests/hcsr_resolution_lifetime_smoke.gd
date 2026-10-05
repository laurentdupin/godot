extends SceneTree
var failed=false
func _initialize():
    run.call_deferred()
    create_timer(60).timeout.connect(func(): quit(2))
func settle():
    for i in 40:
        await process_frame
        await RenderingServer.frame_post_draw
func require(value,label):
    if not value:
        failed=true
        push_error("RESOLUTION_LIFETIME_FAILED "+label)
func run():
    Engine.max_fps=0
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    var view=HTMLView.new()
    view.size=Vector2(240,180)
    view.viewport_size_mode=HTMLView.VIEWPORT_SIZE_FIXED
    view.logical_size=Vector2i(240,180)
    view.backend_preference=HTMLView.BACKEND_GPU_AUTO
    var image=Image.create(64,64,false,Image.FORMAT_RGBA8)
    image.fill(Color.RED)
    var src="data:image/png;base64,"+Marshalls.raw_to_base64(image.save_png_to_buffer())
    var doc=HTMLDocument.new()
    doc.html="<style>html,body{margin:0}img{position:absolute;width:64px;height:64px}#b{left:80px;width:16px;height:16px}#label{position:absolute;top:80px;font:16px Arial}</style><img id='a' src='%s'><img id='b' src='%s'><span id='label'>A</span>"%[src,src]
    view.document=doc
    root.add_child(view)
    var output=view.create_output(Vector2i(240,180),false)
    await settle()
    for cycle in 12:
        var large=cycle%2==0
        view.set_element_attribute("a","style","width:128px;height:128px" if large else "width:16px;height:16px")
        view.set_element_attribute("label","style","font-size:72px" if large else "font-size:16px")
        await settle()
        var stats=view.get_frame_scheduler_diagnostics().get("frame_synchronization",{}).get("image_atlas",{})
        require(stats.get("glyph_entries",0)==1,"glyph variants "+str(stats))
        require(stats.get("image_entries",0)==(2 if large else 1),"simultaneous image sizes "+str(stats))
        var pixels=output.texture.get_image()
        require(pixels.get_pixel(8,8).r>.9 and pixels.get_pixel(88,8).r>.9,"both images remain red")
    var final=view.get_frame_scheduler_diagnostics().get("frame_synchronization",{}).get("image_atlas",{})
    require(final.get("retired_resolution_variants",0)>10,"historical variants actually retired")
    print("RESOLUTION_LIFETIME_SMOKE ","FAIL" if failed else "OK"," cycles=12 simultaneous_sizes=true glyph_pixels=true")
    quit(1 if failed else 0)

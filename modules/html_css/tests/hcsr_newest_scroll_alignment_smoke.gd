extends SceneTree
var failed:=false
func _initialize():
    run.call_deferred()
    create_timer(20).timeout.connect(func():quit(2))
func settle():
    for i in 10:
        await process_frame
        await RenderingServer.frame_post_draw
func require(value:bool,label:String):
    if not value:
        failed=true
        push_error("SCROLL_ALIGNMENT_FAILED "+label)
func run():
    var view:=HTMLView.new()
    view.size=Vector2(320,180)
    view.logical_size=Vector2i(320,180)
    view.viewport_size_mode=HTMLView.VIEWPORT_SIZE_FIXED
    view.backend_preference=HTMLView.BACKEND_GPU_AUTO if OS.get_cmdline_user_args().has("--gpu") else HTMLView.BACKEND_CPU
    var doc:=HTMLDocument.new()
    doc.html="<style>html,body{margin:0;overflow:hidden;background:black}#box{position:absolute;left:10px;top:10px;width:140px;height:100px;overflow-y:scroll}#top{height:120px;background:blue}#target{display:block;width:100px;height:20px;margin:0;padding:0;border:0;background:red}#tail{height:200px;background:blue}</style><div id='box'><div id='top'></div><button id='target' data-godot-action='target'></button><div id='tail'></div></div>"
    view.document=doc
    root.add_child(view)
    await settle()
    var sync:Dictionary=view.get_frame_scheduler_diagnostics().get("frame_synchronization",{})
    require(not sync.get("terminal",false),"nonterminal")
    if OS.get_cmdline_user_args().has("--gpu"):
        require(sync.get("renderer", "cpu")!="cpu" and sync.get("image_atlas",{}).get("gpu_geometry",false),"GPU path selected")
    var actions:Array[StringName]=[]
    view.action_requested.connect(func(action:StringName,_payload:Dictionary):actions.append(action))
    for alignment in [&"start",&"center",&"end",&"nearest"]:
        require(view.scroll_element_into_view(&"top",&"start")==OK,"reset")
        await settle()
        require(view.scroll_element_into_view(&"target",alignment)==OK,str(alignment)+" accepted")
        await settle()
        var y:int=20 if alignment==&"start" else 60 if alignment==&"center" else 100
        var image:Image=view.get_texture().get_image()
        require(image.get_pixel(20,y).r>.9 and image.get_pixel(20,y).b<.1,str(alignment)+" placement")
        for pressed in [true,false]:
            var event:=InputEventMouseButton.new()
            event.position=Vector2(20,y)
            event.global_position=event.position
            event.button_index=MOUSE_BUTTON_LEFT
            event.button_mask=MOUSE_BUTTON_MASK_LEFT if pressed else 0
            event.pressed=pressed
            view.dispatch_input_event(event)
        await settle()
        require(actions.size()==1 and actions[0]==&"target",str(alignment)+" hit target")
        actions.clear()
    require(view.scroll_element_into_view(&"target",&"invalid")==ERR_INVALID_PARAMETER,"invalid alignment rejected")
    print("SCROLL_ALIGNMENT_", "FAILED" if failed else "OK")
    quit(1 if failed else 0)

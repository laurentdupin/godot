# Run with a GPU project using d3d12, vulkan, or metal. Samples each mip on a 3D quad.
# On Metal this also covers argument-buffer residency of shared texture views.
extends SceneTree
var view: HTMLView
var output: HTMLViewOutput
var space: SubViewport
var material: ShaderMaterial
func _initialize():
    run.call_deferred()
func run():
    Engine.max_fps = 60
    view = HTMLView.new()
    view.size = Vector2(128,128)
    view.logical_size = Vector2i(128,128)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    var doc := HTMLDocument.new()
    doc.html = "<style>body{margin:0}#tile{width:128px;height:128px;background:red}</style><div id='tile'></div>"
    view.document = doc
    root.add_child(view)
    output = view.create_output(Vector2i(128,128),true)
    space = SubViewport.new()
    space.size = Vector2i(128,128)
    space.own_world_3d = true
    space.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(space)
    var camera := Camera3D.new()
    camera.position.z = 2
    space.add_child(camera)
    var quad := MeshInstance3D.new()
    var mesh := QuadMesh.new()
    mesh.size = Vector2(2,2)
    quad.mesh = mesh
    material = ShaderMaterial.new()
    var shader := Shader.new()
    shader.code = "shader_type spatial;render_mode unshaded;uniform sampler2D menu_texture:filter_linear_mipmap;uniform float mip_level=3.0;void fragment(){ALBEDO=textureLod(menu_texture,UV,mip_level).rgb;}"
    material.shader = shader
    material.set_shader_parameter("menu_texture",output.texture)
    quad.material_override = material
    space.add_child(quad)
    for phase in 2:
        if phase == 1: view.set_element_attribute("tile","style","background:lime")
        for i in 20:
            await process_frame
            await RenderingServer.frame_post_draw
        for level in range(8):
            material.set_shader_parameter("mip_level", float(level))
            for i in 3:
                await process_frame
                await RenderingServer.frame_post_draw
            var img := space.get_texture().get_image()
            var c := img.get_pixel(64,64)
            print("MENU_MIP_COLOR ",phase," level=",level," ",c)
            if (phase == 0 and (c.r < .8 or c.g > .1)) or (phase == 1 and (c.g < .8 or c.r > .1)) or c.b > .1:
                push_error("3D menu mip level %s is incorrect" % level)
                quit(1)
                return
    print("MENU_3D_MIPMAP_OK")
    output.release()
    quit()

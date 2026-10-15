extends SceneTree

class LegacyTriangle:
    extends Node2D
    func _draw() -> void:
        draw_polygon(PackedVector2Array([Vector2(0,0),Vector2(100,0),Vector2(0,100)]),
            PackedColorArray([Color("ffffff29"),Color("202020f7"),Color("202020f7")]))

var failed := false
var traces: Array = []

func _initialize() -> void:
    _run.call_deferred()
    create_timer(35).timeout.connect(func(): quit(2))

func _check(value: bool, message: String) -> void:
    if not value:
        failed = true
        push_error(message)

func _rounded_distance(point: Vector2, rect: Rect2, radius: float) -> float:
    var q := (point-rect.get_center()).abs()-rect.size*0.5+Vector2.ONE*radius
    return q.max(Vector2.ZERO).length()+minf(maxf(q.x,q.y),0.0)-radius

func _run() -> void:
    RenderingServer.set_default_clear_color(Color("181818"))
    var viewport := SubViewport.new()
    viewport.size = Vector2i(680,240)
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    var triangle := LegacyTriangle.new()
    triangle.position = Vector2(550,10)
    viewport.add_child(triangle)
    var panel := Panel.new()
    panel.position = Vector2(20,40)
    panel.size = Vector2(240,52)
    panel.oversampling_with_scale = CanvasItem.OVERSAMPLING_WITH_SCALE_ENABLED
    var style := StyleBoxFlat.new()
    style.bg_color = Color("202020f7")
    style.border_color = Color("ffffff29")
    style.set_border_width_all(1)
    style.set_corner_radius_all(10)
    style.corner_detail = 20
    panel.add_theme_stylebox_override("panel",style)
    viewport.add_child(panel)
    viewport.move_child(triangle,-1) # Verify the opt-in cannot leak to a following polygon.
    for alpha in [1.0,0.5]:
        for factor in [0.25,0.6,1.0,1.6,2.0]:
            panel.scale = Vector2.ONE*factor
            panel.modulate.a = alpha
            for frame in 5:
                await process_frame
                await RenderingServer.frame_post_draw
            var image := viewport.get_texture().get_image()
            var peak := 0.0
            var maximum_error := 0.0
            var outer := Rect2(panel.position,panel.size*factor)
            var inner := outer.grow(-factor)
            var right: float = panel.position.x+panel.size.x*factor
            for y in range(38,ceili(40+10*factor+3)):
                for x in range(floori(right-10*factor-3),ceili(right+3)):
                    var pixel := image.get_pixel(x,y)
                    peak = maxf(peak,pixel.r*255)
                    var center := Vector2(x+0.5,y+0.5)
                    var outer_coverage := clampf(0.5-_rounded_distance(center,outer,10*factor),0.0,1.0)
                    var fill_coverage := clampf(0.5-_rounded_distance(center,inner,9*factor),0.0,1.0)
                    var border_coverage := maxf(0.0,outer_coverage-fill_coverage)
                    var border_alpha: float = style.border_color.a*alpha*border_coverage
                    var fill_alpha: float = style.bg_color.a*alpha*fill_coverage
                    var expected: float = style.border_color.r*border_alpha+style.bg_color.r*fill_alpha+(24.0/255.0)*(1-border_alpha-fill_alpha)
                    maximum_error = maxf(maximum_error,absf(pixel.r-expected)*255)
            # A coverage mixture cannot exceed the full border over the fill.
            # The old straight-RGBA interpolation peaks at 92 instead of <=68.
            var upper: float = 255*style.border_color.a*alpha+32*(1-style.border_color.a*alpha)
            _check(peak<=upper+1.1,"Rounded border overshot its physical color bound: %.1f > %.1f, scale=%s alpha=%s" % [peak,upper,factor,alpha])
            _check(maximum_error<=2.1,"Rounded border differs from analytic coverage: %s at scale %s alpha %s" % [maximum_error,factor,alpha])
            var legacy := image.get_pixel(600,11).r*255
            _check(legacy>80 and legacy<105,"Ordinary polygon COLOR interpolation changed: %s" % legacy)
            traces.append({"scale":factor,"modulate_alpha":alpha,"corner_peak":peak,"maximum_color_error":maximum_error,
                "draw_calls":viewport.get_render_info(Viewport.RENDER_INFO_TYPE_CANVAS,Viewport.RENDER_INFO_DRAW_CALLS_IN_FRAME),
                "primitives":viewport.get_render_info(Viewport.RENDER_INFO_TYPE_CANVAS,Viewport.RENDER_INFO_PRIMITIVES_IN_FRAME)})
            if factor==1.0 and alpha==1.0 and OS.has_environment("STYLEBOX_ALPHA_CAPTURE"):
                image.save_png(OS.get_environment("STYLEBOX_ALPHA_CAPTURE"))
    print("STYLEBOX_ALPHA_", "FAILED" if failed else "OK", " samples=10 legacy_polygon_unchanged=true trace=",JSON.stringify(traces))
    quit(1 if failed else 0)



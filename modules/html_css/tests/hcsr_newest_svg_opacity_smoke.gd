extends SceneTree
# A filtered group's opacity applies after filtering. Verify both the halo and
# its attenuation; merely dropping the filter must not pass this regression.
func _initialize():
    var template = "<svg xmlns='http://www.w3.org/2000/svg' width='80' height='80'><defs><filter id='b' x='-50%' y='-50%' width='200%' height='200%'><feGaussianBlur stdDeviation='3'/></filter></defs><g opacity='OPACITY' filter='url(#b)'><rect x='20' y='20' width='40' height='40' fill='#40dff0'/></g></svg>"
    var opaque = Image.new()
    var faded = Image.new()
    if opaque.load_svg_from_string(template.replace("OPACITY", "1")) != OK or faded.load_svg_from_string(template.replace("OPACITY", ".3")) != OK:
        push_error("SVG decoding failed")
        quit(1)
        return
    for point in [Vector2i(40,40), Vector2i(18,40), Vector2i(40,18)]:
        var expected = opaque.get_pixelv(point).a * .3
        var actual = faded.get_pixelv(point).a
        if expected < .005 or abs(expected-actual) > .012:
            push_error("SVG opacity/blur mismatch at %s: expected %s, got %s" % [point,expected,actual])
            quit(1)
            return
    # Nested opacity must multiply once, with no amplification of the blurred source.
    var nested = Image.new()
    var nested_svg = (template.replace("OPACITY", ".3")).replace("<g opacity=", "<g opacity='.5'><g opacity=").replace("</g></svg>", "</g></g></svg>")
    if nested.load_svg_from_string(nested_svg) != OK:
        quit(1)
        return
    for point in [Vector2i(40,40), Vector2i(18,40)]:
        if abs(nested.get_pixelv(point).a-opaque.get_pixelv(point).a*.15) > .012:
            push_error("Nested SVG opacity mismatch")
            quit(1)
            return
    print("SVG_OPACITY_OK filtered_group=true halo=true nested_opacity=true")
    quit()

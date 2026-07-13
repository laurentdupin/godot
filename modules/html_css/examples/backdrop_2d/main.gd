extends Node

class AnimatedBackdrop:
	extends Control

	var elapsed := 0.0

	func _ready() -> void:
		mouse_filter = Control.MOUSE_FILTER_IGNORE
		set_process(true)

	func _process(delta: float) -> void:
		elapsed += delta
		queue_redraw()

	func _draw() -> void:
		var viewport_size := size
		draw_rect(Rect2(Vector2.ZERO, viewport_size), Color("101a38"))

		var band_height := viewport_size.y / 6.0
		var band_colors := [
			Color("293f80"), Color("1f5b78"), Color("32694d"),
			Color("75612c"), Color("793e59"), Color("4a3476")
		]
		for index in range(band_colors.size()):
			draw_rect(Rect2(0, index * band_height, viewport_size.x, band_height + 1.0), band_colors[index])

		var grid_color := Color(1.0, 1.0, 1.0, 0.12)
		for x in range(0, int(viewport_size.x) + 80, 80):
			draw_line(Vector2(x, 0), Vector2(x, viewport_size.y), grid_color, 1.0)
		for y in range(0, int(viewport_size.y) + 80, 80):
			draw_line(Vector2(0, y), Vector2(viewport_size.x, y), grid_color, 1.0)

		var moving_centers := [
			Vector2(viewport_size.x * 0.18 + sin(elapsed * 0.72) * 90.0, viewport_size.y * 0.28 + cos(elapsed * 0.51) * 65.0),
			Vector2(viewport_size.x * 0.52 + cos(elapsed * 0.43) * 130.0, viewport_size.y * 0.50 + sin(elapsed * 0.66) * 80.0),
			Vector2(viewport_size.x * 0.82 + sin(elapsed * 0.58) * 85.0, viewport_size.y * 0.70 + cos(elapsed * 0.39) * 55.0)
		]
		var moving_colors := [Color("ff4f70"), Color("35d9d0"), Color("ffc857")]
		var moving_radii := [118.0, 145.0, 105.0]
		for index in range(moving_centers.size()):
			draw_circle(moving_centers[index], moving_radii[index], moving_colors[index])
			draw_circle(moving_centers[index] + Vector2(-28, -32), moving_radii[index] * 0.45, Color(1, 1, 1, 0.22))

		var ribbon_offset := fmod(elapsed * 75.0, viewport_size.x + 420.0) - 420.0
		var ribbon := PackedVector2Array([
			Vector2(ribbon_offset, viewport_size.y * 0.80),
			Vector2(ribbon_offset + 390.0, viewport_size.y * 0.10),
			Vector2(ribbon_offset + 500.0, viewport_size.y * 0.10),
			Vector2(ribbon_offset + 110.0, viewport_size.y * 0.80)
		])
		draw_colored_polygon(ribbon, Color("8b5cf6"))

		var font := ThemeDB.fallback_font
		draw_string(font, Vector2(38, viewport_size.y - 32), "ANIMATED GODOT CANVAS CONTENT", HORIZONTAL_ALIGNMENT_LEFT, -1, 28, Color(1, 1, 1, 0.65))


const GALLERY_HTML := """<!DOCTYPE html>
<html>
<head>
<style>
html, body {
    margin: 0;
    width: 100vw;
    height: 100vh;
    background: transparent;
    color: #ffffff;
    font-family: Arial;
}
.title {
    position: absolute;
    left: 38px;
    top: 22px;
    font-size: 30px;
    font-weight: 700;
}
.subtitle {
    position: absolute;
    left: 40px;
    top: 61px;
    color: #dbeafe;
    font-size: 14px;
}
.card {
    position: absolute;
    width: 260px;
    height: 214px;
    padding: 18px;
    border: 1px solid rgba(255, 255, 255, 0.42);
    border-radius: 22px;
    background: rgba(12, 20, 42, 0.18);
    box-shadow: 0 18px 45px rgba(0, 0, 0, 0.24);
}
.card h2 {
    margin: 0 0 8px 0;
    font-size: 21px;
    line-height: 26px;
}
.card p {
    margin: 0;
    color: #eef2ff;
    font-size: 13px;
    line-height: 19px;
}
.value {
    position: absolute;
    left: 18px;
    bottom: 16px;
    padding: 6px 10px;
    border-radius: 9px;
    background: rgba(0, 0, 0, 0.34);
    color: #ffffff;
    font-family: Consolas;
    font-size: 12px;
}
.blur       { left: 38px;  top: 102px; backdrop-filter: blur(16px); }
.contrast   { left: 348px; top: 102px; backdrop-filter: contrast(180%); }
.sepia      { left: 658px; top: 102px; backdrop-filter: sepia(100%); }
.saturate   { left: 968px; top: 102px; backdrop-filter: saturate(240%); }
.brightness { left: 38px;  top: 374px; backdrop-filter: brightness(155%); }
.grayscale  { left: 348px; top: 374px; backdrop-filter: grayscale(100%); }
.invert     { left: 658px; top: 374px; backdrop-filter: invert(100%); }
.combined   { left: 968px; top: 374px; backdrop-filter: blur(9px) contrast(135%) saturate(180%) sepia(30%); }
</style>
</head>
<body>
    <div class="title">HCSR 2D Backdrop Gallery</div>
    <div class="subtitle">Each HTML panel filters the animated Godot canvas underneath it.</div>
    <section class="card blur"><h2>Gaussian blur</h2><p>Moving shapes and grid lines diffuse while the HTML text remains sharp.</p><div class="value">blur(16px)</div></section>
    <section class="card contrast"><h2>Contrast</h2><p>Dark and light scene values are pushed farther apart.</p><div class="value">contrast(180%)</div></section>
    <section class="card sepia"><h2>Sepia</h2><p>The sampled canvas is transformed through a warm photographic matrix.</p><div class="value">sepia(100%)</div></section>
    <section class="card saturate"><h2>Saturation</h2><p>Scene colors become deliberately intense without changing the HTML foreground.</p><div class="value">saturate(240%)</div></section>
    <section class="card brightness"><h2>Brightness</h2><p>The background is lifted while preserving its moving geometry.</p><div class="value">brightness(155%)</div></section>
    <section class="card grayscale"><h2>Grayscale</h2><p>Host canvas color is removed only inside this rounded panel.</p><div class="value">grayscale(100%)</div></section>
    <section class="card invert"><h2>Invert</h2><p>Every sampled RGB channel is inverted for a high-contrast negative.</p><div class="value">invert(100%)</div></section>
    <section class="card combined"><h2>Combined stack</h2><p>Operations are evaluated in CSS order on the same captured backdrop.</p><div class="value">blur + contrast + saturate + sepia</div></section>
</body>
</html>"""


func _ready() -> void:
	var backdrop := AnimatedBackdrop.new()
	backdrop.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(backdrop)

	var document := HTMLDocument.new()
	document.html = GALLERY_HTML
	document.resource_root = "res://"
	document.background_color = Color(0, 0, 0, 0)

	var html_view := HTMLView.new()
	html_view.name = "BackdropHTML"
	html_view.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	html_view.backend_preference = HTMLView.BACKEND_CPU
	html_view.backdrop_filter_enabled = true
	html_view.document = document
	add_child(html_view)

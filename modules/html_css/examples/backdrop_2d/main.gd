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


func _ready() -> void:
	var backdrop := AnimatedBackdrop.new()
	backdrop.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(backdrop)

	var document := HTMLDocument.new()
	document.html_file = "res://gallery.html"
	document.resource_root = "res://"
	document.background_color = Color(0, 0, 0, 0)

	var html_view := HTMLView.new()
	html_view.name = "BackdropHTML"
	html_view.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	html_view.backend_preference = HTMLView.BACKEND_GPU_AUTO
	html_view.backdrop_filter_enabled = true
	html_view.document = document
	add_child(html_view)

extends Node

const TRACKER_SIZE := 54.0

var camera: Camera3D
var cube: MeshInstance3D
var html_view: HTMLView
var elapsed := 0.0
var projected_cube_center := Vector2.ZERO
var validation_enabled := false
var validated_frame_count := 0
var validation_warmup_frames := 0


func _ready() -> void:
	_build_world()
	_build_html_overlay()
	validation_enabled = "--validate" in OS.get_cmdline_user_args()
	if validation_enabled:
		RenderingServer.frame_post_draw.connect(_validate_composed_frame)


func _process(delta: float) -> void:
	elapsed += delta
	cube.position = Vector3(
		sin(elapsed * 0.9) * 2.2,
		cos(elapsed * 1.3) * 1.1,
		sin(elapsed * 0.55) * 0.6
	)
	var projected := camera.unproject_position(cube.global_position)
	projected_cube_center = projected
	if html_view.get_generation() == 0:
		return
	var tracker_style := (
		"left:%.4fpx;top:%.4fpx;width:%.1fpx;height:%.1fpx;"
		% [
			projected.x - TRACKER_SIZE * 0.5,
			projected.y - TRACKER_SIZE * 0.5,
			TRACKER_SIZE,
			TRACKER_SIZE,
		]
	)
	var error := html_view.set_element_style("cube-tracker", tracker_style)
	if error != OK:
		push_error("Could not update the synchronized HTML cube tracker: %s" % error_string(error))


func _validate_composed_frame() -> void:
	if not validation_enabled or html_view.get_generation() == 0:
		return
	validation_warmup_frames += 1
	var image := get_viewport().get_texture().get_image()
	var image_bounds := Rect2i(Vector2i.ZERO, image.get_size())
	var red_bounds := _find_color_bounds(
		image,
		image_bounds,
		func(color: Color) -> bool: return color.r > 0.75 and color.g < 0.55 and color.b < 0.5
	)
	var blue_bounds := _find_color_bounds(
		image,
		image_bounds,
		func(color: Color) -> bool: return color.b > 0.7 and color.g > 0.25 and color.r < 0.35
	)
	if red_bounds.size == Vector2i.ZERO or blue_bounds.size == Vector2i.ZERO:
		if validation_warmup_frames >= 120:
			image.save_png("res://validation-failure.png")
			push_error(
				"The composed frame did not contain both renderers after 120 frames; red=%s blue=%s."
				% [red_bounds, blue_bounds]
			)
			get_tree().quit(1)
		return
	var tracker_center := red_bounds.get_center()
	var cube_center := blue_bounds.get_center()
	var alignment_error := tracker_center.distance_to(cube_center)
	if alignment_error > 1.5:
		validated_frame_count = 0
		if validation_warmup_frames >= 120:
			push_error(
				"HTML/3D frame alignment diverged by %.3f pixels in one composed frame: tracker=%s cube=%s."
				% [alignment_error, tracker_center, cube_center]
			)
			get_tree().quit(1)
		return
	validated_frame_count += 1
	if validated_frame_count >= 30:
		validation_enabled = false
		print("SYNCHRONIZED_HTML_3D_ALIGNMENT_OK frames=%d max_error_px=1.5" % validated_frame_count)
		get_tree().quit(0)


func _find_color_bounds(image: Image, bounds: Rect2i, predicate: Callable) -> Rect2i:
	var color_min := Vector2i(1 << 30, 1 << 30)
	var color_max := Vector2i(-1, -1)
	for y in range(bounds.position.y, bounds.end.y):
		for x in range(bounds.position.x, bounds.end.x):
			var color := image.get_pixel(x, y)
			if predicate.call(color):
				color_min.x = mini(color_min.x, x)
				color_min.y = mini(color_min.y, y)
				color_max.x = maxi(color_max.x, x)
				color_max.y = maxi(color_max.y, y)
	if color_max.x < color_min.x:
		return Rect2i()
	return Rect2i(color_min, color_max - color_min + Vector2i.ONE)


func _build_world() -> void:
	var world := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.025, 0.035, 0.06)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.35, 0.4, 0.55)
	environment.ambient_light_energy = 0.7
	world.environment = environment
	add_child(world)

	camera = Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 7.5)
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 6.75
	camera.current = true
	add_child(camera)

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-35.0, -25.0, 0.0)
	light.light_energy = 1.6
	add_child(light)

	cube = MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(0.8, 0.8, 0.8)
	cube.mesh = box
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.15, 0.55, 1.0)
	material.metallic = 0.15
	material.roughness = 0.3
	cube.material_override = material
	add_child(cube)


func _build_html_overlay() -> void:
	html_view = HTMLView.new()
	html_view.position = Vector2.ZERO
	html_view.size = get_viewport().get_visible_rect().size
	html_view.mouse_filter = Control.MOUSE_FILTER_IGNORE
	html_view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_CONTROL_PHYSICAL_ADJUSTED
	html_view.backend_preference = (
		HTMLView.BACKEND_CPU if "--cpu" in OS.get_cmdline_user_args() else HTMLView.BACKEND_GPU_AUTO
	)
	html_view.html = """
		<!DOCTYPE html>
		<html>
		<head>
			<style>
				html, body {
					width: 100vw;
					height: 100vh;
					margin: 0;
					overflow: hidden;
					background: transparent;
				}
				#cube-tracker {
					position: absolute;
					box-sizing: border-box;
					border: 4px solid rgb(255, 80, 70);
					border-radius: 8px;
					background: rgba(255, 80, 70, 0.08);
				}
				#contract {
					position: absolute;
					left: 18px;
					top: 14px;
					padding: 8px 12px;
					color: white;
					background: rgba(0, 0, 0, 0.65);
					font: 18px sans-serif;
					border-radius: 6px;
				}
			</style>
		</head>
		<body>
			<div id="cube-tracker"></div>
			<div id="contract">Engine-frame synchronized HTML overlay</div>
		</body>
		</html>
	"""
	add_child(html_view)

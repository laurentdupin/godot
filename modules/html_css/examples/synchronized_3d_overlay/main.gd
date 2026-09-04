extends Node

const TRACKER_SIZE := 54.0

class LateFrameDriver extends Node:
	var example: Node
	func _process(delta: float) -> void:
		example._process(delta)

var camera: Camera3D
var cube: MeshInstance3D
var html_view: HTMLView
var elapsed := 0.0
var projected_cube_center := Vector2.ZERO
var validation_enabled := false
var validated_frame_count := 0
var validation_warmup_frames := 0
var validation_started := false
var last_validated_generation := 0
var last_validated_cube_center := Vector2.ZERO
var cumulative_projected_motion := 0.0
var projected_history: Array[Vector2] = []
var maximum_alignment_error := 0.0


func _ready() -> void:
	_build_world()
	_build_html_overlay()
	validation_enabled = "--validate" in OS.get_cmdline_user_args()
	if validation_enabled:
		OS.low_processor_usage_mode = "--low-processor" in OS.get_cmdline_user_args()
		# Exercise script updates after HTMLView's normal process order.
		set_process(false)
		var driver := LateFrameDriver.new()
		driver.example = self
		driver.process_priority = 1000
		add_child(driver)
		RenderingServer.frame_post_draw.connect(_validate_composed_frame)


func _process(delta: float) -> void:
	# Readback is deliberately slow; advance by a fixed amount so the test
	# measures frame correspondence, independently of validation overhead.
	if validation_enabled:
		delta = 1.0 / 60.0
	elapsed += delta
	cube.position = Vector3(
		sin(elapsed * 0.9) * 2.2,
		cos(elapsed * 1.3) * 1.1,
		sin(elapsed * 0.55) * 0.6
	)
	var projected := camera.unproject_position(cube.global_position)
	projected_cube_center = projected
	projected_history.push_front(projected)
	if projected_history.size() > 16:
		projected_history.pop_back()
	if html_view.get_generation() == 0:
		return
	var left := projected.x - TRACKER_SIZE * 0.5
	var top := projected.y - TRACKER_SIZE * 0.5
	var mutations: Array[Dictionary] = [
		{ "operation": "set_style", "id": "tracker-top", "value": "left:%.4fpx;top:%.4fpx;width:54px;height:4px;" % [left, top] },
		{ "operation": "set_style", "id": "tracker-right", "value": "left:%.4fpx;top:%.4fpx;width:4px;height:54px;" % [left + 50.0, top] },
		{ "operation": "set_style", "id": "tracker-bottom", "value": "left:%.4fpx;top:%.4fpx;width:54px;height:4px;" % [left, top + 50.0] },
		{ "operation": "set_style", "id": "tracker-left", "value": "left:%.4fpx;top:%.4fpx;width:4px;height:54px;" % [left, top] },
	]
	var error := html_view.apply_element_mutations(mutations)
	if error != OK:
		push_error("Could not update the synchronized HTML tracker atomically: %s" % error_string(error))


func _validate_composed_frame() -> void:
	if not validation_enabled:
		return
	if html_view.get_generation() == 0:
		return
	validation_warmup_frames += 1
	if validation_warmup_frames > 600:
		push_error("Alignment validation did not observe enough advancing HTML generations within 600 frames.")
		get_tree().quit(1)
		return
	var image := get_viewport().get_texture().get_image()
	var image_bounds := Rect2i(Vector2i.ZERO, image.get_size())
	if validation_started:
		# Search the moving objects, including recent positions for lag diagnosis.
		var search := Rect2(projected_cube_center - Vector2(100, 100), Vector2(200, 200))
		for position in projected_history:
			search = search.expand(position - Vector2(100, 100)).expand(position + Vector2(100, 100))
		image_bounds = image_bounds.intersection(Rect2i(search))
	var red_bounds := _find_color_bounds(
		image,
		image_bounds,
		func(color: Color) -> bool: return (color.r > 0.75 and color.g < 0.55 and color.b < 0.5) or (color.r > 0.04 and absf(color.r - color.g) < 0.01 and absf(color.g - color.b) < 0.01)
	)
	var blue_bounds := _find_color_bounds(
		image,
		image_bounds,
		func(color: Color) -> bool: return color.b > 0.7 and color.g > 0.25 and color.r < 0.35
	)
	if red_bounds.size == Vector2i.ZERO or blue_bounds.size == Vector2i.ZERO:
		if validation_started or validation_warmup_frames >= 120:
			image.save_png("res://validation-failure.png")
			push_error(
				"The composed frame did not contain both renderers after 120 frames; red=%s blue=%s."
				% [red_bounds, blue_bounds]
			)
			get_tree().quit(1)
		return
	var tracker_center := red_bounds.get_center()
	var cube_center := blue_bounds.get_center()
	# Readback can drain a newer queued frame in separate-thread mode. Identify
	# that frame from Godot's cube, independently of the HTML tracker.
	var composed_projection := projected_cube_center
	if not RenderingServer.is_on_render_thread():
		for position in projected_history:
			if cube_center.distance_to(position) < cube_center.distance_to(composed_projection):
				composed_projection = position
	var alignment_error := maxf(tracker_center.distance_to(cube_center), tracker_center.distance_to(composed_projection))
	maximum_alignment_error = maxf(maximum_alignment_error, alignment_error)
	if alignment_error > 1.5:
		validated_frame_count = 0
		if validation_started or validation_warmup_frames >= 120:
			image.save_png("res://validation-failure.png")
			var nearest_history_index := -1
			var nearest_history_error := INF
			for history_index in range(projected_history.size()):
				var history_error := tracker_center.distance_to(projected_history[history_index])
				if history_error < nearest_history_error:
					nearest_history_error = history_error
					nearest_history_index = history_index
			push_error(
				"HTML/3D frame alignment diverged by %.3f pixels in one composed frame: tracker=%s cube=%s projected=%s generation=%d nearest_history=%d/%.3fpx."
				% [alignment_error, tracker_center, cube_center, projected_cube_center, html_view.get_generation(), nearest_history_index, nearest_history_error]
			)
			get_tree().quit(1)
		return
	if not validation_started:
		validation_started = true
		last_validated_generation = html_view.get_generation()
		last_validated_cube_center = cube_center
		# CPU readback measures alignment, not interactive frame performance.
		html_view.frame_budget_milliseconds = 0.0
		return
	if html_view.get_generation() <= last_validated_generation:
		return
	cumulative_projected_motion += last_validated_cube_center.distance_to(cube_center)
	last_validated_cube_center = cube_center
	last_validated_generation = html_view.get_generation()
	validated_frame_count += 1
	if validated_frame_count >= 120 and cumulative_projected_motion >= 150.0:
		validation_enabled = false
		image.save_png("user://synchronized-overlay-validation.png")
		print("SYNCHRONIZED_HTML_3D_ALIGNMENT_OK generations=%d motion_px=%.3f max_error_px=%.3f" % [validated_frame_count, cumulative_projected_motion, maximum_alignment_error])
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
		<html>
			<head><style>
				html { display:block; position:relative; width:1152px; height:648px; }
				.edge { display:block; position:absolute; left:0px; top:0px; background:#ff5046; }
			</style></head>
			<div id="tracker-top" class="edge"></div>
			<div id="tracker-right" class="edge"></div>
			<div id="tracker-bottom" class="edge"></div>
			<div id="tracker-left" class="edge"></div>
		</html>
	"""
	add_child(html_view)

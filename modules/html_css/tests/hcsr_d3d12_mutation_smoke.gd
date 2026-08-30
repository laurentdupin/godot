extends SceneTree

const WIDTH := 420
const HEIGHT := 180
const STYLE := "html,body{margin:0;width:100%;height:100%;background:#111827;color:#f9fafb;font-size:18px}.row{display:flex;gap:12px;padding:12px}.label{width:112px}.ui-select-options{display:none;position:absolute;left:126px;top:42px;width:180px;background:#1f2937}.ui-select-options.open{display:block}"
const BODY_PREFIX := "<div class='row'><span class='label'>Alpha glyphs</span><span class='label'>Bravo glyphs</span><span class='label'>Charlie glyphs</span></div>"
const OPTIONS := "<div id='options' class='%s'><div>First option</div><div>Second option</div><div>Third option</div></div>"
const BODY_SUFFIX := "<div class='row'><span class='label'>Delta glyphs</span><span class='label'>Echo glyphs</span><span class='label'>Foxtrot glyphs</span></div>"

var backend_preference := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"

func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	elif OS.get_cmdline_user_args().has("--metal"):
		backend_preference = HTMLView.BACKEND_METAL
		backend_name = "Metal"
	root.size = Vector2i(WIDTH * 2, HEIGHT)
	var mutated := _make_view("ui-select-options")
	var fresh := _make_view("ui-select-options open")
	fresh.position = Vector2(WIDTH, 0)
	root.add_child(mutated)
	root.add_child(fresh)
	if not await _wait_for_generation(mutated, 0, 120) or not await _wait_for_generation(fresh, 0, 120):
		_fail("%s mutation smoke did not activate its initial comparison surfaces." % backend_name)
		return
	var mutation_base_generation: int = mutated.get_generation()
	if mutated.set_element_attribute(&"options", &"class", "ui-select-options open") != OK:
		_fail("%s mutation was rejected." % backend_name)
		return
	if not await _wait_for_generation(mutated, mutation_base_generation, 120):
		_fail("%s mutation successor did not activate." % backend_name)
		return

	var viewport_image := root.get_texture().get_image()
	if viewport_image == null or viewport_image.get_width() < WIDTH * 2 or viewport_image.get_height() < HEIGHT:
		_fail("%s mutation smoke could not read back the composed viewport (size=%s)." % [backend_name, viewport_image.get_size() if viewport_image != null else Vector2i.ZERO])
		return
	var changed := 0
	var changed_min := Vector2i(WIDTH, HEIGHT)
	var changed_max := Vector2i.ZERO
	for y in range(HEIGHT):
		for x in range(WIDTH):
			if viewport_image.get_pixel(x, y) != viewport_image.get_pixel(x + WIDTH, y):
				changed += 1
				changed_min.x = mini(changed_min.x, x)
				changed_min.y = mini(changed_min.y, y)
				changed_max.x = maxi(changed_max.x, x)
				changed_max.y = maxi(changed_max.y, y)
	if changed != 0:
		var mutated_image := viewport_image.get_region(Rect2i(0, 0, WIDTH, HEIGHT))
		var fresh_image := viewport_image.get_region(Rect2i(WIDTH, 0, WIDTH, HEIGHT))
		var mutated_path := "user://hcsr_mutated_failure.png"
		var fresh_path := "user://hcsr_fresh_failure.png"
		mutated_image.save_png(mutated_path)
		fresh_image.save_png(fresh_path)
		_fail("%s class mutation lost or misplaced retained content; %d pixels differ in (%s)-(%s). Artifacts: %s, %s." % [backend_name, changed, changed_min, changed_max, ProjectSettings.globalize_path(mutated_path), ProjectSettings.globalize_path(fresh_path)])
		return

	print("HCSR Godot %s retained mutation smoke passed." % backend_name)
	quit()

func _wait_for_generation(view: HTMLView, prior_generation: int, maximum_frames: int) -> bool:
	for _frame in range(maximum_frames):
		await process_frame
		RenderingServer.force_draw(true)
		if view.get_generation() > prior_generation:
			await RenderingServer.frame_post_draw
			return true
	return false

func _make_view(options_class: String) -> HTMLView:
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><head><style>%s</style></head><body>%s%s%s</body></html>" % [STYLE, BODY_PREFIX, OPTIONS % options_class, BODY_SUFFIX]
	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.size = Vector2(WIDTH, HEIGHT)
	view.document = document
	return view

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

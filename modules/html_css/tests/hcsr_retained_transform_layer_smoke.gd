extends SceneTree

const WIDTH := 320
const HEIGHT := 180
var tested_view: HTMLView

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var use_vulkan := OS.get_cmdline_user_args().has("--vulkan")
	var backend := HTMLView.BACKEND_VULKAN if use_vulkan else HTMLView.BACKEND_D3D12
	var backend_name := "Vulkan" if use_vulkan else "D3D12"
	root.size = Vector2i(WIDTH, HEIGHT)
	Input.warp_mouse(Vector2(300, 170))

	var view := HTMLView.new()
	tested_view = view
	view.backend_preference = backend
	view.size = Vector2(WIDTH, HEIGHT)
	view.html = """<!DOCTYPE html><html><head><style>
html,body{margin:0;width:100vw;height:100vh;background:#102030;overflow:hidden}
.card{position:absolute;left:50px;top:35px;width:200px;height:110px;background:#304860;border-radius:14px;transform-origin:center;transition:transform 200ms linear}
.card.hot{transform:scale(1.04)}
</style></head><body><article id="card" class="card"></article></body></html>"""
	root.add_child(view)

	for _frame in range(12):
		await process_frame
		RenderingServer.force_draw(false)
	if view.get_generation() <= 0:
		_fail("%s retained-transform smoke did not publish its initial frame." % backend_name)
		return

	if view.set_element_attribute("card", "class", "card hot") != OK:
		_fail("%s retained-transform smoke could not activate its transition." % backend_name)
		return

	var last_generation := view.get_generation()
	var published_generations := 0
	var maximum_rasters := 0.0
	var maximum_hits := 0.0
	var maximum_layout_seconds := 0.0
	var maximum_record_seconds := 0.0
	for _frame in range(48):
		await process_frame
		RenderingServer.force_draw(false)
		var rasters: float = Performance.get_custom_monitor("HCSR/Retained Transform Layer Rasters")
		var hits: float = Performance.get_custom_monitor("HCSR/Retained Transform Layer Hits")
		maximum_rasters = max(maximum_rasters, rasters)
		maximum_hits = max(maximum_hits, hits)
		maximum_record_seconds = max(maximum_record_seconds, Performance.get_custom_monitor("HCSR/Record And Submit Time"))
		if view.get_generation() == last_generation:
			continue
		last_generation = view.get_generation()
		published_generations += 1
		if published_generations >= 2:
			maximum_layout_seconds = max(maximum_layout_seconds, Performance.get_custom_monitor("HCSR/Layout Time"))

	if maximum_hits < 1.0:
		_fail("%s transform-only hover never reused its retained layer." % backend_name)
		return
	if published_generations < 10:
		_fail("%s transform-only hover published only %d transition generations." % [backend_name, published_generations])
		return
	if maximum_layout_seconds != 0.0:
		_fail("%s transform-only hover performed layout work: %.6f ms." % [backend_name, maximum_layout_seconds * 1000.0])
		return

	print("HCSR Godot %s retained-transform layer smoke passed: generations=%d max_rasters=%d max_hits=%d max_record=%.3fms." % [backend_name, published_generations, int(maximum_rasters), int(maximum_hits), maximum_record_seconds * 1000.0])
	root.remove_child(view)
	view.free()
	tested_view = null
	for _frame in range(8):
		await process_frame
		RenderingServer.force_draw(false)
	quit(0)

func _fail(message: String) -> void:
	push_error(message)
	if tested_view != null:
		tested_view.free()
		tested_view = null
	quit(1)

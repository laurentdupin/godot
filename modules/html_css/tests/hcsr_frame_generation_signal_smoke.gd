extends SceneTree

const WIDTH := 240
const HEIGHT := 100
const STYLE := "body{margin:0;background:#111827}.target{position:absolute;left:20px;top:20px;width:120px;height:50px}.old{background:#2563eb}.new{background:#dc2626}"

var backend_preference := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"
var queued_generations: Array[int] = []
var activated_generations: Array[int] = []
var event_order: Array[String] = []
var rendered_count := 0

func _initialize() -> void:
	if DisplayServer.get_name() == "headless" or OS.get_cmdline_user_args().has("--cpu"):
		backend_preference = HTMLView.BACKEND_CPU
		backend_name = "CPU"
	elif OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"

	root.size = Vector2i(WIDTH, HEIGHT)
	var target := HTMLRenderTarget.new()
	target.frame_queued.connect(_on_frame_queued)
	target.frame_activated.connect(_on_frame_activated)
	target.rendered.connect(_on_rendered)
	root.add_child(target)
	target.size = Vector2i(WIDTH, HEIGHT)
	target.backend_preference = backend_preference

	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><head><style>%s</style></head><body><div id='target' class='target old'></div></body></html>" % STYLE
	target.document = document
	if not await _wait_for_activation(target, 0):
		return
	var display := TextureRect.new()
	display.size = Vector2(WIDTH, HEIGHT)
	display.texture = target.get_texture()
	root.add_child(display)

	var baseline_generation := activated_generations[-1]
	queued_generations.clear()
	activated_generations.clear()
	event_order.clear()
	rendered_count = 0

	if target.set_element_attribute(&"target", &"class", "target new") != OK:
		_fail("%s frame-generation smoke could not queue the DOM mutation." % backend_name)
		return
	if not queued_generations.is_empty() or not activated_generations.is_empty() or rendered_count != 0:
		_fail("%s DOM setter reported a frame before engine-frame rendering began." % backend_name)
		return
	if not await _wait_for_activation(target, baseline_generation):
		return

	if queued_generations.size() != 1 or activated_generations.size() != 1:
		_fail("%s mutation did not publish exactly one queued and active generation (queued=%s active=%s)." % [backend_name, queued_generations, activated_generations])
		return
	if queued_generations[0] != activated_generations[0] or queued_generations[0] <= baseline_generation:
		_fail("%s queued and active generation identities did not match monotonically." % backend_name)
		return
	if event_order != ["queued", "activated", "rendered"] or rendered_count != 1:
		_fail("%s frame events were not ordered by submission then activation: %s." % [backend_name, event_order])
		return
	if DisplayServer.get_name() != "headless":
		RenderingServer.force_draw(false)
		await RenderingServer.frame_post_draw
		var composed_image := root.get_texture().get_image()
		if composed_image == null:
			_fail("%s frame-generation smoke could not read the CanvasItem composition." % backend_name)
			return
		var new_pixel := composed_image.get_pixel(60, 40)
		if new_pixel.r <= new_pixel.b * 1.3:
			_fail("%s activated generation was not sampled by the CanvasItem (pixel=%s)." % [backend_name, new_pixel])
			return

	print("HCSR Godot %s queued/activated frame-generation smoke passed." % backend_name)
	quit()

func _wait_for_activation(target: HTMLRenderTarget, after_generation: int) -> bool:
	for _frame in range(20):
		await process_frame
		if not activated_generations.is_empty() and activated_generations[-1] > after_generation:
			if target.get_texture() == null:
				_fail("%s activated a generation without a sampled Texture2D." % backend_name)
				return false
			return true
	_fail("%s frame-generation smoke timed out waiting for activation after %d." % [backend_name, after_generation])
	return false

func _on_frame_queued(generation: int) -> void:
	queued_generations.append(generation)
	event_order.append("queued")

func _on_frame_activated(generation: int) -> void:
	activated_generations.append(generation)
	event_order.append("activated")

func _on_rendered() -> void:
	rendered_count += 1
	event_order.append("rendered")

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

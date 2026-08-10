extends SceneTree

const WIDTH := 320
const HEIGHT := 180
const INNER_TOP := 30
const INNER_HEIGHT := 120
const FINAL_HTML := """<div id='section-0' class='section section-0'>Zero</div><div id='section-1' class='section section-1'>One</div><button id='section-mid' class='section section-mid' data-godot-action='mid'>Middle</button><div id='section-3' class='section section-3'>Three</div><div id='section-4' class='section section-4'>Four</div><button id='section-final' class='section section-final' data-godot-action='final'>Final</button>"""
const INITIAL_HTML := """<div id='section-0' class='section section-0'>Zero</div><div id='section-1' class='section section-1'>One</div>"""
const STYLE := """
*{box-sizing:border-box}html,body{margin:0;width:320px;height:180px;overflow:hidden;background:#020617;color:white}
.outer{position:absolute;left:10px;top:10px;width:300px;height:160px;overflow-y:auto;background:#334155}
.outer-marker{height:20px;background:#112233}.inner{width:280px;height:120px;overflow-y:auto;background:#475569}
.outer-tail{height:160px;background:#1e293b}.section{display:block;width:260px;height:50px;margin:0;padding:0;border:0;color:white}
.section-0{background:#991b1b}.section-1{background:#1d4ed8}.section-mid{height:40px;background:#16a34a}
.section-3{background:#7e22ce}.section-4{background:#c2410c}.section-final{height:40px;background:#0891b2}
"""

var backend_preference := HTMLView.BACKEND_CPU
var backend_name := "CPU"
var actions: Array[StringName] = []

func _initialize() -> void:
	if OS.get_cmdline_user_args().has("--d3d12"):
		backend_preference = HTMLView.BACKEND_D3D12
		backend_name = "D3D12"
	elif OS.get_cmdline_user_args().has("--vulkan"):
		backend_preference = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	elif OS.get_cmdline_user_args().has("--metal"):
		backend_preference = HTMLView.BACKEND_METAL
		backend_name = "Metal"

	root.size = Vector2i(WIDTH * 2, HEIGHT)
	var mutated := _make_view(INITIAL_HTML)
	var fresh := _make_view(FINAL_HTML)
	fresh.position = Vector2(WIDTH, 0)
	mutated.action_requested.connect(func(action: StringName, _payload: Dictionary) -> void: actions.append(action))
	root.add_child(mutated)
	root.add_child(fresh)
	if not await _wait_for_initial_views(mutated, fresh):
		return
	if mutated.scroll_element_into_view(&"section-0", &"center") != ERR_INVALID_PARAMETER \
			or mutated.scroll_element_into_view(&"", &"start") != ERR_INVALID_PARAMETER:
		_fail("%s scroll-into-view smoke did not reject an unsupported alignment or empty id." % backend_name)
		return

	var mutated_generation := mutated.get_generation()
	var fresh_generation := fresh.get_generation()
	if mutated.set_element_inner_html(&"inner", FINAL_HTML) != OK \
			or mutated.scroll_element_into_view(&"section-mid", &"start") != OK \
			or fresh.scroll_element_into_view(&"section-mid", &"start") != OK:
		_fail("%s scroll-into-view smoke rejected the same-frame live subtree and middle-section request." % backend_name)
		return
	if not await _wait_for_view_generation(mutated, mutated_generation) \
			or not await _wait_for_view_generation(fresh, fresh_generation):
		return

	var mutated_mid := mutated.get_texture().get_image()
	var fresh_mid := fresh.get_texture().get_image()
	if not _images_equal(mutated_mid, fresh_mid) \
			or not _is_color(mutated_mid.get_pixel(50, INNER_TOP + 5), Color("16a34a")) \
			or not _is_color(mutated_mid.get_pixel(50, 15), Color("112233")):
		_fail("%s middle-section start alignment was not current-DOM, clean-equivalent, or nearest-container-only." % backend_name)
		return

	mutated_generation = mutated.get_generation()
	fresh_generation = fresh.get_generation()
	if mutated.scroll_element_into_view(&"section-final", &"start") != OK \
			or fresh.scroll_element_into_view(&"section-final", &"start") != OK:
		_fail("%s scroll-into-view smoke rejected the final-section request." % backend_name)
		return
	if not await _wait_for_view_generation(mutated, mutated_generation) \
			or not await _wait_for_view_generation(fresh, fresh_generation):
		return

	var mutated_final := mutated.get_texture().get_image()
	var fresh_final := fresh.get_texture().get_image()
	if not _images_equal(mutated_final, fresh_final) \
			or not _is_color(mutated_final.get_pixel(50, INNER_TOP + 75), Color("c2410c")) \
			or not _is_color(mutated_final.get_pixel(50, INNER_TOP + 85), Color("0891b2")) \
			or not _is_color(mutated_final.get_pixel(50, INNER_TOP + INNER_HEIGHT - 2), Color("0891b2")) \
			or not _is_color(mutated_final.get_pixel(50, 15), Color("112233")):
		_fail("%s final-section request did not clamp to the nearest container maximum without a bottom gap." % backend_name)
		return

	mutated_generation = mutated.get_generation()
	fresh_generation = fresh.get_generation()
	if mutated.set_element_attribute(&"section-0", &"data-continuation", "1") != OK \
			or fresh.set_element_attribute(&"section-0", &"data-continuation", "1") != OK:
		_fail("%s anonymous-container continuation mutation was rejected." % backend_name)
		return
	if not await _wait_for_view_generation(mutated, mutated_generation) \
			or not await _wait_for_view_generation(fresh, fresh_generation):
		return
	mutated_final = mutated.get_texture().get_image()
	fresh_final = fresh.get_texture().get_image()
	if not _images_equal(mutated_final, fresh_final) \
			or not _is_color(mutated_final.get_pixel(50, INNER_TOP + 85), Color("0891b2")) \
			or not _is_color(mutated_final.get_pixel(50, INNER_TOP + INNER_HEIGHT - 2), Color("0891b2")):
		_fail("%s did not preserve the anonymous nearest container offset on the following frame." % backend_name)
		return

	_send_click(Vector2(50, INNER_TOP + 90))
	await process_frame
	if actions != [&"final"]:
		_fail("%s scroll-into-view changed the live DOM/action target; actions=%s." % [backend_name, actions])
		return

	var target := HTMLRenderTarget.new()
	root.add_child(target)
	target.backend_preference = backend_preference
	target.size = Vector2i(WIDTH, HEIGHT)
	target.document = _make_document(FINAL_HTML)
	target.render_now()
	if not await _wait_for_target_image(target):
		return
	if target.scroll_element_into_view(&"section-final") != OK:
		_fail("%s HTMLRenderTarget rejected the final-section request." % backend_name)
		return
	if not await _wait_for_target_match(target, fresh_final):
		return

	print("HCSR Godot %s HTMLView/HTMLRenderTarget scroll_element_into_view(start) smoke passed." % backend_name)
	quit()

func _make_document(inner_html: String) -> HTMLDocument:
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><head><style>%s</style></head><body><div class='outer'><div class='outer-marker'></div><div class='inner'><div id='inner'>%s</div></div><div class='outer-tail'></div></div></body></html>" % [STYLE, inner_html]
	return document

func _make_view(inner_html: String) -> HTMLView:
	var view := HTMLView.new()
	view.backend_preference = backend_preference
	view.size = Vector2(WIDTH, HEIGHT)
	view.document = _make_document(inner_html)
	return view

func _wait_for_initial_views(first: HTMLView, second: HTMLView) -> bool:
	for _frame in range(40):
		await process_frame
		if first.get_generation() > 0 and second.get_generation() > 0 \
				and first.get_texture() != null and second.get_texture() != null:
			return true
	_fail("%s scroll-into-view smoke timed out waiting for initial views." % backend_name)
	return false

func _wait_for_view_generation(view: HTMLView, after_generation: int) -> bool:
	for _frame in range(40):
		await process_frame
		if view.get_generation() > after_generation and view.get_texture() != null:
			return true
	_fail("%s scroll-into-view smoke timed out waiting after generation %d." % [backend_name, after_generation])
	return false

func _wait_for_target_image(target: HTMLRenderTarget) -> bool:
	for _frame in range(40):
		await process_frame
		if target.get_texture() != null and target.get_image() != null:
			return true
	_fail("%s scroll-into-view smoke timed out waiting for HTMLRenderTarget." % backend_name)
	return false

func _wait_for_target_match(target: HTMLRenderTarget, reference: Image) -> bool:
	for _frame in range(40):
		await process_frame
		var image := target.get_image()
		if image != null and _images_equal(image, reference):
			return true
	_fail("%s HTMLRenderTarget did not converge to the clamped HTMLView output." % backend_name)
	return false

func _images_equal(first: Image, second: Image) -> bool:
	return first != null and second != null \
			and first.get_size() == second.get_size() \
			and first.get_data() == second.get_data()

func _is_color(actual: Color, expected: Color) -> bool:
	return abs(actual.r - expected.r) <= 0.01 \
			and abs(actual.g - expected.g) <= 0.01 \
			and abs(actual.b - expected.b) <= 0.01 \
			and actual.a >= 0.99

func _send_click(position: Vector2) -> void:
	var down := InputEventMouseButton.new()
	down.button_index = MOUSE_BUTTON_LEFT
	down.position = position
	down.global_position = position
	down.pressed = true
	root.push_input(down, true)
	var up := InputEventMouseButton.new()
	up.button_index = MOUSE_BUTTON_LEFT
	up.position = position
	up.global_position = position
	up.pressed = false
	root.push_input(up, true)

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

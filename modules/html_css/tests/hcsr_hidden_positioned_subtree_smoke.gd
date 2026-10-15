extends SceneTree

const HTML := """<style>
html,body{margin:0;background:white}
.hidden{display:none}
#hidden{position:absolute;left:0;top:0;width:200px;height:160px;overflow:hidden}
.plate{position:absolute;left:0;top:0}
.row{display:flex}
.mark{position:relative;width:100px;height:100px}
.part{position:absolute;inset:0;background:orange;border-radius:10px}
.bar{position:absolute;left:20px;top:20px;width:10px;height:50px;background:#333}
#visible{position:absolute;left:120px;top:110px;width:40px;height:30px;background:orange}
</style><div id="hidden" class="hidden"><div class="plate"><div class="row"><div class="mark"><div class="part"><div class="bar"></div></div></div></div></div></div><div id="visible"></div>"""
var failed := false

func _initialize() -> void:
	_run.call_deferred()
	create_timer(30).timeout.connect(func(): quit(2))

func _check(condition: bool, message: String) -> void:
	if not condition:
		failed = true
		push_error(message)

func _settle() -> void:
	for frame in 8:
		await process_frame
		await RenderingServer.frame_post_draw

func _run() -> void:
	var view := HTMLView.new()
	view.size = Vector2(200,160)
	view.logical_size = Vector2i(200,160)
	view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
	view.backend_preference = HTMLView.BACKEND_GPU_AUTO
	view.html = HTML
	root.add_child(view)
	var output := view.create_output(Vector2i(200,160),false)
	await _settle()
	for cycle in 5:
		var show := cycle % 2 == 1
		if cycle > 0:
			_check(view.set_element_attribute("hidden","class","" if show else "hidden") == OK,"toggle hidden section")
			await _settle()
		var image: Image = output.texture.get_image()
		_check(image != null,"output image exists")
		if image != null:
			var corner := image.get_pixel(50,50)
			_check(corner.g < .8 if show else corner.g > .98,"hidden descendant pixels: cycle %d, %s" % [cycle,corner])
			_check(image.get_pixel(130,120).g < .8,"visible independent absolute box")
			if cycle == 0 and OS.has_environment("HCSR_HIDDEN_TREE_CAPTURE"):
				image.save_png(OS.get_environment("HCSR_HIDDEN_TREE_CAPTURE"))
	# Empty input must identify its cause, stop retrying, and recover on source change.
	view.html = " \n "
	await _settle()
	var reason: String = str(view.get_frame_scheduler_diagnostics().get("frame_synchronization", {}).get("terminal_reason", ""))
	_check("empty" in reason.to_lower(),"empty document has a descriptive failure: " + reason)
	await _settle()
	_check(str(view.get_frame_scheduler_diagnostics().get("frame_synchronization", {}).get("terminal_reason", "")) == reason,"empty source failure stays stable")
	view.html = HTML
	await _settle()
	_check(str(view.get_frame_scheduler_diagnostics().get("frame_synchronization", {}).get("terminal_reason", "")).is_empty(),"valid source clears recoverable failure")
	_check(output.texture.get_image().get_pixel(50,50).g > .98,"recovered scene has no hidden pixels")
	view.html = ""
	view.html_file = "res://__missing_hidden_tree_source__.html"
	await _settle()
	var missing: String = str(view.get_frame_scheduler_diagnostics().get("frame_synchronization", {}).get("terminal_reason", ""))
	_check("__missing_hidden_tree_source__.html" in missing,"missing document identifies its path: " + missing)
	view.html_file = ""
	view.html = HTML
	await _settle()
	_check(str(view.get_frame_scheduler_diagnostics().get("frame_synchronization", {}).get("terminal_reason", "")).is_empty(),"missing source failure recovers")
	output.release()
	view.queue_free()
	await _settle()
	print("HCSR_HIDDEN_POSITIONED_SUBTREE_", "FAILED" if failed else "OK")
	quit(1 if failed else 0)

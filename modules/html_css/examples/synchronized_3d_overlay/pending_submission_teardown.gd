extends SceneTree

# Run with --script res://pending_submission_teardown.gd. Destroy each view
# after its pre-draw preparation, while its driver callback is in the graph.
func _initialize() -> void:
	_run.call_deferred()

func _run() -> void:
	for iteration in range(8):
		var view := HTMLView.new()
		view.size = Vector2(320, 180)
		view.backend_preference = HTMLView.BACKEND_GPU_AUTO
		view.html = "<div style='width:100px;height:60px'></div>"
		root.add_child(view)
		RenderingServer.frame_pre_draw.connect(func():
			# Drain the render-thread registration before destroying its owner.
			RenderingServer.force_sync()
			view.free(), CONNECT_ONE_SHOT)
		await RenderingServer.frame_post_draw
	print("SYNCHRONIZED_HTML_PENDING_TEARDOWN_OK iterations=8")
	quit()

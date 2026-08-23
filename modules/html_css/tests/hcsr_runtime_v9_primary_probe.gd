extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var root := Control.new()
	root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	get_root().add_child(root)
	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_D3D12
	view.logical_size = Vector2i(320, 180)
	view.size = Vector2(320, 180)
	view.html = "<html><body><div style='width:80px;height:50px;background:#20c060'></div></body></html>"
	root.add_child(view)
	for _frame in range(240):
		await process_frame
		if view.get_generation() > 0:
			print("HCSR runtime v9 primary probe passed.")
			quit(0)
			return
	push_error("Primary generation did not publish.")
	quit(1)

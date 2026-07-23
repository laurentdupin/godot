extends SceneTree

var backend := HTMLView.BACKEND_D3D12
var backend_name := "D3D12"
var queued_count := 0
var activated_count := 0

func _initialize() -> void:
	if "--vulkan" in OS.get_cmdline_user_args():
		backend = HTMLView.BACKEND_VULKAN
		backend_name = "Vulkan"
	elif "--cpu" in OS.get_cmdline_user_args():
		backend = HTMLView.BACKEND_CPU
		backend_name = "CPU"
	call_deferred("_run")

func _run() -> void:
	var root := Control.new()
	root.size = Vector2(160, 90)
	get_root().add_child(root)
	var view := HTMLView.new()
	view.backend_preference = backend
	view.logical_size = Vector2i(160, 90)
	view.size = Vector2(160, 90)
	view.html = "<html><head><style>html,body{margin:0;background:#182838}.panel{position:absolute;inset:10px;background:#365848}</style></head><body><div class='panel'></div></body></html>"
	view.frame_queued.connect(func(_generation: int) -> void: queued_count += 1)
	view.frame_activated.connect(func(_generation: int) -> void: activated_count += 1)
	root.add_child(view)

	for _frame in range(240):
		await process_frame
		if view.get_generation() > 0:
			break
	if view.get_generation() <= 0:
		_fail("%s static-idle fixture did not activate its required frame." % backend_name)
		return
	for _settle in range(12):
		await process_frame
	var settled_generation := view.get_generation()
	var settled_queued := queued_count
	var settled_activated := activated_count
	var settled_texture := view.get_texture()
	var idle_start_frame := Engine.get_process_frames()
	for _idle in range(180):
		await process_frame
	if view.get_generation() != settled_generation \
			or queued_count != settled_queued \
			or activated_count != settled_activated \
			or view.get_texture() != settled_texture:
		_fail("%s static HTML performed presentation work after settling: generation %d->%d queued %d->%d activated %d->%d." % [
			backend_name,
			settled_generation,
			view.get_generation(),
			settled_queued,
			queued_count,
			settled_activated,
			activated_count,
		])
		return
	print("HCSR_STATIC_IDLE backend=%s generation=%d idle_engine_frames=%d queued_delta=0 activated_delta=0" % [
		backend_name,
		settled_generation,
		Engine.get_process_frames() - idle_start_frame,
	])
	quit(0)

func _fail(message: String) -> void:
	push_error(message)
	quit(1)

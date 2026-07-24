extends SceneTree

var view: HTMLView
var activated_generation := 0
var activated_process_frame := 0

func _init() -> void:
	call_deferred("_run")

func _run() -> void:
	var host := Control.new()
	host.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	root.add_child(host)
	view = HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_D3D12 if "--d3d12" in OS.get_cmdline_user_args() else (
		HTMLView.BACKEND_VULKAN if "--vulkan" in OS.get_cmdline_user_args() else (
			HTMLView.BACKEND_METAL if "--metal" in OS.get_cmdline_user_args() else HTMLView.BACKEND_CPU
		)
	)
	view.logical_size = Vector2i(320, 180)
	view.size = Vector2(320, 180)
	view.html = "<!DOCTYPE html><html><body style='margin:0;background:#234;color:white'>frame evidence</body></html>"
	view.frame_activated.connect(_on_frame_activated)
	host.add_child(view)
	for _frame in range(60):
		await process_frame
		if activated_generation > 0:
			break
	if activated_generation <= 0:
		push_error("The HTMLView did not activate a frame.")
		quit(1)
		return
	if view.get_generation() != activated_generation:
		push_error("The HTMLView evidence generation did not match the activated generation.")
		quit(1)
		return
	var host_frame := view.get_host_frame_number()
	if host_frame <= 0 or host_frame > activated_process_frame:
		push_error("The HTMLView did not preserve the process frame that requested its logical frame (host=%d activated=%d current=%d generation=%d)." % [
			host_frame,
			activated_process_frame,
			Engine.get_process_frames(),
			activated_generation,
		])
		quit(1)
		return
	if view.get_timeline_time_seconds() < 0.0:
		push_error("The HTMLView exposed an invalid frame timeline.")
		quit(1)
		return
	print("HCSR_HOST_FRAME_EVIDENCE_OK generation=%d host_frame=%d activated_frame=%d timeline=%.6f" % [
		activated_generation,
		host_frame,
		activated_process_frame,
		view.get_timeline_time_seconds(),
	])
	print("HCSR host-frame evidence smoke passed.")
	quit(0)

func _on_frame_activated(generation: int) -> void:
	activated_generation = generation
	activated_process_frame = Engine.get_process_frames() + 1

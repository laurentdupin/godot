extends SceneTree

func _initialize() -> void:
	var interface := XRServer.find_interface("OpenXR")
	if interface == null:
		push_error("The deferred OpenXR interface was not registered.")
		quit(1)
		return
	if interface.is_initialized():
		push_error("OpenXR initialized before an explicit request.")
		quit(1)
		return

	print("OPENXR_LAZY_STARTUP_READY")
	if OS.get_cmdline_user_args().has("--probe"):
		var first_probe: bool = interface.call("is_hmd_available")
		var second_probe: bool = interface.call("is_hmd_available")
		print("OPENXR_LAZY_PROBE_RESULTS=%s,%s;initialized=%s" % [first_probe, second_probe, interface.is_initialized()])
	if OS.get_cmdline_user_args().has("--initialize"):
		var initialized := interface.initialize()
		print("OPENXR_LAZY_INITIALIZE_RESULT=%s" % initialized)
		if initialized:
			interface.uninitialize()
	quit()

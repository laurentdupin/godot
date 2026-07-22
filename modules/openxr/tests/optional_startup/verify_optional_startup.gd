extends SceneTree

func _initialize() -> void:
	var interface := XRServer.find_interface("OpenXR")
	if interface == null:
		push_error("The optional-startup OpenXR interface was not registered.")
		quit(1)
		return

	print("OPENXR_OPTIONAL_STARTUP_INITIALIZED=%s" % interface.is_initialized())
	quit()

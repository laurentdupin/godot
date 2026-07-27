extends SceneTree


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	await process_frame
	var openxr := XRServer.find_interface("OpenXR")
	if openxr == null:
		push_error("OpenXR interface is not registered.")
		quit(1)
		return

	if not openxr.is_hmd_available():
		print("OPENXR_REPEATABLE_LIFECYCLE_UNAVAILABLE")
		quit()
		return

	for cycle in range(2):
		if not openxr.initialize():
			push_error("OpenXR initialization failed during cycle %d." % cycle)
			quit(1)
			return
		if not openxr.is_initialized():
			push_error("OpenXR did not report initialized during cycle %d." % cycle)
			quit(1)
			return

		root.use_xr = true
		for _frame in 120:
			await process_frame

		# This is the supported public lifecycle ordering used by ordinary UI
		# callbacks: stop routing the root viewport to XR, then tear down the
		# interface immediately.
		root.use_xr = false
		openxr.uninitialize()
		if openxr.is_initialized():
			push_error("OpenXR remained initialized after cycle %d." % cycle)
			quit(1)
			return
		for _frame in 4:
			await process_frame

	print("OPENXR_REPEATABLE_RENDERED_LIFECYCLE_OK")
	quit()

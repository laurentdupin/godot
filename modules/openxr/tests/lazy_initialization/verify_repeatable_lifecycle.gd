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

	for cycle in range(4):
		var initialize_started := Time.get_ticks_msec()
		if not openxr.initialize():
			push_error("OpenXR initialization failed during cycle %d." % cycle)
			quit(1)
			return
		var initialize_duration := Time.get_ticks_msec() - initialize_started
		if initialize_duration > 5000:
			push_error("OpenXR initialization blocked for %d ms during cycle %d." % [initialize_duration, cycle])
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
		var uninitialize_started := Time.get_ticks_msec()
		openxr.uninitialize()
		var uninitialize_duration := Time.get_ticks_msec() - uninitialize_started
		if uninitialize_duration > 2000:
			push_error("OpenXR uninitialization blocked for %d ms during cycle %d." % [uninitialize_duration, cycle])
			quit(1)
			return
		if openxr.is_initialized():
			push_error("OpenXR remained initialized after cycle %d." % cycle)
			quit(1)
			return
		for _frame in 4:
			await process_frame

	print("OPENXR_REPEATABLE_RENDERED_LIFECYCLE_OK")
	quit()

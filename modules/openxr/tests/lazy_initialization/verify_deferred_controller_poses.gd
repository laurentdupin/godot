extends SceneTree

const TEST_DURATION_MSEC := 15000
const REQUIRED_CONSECUTIVE_TRACKED_FRAMES := 30
const INITIALIZATION_CYCLES := 2


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
		print("OPENXR_DEFERRED_CONTROLLER_POSES_UNAVAILABLE")
		quit()
		return
	for cycle in INITIALIZATION_CYCLES:
		if not openxr.initialize():
			push_error("OpenXR failed to initialize during cycle %d after the availability probe." % cycle)
			quit(1)
			return

		root.use_xr = true
		if not await _wait_for_controller_poses(cycle):
			root.use_xr = false
			openxr.uninitialize()
			quit(1)
			return
		root.use_xr = false
		openxr.uninitialize()
		for _frame in 4:
			await process_frame

	print("OPENXR_DEFERRED_CONTROLLER_POSES_OK")
	quit()


func _wait_for_controller_poses(cycle: int) -> bool:
	var started_at := Time.get_ticks_msec()
	var consecutive_tracked_frames := 0
	var maximum_consecutive_tracked_frames := 0
	var observed_touch_profile := false
	while Time.get_ticks_msec() - started_at < TEST_DURATION_MSEC:
		await process_frame
		var left_tracker := XRServer.get_tracker("left_hand")
		var right_tracker := XRServer.get_tracker("right_hand")
		var both_tracked := _tracker_has_controller_pose(left_tracker) and _tracker_has_controller_pose(right_tracker)
		if left_tracker != null and left_tracker.get_tracker_profile() != "/interaction_profiles/none":
			observed_touch_profile = true
		if right_tracker != null and right_tracker.get_tracker_profile() != "/interaction_profiles/none":
			observed_touch_profile = true
		if both_tracked:
			consecutive_tracked_frames += 1
			maximum_consecutive_tracked_frames = maxi(maximum_consecutive_tracked_frames, consecutive_tracked_frames)
			if consecutive_tracked_frames >= REQUIRED_CONSECUTIVE_TRACKED_FRAMES:
				return true
		else:
			consecutive_tracked_frames = 0

	push_error(
			"OpenXR cycle=%d observed_profile=%s but never published both controller poses for %d consecutive frames (maximum=%d)."
			% [cycle, observed_touch_profile, REQUIRED_CONSECUTIVE_TRACKED_FRAMES, maximum_consecutive_tracked_frames]
	)
	return false


func _tracker_has_controller_pose(tracker: XRPositionalTracker) -> bool:
	if tracker == null:
		return false
	return tracker.has_pose("default") or tracker.has_pose("aim") or tracker.has_pose("grip")

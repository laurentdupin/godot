extends SceneTree


func _initialize() -> void:
	var arguments := OS.get_cmdline_user_args()
	if arguments.size() < 2 or not ProjectSettings.load_resource_pack(arguments[0], true):
		push_error("Usage: verify_external_pack_entries.gd -- <pack> <required-path>...")
		quit(2)
		return
	for index in range(1, arguments.size()):
		if not FileAccess.file_exists(arguments[index]):
			push_error("Required package entry is missing: %s" % arguments[index])
			quit(1)
			return
	print("External HCSR package entries verified.")
	quit()

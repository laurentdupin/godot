extends SceneTree

func _initialize() -> void:
	var expected_files := [
		"res://Entry.html",
		"res://Entry.css",
		"res://Entry.hcsrpkg",
		"res://RuntimeTemplates.html",
	]
	for path in expected_files:
		if not FileAccess.file_exists(path):
			push_error("Expected exported HCSR asset is missing: %s" % path)
			quit(1)
			return

	var package := FileAccess.open("res://Entry.hcsrpkg", FileAccess.READ)
	if package == null or package.get_length() == 0:
		push_error("Exported HCSR entry package is empty.")
		quit(1)
		return

	var templates := FileAccess.get_file_as_string("res://RuntimeTemplates.html")
	if templates.find("{{TITLE}}") < 0:
		push_error("Auxiliary HTML template source was not preserved verbatim.")
		quit(1)
		return

	var forbidden_files := [
		"res://RuntimeTemplates.hcsrpkg",
		"res://Excluded.html",
		"res://Excluded.hcsrpkg",
	]
	for path in forbidden_files:
		if FileAccess.file_exists(path):
			push_error("Unexpected exported HCSR asset exists: %s" % path)
			quit(1)
			return

	print("HCSR export package contents smoke passed.")
	quit()

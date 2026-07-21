extends Node

const PROGRAMMATIC_DOCUMENT_PATH := "res://ProgrammaticEntry.html"


func _make_programmatic_document() -> HTMLDocument:
	var document := HTMLDocument.new()
	document.html_file = PROGRAMMATIC_DOCUMENT_PATH
	return document


func _ready() -> void:
	var expected_files := [
		"res://Entry.html",
		"res://Entry.css",
		"res://Entry.hcsrpkg",
		"res://ProgrammaticEntry.html",
		"res://ProgrammaticEntry.hcsrpkg",
		"res://RuntimeTemplates.html",
	]
	for path in expected_files:
		if not FileAccess.file_exists(path):
			push_error("Expected exported HCSR asset is missing: %s" % path)
			get_tree().quit(1)
			return

	var package := FileAccess.open("res://Entry.hcsrpkg", FileAccess.READ)
	if package == null or package.get_length() == 0:
		push_error("Exported HCSR entry package is empty.")
		get_tree().quit(1)
		return

	var templates := FileAccess.get_file_as_string("res://RuntimeTemplates.html")
	if templates.find("{{TITLE}}") < 0:
		push_error("Auxiliary HTML template source was not preserved verbatim.")
		get_tree().quit(1)
		return

	var forbidden_files := [
		"res://RuntimeTemplates.hcsrpkg",
		"res://Excluded.html",
		"res://Excluded.hcsrpkg",
	]
	for path in forbidden_files:
		if FileAccess.file_exists(path):
			push_error("Unexpected exported HCSR asset exists: %s" % path)
			get_tree().quit(1)
			return

	print("HCSR export package contents smoke passed.")
	get_tree().quit()

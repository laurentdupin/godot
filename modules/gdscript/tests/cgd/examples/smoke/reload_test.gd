extends SceneTree

func _initialize() -> void:
	var path := "user://gdc_reload.cgd"
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string("extends RefCounted; int value(){return 1;}")
	file = null
	var script: Script = load(path)
	assert(script != null)
	assert(script.new().value() == 1)
	file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string("extends RefCounted; int value(){return 2;}")
	file = null
	var refreshed: Script = ResourceLoader.load(path, "GDScript", ResourceLoader.CACHE_MODE_IGNORE)
	assert(refreshed != null)
	assert(refreshed.new().value() == 2)
	assert(refreshed.source_code.contains("int value()"))
	assert(ResourceSaver.save(refreshed, path) == OK)
	assert(FileAccess.get_file_as_string(path).contains("int value()"))
	DirAccess.remove_absolute(path)
	print("GDC_RELOAD_OK")
	quit(0)

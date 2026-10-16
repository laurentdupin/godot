extends SceneTree

var failed := false

func _initialize() -> void:
    _run.call_deferred()
    create_timer(30).timeout.connect(func(): quit(2))

func _check(condition: bool, message: String) -> void:
    if not condition:
        failed = true
        push_error(message)

func _run() -> void:
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    Engine.max_fps = 240
    var view := HTMLView.new()
    view.size = Vector2(800,600)
    view.logical_size = Vector2i(800,600)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.backend_preference = HTMLView.BACKEND_GPU_AUTO
    view.html = """<!doctype html><style>html,body{margin:0}
        #panel{width:280px;height:1400px;background:linear-gradient(27deg,#123456,#abcdef);border-radius:17px}
        #label{font:20px Arial;color:white}img{width:100px;height:100px}
        </style><div id='panel'><span id='label'>Atlas diagnostic ABC</span>
        <img src="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='100' height='100'%3E%3Cpath fill='%2312abef' d='M0 0h100v100H0z'/%3E%3C/svg%3E"></div>"""
    root.add_child(view)
    var output := view.create_output(Vector2i(800,600),false)
    for frame in 12:
        await process_frame
        await RenderingServer.frame_post_draw
    # Exercise throttling and an idle final revision after a rapid appearance change.
    _check(view.set_element_attribute("panel","style","border-radius:23px") == OK, "Appearance mutation accepted")
    await create_timer(1.5).timeout
    await RenderingServer.frame_post_draw
    var stats: Dictionary = view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas
    _check(int(stats.get("atlas_used_rows",0)) > 0,"Live used rows reported")
    _check(int(stats.get("atlas_packed_area_pixels",0)) > 0,"Live packed area reported")
    _check(int(stats.get("atlas_largest_entry",{}).get("h",0)) > 0,"Largest allocation reported")
    var directory := OS.get_environment("HCSR_DUMP_ATLAS_DIR")
    if not directory.is_empty():
        _verify_dump(directory,stats)
    print("ATLAS_DUMP_STATS ",JSON.stringify(stats))
    output.release()
    view.queue_free()
    for frame in 4: await process_frame
    print("ATLAS_DUMP_SMOKE_", "FAILED" if failed else "PASS")
    quit(1 if failed else 0)

func _verify_dump(directory: String, stats: Dictionary) -> void:
    var found := false
    for name in DirAccess.get_files_at(directory):
        if not name.ends_with("-latest.json"):
            continue
        var metadata: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(directory.path_join(name)))
        if int(metadata.get("process_id",0)) != OS.get_process_id():
            continue
        found = true
        _check(int(metadata.revision) == int(stats.atlas_revision), "Final idle revision dumped after throttling")
        var path := directory.path_join(name.trim_suffix(".json"))
        var image := Image.load_from_file(path + ".png")
        var overlay := Image.load_from_file(path + "-overlay.png")
        _check(image.get_size() == Vector2i(int(metadata.width),maxi(1,int(metadata.used_rows))), "Full width, live-height crop")
        _check(image.get_size() == overlay.get_size(), "Overlay dimensions match")
        var area := 0
        var rows := 0
        var sizes: Array[int] = []
        for entry: Dictionary in metadata.entries:
            _check(int(entry.page) > 0, "Permanent sampling page omitted")
            area += int(entry.w)*int(entry.h)
            rows = maxi(rows,int(entry.y)+int(entry.h))
            sizes.append(int(entry.h))
            if entry.kind == "image":
                var color := image.get_pixel(int(entry.x)+int(entry.w)/2,int(entry.y)+int(entry.h)/2)
                _check(color.is_equal_approx(Color("12abef")), "BGRA converted to RGBA correctly")
            elif entry.kind == "surface":
                _check(not entry.identity.owners.is_empty(), "Surface owners resolved")
        _check(area == int(metadata.packed_area_pixels), "Packed area matches live rectangles")
        _check(rows == int(metadata.used_rows), "Used rows match live rectangles")
        _check(is_equal_approx(float(metadata.efficiency),float(area)/(rows*int(metadata.width))), "Packing efficiency matches")
        sizes.sort()
        sizes.reverse()
        for index in metadata.ten_tallest_entries.size():
            _check(int(metadata.ten_tallest_entries[index].h) == sizes[index], "Tallest entries sorted")
    _check(found, "Latest atlas dump exists")

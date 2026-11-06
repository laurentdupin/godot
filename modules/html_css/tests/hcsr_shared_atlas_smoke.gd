extends SceneTree

var failed := false
var views: Array[HTMLView] = []
var directory := OS.get_environment('HCSR_SHARED_ATLAS_PROBE_DIR')

func _initialize():
    run.call_deferred()
    create_timer(120).timeout.connect(func(): quit(2))

func require(ok: bool, label: String):
    if not ok:
        failed = true
        push_error('SHARED_ATLAS_FAILED ' + label)

func frames(count := 8):
    for i in count:
        await process_frame
        await RenderingServer.frame_post_draw

func statistics(view: HTMLView) -> Dictionary:
    return view.get_frame_scheduler_diagnostics().frame_synchronization.image_atlas

func pixels(view: HTMLView) -> PackedByteArray:
    var image := view.get_texture().get_image()
    image.convert(Image.FORMAT_RGBA8)
    return image.get_data()

func near_pixels(view: HTMLView, expected: PackedByteArray) -> bool:
    var actual := pixels(view)
    if actual.size() != expected.size(): return false
    var maximum := 0
    for i in actual.size(): maximum = maxi(maximum, absi(int(actual[i]) - int(expected[i])))
    print('SHARED_ATLAS_PIXEL_DELTA ', maximum)
    return maximum <= 1

func create_view(name: String, color: Color) -> HTMLView:
    var folder := 'user://hcsr_shared_atlas_' + name
    DirAccess.make_dir_recursive_absolute(folder)
    var image := Image.create_empty(32, 32, false, Image.FORMAT_RGBA8)
    image.fill(color)
    require(image.save_png(folder.path_join('same.png')) == OK, 'write isolation fixture')
    var document := HTMLDocument.new()
    document.resource_root = folder
    document.html = "<style>html,body{margin:0;font:22px Arial;color:#eee;background:#123}#card{position:absolute;left:4px;top:4px;width:300px;height:150px;border:1.5px solid #adb;border-radius:17px;box-shadow:3px 4px 8px #0009}img{position:absolute;left:10px;top:10px;width:32px;height:32px}#caption{position:absolute;left:12px;top:60px}.large{font-size:44px}</style><div id=card><img src='same.png'><span id=caption>Shared glyphs 日本語</span><div id=extra></div></div>"
    if '--groups' in OS.get_cmdline_user_args():
        document.html += "<div style='position:absolute;left:210px;top:108px;width:65px;height:30px;background:#d86350;mask-image:linear-gradient(to right,transparent,black);filter:blur(1px);opacity:.7'></div>"
    var view := HTMLView.new()
    view.document = document
    view.size = Vector2(320, 180)
    view.logical_size = Vector2i(320, 180)
    view.fixed_viewport_size = Vector2i(320, 180)
    view.viewport_size_mode = HTMLView.VIEWPORT_SIZE_FIXED
    view.backend_preference = HTMLView.BACKEND_GPU_AUTO
    view.release_targets_when_hidden = true
    root.add_child(view)
    views.append(view)
    return view

func run():
    Engine.max_fps = 0
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    var a := create_view('a', Color.RED)
    await frames()
    var original := pixels(a)
    if not directory.is_empty():
        DirAccess.make_dir_recursive_absolute(directory)
        a.get_texture().get_image().save_png(directory.path_join("original.png"))
    var first := statistics(a)
    require(int(first.get('shared_atlas_gpu_bytes', 0)) > 0, 'actual shared GPU allocation')
    var b := create_view('b', Color.BLUE)
    await frames()
    var second := statistics(b)
    require(first.atlas_pool_id == second.atlas_pool_id, 'same render-context pool')
    require(int(second.atlas_pool_views) == 2, 'two owners')
    require(int(second.shared_atlas_glyphs) <= int(first.glyph_entries) + 2, 'duplicate glyphs have one slot')
    require(pixels(a) == original, 'second view leaves first output intact')
    require(a.get_texture().get_image().get_pixel(20,20).r > .99, 'first document asset access')
    require(b.get_texture().get_image().get_pixel(20,20).b > .99, 'same relative source isolated by document')
    # Grow the shared page and then alternate pixel/scale changes. The untouched
    # view must rebind any replaced texture when it next draws.
    require(b.set_element_inner_html('extra', "<div style='width:1100px;height:1100px;background:linear-gradient(35deg,red,blue);border-radius:150px'></div>") == OK, 'force pool growth')
    await frames()
    require(int(statistics(b).shared_atlas_gpu_bytes) > int(second.shared_atlas_gpu_bytes), 'shared page actually grew')
    for i in 6:
        require(a.set_element_attribute('caption', 'class', 'large' if i % 2 == 0 else '') == OK, 'resolution change')
        require(b.set_element_text('caption', 'Other glyphs %d' % i) == OK, 'other owner mutation')
        await frames(3)
    require(a.set_element_attribute('caption','class','') == OK, 'restore first resolution')
    await frames()
    require(near_pixels(a, original), 'growth and interleaved mutations preserve restored pixels')
    a.hide()
    await frames()
    require(b.set_element_inner_html('extra', '') == OK, 'retire large appearance')
    await frames()
    a.show()
    await frames()
    require(near_pixels(a, original), 'hidden owner returns after shared page changes')
    b.queue_free()
    views.erase(b)
    await frames()
    require(a.set_element_text('caption','Shared glyphs 日本語!') == OK, 'surviving owner draws')
    await frames()
    require(a.set_element_text('caption','Shared glyphs 日本語') == OK, 'restore survivor')
    await frames()
    require(near_pixels(a, original), 'destroying borrower preserves survivor glyphs')
    var last := statistics(a)
    require(int(last.atlas_pool_views) == 1, 'destroyed borrower released pool ownership')
    require(int(last.failed_allocations) == 0, 'no allocator failures')
    require(int(last.shared_atlas_gpu_bytes) <= int(first.shared_atlas_gpu_bytes), 'temporary growth shrinks after large borrower retires')
    if not directory.is_empty():
        DirAccess.make_dir_recursive_absolute(directory)
        a.get_texture().get_image().save_png(directory.path_join('survivor.png'))
        FileAccess.open(directory.path_join('statistics.json'),FileAccess.WRITE).store_string(JSON.stringify({'first':first,'second':second,'last':last}))
    var old_pool := int(last.atlas_pool_id)
    a.queue_free()
    views.clear()
    await frames(12)
    var fresh := create_view('fresh', Color.RED)
    await frames()
    require(int(statistics(fresh).atlas_pool_id) != old_pool, 'last borrower destroys pool')
    require(pixels(fresh) == original, 'recreated pool reproduces original pixels')
    fresh.queue_free()
    views.clear()
    await frames()
    print('SHARED_ATLAS_', 'FAILED' if failed else 'PASS')
    quit(1 if failed else 0)

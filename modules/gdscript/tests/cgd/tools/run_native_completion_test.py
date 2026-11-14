#!/usr/bin/env python3
"""Verify asynchronous native completion and preamble creation in the editor."""
import pathlib, subprocess, sys, tempfile
with tempfile.TemporaryDirectory() as tmp:
    root=pathlib.Path(tmp)
    addon=root/"addons/check"
    addon.mkdir(parents=True)
    (root/"project.godot").write_text('config_version=5\n[editor_plugins]\nenabled=PackedStringArray("res://addons/check/plugin.cfg")\n')
    (root/"main.cgd").write_text("extends Node;\nnative {\n#include <stdio.h>\n}\nnative int f() { return 1; }\n")
    (addon/"plugin.cfg").write_text('[plugin]\nname="Async check"\nscript="plugin.gd"\n')
    (addon/"plugin.gd").write_text(r'''@tool
extends EditorPlugin
func _enter_tree():
    run_test.call_deferred()
func run_test():
    await get_tree().create_timer(1).timeout
    EditorInterface.edit_script(load("res://main.cgd"))
    await get_tree().create_timer(1).timeout
    var code = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
    code.text = code.text.replace("return 1;", "fop; return 1;")
    code.set_caret_line(4)
    code.set_caret_column(code.get_line(4).find("fop") + 3)
    code.request_code_completion(true)
    var immediate = code.get_code_completion_options().any(func(option): return option.display_text == "fopen")
    var found = false
    for i in range(40):
        await get_tree().create_timer(0.05).timeout
        found = code.get_code_completion_options().any(func(option): return option.display_text == "fopen")
        if found:
            break
    print("CGD_ASYNC ", not immediate and found)
    code.text = code.text.replace("fop;", "fop; int changed = 2;")
    code.set_caret_line(4)
    code.set_caret_column(code.get_line(4).find("fop") + 3)
    var started = Time.get_ticks_usec()
    code.request_code_completion(true)
    for i in range(120):
        await get_tree().process_frame
        if code.get_code_completion_options().any(func(option): return option.display_text == "fopen"):
            print("CGD_WARM_COMPLETION_US ", Time.get_ticks_usec() - started)
            break
    get_tree().quit(0 if not immediate and found else 1)
''')
    p=subprocess.run([sys.argv[1],"--headless","--editor","--verbose","--path",tmp],capture_output=True,text=True,encoding="utf-8",errors="replace",timeout=25)
    print(p.stdout,p.stderr)
    print("PCH_COUNT", len(list(root.rglob("*.pch"))))
    assert list(root.rglob("*.pch"))
    sys.exit(p.returncode or ("CGD_ASYNC true" not in p.stdout))

# GD-C integration

GD-C adds C-style syntax to GDScript using `.cgd` files. Source is transpiled in memory and uses the existing GDScript analyzer, compiler and VM. It is not native C/C++ compilation. See [syntax reference](CGD_SYNTAX.md).

## Creating scripts

In the FileSystem dock, right-click and choose **Create New > GD-C Script...** (or **New GD-C Script...**, depending on the context). The script dialog also offers **GD-C (.cgd)** in its language list, including when attaching a script to a node.

The dialog supplies an empty template and a Node template using declarations such as `void _ready() {}`, saves original C-style source, remembers the selected dialect, and permits inheritance between `.gd` and `.cgd` files. GD-C scripts must be external files: built-in scene scripts do not retain a dialect extension.

GD-C accepts only its C-style source grammar. GDScript declaration aliases (`func`, `fn`, `var`, `pass`, suffix types), word operators, indentation-based blocks, and GDScript comment/string shorthands are rejected. Generated callbacks also use typed C-style declarations. Inferred/dynamic declarations, typed lambdas, and cast/type-test/membership intrinsics preserve the corresponding native semantics. Global classes and autoloads use the same analyzer paths as `.gd`. The internal translation still targets GDScript; normal `.gd` resources retain their original language.

Editor integration includes C-style comment highlighting/toggling, generated signal callbacks, locating existing callbacks, brace-aware reindentation that preserves multiline literals, translation extraction, and source mapping for function headers and local-variable diagnostics. Export-plugin errors now fail export rather than reporting success with missing resources. Ordinary `.gd` behavior retains its existing defaults.

## Builds and checks

From the engine checkout on Windows:

```powershell
py -3.12 -m SCons platform=windows target=editor dev_build=yes tests=yes module_html_css_renderer=hcsr_newest extra_suffix=hcsr_newest -j8
py -3.12 -m SCons platform=windows target=template_release module_html_css_renderer=hcsr_newest extra_suffix=hcsr_newest -j8
python modules/gdscript/tests/cgd/tools/run_tests.py --compiler clang++
python modules/gdscript/tests/cgd/tools/run_editor_creation_test.py --godot bin/godot.windows.editor.dev.x86_64.hcsr_newest.console.exe
python modules/gdscript/tests/cgd/tools/run_editor_editing_test.py --godot bin/godot.windows.editor.dev.x86_64.hcsr_newest.console.exe
python modules/gdscript/tests/cgd/tools/run_resource_script_test.py --godot bin/godot.windows.editor.dev.x86_64.hcsr_newest.console.exe
python modules/gdscript/tests/cgd/tools/run_language_server_test.py --godot bin/godot.windows.editor.dev.x86_64.hcsr_newest.console.exe
python modules/gdscript/tests/cgd/tools/run_engine_tests.py --godot bin/godot.windows.editor.dev.x86_64.hcsr_newest.console.exe --template bin/godot.windows.template_release.x86_64.hcsr_newest.console.exe --native-tests
```

The portable tests compile the actual module implementation. The creation test exercises the real FileSystem popup and dialog through a temporary editor plugin, then executes the created script outside editor mode. The smoke runner copies its project to a temporary directory and checks scene attachment, mixed inheritance, global classes, source reload/save, same-stem `.gd`/`.cgd` files, and text/binary/compressed packs.

The editing test opens a Node3D CGD script in the actual script editor and requests completion with unfinished declarations, strings and expressions. Conversion can fail before a native syntax tree exists; completion and symbol lookup must return a parse error in that case instead of invoking the analyzer. Native tests also exercise every character prefix of the starter script. Partial native trees remain available for normal completion recovery.

## Compatibility and limits

- `.gdc` remains native compiled GDScript. GD-C compiled resources use `.cgd.gdbin`, retaining source maps without colliding with ordinary `.gd` exports.
- Export templates must contain this integration. Stock templates cannot load these files.
- All ten built-in templates have C-style counterparts. Project and editor `.cgd` templates support `// meta-` metadata. Completion recovers an unfinished statement at the caret; not every incomplete C-style construct has a recovery rule.
- Folding still follows the existing editor's indentation behavior. Use brace-aware reindentation for source that has no indentation.
- Exported token buffers preserve source lines; precise columns remain limited by Godot's native token format.
- Malformed-script class discovery and selected-resource dependency discovery retain the limitations described in the candidate. Cross-platform builds have not been validated here.

The initial bundle targets commit `95d622f2966ad862c8f7fe317267dff1f6a5413e`; all nine original file hashes also matched this checkout at `a34464c9a834975c7ab6dd60055498cafdf019e4`. The bundled implementation is MIT licensed; its notice is retained in `gdc/LICENSE`.

## Editor feature audit

The follow-up audit covers explicit `.gd` checks and language-extension comparisons in the editor, resource system and GDScript language server. Details and validation are in [CGD_EDITOR_AUDIT.md](CGD_EDITOR_AUDIT.md).

## Validation on September 6, 2026

- Windows editor (`dev_build=yes`, `tests=yes`) and non-editor release template compiled successfully with the hcsr_newest renderer configuration.
- Final native run: 17 test cases, 753 assertions passed. This includes 12 GD-C cases, the existing GDScript native cases, and the complete source compilation/runtime suite.
- Portable frontend: 17,137 checks passed with Windows Clang, including strict-syntax rejection in normal and completion modes, inferred/dynamic declarations, typed lambdas, type checks/casts/membership, source fuzzing, random binary inputs, valid-envelope mutations and example translation.
- Real editor creation test: the GD-C FileSystem menu opened the dialog with the correct dialect, saved the Node template with C-style callbacks and no compatibility syntax, and the created script executed successfully.
- The optional existing GDScript binary-token suite reports seven diagnostic-output mismatches (match patterns, untyped declarations, an unexpected class-body identifier, and four empty-file warnings). The identical seven failures were reproduced by compiling and testing the original pre-integration parser. They are recorded rather than suppressed.
- Final deployed-release checks passed for text, binary and compressed packs, including mixed inheritance and same-stem scripts. Invalid GD-C source returns a failed export status.

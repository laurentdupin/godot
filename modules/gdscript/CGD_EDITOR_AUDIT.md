# CGD editor integration audit — September 6, 2026

The audit examined hard-coded `.gd` checks, script-language extension comparisons, and editor actions that generate GDScript syntax. CGD continues to use GDScript's analyzer, compiler, VM, debugger and script resource type, with a separate C-style source dialect.

| Feature | Result |
| --- | --- |
| Open/attach/inherit script dialogs | Combined `.gd,.cgd` filter; selecting a CGD file switches the dialect and permits attachment. |
| Resource Inspector Script field | Shader, ShaderMaterial and other Resource objects use the script-creation dialog, including CGD and external-script extension. The owner class supplies the script base. |
| Autocomplete | Inherited properties, local variables and typed members work while a statement is unfinished. Exact caret mapping preserves member completion after a dot. GDScript-only keywords are excluded. |
| Invalid editing buffers | Completion and symbol lookup return a parse error safely when conversion produces no syntax tree. Normal compilation still requires semicolons. |
| Script discovery | Initial filesystem scans recognize both source extensions, including global classes. |
| Autoload completion/navigation | CGD scripts follow the same type inference and lookup paths as GD scripts. |
| Templates | Ten built-in templates have CGD versions. Project/editor CGD templates use the existing selection/history mechanism and `// meta-` metadata. |
| Editor plugin wizard | GD-C is selectable; generated plugins use C-style syntax. Existing CGD plugin filenames remain valid. |
| Drag/drop snippets | Node access uses `get_node`, declarations use prefix types, constants and exported members have semicolons, and paths use double quotes and `NodePath` constructors. |
| Signal callbacks | The editor and language server generate callbacks through the path-aware language helper. |
| Search/assets/version control | CGD is in default search extensions, receives the script icon in asset installation, and opens as a script from version-control changes. |
| Debugger | CGD errors are recognized as script-language errors. Existing mapped line numbers and breakpoints remain in use. |
| Language server | Closed/open CGD documents are recognized; `cgd` language IDs and file-operation patterns are supported. Completion, symbol lookup and callback generation preserve the source path. |
| Resource/export paths | Native `.gdc` remains GD bytecode; `.cgd.gdbin` remains CGD bytecode. Directory listing restores the original CGD filename. |

Existing path-aware integration also provides highlighting, comment toggling, reindentation, function navigation, reload/save, translation string extraction and export. Generic script/resource features use the existing Script resource routes.

## Validation

- 15 CGD native cases, 326 assertions passed, including all ten templates parsed and analyzed.
- Combined final CGD and native GDScript unit run: 19 cases, 455 assertions passed.
- Existing GDScript source compilation/runtime, completion and language-server suites: 6 cases, 58,705 assertions passed.
- Portable frontend: 17,140 checks passed, including strict syntax and completion recovery.
- Actual editor plugin harness: autocomplete popup contains `position`; unfinished buffers do not crash; Open Script includes both extensions; a CGD script attaches to a Node3D; the plugin wizard generates a compiling CGD EditorPlugin.
- Creation harness: FileSystem creation menu, custom project template discovery, generated script save and runtime instantiation passed.
- TCP language-server harness: closed-file symbol discovery, `cgd` document registration, definition navigation and unfinished-statement completion passed.
- Engine smoke harness: reload, mixed GD/CGD inheritance and text/binary/compressed exported packs passed. Invalid CGD still fails export.
- Development editor and the user's Mono editor were rebuilt. Actual attachment/completion/plugin and language-server harnesses passed on the Mono executable.

The resource-picker follow-up reproduces the original raw-GDScript Inspector crash, which occurs when script editing reenters `EditorMainScreen::edit` and changes the selected plugin. Editor selection now checks that the selected plugin is still current after editor callbacks. The resource harness creates CGD scripts for Shader, ShaderMaterial and Resource, verifies attachment and `.tres` save/reload, and opens a legacy shader-owned built-in script to exercise reentrant editor selection. Run it with `run_resource_script_test.py --godot <editor executable>`.

## Deliberate boundaries

- CGD scripts remain external `.cgd` resources. Built-in scene scripts do not currently store a source-dialect identifier.
- Save As does not translate a script between GD and CGD syntax.
- A `.gdshader` file stores shader source only. Save a Shader resource as `.tres` or `.res` to persist its attached script and other Resource properties.
- Native `.gdc` handling, GD-only golden test inputs, and the Godot 3-to-4 source converter retain their native format rules.
- Folding uses the editor's existing indentation and region handling. Completion does not promise recovery for every possible malformed buffer.
- This is an audit and targeted regression coverage, not a claim that every editor operation or external LSP client has been exhaustively tested. Translation comments and hover documentation still use the existing frontend/native documentation behavior.

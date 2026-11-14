# GD-C integration

GD-C adds C-style syntax to GDScript using `.cgd` files. Source is transpiled in memory and uses the existing GDScript analyzer, compiler and VM. Explicit native declarations can also compile inline C on supported targets. See [syntax reference](CGD_SYNTAX.md).

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

## Inline C

A file-scope `native { ... }` block supplies C headers, macros and helpers to that script's generated C file. A `native int f() { ... }` declaration exposes a typed script method whose body is compiled as C17. Native parameters and returns support `int` (C `int64_t`), `float` (C `double`) and `bool`. Functions may also return `void`. The CGD declaration stays at script scope. C braces must balance across the source of each block, including conditional preprocessing branches.

Native parameters also support read-only `PackedByteArray` views. Use `static native` for a static script method. Native builds use `-O2` or `/O2`. The runtime checks the native ABI and engine precision before dispatch, so rebuild native exports and their templates together after updating.

Native call wrappers use interned library IDs, symbol names, and signatures. The runtime caches the resolved symbol and its UTF-8 signature after the first successful call. It checks signature identity and argument count on every call.

ExtractAndTranslate uses a static CGD native function for fingerprint comparisons on Windows and macOS. The project retains its GDExtension and script fallbacks for other runtimes and platforms. Its fingerprint parity test exercises the same native function used in production.

Run `python modules/gdscript/tests/cgd/tools/run_native_reload_test.py --godot <editor>` to exercise native reload through the editor debugger. The test saves a CGD-only edit and a C edit while the same game process is running, checks both results, and checks that the run cache contains one library identity for each C version.

Run preflight checks each compiler and header against its saved size and modification time. When either changes, it checks the content hash and refreshes the metadata if the contents still match. Configured library directories use their file listing and metadata. If an external tool replaces a file while preserving both size and modification time, clear the project's `.godot/cgd_native` cache to force a rebuild. Use `run_native_preflight_test.py --godot <editor>` in the CGD test tools to measure cold and warm preflight on five scripts.

```ini
[cgd_native]

include_dirs=PackedStringArray("res://native/include")
windows/x86_64/toolchain="clang-cl"
windows/x86_64/defines=PackedStringArray("WIN32_LEAN_AND_MEAN")
windows/x86_64/libraries=PackedStringArray("user32.lib")
```

Windows x86-64 defaults to `clang-cl` with the Visual C++ build tools and Windows SDK. Set `CGD_NATIVE_WINDOWS_X86_64_COMPILER` to override the clang-cl executable or set `windows/x86_64/compiler_path` to select it in the project. Otherwise the editor checks PATH and the Visual Studio installation. Set `windows/x86_64/toolchain="msvc"` to build with `cl.exe` and launch Godot from an x64 Visual Studio Developer Command Prompt so it inherits the compiler and SDK environment. `compiler_path` can select a different executable within that environment.

The source script's directory comes before configured include directories. With MSVC it also participates in angle-bracket include lookup. Common include directories precede target-specific directories. Target-specific defines override common defines by name. Targets accept `library_dirs` and `runtime_libraries` as packed string arrays.

Before Run Project or Run Current Scene, the editor checks the project's native CGD scripts and compiles stale libraries. The cache tracks generated C, the compiler, build settings, and headers reported by the compiler. CGD-only edits reuse the library, even when they move a native block. In a reused library, values derived from `__LINE__` reflect the source locations from its last compilation. A C build failure stops the launch. A game launched directly from source can build or reuse the cached library on the first native call. After a saved C edit is reloaded in an editor-launched game, the first call can compile the new library, which starts with fresh C static state. Relaunch after changing headers or build settings. Windows x86-64 export compiles the script DLLs and copies them and configured runtime DLLs into `cgd_native` beside the game executable. Use an export template built from this fork. The C compiler and SDK are build-machine dependencies.

For Android, export from a Windows editor with the Android NDK installed. Set `ANDROID_NDK_ROOT` to select an NDK explicitly. The exporter compiles a shared library for each enabled ABI and packages it with the APK. Use target keys such as `android/arm32/toolchain="ndk-clang"` for `armeabi-v7a`. Android native C bodies can use `cgd_android_env` and `cgd_android_activity` for JNI calls. These values are borrowed for the current native call, and the Activity may be null.

Linux defaults to Clang and also supports `linux/x86_64/toolchain="gcc"`. Export from a Linux editor of the same architecture as the target and keep the exported `cgd_native` directory beside the game. On macOS, install Xcode Command Line Tools and export from a macOS editor. The exporter places `.dylib` files in the app's `Contents/Frameworks` directory. Universal exports compile both x86-64 and ARM64 slices. Target settings use keys such as `linux/x86_64/compiler_path` and `macos/universal/libraries` under `[cgd_native]`.

Projects using ordinary `.gd` or `.cgd` scripts require no native build settings or C compiler. Export compiles scripts containing native C and ships the resulting libraries with the game.

For macOS arm64 export validation, package the matching arm64 release template without AppleDouble metadata (for example, use `ditto --norsrc -c -k --keepParent`). Enable ETC2/ASTC import in the test project. Verify the signed app with `codesign --verify --deep --strict` and run the exported executable to exercise the native library in `Contents/Frameworks`. The ExtractAndTranslate fingerprint parity pilot passed from source and from an ad hoc signed arm64 export.

Inside a native block or body, the script editor asks Clang for C17 completion using the unsaved script and the selected target's include directories and defines. The default is the editor's OS and architecture. Set `completion_target="android/arm32"` under `[cgd_native]` to edit Android code from a Windows editor with the NDK installed. A universal macOS profile uses the editor's architecture for completion.

When the caret is inside a C function call, the editor shows the prototype and highlights the active parameter. CGD highlighting colors the `native` keyword, preprocessor directives such as `#include` and `#import`, and angle-bracket header paths.

The editor checks native C in the background after the script validation delay. It shows Clang diagnostics from the script in the errors and warnings panels, and provides hover signatures for file-scope C symbols. Toggle background analysis in Editor Settings under **Text Editor → CGD → Live Native Diagnostics**. For an actual compile and link of the current buffer, use **Edit → Check Native C** (`Ctrl+Alt+F7`, or `Cmd+Option+F7` on macOS). This uses the selected target's build compiler and prints build failures to Output.

For GCC, the editor queries the selected compiler for its target triple, GNU version, and system header search paths. For MSVC, completion uses the developer prompt's `INCLUDE` order and Visual C++ version. GCC and MSVC builds can run with their respective compilers alone. C completion for either also needs Clang, with an optional `completion_compiler_path` under `[cgd_native]` to select the Clang executable. Compiler-specific extensions and predefined macros can differ from the completion frontend. Compiler diagnostics appear when running or exporting the project.

The initial bundle targets commit `95d622f2966ad862c8f7fe317267dff1f6a5413e`; all nine original file hashes also matched this checkout at `a34464c9a834975c7ab6dd60055498cafdf019e4`. The bundled implementation is MIT licensed; its notice is retained in `gdc/LICENSE`.

Analysis results are reused across CGD-only edits, including edits that move native bodies. The editor checks included headers when the application regains focus and refreshes analysis if they have changed. Diagnostics include Clang's notes and identify failures in generated native wrappers.

**Text Editor → CGD → Native Diagnostics Delay Ms** adds a 500 ms delay by default after script validation. C diagnostics show the line and column and underline the rest of the identifier at that position.

The **Check Native C** toolbar button checks the active native script. **Edit → Check All Native Scripts** builds saved native scripts in the background for the editor's Run target and reports failures in Output and in open script editors. The single-script check reports individual compiler errors at their source positions. Linker failures without positions are shown at the first native body.

## Editor feature audit

The follow-up audit covers explicit `.gd` checks and language-extension comparisons in the editor, resource system and GDScript language server. Details and validation are in [CGD_EDITOR_AUDIT.md](CGD_EDITOR_AUDIT.md).

## Validation on September 6, 2026

- Windows editor (`dev_build=yes`, `tests=yes`) and non-editor release template compiled successfully with the hcsr_newest renderer configuration.
- Final native run: 17 test cases, 753 assertions passed. This includes 12 GD-C cases, the existing GDScript native cases, and the complete source compilation/runtime suite.
- Portable frontend: 17,137 checks passed with Windows Clang, including strict-syntax rejection in normal and completion modes, inferred/dynamic declarations, typed lambdas, type checks/casts/membership, source fuzzing, random binary inputs, valid-envelope mutations and example translation.
- Real editor creation test: the GD-C FileSystem menu opened the dialog with the correct dialect, saved the Node template with C-style callbacks and no compatibility syntax, and the created script executed successfully.
- The optional existing GDScript binary-token suite reports seven diagnostic-output mismatches (match patterns, untyped declarations, an unexpected class-body identifier, and four empty-file warnings). The identical seven failures were reproduced by compiling and testing the original pre-integration parser. They are recorded rather than suppressed.
- Final deployed-release checks passed for text, binary and compressed packs, including mixed inheritance and same-stem scripts. Invalid GD-C source returns a failed export status.

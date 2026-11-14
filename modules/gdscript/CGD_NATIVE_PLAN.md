# CGD native: implementation plan (revision 2)

Audience: the implementer agent. Read the referenced code before changing it. Line numbers are approximate.

Revision 2 records what has landed, reviews it, lists the remaining work, and adds three new tracks:
- packed arrays and strings in the native ABI;
- one native library per export target instead of one per script;
- asynchronous completion with a precompiled preamble;
- C++ (and Objective-C++) bodies.

The goal for the new tracks: the native addons of the ExtractAndTranslate project (`C:\Repos\ExtractAndTranslate`) that are small algorithms or OS glue should be able to move into `.cgd` scripts. The large subsystems stay as GDExtensions. See "Target: ExtractAndTranslate" at the end.

Keep one commit per phase. Run the existing suites before and after each phase: `run_engine_tests.py --native-tests`, `run_editor_editing_test.py`, and the `test_core.cpp` doctest cases. Commands are in `CGD_INTEGRATION.md`.

---

## Status after the first implementation pass (verified against the code)

| Phase | Status | What is in the code | What is missing |
|---|---|---|---|
| 1. Faster calls | Done, small gain | Function pointer cache (`functions[p_id][p_symbol]`), 8-entry stack argument buffer. 1M calls: 808 ms → 639 ms (~0.64 µs per call). | See review item R1. Phase 1b below. |
| 2. C-only library ID | Implemented, not fully verified | Normalized `#line` numbers (`native_line_numbers`), ID placeholder patching (`native_library_id_offsets`), schema 2, `.cgd` dependency removed, the editor-launched game builds when the cache misses. | End-to-end test of live reload through the editor debugger. A test that goes through `GDCFrontend::transpile` (R7). |
| 3. Fast Run check | Partial | Stamps are shared across scripts in one Run (`p_run_stamps`, `build_mutex`). | Fast stamp (size + mtime) stored in the manifest, SHA-256 only on change. Timing test with 5 scripts. |
| 4. Background analysis | Partial | `GDCNative::request_analysis` → one `WorkerThreadPool` task at a time, latest request wins per editor. Two Clang runs: `-fsyntax-only` diagnostics plus file-scope completion for symbols. First-line column fix. | Result reuse (R2), generated-code diagnostics (R3), `note:` lines, header-change refresh. |
| 5. Live diagnostics | Partial | `native_errors`, `native_warnings` and `native_build_errors` in `ScriptTextEditor`, kept apart from GDScript validation. Setting `text_editor/cgd/live_native_diagnostics`. | `text_editor/cgd/native_diagnostics_delay_ms`, diagnostic ranges (R5), the R2 flicker. |
| 6. Hover prototypes | Done for file-scope symbols | `native_symbols` dictionary, `is_native_position`, tooltip through `EditorHelpBitTooltip`. | Overloads (needed for C++, Phase 11). Locals and qualified names (Phase 10). |
| 7. Check Native C | Partial | Edit menu item, `Ctrl+Alt+F7`, real toolchain build with `p_cache_for_run = true`, toast and Output. | Per-line diagnostic parsing (one blob on the first body line today), Check All, toolbar button. |
| 8. Docs | Partial | `CGD_INTEGRATION.md` covers what shipped. | Update as each phase lands. |
| 9. ABI (first milestone) | Partial, verified on Windows | `static native`; read-only `PackedByteArray` inputs that share Godot's storage; `-O2` / `/O2`; `cgd_native_abi_v2` ABI and precision check at load; compiler runs outside the build mutex (fixes R4). Pilot: 2,000 cases match; at 9,216 bytes, CGD 3.50–3.70 µs, GDExtension 2.92–3.18 µs, GDScript 293–298 µs. | Other packed types, `String` in/out, packed array returns, macOS validation. The ~0.6 µs gap to the GDExtension is the per-call cost (R1). |

---

## Review of the landed code

Fix these as part of finishing the phases. They are ordered by impact.

**R1. Each call is still expensive.** 0.64 µs per call is still several times a plain GDScript call. `invoke` (`gdc_native.cpp`, around line 1049) still does the following every time:
- `is_valid_hex_number` on the 64-character ID;
- `substr` + `is_valid_int` on the symbol;
- `functions[p_id]`, which hashes a 64-character string and *inserts* an entry when it's missing;
- `p_signature.utf8()`.

The GDScript side also builds an `Array` and makes a ClassDB call with 5 arguments. Phase 1b covers the fix. This matters less for ExtractAndTranslate, whose native calls happen once per frame or per OCR pass, than for code that calls native once per item.

**R2. Analysis is never reused.** `_validate_script` clears `native_errors`, `native_warnings` and `native_symbols` and requests a new analysis on *every* validation, including edits that only touch GDScript. The result:
- two Clang processes per idle timeout;
- C diagnostics flicker off and on;
- hover is unavailable until the new result arrives, because it requires `get_text() == native_analysis_code`.

Fix: cache results in `GDCNative`, keyed by `sha256(native source with real line numbers + frontend args)`, with the header stamps as dependencies. When the native source is unchanged, return the cached result synchronously and keep the old diagnostics on screen. If the native source changed only through `#line` numbers (GDScript lines added above), reuse the result and shift its lines. Track the per-body line offset to do this.

**R3. Errors in generated code are dropped silently.** Diagnostics in `<cgd-generated>` don't start with the script path, so the parser skips them. A user can see no live error while Check Native C fails. Map them to the native declaration's line using `native_bodies`, with the prefix "In generated wrapper for `name`:".

**R4. Check builds can block the editor.** *(Fixed in the Phase 9 milestone: the compiler now runs outside the shared build mutex.)* Check runs `build()` under `build_mutex` on a worker thread. A `@tool` script's first native call on the main thread waits on the same mutex for the whole compile. Either:
- build into a per-request folder without `build_mutex` and lock only to publish the cache entry; or
- at minimum, show "waiting for the native C check" instead of freezing silently.

**R5. Diagnostic ranges are one character wide.** `end_column = column + 1`. Use `-fdiagnostics-print-source-range-info` to get real ranges, or underline to the end of the token.

**R6. The editor core includes a module header.** `editor/script/script_text_editor.cpp` includes `modules/gdscript/gdc_native.h` and checks the `.cgd` extension directly. That's acceptable in a fork. If it causes merge pain, move it behind `EditorLanguage` virtuals (`request_native_analysis`, `lookup_native_symbol`, `check_native`). It's optional cleanup; do it before Phase 11 if you're already editing these files.

**R7. The ID test copies the normalization code.** `test_core.cpp` re-implements the normalization in a lambda. Add one test that calls `GDCFrontend::transpile` twice (a GDScript line added above versus not) and compares the returned `r_native_id` strings.

---

## Part A: finish the current phases

**A1 (Phase 2).** Add the editor-debugger live reload engine test:
1. Run.
2. Edit a GDScript line in a native script.
3. Let the editor push the reload.
4. Call the native function. It must succeed with no build.
5. Then edit the C, reload, and call again. It must build once (look for the "Compiling CGD native C after reload" line) and succeed.

Add the R7 test.

**A2 (Phase 3).** Store `{size, mtime, sha256}` per input in the manifest. Check size + mtime first and hash only on change. When the hash is unchanged, rewrite the manifest with the new fast stamp. Hash `library_dirs` by listing plus fast stamps. Add the 5-script timing test (`print_verbose` the duration). Target: milliseconds when nothing changed.

**A3 (Phase 4).**
- R2 result cache and line shifting.
- R3 generated-code diagnostics.
- Attach `note:` lines to the previous diagnostic.
- Header refresh: when an included header's fast stamp changes, either on editor focus-in or through `EditorFileSystem` change signals, drop the cached results that depend on it and re-request analysis for open editors.

**A4 (Phase 5).**
- Add `text_editor/cgd/native_diagnostics_delay_ms` (default 500), applied after the idle timer before calling `request_analysis`.
- R5 ranges.
- Engine editing test: an undeclared identifier shows up on the right line and column, and fixing it clears the error.

**A5 (Phase 7).**
- Parse the real compiler output per line, in the Clang/GCC `path:line:col: error:` format and the MSVC `path(line[,col]): error C1234:` format. Errors with no position (`LNK…`, `ld:`) go on the first native declaration line.
- Same `<cgd-generated>` handling as R3.
- Add **Check All Native Scripts**: the same as `prepare_editor_run` without launching, in the background, with results routed to open editors.
- Add a toolbar button that is visible only for `.cgd` scripts containing native code.
- Fix R4.

**A6 (Phase 8).** Update the docs as each item lands.

---

## Phase 1b: call cost (lower priority than Phases 9–11)

1. Move all validation of the ID, symbol and signature into the cache-miss path.
2. Make the cache key a single `uint64` (hash of ID + symbol) or a `StringName`, and look it up with `getptr`, never `operator[]`.
3. Cache the signature's `CharString` in the cache entry.
4. Measure again and report the call cost against an empty GDScript method call. If the gap is still over about 3x, change the generated wrapper:
   - a per-script `static var __cgd_handles: PackedInt64Array`, resolved lazily through `CGDNative.resolve(path, id, symbol, signature) -> int`;
   - then `CGDNative.call_handle(handle, [args])`, which skips all string handling.

   Update `test_core.cpp:50` accordingly.

---

## Phase 9: packed arrays and strings in the ABI (top new priority)

This unlocks moving hot loops into scripts (see the target section).

### Types and C mapping
| CGD parameter type | Signature letter | C type in the body |
|---|---|---|
| `PackedByteArray` | `B` | `CGDBytes { const uint8_t *data; int64_t size; }` |
| `PackedInt32Array` | `I` | `CGDInt32s { const int32_t *data; int64_t size; }` |
| `PackedInt64Array` | `L` | `CGDInt64s { const int64_t *data; int64_t size; }` |
| `PackedFloat32Array` | `F` | `CGDFloat32s { const float *data; int64_t size; }` |
| `PackedFloat64Array` | `D` | `CGDFloat64s { const double *data; int64_t size; }` |
| `PackedVector2Array` | `2` | `CGDVector2s { const cgd_real *data; int64_t size; }` (`size` counts vectors; data is x,y pairs) |
| `PackedVector3Array` | `3` | `CGDVector3s { const cgd_real *data; int64_t size; }` |
| `String` | `s` | `CGDString { const char *utf8; int64_t length; }` |

- `cgd_real` is `float`, or `double` when the engine is built with `precision=double`. Pass `-DCGD_REAL_IS_DOUBLE` from the build, and include it in the build fingerprint and the library ID inputs, since an export template with different precision must not load the library.
- Inputs are **read-only views**, valid only during the call. Document that native code must not keep the pointers.
- `invoke` takes the pointers from the Variants held in the argument `Array` (with `ptr()`), so there's no copy. For `String`, convert with `utf8()` into a local `CharString` array that stays alive for the duration of the call.

### Returns
- `String` return: the body returns `CGDString`. The pointer must stay valid until the thunk returns, for example a static or thread-local buffer. `invoke` copies it right away.
- Packed array returns, for any type in the table: the generated function receives a `CGDAllocator { void *ctx; void *(*alloc)(void *ctx, int64_t count); }`.
  - `invoke` implements `alloc` by resizing a Packed array of the declared type held in the result Variant and returning `ptrw()`.
  - The body writes into the buffer and returns. If it calls `alloc` again, the previous buffer is discarded.
  - In C, the body takes it as a final hidden parameter `CGDAllocator *cgd_out`, with the helper `cgd_out_alloc(cgd_out, count)`. Write the exact signature form into `CGD_SYNTAX.md`.

### Mechanics
- GDScript passes Packed arrays by value with copy-on-write, so there's no in-place output parameter: outputs come back through the return value only.
- Extend `NativeValue` and `CGDNativeValue` with `struct { const void *data; int64_t size; } span;`.
- Keep the signature string check in the generated function.
- Bump `NATIVE_BUILD_SCHEMA`.
- The type-name mapping in the transpiler (`native_type`, `native_field`) and the GDScript wrapper's parameter types must cover every new type.
- Tests:
  - doctest: generated C and wrapper for each type;
  - engine test: round-trip each type, including empty arrays and strings with non-ASCII text;
  - benchmark: the `fingerprint_changed` loop (two 64 KB `PackedByteArray`s) in CGD native, in GDScript, and through the existing GDExtension.

### Static native functions
The generated wrapper is always an instance method (`func name(...)`), so static code can't call native functions. ExtractAndTranslate calls its algorithms from static functions (for example `ParagraphDetector.detect`). Support `static native <type> name(...)`, generating `static func name(...)`.

---

## Phase 9b: one native library per export target

**Problem.** Each `.cgd` script with native code produces its own shared library, both in the editor and in exports. A project with many native scripts ships many DLLs or dylibs:
- clutter in `cgd_native/` (Windows, Linux) and `Contents/Frameworks` (macOS);
- one code signature per dylib on macOS;
- one antivirus scan per DLL on Windows;
- with C++ (Phase 11), one copy of the C++ standard library per library.

**Decision.** Keep **one library per script for editor builds** (Run cache, live reload, `@tool`, Check Native C), because that keeps rebuilds incremental and live reload possible. **Exports link every native script into one library per target.**

### Changes
1. **Generated names unique per project.**
   - Generated functions become `cgd_native_<id16>_<N>`, where `<id16>` is the first 16 hex characters of the script's library ID.
   - Use the same name in editor builds, so editor and export builds differ only in how they link.
   - Update the wrapper, the `invoke` symbol validation and `test_core.cpp`.
2. **Export pipeline** (`GDScriptExportPlugin`, `register_types.cpp`, around line 140):
   - Stop building one library per script.
   - Collect every native script for the target, then compile each one to an **object file** (`-c` / `/c`). Reuse a per-script object cache keyed like the Run cache (fingerprint + header dependencies), so re-exports only recompile changed scripts.
   - Link all objects into **one** library per target, `cgd_native.dll` / `libcgd_native.so` / `libcgd_native.dylib`, in `_export_end` or the equivalent hook after all files are processed. Link with the C++ driver if any script is C++ (Phase 11). Pass the `libraries`, `library_dirs` and `runtime_libraries` settings once.
   - Android: one `libcgd_native.so` per ABI.
   - macOS universal: link each slice, then combine with `lipo`. Or link with both `-arch` flags, as single-script builds already do.
   - Generate a small manifest (`cgd_native.json`, or a `PackedStringArray` in the exported project settings) listing the IDs included, so the game can report a clear error for a missing ID instead of "missing symbol".
3. **Runtime** (`invoke`): in a datapack (exported) run, open the single project library once and look up `cgd_native_<id16>_<N>` in it. Leave editor and source runs unchanged (one library per ID).
4. **Static helpers rule.**
   - Code in `native { }` blocks and helper functions written by the user must be `static` (or `static inline`). A non-static definition in two scripts would clash at link time. Even in one script, it would be visible to other scripts in the export but not in editor builds, so the two would behave differently.
   - Enforce this in the background analysis (Phase 4 service): after the syntax check, list external definitions from the native code and report each non-`static` function or variable *definition* as a C error on its line: "Native helpers must be static (scripts are linked together on export)."
     - With Clang, use `-Xclang -ast-dump=json -fsyntax-only` filtered to top-level `FunctionDecl`/`VarDecl` that have a body or initializer and `storageClass != static`. Or compile to an object and inspect it with `llvm-nm --defined-only --extern-only`, ignoring `cgd_native_*`. Choose whichever is cheaper. It can run only on save or Check.
     - Declarations (prototypes, `extern` without a definition) and C++ `inline`/template definitions are allowed.
   - **Check All Native Scripts** (A5) must also do the real merged link for the editor's platform, so a duplicate-symbol error shows up before export.
5. **Runtime libraries** (`runtime_libraries` setting) are still copied once next to the merged library.
6. Bump `NATIVE_BUILD_SCHEMA`. Update `CGD_INTEGRATION.md`: the static rule, the single exported library, the file names.

**Done when.**
- An export of a project with 3 native scripts (2 C, and after Phase 11, 1 C++) contains exactly one native library per target, and all three scripts' functions work in the exported game.
- Two scripts that each define a non-static `helper()` get an editor error on both lines, and the export fails with that message instead of a linker error.
- Re-exporting after changing one script recompiles only that script's object file (check `print_verbose`).

---

## Phase 10: asynchronous completion and a precompiled preamble

Prerequisite for C++, which parses headers far more slowly. Also worth doing for C with `windows.h`.

1. **Async completion.**
   - `GDCNative::complete_code` must not run Clang on the main thread.
   - On a request at a native position, return any cached result for `(native source hash, cursor offset)`. Otherwise, queue a background completion job and return no options.
   - When the job finishes, if the editor's text and caret are unchanged, call `CodeEdit::request_code_completion(true)` on that editor. The second call then finds the cached result.
   - Call hints (`OVERLOAD:`) work the same way.
2. **Position-specific symbol lookup.** For a hovered word that isn't in the file-scope table, such as a local, a parameter, a struct field, or a qualified name like `std::sort`:
   - queue a background completion at the *start* of the hovered identifier;
   - keep the entries whose name matches exactly;
   - show the tooltip if the mouse is still on that word when the result arrives (call `_show_symbol_tooltip` again), and cache the result.
   - Store the symbol table as `name → Array[String]` so it can hold overloads.
3. **Precompiled preamble.**
   - Treat the generated preamble plus every `native { }` declaration block as a header.
   - Build it with `-x c-header` (or `-x c++-header`) into `.godot/cgd_native/pch/<hash>.pch`, keyed by the header text and the frontend args.
   - Run analysis, completion and lookup on the function bodies only, with `-Xclang -include-pch -Xclang <file>`, which works under both the `clang` and `clang-cl` drivers.
   - Diagnostics from the PCH build are part of the analysis result.
   - Delete PCH files older than the current set on editor start.
   - Editor-only. Real builds don't use the PCH.
4. **One Clang call per analysis where possible.** Check whether `-code-completion-at` still prints every diagnostic. If it does, merge the two runs in `analyze()`.

**Done when.**
- A script including `windows.h` triggers no Clang process on the main thread while typing or hovering.
- Completion appears within one Clang run time after typing `.`, `->` or `(`.
- Re-analysis after a body edit is measurably faster with the PCH. Report both times.

---

## Phase 11: C++ (and Objective-C++ on macOS)

### Language choice (decided)
- **Each script chooses its language.** Plain `native` stays C. C++ is written:
  ```c
  native cpp {
  #include <vector>
  }
  native cpp int count_big(PackedInt32Array values) { ... }
  ```
  - `native objcpp { ... }` / `native objcpp <type> name(...) { ... }` is Objective-C++, for macOS-only scripts.
  - `cpp` and `objcpp` are contextual words: they only mean something right after `native`, followed by `{` or a type. They are not reserved anywhere else.
  - All native blocks in one script must use the same language. A script that mixes plain `native` and `native cpp` gets an error at the first mismatch: "All native code in a script must use the same language."
  - One C or C++ file per script stays the rule (Phase 9b links scripts of different languages together on export).
- **Optional project default:** `cgd_native/default_language` (`"c"`, `"cpp"`). When set to `"cpp"`, plain `native` means C++ in every script. An explicit `native cpp` / `native objcpp` always wins. This is a convenience only. Implement it last within this phase.
- The CGD highlighter colors `cpp` / `objcpp` like `native` when they follow it. Completion offers `cpp` and `objcpp` after `native `.
- Update `CGD_SYNTAX.md` with the forms and the mixing rule.

### Code generation
- Generated functions become `extern "C" CGD_EXPORT ... cgd_native_<id16>_<N>(...)` (names from Phase 9b).
- The preamble uses `thread_local` instead of `_Thread_local`. The C headers it includes are valid C++.
- Exceptions:
  - Each generated function wraps the body call in `try { ... } catch (const std::exception &e) { ... } catch (...) { ... }`.
  - It reports a status code instead of `bool`: `0` OK, `1` signature mismatch, `2` exception.
  - Add an exported `const char *cgd_native_last_error(void)` (thread-local message) so `invoke` can print `what()`.
  - This changes the ABI for C too, so bump the schema and apply it to both languages.
- Body tokenizer (the transpiler's brace balancing), C++ additions:
  - digit separators (`1'000'000` must not open a character literal);
  - raw strings `R"delim( ... )delim"` (including `u8R`, `LR` and similar prefixes), whose content is opaque;
  - `[[attributes]]` and `<:` digraphs need no special handling.
  - Add doctest cases for each.

### Build flags
| Toolchain | Flags |
|---|---|
| clang / gcc | `-x c++ -std=c++20`; link with the C++ driver (`clang++` / `g++`) **or** add the runtime explicitly. A plain `clang` driver doesn't link the C++ standard library. |
| Linux | `-static-libstdc++ -static-libgcc`, so the libraries don't depend on the target's libstdc++ version. |
| Android NDK | `-static-libstdc++`, which selects `c++_static`. |
| macOS | system `libc++` (default); `objcpp` adds `-x objective-c++ -fobjc-arc`. Frameworks come from the existing `libraries` setting (`-framework X`; extend the validation to allow that form). |
| clang-cl / MSVC | `/TP /std:c++20 /EHsc /Zc:__cplusplus`; `/MT` already links the runtime statically. |

- Add a C++ flag setting per target, `cgd_native/<platform>/<arch>/cxx_flags`, limited to an allowlist (`-std=`, `-O`, `-f[no-]exceptions`, `-f[no-]rtti`, `/std:`, `/O`). Keep rejecting arbitrary flags.
- `gcc_completion_flags` must query with `-x c++` for C++ scripts so it finds libstdc++'s include paths. Cache the C and C++ results separately.
- The language becomes part of the library ID inputs and the build fingerprint.

### Editor
- Add `:` to the `.cgd` completion triggers, firing only for `::`.
- In the completion output, skip `(Hidden)` entries. Keep namespaces as their own kind.
- Hover uses the overload list from Phase 10 and shows every overload.
- The highlighter needs no C++ keyword list for now.
- Diagnostics: C++ template errors produce long `note:` chains. Collapse them to the first line plus "(N notes)", with the full text in the errors panel.

**Done when.**
- A `native cpp` script using `<vector>`, `<algorithm>` and `std::string` builds and runs on Windows (clang-cl and MSVC) and Linux (clang and gcc), and exports for Windows x86_64 and macOS arm64.
- A thrown `std::runtime_error` reaches the Godot error log with its message, and the game keeps running.
- Live diagnostics, completion after `std::` and `.`, and hover on `std::sort` work without blocking the editor.

---

## Target: ExtractAndTranslate

The project's own native code is one GDExtension (`Addons/VisualNovelToolbarNative`, built by `Tools/Build/BuildWindowsNativeComponents.ps1` and `BuildMacosNativeComponents.sh`). The export presets are **Windows x86_64** and **macOS arm64**. The table maps each part to what it needs from this plan.

| Code | Size | Shape | Needs | Verdict |
|---|---|---|---|---|
| `VNToolbarNativeAlgorithms.fingerprint_changed` (`Addons/NativeAlgorithms`) | ~25 lines | 2× `PackedByteArray` + 2 ints → `bool`; called from `Controllers/OverlayRenderer.cgd` | Phase 9 | **Pilot.** Move into `OverlayRenderer.cgd` or a small `Services/native_algorithms.cgd`. |
| `DesktopLifecycle`, Windows side | ~220 lines | Win32 mutex/event handles, registry Run key, window visibility, GPU priority; String in and out | Phase 9 strings. Handles become C `static` state (one instance per process). | **Move** for Windows. |
| `DesktopLifecycle`, macOS side (`.mm`) | ~170 lines | Objective-C APIs | Phase 11 `objcpp` | Move after Phase 11, or keep in the GDExtension until then. |
| `detect_paragraphs` (`paragraph_detection.cpp`) | ~1,100 lines C++, about 14x faster than the script version | Takes `Array` of `Dictionary` (rects, text, per-character entries) and returns `Array[Dictionary]` | Phase 9 + Phase 11, plus **flattening**: the script packs detections into `PackedFloat32Array` (rects) + `PackedInt32Array` (offsets) + `String`/`PackedInt32Array` (codepoints); native returns `PackedInt32Array` group assignments; the script rebuilds the Dictionaries | Possible after Phase 11. Measure first that packing and unpacking in GDScript doesn't cancel the gain. |
| `ExtractMeCabReader` (`Addons/JapaneseReading`) | ~130 lines | Loads MeCab at run time; `analyze(String) -> Dictionary` of tokens | Phase 9 strings; return a flat TSV `String` or packed arrays and parse it in the script | Possible. Low priority: it already runs on a worker, and MeCab stays a separate library either way. |
| `ScreenCapture`, `DesktopAudioCapture`, `HTMLDesktopOverlay` | 2k–6k lines each | Own threads, GPU textures, render-thread callbacks, Godot objects and signals | Native code calling back into Godot (out of scope) | **Stay** as a GDExtension. |
| Inferbridge, models, llama.cpp, OCR workers | Large, third-party, CMake | Separate build systems and processes | (none) | **Stay.** |

### Gaps specific to this project
1. **Static native functions** (Phase 9). The algorithms are called from static functions.
2. **The `ClassDB.class_has_method` fallback pattern.** Scripts check whether the extension exists and fall back to GDScript. With CGD native, the native function always exists on supported targets. Keep the GDScript fallback only where a platform has no native build. Leave this to the project, not the engine.
3. **Moving GDExtension code into CGD changes the compiler.** GDExtension code is built by the project's CMake and `.ps1` scripts with their own flags. CGD native builds with the editor-resolved toolchain, through the `cgd_native/*` settings. The pilot must confirm that the same compiler settings (optimization level especially) are used. Today CGD native passes no `-O` flag, so it builds **unoptimized**. **Fix this in Phase 9:** default to `-O2` / `/O2` for export and Run builds, and add it to the fingerprint.
4. **The macOS export happens on a Mac** (`BuildMacosApplication.py`), which matches CGD native's rule that each OS builds its own target. No gap there, but the macOS editor needs Xcode Command Line Tools, and the Phase 11 `objcpp` path must be tested on that machine.
5. **Tests.** The project's `tests/integration/native_fingerprint_change_smoke.cgd` and `native_paragraph_detection_smoke.cgd` compare native and script results. Keep them as the acceptance tests for each migration, pointed at the CGD native version.

### Pilot (after Phase 9; validation for this plan, done in the ExtractAndTranslate repo)
1. Add `static native bool fingerprint_changed(PackedByteArray current, PackedByteArray previous, int threshold, int min_pixels)` to a project script, with the same loop as `native_algorithms.cpp`.
2. Run `native_fingerprint_change_smoke.cgd` against it.
3. Benchmark CGD native, GDScript and the GDExtension at the real fingerprint size. CGD native should be within noise of the GDExtension.
4. Only then remove `fingerprint_changed` from `VNToolbarNativeAlgorithms`.

---

## Order of work (final)

**Rule: finish what is started before starting anything new.** Work strictly in this order:
- A step is finished when all of its items are done, its "done when" checks pass on **Windows and macOS arm64**, `CGD_INTEGRATION.md` / `CGD_SYNTAX.md` describe it, and it is committed.
- Don't start the next step before that.
- If something blocks a step, stop and report it. Don't skip ahead.

### Stage 1: secure and finish the phases already started

| Step | Phase | Remaining work |
|---|---|---|
| 0 | (none) | **Commit what exists.** `gdc_native.cpp/.h` are untracked, and the tree also holds unrelated `html_css` and Android changes. Commit the CGD native work on its own, one commit per phase where practical. |
| 1 | Verify landed work | Run everything that has landed (Phases 1, 2, 6 and the Phase 9 first milestone) on **macOS arm64**: editor and template build, the byte-array smoke test and the ExtractAndTranslate pilot test from source and from a macOS export (dylib in `Contents/Frameworks`, signed). Fix what fails. |
| 2 | Phase 1 | Phase 1b steps 1–3 (R1): validation only on cache miss, a key that needs no 64-character hashing, cached signature `CharString`. Re-run the pilot benchmark; target CGD within noise of the GDExtension. |
| 3 | Phase 2 | A1: live reload through the editor debugger, end to end. R7: the ID test through `GDCFrontend::transpile`. |
| 4 | Phase 3 | A2: fast stamps (size + mtime) in the manifest, SHA-256 only on change, `library_dirs` by listing; the 5-script timing test. |
| 5 | Phase 4 | A3 + R2 + R3: result cache and line shifting (no flicker, hover always available), errors in generated code, `note:` lines, header-change refresh. |
| 6 | Phase 5 | A4 + R5: `native_diagnostics_delay_ms`, real diagnostic ranges, the editing engine test. |
| 7 | Phase 7 | A5: per-line parsing of real compiler output (Clang/GCC and MSVC formats, linker errors), **Check All Native Scripts**, toolbar button. |
| 8 | (project) | In ExtractAndTranslate: **switch the production `fingerprint_changed` path to CGD native** (Windows and macOS). Keep the GDExtension function one release as a fallback, then remove it. |

At the end of Stage 1, Phases 1–8 are complete and the pilot is in production.

### Stage 2: new capabilities

| Step | Phase | Work |
|---|---|---|
| 9 | Phase 9 | The remaining packed types (`Int32`, `Int64`, `Float32`, `Float64`, `Vector2`, `Vector3`), `String` in and out, packed array returns through `CGDAllocator`. Then port **`DesktopLifecycle` (Windows)** in ExtractAndTranslate. |
| 10 | Phase 9b | One native library per export target, generated names `cgd_native_<id16>_<N>`, the static-helper check, combined link in Check All. |
| 11 | Phase 10 | Async completion, position-specific lookup, precompiled preamble, merged Clang run. |
| 12 | Phase 11 | `native cpp` / `native objcpp`, then the `default_language` setting. Then port `DesktopLifecycle` (macOS, `objcpp`). Then evaluate `detect_paragraphs`: measure the GDScript cost of flattening detections into packed arrays first, and port only if the total stays well ahead of the script version. |
| 13 | Phase 1b step 4 | Handle-based wrapper, **only** if a real workload shows the per-call cost still matters. Otherwise skip. |

## Out of scope

- Native code calling back into Godot (objects, signals, the GDExtension interface). That's what GDExtension is for.
- Unloading or reloading native libraries inside the editor process.
- C#, web and iOS targets.

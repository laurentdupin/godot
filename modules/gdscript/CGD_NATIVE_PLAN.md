# CGD native: implementation plan (revision 5)

Audience: the implementer agent. Read the referenced code before changing it. Line numbers are approximate.

Revision 3 reviews Phases 9b–11 as landed, records the fixes made in `cc02edd6f3`, and adds **Phase 12: editor/export parity** (per-script symbol isolation on export, Run on the merged library, Run cache cleanup). See "Review of Phases 9b–11" and "Stage 3" in the order of work.

Revision 4 adds **Phase 13: printing to the Godot console from native code** (`cgd_print`, `cgd_print_warning`, `cgd_print_error`, and a C++ `cgd::print`). See "Stage 4" in the order of work.

Revision 5 adds **Phase 14: project-pinned C and C++ standards** (`cgd_native/c_standard`, `cgd_native/cpp_standard`), used identically by builds, exports, Run, diagnostics, completion and preambles. See "Stage 5" in the order of work.

Revision 2 recorded what had landed, reviewed it, listed the remaining work, and added these tracks:
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
| 9b. One library per export target | Landed (`3537ae60f6`, fixes in `cc02edd6f3`) | `cgd_native_<id16>_<N>` names, per-script object cache, merged `cgd_native.dll` / `libcgd_native.so` / `.dylib`, `lipo` for universal macOS, linker duplicate-symbol errors mapped to scripts, Check All links the merged library. | Name isolation and Run parity (Phase 12). Users still have to write `static` helpers. |
| 10. Async completion and preamble | Landed (`7d89d4e5f8`, `1601f95658`, `15e5101c72`, fixes in `cc02edd6f3`) | Completion and hover on a worker, PCH shared by completion, diagnostics and symbol indexing, overload lists for hover, PCH cache capped at 32 entries / 256 MiB (LRU) in an editor temp directory. | Nothing known. |
| 11. C++ / Objective-C++ | Landed (`b24672c301`) | `native cpp` / `native objcpp`, `cgd_native/default_language`, exceptions caught at the ABI boundary, `cxx_flags`. | Silent merging of same-named C++ inline functions across scripts on export (R15, Phase 12). |

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

## Review of Phases 9b–11 (revision 3)

### Fixed in `cc02edd6f3`
- **R8.** The Windows static-helper check ran `llvm-nm` and rejected symbols clang-cl emits on its own: `__real@…` (double constants), `??_C@…` (string literals), and UCRT inline functions such as `sprintf`. The nm pass is gone; duplicate definitions now come from the linker (`LNK2005`, `duplicate symbol`, `multiple definition`) and are mapped to the scripts.
- **R9.** `fail_export()` from `_export_finalize_files` was never checked, so an export with a failed native link still produced a package. `export_project_files` now returns the plugin's error after the finalize loop.
- **R10.** Precompiled preambles were pruned only when older than 7 days, so active editing could grow `.godot/cgd_native/pch` without bound. The cache now lives in an editor temp directory, keeps 32 entries / 256 MiB, and evicts least recently used first.
- **R11.** Export temp workspaces were held in `export_directories` until the editor quit. They are now released in `_export_end`.
- **R12.** The export object cache was keyed by the library ID, which ignores line numbers, so `__LINE__` and debug line info could be stale. The fingerprint now includes the hash of the generated source.
- **R13.** Two workers could delete a PCH the other was using. Preambles are now written under a pending name and renamed into place, and rejected preambles are no longer deleted from under another query.

### Open
**R14. The Run cache is never cleaned up.** `build()` publishes each Run build to `.godot/cgd_native/<platform>/<arch>/<id>/build_<pid>_<time>_<n>/` and points `current` at it. Nothing deletes older `build_*` folders, and every change to a script's native C creates a new `<id>` folder that is never removed. ExtractAndTranslate already has 4 `<id>` folders under `windows/x86_64`. Fix in Phase 12, item P4.

**R15. C++ inline functions with the same name are silently merged on export.** Class member functions defined in the class body, `inline` functions and template instantiations are mergeable (COMDAT/weak) symbols. If two scripts each define `struct Parser { int parse() { ... } };` with different bodies, the merged link succeeds with **no error** and both scripts call the same `parse`. In the editor, each script has its own library, so each gets its own `parse`. Reproduced with clang-cl at `/Od`: script B returned script A's value (1 instead of 2). At `/O2` the call is often inlined, so the bug depends on optimization. The linker can't report this; only isolation (Phase 12, P1) fixes it.

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
4. **Static helpers rule.** *(Superseded by Phase 12, P1: exports isolate each script's symbols, so `static` stops being required. Keep the linker duplicate-symbol mapping until P1 lands.)*
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

## Phase 12: editor/export parity

**Problem.** The editor builds one library per script; exports link every script into one library. Code that works in the editor can fail or misbehave in the export:

| Situation | Editor (one library per script) | Export (one library) today |
|---|---|---|
| Non-static helper or global with the same name in two scripts (`int counter;`, `int helper()`) | Each script has its own copy | Link error |
| Single-header library implemented in two scripts (stb, cJSON, miniaudio `*_IMPLEMENTATION`) | Works | Link error |
| Same-named C++ inline function or class member with different bodies (R15) | Each script has its own | **Silently merged**, wrong code runs |
| C runtime on Windows (`/MT`) | Each script has its own heap, `errno`, `rand()` state, stdio buffers, `atexit` list | One shared C runtime |
| `DllMain`, `__attribute__((constructor))`, C++ static initializers | Run when that script's library loads | One `DllMain` only; every script's initializers run on the first native call in the project |

**Goal.** Make the export behave like the editor for names, so users no longer need the `static` rule, and make Run use what is shipped so the remaining differences show up during development.

### P1. Isolate each script's symbols in the merged export
After compiling a script's object file, and before linking:
1. List the symbols the object **defines** (`llvm-nm --defined-only --extern-only --format=just-symbols`, or `nm` on Unix). Never rename undefined (referenced) symbols.
2. Rename every defined symbol except the script's `cgd_native_<id16>_*` exports and the object-format markers (`@feat.00`, `_fltused`, `__fltused`) to `<name>__cgd_<id16>`, with `llvm-objcopy --redefine-sym` (or `--redefine-syms <file>` for long lists). References inside the object are renamed with the definition.
3. Link as today. Store the renamed object in the export object cache (the rename list is derived from the object, so the existing fingerprint covers it).

Verified on Windows (clang-cl objects, `llvm-objcopy` from Visual Studio's LLVM component):
- two scripts each defining a non-static `int counter;` and `int helper()`: a plain link fails with "duplicate symbol: helper"; after renaming, the DLL links and each script's counter is independent (`[1, 2, 3]` vs `[1, 2]`);
- the R15 C++ case: after renaming `?parse@Parser@@QEAAHXZ` per script, script A returns 1 and script B returns 2.

`llvm-objcopy --localize-symbol` is **not** supported for COFF ("option is not supported for COFF"), which is why renaming is the method for every platform.

Tooling per target:
| Target | Tool | Notes |
|---|---|---|
| Windows clang-cl | `llvm-objcopy.exe` next to `clang-cl.exe` (VS "C++ Clang tools") | Verified. |
| Windows MSVC-only | none ships with MSVC | Require the VS LLVM component for exports with native code, or fall back to today's linker duplicate check with a clear message. Choose one and document it. |
| Android | NDK `llvm-objcopy` in the toolchain `bin` | Use the NDK's own tool, not the host's. |
| Linux | `llvm-objcopy` or binutils `objcopy` | Both support `--redefine-sym(s)`. |
| macOS | Xcode doesn't ship `llvm-objcopy` | Try `ld -r` per script with `-exported_symbols_list` (keeping only the exports) and confirm that the hidden symbols no longer collide in the final link. **Unverified**; if it doesn't work, require Homebrew LLVM's `llvm-objcopy`. |

Consequences:
- Renaming COMDAT symbols (string literals, inline functions, template instantiations such as `std::vector<int>` members) gives each script its own copy. That is correct but increases size; measure the merged library size on a project with several C++ scripts and report it.
- Symbols that the runtime finds **by name** stop working when renamed: `DllMain`, replacements of `operator new`/`operator delete`, `main`-style hooks. Detect them by name before renaming and report "`DllMain` is not supported in CGD native scripts" (and similar) as a build error on the native declaration line.
- Once P1 lands, remove the `static` requirement from `CGD_INTEGRATION.md`, keep the linker duplicate mapping only for the MSVC-only fallback (if chosen), and drop the related "done when" check in Phase 9b.

### P2. Run uses the merged library
- The game started by Run is a separate process, so it doesn't need one library per script. Build the merged library for Run from the cached objects (recompile changed scripts, relink), publish it in the Run cache, and have `invoke` load it in source runs as it does in datapack runs.
- Keep one library per script **only** for code that runs inside the editor process (`@tool` scripts, Check Native C on one script). Reloading in the editor process stays out of scope.
- Report the Run preflight time with one changed script out of 5 (`print_verbose`), next to the per-script build time it replaces.

### P3. Document what still differs
In `CGD_INTEGRATION.md`, state that:
- `DllMain` and runtime-replaced symbols are not supported (P1 enforces it);
- on Windows, all native scripts share one C runtime in the merged library (heap, `errno`, `rand()`, stdio, locale), so one script's runtime state is visible to the others;
- initializers (`__attribute__((constructor))`, C++ static constructors) of every script run when the first native function in the project is called.

### P4. Clean the Run cache (R14)
- When `build()` publishes a new `current` for an `<id>`, delete that `<id>`'s older `build_*` folders that no process has loaded. A DLL loaded by a running game is locked on Windows: skip folders that fail to delete and retry on the next publish.
- During the Run preflight and Check All, delete `<id>` folders under `.godot/cgd_native/<platform>/<arch>/` that no current project script produces. A missing entry is rebuilt on the next Run, so deletion is always safe.
- After P2, apply the same rule to the published merged libraries.

**Done when.**
- Two scripts with a non-static `helper()` and `counter` export and run, each with its own state, on Windows and macOS arm64.
- The R15 C++ case (`struct Parser` with different inline bodies in two scripts) returns each script's own value in the export, at `/Od` and `/O2`.
- A script defining `DllMain` gets a build error on its line.
- Run uses the merged library; the timing for one changed script is reported.
- After 10 edit-and-run cycles on one script, its `<id>` folders and `build_*` folders don't accumulate.
- `CGD_INTEGRATION.md` no longer asks for `static` helpers and lists the P3 differences.

---

## Phase 13: printing to the Godot console from native code

**Problem.** Native code can't print to Godot's console. `printf` goes to the process's stdout: it doesn't reach the editor's Output panel, the log file or the remote debugger, and on Windows each native library has its own C runtime buffers, so output appears late or not at all without `fflush`. Today the only way is to collect text in a buffer, return it as a `String`, and `print()` it from the script.

**Decision.** Add three logging functions as a deliberate, narrow exception to "native code doesn't call back into Godot". No objects, signals or other engine APIs.

### C API (printf-style)
Declared in the generated preamble for C, C++ and Objective-C++:

```c
void cgd_print(const char *format, ...);          // print_line
void cgd_print_warning(const char *format, ...);  // WARN_PRINT: yellow in Output, like push_warning
void cgd_print_error(const char *format, ...);    // ERR_PRINT: red in Output, like push_error
```

- C's `...` carries no types, so it can't concatenate arbitrary values like GDScript's `print(a, b, c)`. Use printf formats; this is the C convention and gives compile-time checks.
- Mark each with `__attribute__((format(printf, 1, 2)))` for Clang, GCC and clang-cl, and `_Printf_format_string_` (from `<sal.h>`) for MSVC. Live diagnostics then flag mismatched arguments (for example `%d` given a `double`) while typing.
- Implementation in the preamble (`static inline` or `static` helpers, so per-script isolation leaves them alone): `va_list` → `vsnprintf` into a 1 KiB stack buffer; if the result is longer, allocate exactly, format again, and free. On a negative `vsnprintf` result, print "(invalid format)". Text is UTF-8. A trailing newline is not required and one trailing `\n` is stripped, so `cgd_print("x\n")` and `cgd_print("x")` look the same.
- Thread safety: callable from any thread, including native worker threads. Godot's `print_line`, `WARN_PRINT` and `ERR_PRINT` are thread-safe.
- Warnings and errors should carry a useful location. Make `cgd_print_warning` / `cgd_print_error` macros that pass `__FILE__` and `__LINE__` to the host function. Because of the `#line` directives, those are the `.cgd` path and its line, so the Output entry points at the script.

### C++ API (`native cpp`)
Like GDScript's `print`, joining arguments of any printable type:

```cpp
cgd::print("found ", count, " paragraphs in ", elapsed, " ms");
cgd::print_warning(...);
cgd::print_error(...);
```

- A variadic template with a fold expression that formats each argument (`std::format("{}", value)`, or `std::to_chars` / an `ostringstream` if `<format>` is unavailable on a target's standard library) and calls the same host functions. `const char *`, `std::string`, `std::string_view`, integers, floating point and `bool` must work; anything with a `std::formatter` works too.
- Defined in the C++ preamble only.

### Host table and ABI
- The host functions reach native code through one table, set once per library at load, not passed on every call, so the call cost doesn't change:
  ```c
  typedef struct { uint32_t size; void (*print)(const char *utf8, int64_t length);
      void (*warning)(const char *utf8, int64_t length, const char *file, int32_t line);
      void (*error)(const char *utf8, int64_t length, const char *file, int32_t line); } CGDHost;
  ```
  `size` lets later versions add entries; native code must check it before using a new entry.
- `invoke` calls an exported `cgd_native_set_host(const CGDHost *)` right after the ABI check, before resolving any function. The table is a static in `gdc_native.cpp` and lives for the whole process.
- **Merged libraries (export and Run).** The pointer must exist once per library. Define `cgd_host` and `cgd_native_set_host` in the shared ABI object (`abi.c`, built from the preamble without `CGD_MERGED_EXPORT`). Script objects only declare `extern const CGDHost *cgd_host;`. Phase 12 P1 renames only *defined* symbols, so these references survive and resolve to the single copy. Don't define the pointer in each script: P1 would give every script its own copy and only one would be set.
- **Per-script libraries (editor process, debugger reload).** Each library defines `cgd_host` and `cgd_native_set_host` itself (the existing `#ifndef CGD_MERGED_EXPORT` block), and `invoke` sets it per library when loading.
- If the table hasn't been set (for example, a library loaded by an older engine), the functions fall back to `fputs` to `stderr` plus `fflush`, so output is never lost and nothing crashes.
- Bump `NATIVE_BUILD_SCHEMA` and the ABI symbol (`cgd_native_abi_v4`). The ABI check at load must reject libraries built without the host table.

### Editor
- Completion and hover show the three functions and `cgd::print` with their prototypes; they come from the preamble, so this should need no extra work. Verify it.
- `CGD_SYNTAX.md` and `CGD_INTEGRATION.md`: document the functions, the printf-style rule for C, the C++ form, thread safety, and that `printf` still goes to stdout only.

**Done when.**
- A C script calling `cgd_print("value=%d", 42)` shows `value=42` in the editor's Output panel during Run, in the exported game's log file, and from a `@tool` script inside the editor, on Windows and macOS arm64.
- `cgd_print_error` appears as an error in Output with the `.cgd` path and line; `cgd_print_warning` as a warning.
- A `native cpp` script calling `cgd::print("n=", 3, " x=", 1.5, " ", std::string("ok"))` prints `n=3 x=1.5 ok`.
- `cgd_print("%d", 1.5)` produces a live diagnostic warning on that line.
- Two scripts in one merged export both print, which proves the host pointer is shared and not renamed per script.
- Eight native worker threads printing 1,000 lines each don't crash or interleave inside one line.
- A message longer than 1 KiB prints in full.

---

## Phase 14: project-pinned C and C++ standards

**Problem.** The language standard isn't under the project's control:
- C is hard-coded to C17 in `language_flags` (`-std=c17` / `/std:c17`), with no setting.
- C++ defaults to C++20, and a standard can only be changed through the per-target `cxx_flags` allowlist (`add_cxx_flags`). Different targets of one project can therefore compile the same script under different standards, and the choice is buried among optimization flags.
- Editor completion and the GCC include query (`gcc_completion_flags`) use their own hard-coded `-std=c17` / `-std=c++20`, so they can disagree with the build.
- The Phase 13 C++ preamble includes `<source_location>` and `<format>`, which need C++20. The working tree's `cxx_flags` allowlist now also accepts `-std=c++17` / `/std:c++17`, which would make every C++ script fail to compile.

**Decision.** Two project-wide settings pin the standards. Every compiler invocation for that language uses exactly that standard: per-script builds, merged export and Run builds, live diagnostics, completion, hover lookup, precompiled preambles, and the GCC include query. Nothing else can set a standard.

### Settings
Register both in `GDCNative` (or the module's `register_types.cpp`) with `GLOBAL_DEF` and an enum property hint, so they appear in **Project Settings → Cgd Native** with a dropdown:

| Setting | Values | Default |
|---|---|---|
| `cgd_native/c_standard` | `c11`, `c17`, `c23`, `gnu11`, `gnu17`, `gnu23` | `c17` (today's behavior) |
| `cgd_native/cpp_standard` | `c++20`, `c++23`, `gnu++20`, `gnu++23` | `c++20` (today's behavior) |

- C++17 is not offered: the generated C++ preamble needs C++20 (`<source_location>`, `<format>`, CTAD for `cgd::print_warning`/`print_error`). Remove `-std=c++17`, `-std=gnu++17` and `/std:c++17` from the working tree's allowlist. If C++17 is ever wanted, the preamble must first guard those parts with `__has_include` / `__cpp_lib_*` and fall back to `__FILE__`/`__LINE__` macros.
- Objective-C++ uses `cpp_standard`. Plain Objective-C isn't a CGD language.
- Values are lowercase strings; reject anything else with "Unsupported cgd_native/c_standard value: …".

### Flag mapping
One function, `standard_flags(target, language) -> List<String>` (or an error), used everywhere `language_flags` adds a standard today:

| Setting | clang / gcc | clang-cl | MSVC (`cl.exe`) |
|---|---|---|---|
| `c11` / `c17` | `-std=c11` / `-std=c17` | `/std:c11` / `/std:c17` | `/std:c11` / `/std:c17` |
| `c23` | `-std=c23` (older Clang/GCC: `-std=c2x`) | `/clang:-std=c23` | `/std:clatest` |
| `gnu11` / `gnu17` / `gnu23` | `-std=gnu11` / `-std=gnu17` / `-std=gnu23` | `/clang:-std=gnu11` … | **error**: "GNU C dialects need Clang or GCC; MSVC doesn't support them." |
| `c++20` | `-std=c++20` | `/std:c++20` | `/std:c++20` |
| `c++23` | `-std=c++23` (older: `-std=c++2b`) | `/std:c++latest` (or `/clang:-std=c++23`) | `/std:c++latest` |
| `gnu++20` / `gnu++23` | `-std=gnu++20` / `-std=gnu++23` | `/clang:-std=gnu++20` … | **error**, as for GNU C |

- Probe once per compiler (cache by compiler path and stamp): compile an empty file with the mapped flag. If the compiler rejects it, fail with "This compiler doesn't support <standard>: <compiler>" instead of a raw compiler error. Use the fallback spellings (`c2x`, `c++2b`) only when the probe says the modern spelling is unknown.
- Keep `/Zc:__cplusplus` and `/EHsc` for C++ on the MSVC-style toolchains.

### Removing the old paths
- Remove standard flags from the `cxx_flags` allowlist. If a project still lists one, fail with "Set the C++ standard with cgd_native/cpp_standard instead of cxx_flags" and the offending entry. `cxx_flags` keeps only optimization and RTTI flags.
- Replace the hard-coded standards in `language_flags`, `gcc_completion_flags`, the analysis and completion arguments, and the preamble builds with `standard_flags`.

### Caches and identities
- The standard is already part of the compiler arguments, so it enters the per-script build fingerprint, the export object fingerprint, the merged Run link fingerprint and the PCH key automatically. Verify each one, and that changing the setting rebuilds everything on the next Run without a manual cache clear.
- The library ID doesn't need to change: it identifies the source, and the fingerprints handle rebuilding.

### Editor
- Live diagnostics, completion and hover must use the pinned standard, so a C23 or C++23 feature shows no false error when the setting allows it, and shows a real error when it doesn't.
- Changing either setting re-requests analysis for open native scripts (listen to `ProjectSettings::settings_changed`).

### Docs
`CGD_INTEGRATION.md` and `CGD_SYNTAX.md`: replace "compiled as C17" / "C++20" with the settings and their defaults, list the MSVC limits, and remove the standard flags from the `cxx_flags` description.

**Done when.**
- With `c_standard="c23"`, a C script using `constexpr`, `nullptr`, `bool` without `<stdbool.h>` and `typeof` builds, runs and exports on Windows (clang-cl), macOS arm64 and Android, and shows no live diagnostic. With `c17`, the same script shows the errors live and fails to build.
- With `cpp_standard="c++23"`, a script using `std::expected` and `if consteval` builds on Windows and macOS. With `c++20`, it fails with a clear error.
- `gnu17` with `toolchain="msvc"` fails with the MSVC message, before any compile.
- A `cxx_flags` entry of `-std=c++23` fails with the "use cgd_native/cpp_standard" message.
- Changing a setting and pressing Run rebuilds the affected objects and relinks (check `print_verbose`); pressing Run again reuses them.
- Both settings appear in Project Settings with dropdowns and their defaults.

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

### Stage 3: editor/export parity (revision 3)

| Step | Phase | Work |
|---|---|---|
| 14 | Phase 12 P4 | Run cache cleanup (R14). Small and independent; do it first. |
| 15 | Phase 12 P1 + P3 | Per-script symbol isolation on export, `DllMain` / runtime-symbol check, documentation of the remaining differences. Fixes R15, which can produce wrong code today in any project with two C++ scripts. Decide the MSVC-only fallback and verify the macOS method before writing the code. |
| 16 | Phase 12 P2 | Run on the merged library, with the timing report. |

### Stage 4: native logging (revision 4)

| Step | Phase | Work |
|---|---|---|
| 17 | Phase 13 | `cgd_print`, `cgd_print_warning`, `cgd_print_error`, the host table set at load (shared ABI object in merged libraries), `cgd::print` for C++, docs. Then replace ad hoc native logging in ExtractAndTranslate (returned log strings, `fprintf(stderr, ...)`) where it exists. |

### Stage 5: language standards (revision 5)

| Step | Phase | Work |
|---|---|---|
| 18 | Phase 14 | `cgd_native/c_standard` and `cgd_native/cpp_standard` with dropdowns, one flag mapping for every toolchain and every compiler invocation (build, export, Run, diagnostics, completion, preambles, GCC include query), standard flags removed from `cxx_flags`, re-analysis on change, docs. Finish and commit Phase 13 first; drop the C++17 allowlist entries added during it. |

## Out of scope

- Native code calling back into Godot (objects, signals, the GDExtension interface). That's what GDExtension is for. The only exception is the Phase 13 logging functions.
- Unloading or reloading native libraries inside the editor process.
- C#, web and iOS targets.


### Step 12 workload evaluation

The macOS lifecycle port passed the single-instance and activation smoke tests, including a separate duplicate process and a signed arm64 app export. The Windows lifecycle smoke still passes.

The paragraph evaluation used 300 synthetic OCR detections with per-character rectangles. On macOS arm64 the existing native detector took about 8.0 ms, the CGD script fallback took 153 ms, and flattening inputs and rebuilding mock groups took 3.4 ms. The prototype did not run the grouping algorithm through CGD or reconstruct full paragraph metadata. The measured bridge cost does not establish a production speedup over the existing native detector, so the detector remains in the extension.

The evaluated production workloads call native once per capture or OCR batch, or on lifecycle events. No measured workload justified a handle-based wrapper in Step 13.


### Step 16 Run parity validation

The five-script editor Run test measured 1122 ms cold, 723 ms with one changed script, and 604 ms with all objects cached on Windows. On macOS arm64 it measured 473 ms, 173 ms, and 93 ms respectively. The incremental run recompiled one object and reused four. The warm run reused all five objects and relinked. The game returned the expected native values on all three launches, and the debugger reload test passed on both platforms.

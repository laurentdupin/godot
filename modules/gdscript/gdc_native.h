// SPDX-License-Identifier: MIT
#ifndef GDC_NATIVE_H
#define GDC_NATIVE_H

#include "core/io/dir_access.h"
#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/variant/array.h"

#ifdef TOOLS_ENABLED
#include "core/object/script_language.h"
#include "core/object/worker_thread_pool.h"
#endif

class GDCNative : public Object {
	GDCLASS(GDCNative, Object);

	static GDCNative *singleton;
	Mutex mutex;
	Mutex build_mutex;
	HashMap<String, void *> libraries;
	struct NativeFunctionEntry {
		void *address = nullptr;
		void *error_getter = nullptr;
		StringName signature;
		CharString signature_utf8;
	};
	HashMap<StringName, HashMap<StringName, NativeFunctionEntry>> functions;
	Ref<DirAccess> build_dir;
#ifdef TOOLS_ENABLED
	WorkerThreadPool::TaskID analysis_task = WorkerThreadPool::INVALID_TASK_ID;
	struct AnalysisRequest {
		String path;
		String code;
		Callable callback;
		bool check_build = false;
		PackedStringArray project_paths;
	};
	HashMap<ObjectID, AnalysisRequest> pending_analysis;
	HashMap<ObjectID, AnalysisRequest> pending_build_checks;
	struct AnalysisResult {
		String key;
		Array diagnostics;
		Dictionary symbols;
		Dictionary headers;
		Vector<int> body_lines;
		Vector<int> body_columns;
	};
	// Accessed only by the serialized analysis worker.
	HashMap<String, AnalysisResult> analysis_cache;
	void analyze(AnalysisRequest p_request);
	void start_analysis();
	void finish_analysis(const String &p_path, const String &p_code, const Callable &p_callback, const Array &p_diagnostics, const Dictionary &p_symbols);
	struct CompletionRequest {
		ObjectID editor;
		String path;
		String code;
		Callable callback;
		String symbol;
	};
	struct CompletionResult {
		String path;
		String code;
		List<ScriptLanguage::CodeCompletionOption> options;
		String hint;
		String symbol;
		String prototype;
	};
	WorkerThreadPool::TaskID completion_task = WorkerThreadPool::INVALID_TASK_ID;
	HashMap<ObjectID, CompletionRequest> pending_completions;
	uint64_t completion_revision = 0;
	uint64_t active_completion_revision = 0;
	CompletionResult completion_result;
	CompletionResult worker_completion_result;
	void start_completion();
	void compute_completion(CompletionRequest p_request);
	void finish_completion(const Callable &p_callback);
#endif

	Variant invoke(const String &p_path, const StringName &p_id, const StringName &p_symbol, const StringName &p_signature, const Array &p_args);

protected:
	static void _bind_methods();

public:
	static bool supports_source(const String &p_source, const String &p_platform);
	struct NativeUnit {
		String path;
		String id;
		String source;
	};
	static GDCNative *get_singleton() { return singleton; }
	static PackedStringArray runtime_libraries(const String &p_platform, const String &p_arch);
	Error build(const String &p_path, const String &p_id, const String &p_native_source, String &r_library, String &r_error, const String &p_platform, const String &p_arch, bool p_cache_for_run = false, HashMap<String, Dictionary> *p_run_stamps = nullptr);
	struct MergedRunCache {
		String directory;
		Dictionary previous;
		Dictionary inputs;
		String fingerprint;
		bool hit = false;
	};

	Error build_export(const Vector<NativeUnit> &p_units, const String &p_platform, const String &p_arch, String &r_library, String &r_error, Vector<Ref<DirAccess>> &r_workspaces, MergedRunCache *r_run_cache = nullptr);
#ifdef TOOLS_ENABLED
	static bool prepare_editor_run();
	void invalidate_native_completion();
	void request_analysis(ObjectID p_editor, const String &p_path, const String &p_code, const Callable &p_callback, bool p_check_build = false);
	void request_project_check(ObjectID p_editor, const Callable &p_callback);
	static bool is_native_position(const String &p_code, const String &p_path, int p_row, int p_column);
	static bool complete_code(const String &p_code, const String &p_path, List<ScriptLanguage::CodeCompletionOption> &r_options, String &r_call_hint, const String &p_symbol = String(), String *r_prototype = nullptr);
	bool complete_in_editor(ObjectID p_editor, const String &p_code, const String &p_path, const Callable &p_callback, List<ScriptLanguage::CodeCompletionOption> &r_options, String &r_hint);
	void request_native_hover(ObjectID p_editor, const String &p_code, const String &p_path, const String &p_symbol, const Callable &p_callback);
#endif

	GDCNative();
	~GDCNative();
};

#endif // GDC_NATIVE_H

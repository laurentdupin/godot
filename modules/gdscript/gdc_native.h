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
		StringName signature;
		CharString signature_utf8;
	};
	HashMap<StringName, HashMap<StringName, NativeFunctionEntry>> functions;
	Ref<DirAccess> build_dir;
	Vector<Ref<DirAccess>> export_directories;
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
#endif

	Variant invoke(const String &p_path, const StringName &p_id, const StringName &p_symbol, const StringName &p_signature, const Array &p_args);

protected:
	static void _bind_methods();

public:
	struct NativeUnit {
		String path;
		String id;
		String source;
	};
	static GDCNative *get_singleton() { return singleton; }
	static PackedStringArray runtime_libraries(const String &p_platform, const String &p_arch);
	Error build(const String &p_path, const String &p_id, const String &p_native_source, String &r_library, String &r_error, const String &p_platform, const String &p_arch, bool p_cache_for_run = false, HashMap<String, Dictionary> *p_run_stamps = nullptr);
	Error build_export(const Vector<NativeUnit> &p_units, const String &p_platform, const String &p_arch, String &r_library, String &r_error);
#ifdef TOOLS_ENABLED
	static bool prepare_editor_run();
	void request_analysis(ObjectID p_editor, const String &p_path, const String &p_code, const Callable &p_callback, bool p_check_build = false);
	void request_project_check(ObjectID p_editor, const Callable &p_callback);
	static bool is_native_position(const String &p_code, const String &p_path, int p_row, int p_column);
	static bool complete_code(const String &p_code, const String &p_path, List<ScriptLanguage::CodeCompletionOption> &r_options, String &r_call_hint);
#endif

	GDCNative();
	~GDCNative();
};

#endif // GDC_NATIVE_H

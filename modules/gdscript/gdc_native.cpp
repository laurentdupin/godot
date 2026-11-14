// SPDX-License-Identifier: MIT
#include "gdc_native.h"

#include "gdc_frontend.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/string/char_utils.h"
#include "core/string/regex.h"
#include "core/templates/local_vector.h"

#ifdef ANDROID_ENABLED
#include "platform/android/java_godot_wrapper.h"
#include "platform/android/os_android.h"
#include "platform/android/thread_jandroid.h"
#endif

#ifdef TOOLS_ENABLED
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"
#endif

GDCNative *GDCNative::singleton = nullptr;

namespace {

constexpr int NATIVE_BUILD_SCHEMA = 6;

union NativeValue {
	int64_t integer;
	double floating;
	bool boolean;
	struct {
		const void *data;
		int64_t size;
	} span;
};

constexpr uint32_t NATIVE_ABI_LAYOUT = (sizeof(real_t) * 8 << 16) | (sizeof(NativeValue) << 8) | alignof(NativeValue);

struct NativeAllocator {
	void *context;
	void *(*allocate)(void *, int64_t);
};

struct NativeOutput {
	char type;
	Variant array;
	PackedByteArray utf8;
	bool failed = false;
};

template <class T>
void *allocate_native_array(Variant &r_array, int64_t p_count) {
	T array;
	if (array.resize(p_count) != OK) {
		return nullptr;
	}
	void *data = p_count > 0 ? array.ptrw() : nullptr;
	r_array = array;
	return data;
}

static void *allocate_native_output(void *p_context, int64_t p_count) {
	auto &output = *static_cast<NativeOutput *>(p_context);
	if (output.failed || p_count < 0 || p_count > INT32_MAX) {
		output.failed = true;
		return nullptr;
	}
	void *data = nullptr;
	switch (output.type) {
		case 's':
			if (output.utf8.resize(p_count) != OK) {
				output.failed = true;
				return nullptr;
			}
			data = p_count ? output.utf8.ptrw() : nullptr;
			break;
		case 'B':
			data = allocate_native_array<PackedByteArray>(output.array, p_count);
			break;
		case 'I':
			data = allocate_native_array<PackedInt32Array>(output.array, p_count);
			break;
		case 'L':
			data = allocate_native_array<PackedInt64Array>(output.array, p_count);
			break;
		case 'F':
			data = allocate_native_array<PackedFloat32Array>(output.array, p_count);
			break;
		case 'D':
			data = allocate_native_array<PackedFloat64Array>(output.array, p_count);
			break;
		case '2':
			data = allocate_native_array<PackedVector2Array>(output.array, p_count);
			break;
		case '3':
			data = allocate_native_array<PackedVector3Array>(output.array, p_count);
			break;
	}
	output.failed = p_count > 0 && data == nullptr;
	return data;
}

static_assert(sizeof(Vector2) == sizeof(real_t) * 2 && alignof(Vector2) == alignof(real_t));
static_assert(sizeof(Vector3) == sizeof(real_t) * 3 && alignof(Vector3) == alignof(real_t));

String library_filename(const String &p_platform, const String &p_id) {
	if (p_platform == "windows") {
		return p_id + ".dll";
	}
	return "libcgd_" + p_id + (p_platform == "macos" ? ".dylib" : ".so");
}

Variant setting(const String &p_name) {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	return settings->has_setting(p_name) ? settings->get_setting(p_name) : Variant();
}

String target_prefix(const String &p_platform, const String &p_arch) {
	return "cgd_native/" + p_platform + "/" + p_arch + "/";
}

String run_cache_path(const String &p_platform, const String &p_arch, const String &p_id) {
	return ProjectSettings::get_singleton()->get_project_data_path().path_join("cgd_native").path_join(p_platform).path_join(p_arch).path_join(p_id);
}

Dictionary read_run_cache(const String &p_directory) {
	Ref<FileAccess> manifest = FileAccess::open(p_directory.path_join("current"), FileAccess::READ);
	if (manifest.is_valid()) {
		const Variant data = manifest->get_var();
		if (data.get_type() == Variant::DICTIONARY) {
			return data;
		}
	}
	return Dictionary();
}

String cached_library(const String &p_directory, const Dictionary &p_manifest, const String &p_filename) {
	const String generation = p_manifest.get("generation", String());
	if (p_manifest.get("schema", 0) != Variant(NATIVE_BUILD_SCHEMA) || generation.is_empty() || !generation.is_valid_ascii_identifier()) {
		return String();
	}
	const String path = ProjectSettings::get_singleton()->globalize_path(p_directory.path_join(generation).path_join(p_filename));
	return FileAccess::exists(path) ? path : String();
}

PackedStringArray target_array(const String &p_name, const String &p_platform, const String &p_arch) {
	return setting(target_prefix(p_platform, p_arch) + p_name);
}

#ifdef TOOLS_ENABLED
// Clang's make depfile escapes whitespace and uses backslash-newline for
// continuation. Windows depfiles also use literal backslashes in paths.
PackedStringArray compiler_dependencies(const String &p_depfile) {
	String contents = FileAccess::get_file_as_string(p_depfile).replace("\\\r\n", " ").replace("\\\n", " ");
	const int separator = contents.find(": ");
	PackedStringArray paths;
	if (separator < 0) {
		return paths;
	}
	String path;
	for (int i = separator + 2; i < contents.length(); i++) {
		const char32_t c = contents[i];
		if (c == '\\' && i + 1 < contents.length() && (contents[i + 1] == ' ' || contents[i + 1] == '#' || contents[i + 1] == '\t')) {
			path += contents[++i];
		} else if (c == '$' && i + 1 < contents.length() && contents[i + 1] == '$') {
			path += contents[++i];
		} else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			if (!path.is_empty()) {
				paths.push_back(path.replace_char('\\', '/'));
				path = String();
			}
		} else {
			path += c;
		}
	}
	if (!path.is_empty()) {
		paths.push_back(path.replace_char('\\', '/'));
	}
	return paths;
}

PackedStringArray msvc_dependencies(const String &p_depfile) {
	const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(p_depfile));
	if (parsed.get_type() != Variant::DICTIONARY) {
		return PackedStringArray();
	}
	const Variant data = Dictionary(parsed).get("Data", Variant());
	if (data.get_type() != Variant::DICTIONARY) {
		return PackedStringArray();
	}
	return Dictionary(data).get("Includes", Array());
}

Dictionary input_stamp(const String &p_path, const Dictionary &p_previous, HashMap<String, Dictionary> *p_run_stamps) {
	const uint64_t size = FileAccess::get_size(p_path);
	const uint64_t modified = FileAccess::get_modified_time(p_path);
	if (uint64_t(p_previous.get("size", -1)) == size && uint64_t(p_previous.get("mtime", -1)) == modified) {
		if (p_run_stamps) {
			p_run_stamps->insert(p_path, p_previous);
		}
		return p_previous;
	}
	if (p_run_stamps) {
		if (const Dictionary *cached = p_run_stamps->getptr(p_path)) {
			if (uint64_t(cached->get("size", -1)) == size && uint64_t(cached->get("mtime", -1)) == modified) {
				return *cached;
			}
		}
	}
	Dictionary stamp;
	stamp["size"] = size;
	stamp["mtime"] = modified;
	stamp["sha256"] = FileAccess::get_sha256(p_path);
	if (p_run_stamps) {
		p_run_stamps->insert(p_path, stamp);
	}
	return stamp;
}

Error prepare_directory(EditorFileSystemDirectory *p_directory, const String &p_platform, const String &p_arch, String &r_error, HashMap<String, Dictionary> &r_stamps) {
	for (int i = 0; i < p_directory->get_file_count(); i++) {
		const String path = p_directory->get_file_path(i);
		if (!GDCFrontend::is_source_path(path)) {
			continue;
		}
		const String source = FileAccess::get_file_as_string(path);
		if (!source.contains("native")) {
			continue;
		}
		String generated;
		String native_source;
		String native_id;
		GDC::SourceMap map;
		int line = 1;
		int column = 1;
		if (GDCFrontend::transpile(source, generated, map, r_error, line, column, false, { 0, 0 }, path, &native_source, nullptr, &native_id) != OK) {
			// The normal parser owns CGD syntax errors. Only build valid native units.
			r_error = String();
			continue;
		}
		if (!native_source.is_empty()) {
			String library;
			const Error error = GDCNative::get_singleton()->build(path, native_id, native_source, library, r_error, p_platform, p_arch, true, &r_stamps);
			if (error != OK) {
				return error;
			}
		}
	}
	for (int i = 0; i < p_directory->get_subdir_count(); i++) {
		const Error error = prepare_directory(p_directory->get_subdir(i), p_platform, p_arch, r_error, r_stamps);
		if (error != OK) {
			return error;
		}
	}
	return OK;
}

String native_path(const String &p_path) {
	return ProjectSettings::get_singleton()->globalize_path(p_path);
}

String find_on_path(const String &p_name) {
	OS *os = OS::get_singleton();
	const String separator = os->get_name() == "Windows" ? ";" : ":";
	for (const String &directory : os->get_environment("PATH").split(separator, false)) {
		const String path = directory.trim_prefix("\"").trim_suffix("\"").path_join(p_name).replace_char('\\', '/');
		if (FileAccess::exists(path)) {
			return path;
		}
	}
	return String();
}

#ifdef WINDOWS_ENABLED
String find_clang_cl() {
	OS *os = OS::get_singleton();
	const String compiler = find_on_path("clang-cl.exe");
	if (!compiler.is_empty()) {
		return compiler;
	}
	const String vswhere = os->get_environment("ProgramFiles(x86)").replace_char('\\', '/').path_join("Microsoft Visual Studio/Installer/vswhere.exe");
	String install_path;
	int code = -1;
	if (FileAccess::exists(vswhere) && os->execute(vswhere, { "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath", "-utf8" }, &install_path, &code) == OK && code == 0) {
		return install_path.strip_edges().replace_char('\\', '/').path_join("VC/Tools/Llvm/x64/bin/clang-cl.exe");
	}
	return String();
}

String find_android_clang() {
	OS *os = OS::get_singleton();
	String ndk = os->get_environment("ANDROID_NDK_ROOT").replace_char('\\', '/');
	if (ndk.is_empty()) {
		String sdk = os->get_environment("ANDROID_HOME");
		if (sdk.is_empty()) {
			sdk = EDITOR_GET("export/android/android_sdk_path");
		}
		if (sdk.is_empty()) {
			return String();
		}
		const String ndks = sdk.replace_char('\\', '/').path_join("ndk");
		const PackedStringArray versions = DirAccess::get_directories_at(ndks);
		if (versions.is_empty()) {
			return String();
		}
		String latest = versions[0];
		for (const String &version : versions) {
			if (version.naturalnocasecmp_to(latest) > 0) {
				latest = version;
			}
		}
		ndk = ndks.path_join(latest);
	}
	return ndk.path_join("toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe");
}
#endif

struct NativeTarget {
	String platform;
	String arch;
	String toolchain;
	String compiler;
	String triple;
	bool uses_msvc_flags() const { return toolchain == "msvc" || toolchain == "clang-cl"; }
};

Error resolve_target(const String &p_platform, const String &p_arch, NativeTarget &r_target, String &r_error) {
	OS *os = OS::get_singleton();
	const String host = os->get_name().to_lower();
	const bool windows = p_platform == "windows";
	const bool android = p_platform == "android";
	const bool linux_target = p_platform == "linux";
	const bool macos = p_platform == "macos";
	if ((android ? host != "windows" : host != p_platform) ||
			(windows && p_arch != "x86_64") ||
			(linux_target && p_arch != Engine::get_singleton()->get_architecture_name()) ||
			(macos && p_arch != "x86_64" && p_arch != "arm64" && p_arch != "universal") ||
			(!windows && !android && !linux_target && !macos)) {
		r_error = vformat("CGD native cannot use target %s/%s from this %s editor.", p_platform, p_arch, host);
		return ERR_UNAVAILABLE;
	}
	r_target.platform = p_platform;
	r_target.arch = p_arch;
	const String fallback = windows ? "clang-cl" : android ? "ndk-clang"
														   : "clang";
	const String prefix = target_prefix(p_platform, p_arch);
	const Variant toolchain = setting(prefix + "toolchain");
	r_target.toolchain = toolchain.get_type() == Variant::STRING ? String(toolchain) : String();
	if (r_target.toolchain.is_empty()) {
		r_target.toolchain = fallback;
	}
	if (r_target.toolchain != fallback && !(windows && r_target.toolchain == "msvc") && !(linux_target && r_target.toolchain == "gcc")) {
		r_error = vformat("CGD native toolchain '%s' is unsupported for %s/%s.", r_target.toolchain, p_platform, p_arch);
		return ERR_UNAVAILABLE;
	}
	if (android) {
		if (p_arch == "arm32") {
			r_target.triple = "armv7a-linux-androideabi24";
		} else if (p_arch == "arm64") {
			r_target.triple = "aarch64-linux-android24";
		} else if (p_arch == "x86_32") {
			r_target.triple = "i686-linux-android24";
		} else if (p_arch == "x86_64") {
			r_target.triple = "x86_64-linux-android24";
		} else {
			r_error = "Unsupported CGD native Android architecture: " + p_arch;
			return ERR_UNAVAILABLE;
		}
	} else if (windows) {
		r_target.triple = "x86_64-pc-windows-msvc";
	}
	const Variant configured = setting(prefix + "compiler_path");
	String compiler = configured.get_type() == Variant::STRING ? String(configured) : String();
#ifdef WINDOWS_ENABLED
	if (r_target.toolchain == "clang-cl" && !os->get_environment("CGD_NATIVE_WINDOWS_X86_64_COMPILER").is_empty()) {
		compiler = os->get_environment("CGD_NATIVE_WINDOWS_X86_64_COMPILER");
	}
	if (compiler.is_empty()) {
		compiler = android ? find_android_clang() : r_target.toolchain == "msvc" ? find_on_path("cl.exe")
																				 : find_clang_cl();
	}
#else
	if (compiler.is_empty()) {
		compiler = macos ? "/usr/bin/clang" : r_target.toolchain;
	}
#endif
	if (!compiler.is_empty()) {
		r_target.compiler = compiler == compiler.get_file() ? find_on_path(compiler) : native_path(compiler);
	}
	if (r_target.compiler.is_empty() || !FileAccess::exists(r_target.compiler) ||
			(r_target.toolchain == "msvc" && (os->get_environment("INCLUDE").is_empty() || os->get_environment("LIB").is_empty()))) {
		r_error = vformat("Cannot resolve the CGD native '%s' toolchain for %s/%s.", r_target.toolchain, p_platform, p_arch);
		if (r_target.toolchain == "msvc") {
			r_error += " Launch the editor from an x64 Visual Studio Developer Command Prompt.";
		} else if (android) {
			r_error += " Set ANDROID_NDK_ROOT to the Android NDK installation.";
		} else {
			r_error += " Set " + prefix + "compiler_path or add the compiler to PATH.";
		}
		return ERR_FILE_NOT_FOUND;
	}
	return OK;
}

List<String> language_flags(const NativeTarget &p_target) {
	List<String> args;
	if (p_target.uses_msvc_flags()) {
		args = { "/nologo", "/TC", "/std:c17", "/utf-8", "/MT" };
		if (p_target.toolchain == "clang-cl") {
			args.push_back("--target=" + p_target.triple);
		}
	} else {
		args = { "-x", "c", "-std=c17" };
		if (!p_target.triple.is_empty()) {
			args.push_back("--target=" + p_target.triple);
		}
		if (p_target.platform == "macos") {
			for (const String &arch : p_target.arch == "universal" ? String("x86_64 arm64").split(" ") : p_target.arch.split(" ")) {
				args.push_back("-arch");
				args.push_back(arch);
			}
		}
	}
	return args;
}

Error add_defines(List<String> &r_args, const NativeTarget &p_target, String &r_error) {
	HashMap<String, String> defines;
	const PackedStringArray common = setting("cgd_native/defines");
	const PackedStringArray target = target_array("defines", p_target.platform, p_target.arch);
	for (const PackedStringArray *entries : { &common, &target }) {
		for (const String &entry : *entries) {
			const String name = entry.get_slice("=", 0);
			if (!name.is_valid_ascii_identifier()) {
				r_error = "Invalid CGD native preprocessor define: " + entry;
				return ERR_INVALID_PARAMETER;
			}
			defines.insert(name, entry);
		}
	}
	for (const KeyValue<String, String> &entry : defines) {
		r_args.push_back((p_target.uses_msvc_flags() ? "/D" : "-D") + entry.value);
	}
	return OK;
}

Error add_compile_inputs(List<String> &r_args, const String &p_path, const NativeTarget &p_target, String &r_error) {
	const Error error = add_defines(r_args, p_target, r_error);
	if (error != OK) {
		return error;
	}
	if (p_target.toolchain == "msvc") {
		r_args.push_back("/I" + native_path(p_path.get_base_dir()));
	} else {
		const bool clang_cl = p_target.toolchain == "clang-cl";
		r_args.push_back(clang_cl ? "/clang:-iquote" : "-iquote");
		r_args.push_back((clang_cl ? String("/clang:") : String()) + native_path(p_path.get_base_dir()));
	}
	const PackedStringArray common = setting("cgd_native/include_dirs");
	const PackedStringArray target = target_array("include_dirs", p_target.platform, p_target.arch);
	for (const PackedStringArray *entries : { &common, &target }) {
		for (const String &entry : *entries) {
			r_args.push_back("-I" + native_path(entry));
		}
	}
	return OK;
}

Error capture_compiler(const String &p_compiler, const List<String> &p_args, String &r_output, int *r_code = nullptr) {
#ifdef WINDOWS_ENABLED
	return OS::get_singleton()->execute(p_compiler, p_args, &r_output, r_code, true);
#else
	// OS_Unix::execute wraps captured command arguments in double quotes.
	auto escape = [](const String &p_argument) {
		return p_argument.replace("\\", "\\\\").replace("\"", "\\\"").replace("$", "\\$").replace("\x60", "\\\x60");
	};
	List<String> escaped;
	for (const String &arg : p_args) {
		escaped.push_back(escape(arg));
	}
	return OS::get_singleton()->execute(escape(p_compiler), escaped, &r_output, r_code, true);
#endif
}

bool gcc_completion_flags(const String &p_compiler, List<String> &r_args) {
#ifdef WINDOWS_ENABLED
	return false;
#else
	// Completion can run on every keystroke. Reuse the probe until its inputs change.
	static Mutex cache_mutex;
	MutexLock lock(cache_mutex);
	static Array cached_key;
	static List<String> cached_flags;
	OS *os = OS::get_singleton();
	Array key;
	key.push_back(p_compiler);
	key.push_back(FileAccess::get_modified_time(p_compiler));
	for (const char *name : { "PATH", "CPATH", "C_INCLUDE_PATH", "GCC_EXEC_PREFIX", "COMPILER_PATH" }) {
		key.push_back(os->get_environment(name));
	}
	if (key == cached_key) {
		for (const String &flag : cached_flags) {
			r_args.push_back(flag);
		}
		return true;
	}
	String triple;
	String version;
	String includes;
	int code = -1;
	if (capture_compiler(p_compiler, { "-dumpmachine" }, triple, &code) != OK || code != 0 ||
			capture_compiler(p_compiler, { "-dumpfullversion" }, version, &code) != OK || code != 0 ||
			capture_compiler("/usr/bin/env", { "LC_ALL=C", p_compiler, "-std=c17", "-x", "c", "-E", "-v", "/dev/null" }, includes, &code) != OK || code != 0) {
		return false;
	}
	List<String> flags = { "--target=" + triple.strip_edges(), "-fgnuc-version=" + version.strip_edges() };
	// Keep Clang's intrinsic headers and use the selected GCC's system paths.
	flags.push_back("-nostdlibinc");
	bool reading = false;
	for (const String &line : includes.split("\n", false)) {
		const String entry = line.strip_edges();
		if (entry == "#include <...> search starts here:") {
			reading = true;
		} else if (entry == "End of search list.") {
			if (!reading) {
				break;
			}
			cached_key = key;
			cached_flags = flags;
			for (const String &flag : flags) {
				r_args.push_back(flag);
			}
			return true;
		} else if (reading && entry.is_absolute_path()) {
			flags.push_back("-isystem");
			flags.push_back(entry);
		}
	}
	return false;
#endif
}
bool resolve_analysis_frontend(const String &p_path, NativeTarget &frontend, List<String> &args, String &r_error) {
	const Variant configured = setting("cgd_native/completion_target");
	const String selection = configured.get_type() == Variant::STRING ? String(configured) : String();
	const String platform = selection.is_empty() ? OS::get_singleton()->get_name().to_lower() : selection.get_slice("/", 0);
	const String arch = selection.is_empty() ? Engine::get_singleton()->get_architecture_name() : selection.get_slice("/", 1);
	NativeTarget target;
	if ((!selection.is_empty() && selection.get_slice_count("/") != 2) || resolve_target(platform, arch, target, r_error) != OK) {
		return false;
	}
	frontend = target;
	if (target.toolchain == "msvc" || target.toolchain == "gcc") {
		const Variant override = setting("cgd_native/completion_compiler_path");
		frontend.toolchain = target.toolchain == "msvc" ? "clang-cl" : "clang";
		if (override.get_type() == Variant::STRING && !String(override).is_empty()) {
			const String path = override;
			frontend.compiler = path == path.get_file() ? find_on_path(path) : native_path(path);
		} else {
#ifdef WINDOWS_ENABLED
			frontend.compiler = find_clang_cl();
#else
			frontend.compiler = find_on_path("clang");
#endif
		}
		if (frontend.compiler.is_empty() || !FileAccess::exists(frontend.compiler)) {
			return false;
		}
	}
	if (frontend.arch == "universal") {
		frontend.arch = Engine::get_singleton()->get_architecture_name();
	}
	args = language_flags(frontend);
	if (target.toolchain == "gcc" && !gcc_completion_flags(target.compiler, args)) {
		return false;
	}
	if (add_compile_inputs(args, p_path, target, r_error) != OK) {
		return false;
	}
	if (target.toolchain == "msvc") {
		OS *os = OS::get_singleton();
		args.push_back("/X");
		for (const String &directory : os->get_environment("INCLUDE").split(";", false)) {
			args.push_back("/imsvc" + directory.trim_prefix("\"").trim_suffix("\""));
		}
		const String version = os->get_environment("VCToolsVersion").strip_edges();
		if (version.begins_with("14.")) {
			args.push_back("/clang:-fms-compatibility-version=19." + version.substr(3));
		}
	}
	args.push_back(frontend.uses_msvc_flags() ? "/Zs" : "-fsyntax-only");
	return true;
}
#endif

} // namespace

#ifdef TOOLS_ENABLED
void GDCNative::request_analysis(ObjectID p_editor, const String &p_path, const String &p_code, const Callable &p_callback, bool p_check_build) {
	const AnalysisRequest *pending = pending_analysis.getptr(p_editor);
	if (!p_check_build && pending && pending->check_build) {
		return;
	}
	pending_analysis.insert(p_editor, { p_path, p_code, p_callback, p_check_build });
	start_analysis();
}

void GDCNative::request_project_check(ObjectID p_editor, const Callable &p_callback) {
	PackedStringArray paths;
	Vector<EditorFileSystemDirectory *> pending;
	if (EditorFileSystemDirectory *root = EditorFileSystem::get_singleton()->get_filesystem()) {
		pending.push_back(root);
	}
	while (!pending.is_empty()) {
		EditorFileSystemDirectory *directory = pending[pending.size() - 1];
		pending.remove_at(pending.size() - 1);
		for (int i = 0; i < directory->get_file_count(); i++) {
			const String path = directory->get_file_path(i);
			if (GDCFrontend::is_source_path(path)) {
				paths.push_back(path);
			}
		}
		for (int i = 0; i < directory->get_subdir_count(); i++) {
			pending.push_back(directory->get_subdir(i));
		}
	}
	pending_analysis.insert(p_editor, { String(), String(), p_callback, true, paths });
	start_analysis();
}

void GDCNative::start_analysis() {
	if (analysis_task != WorkerThreadPool::INVALID_TASK_ID || pending_analysis.is_empty()) {
		return;
	}
	const ObjectID editor = pending_analysis.begin()->key;
	const AnalysisRequest request = pending_analysis.begin()->value;
	pending_analysis.erase(editor);
	analysis_task = WorkerThreadPool::get_singleton()->add_template_task(this, &GDCNative::analyze, request, false, "CGD native diagnostics");
}

void GDCNative::finish_analysis(const String &p_path, const String &p_code, const Callable &p_callback, const Array &p_diagnostics, const Dictionary &p_symbols) {
	WorkerThreadPool::get_singleton()->wait_for_task_completion(analysis_task);
	analysis_task = WorkerThreadPool::INVALID_TASK_ID;
	start_analysis();
	if (p_callback.is_valid()) {
		p_callback.call(p_path, p_code, p_diagnostics, p_symbols);
	}
}

static Array positioned_build_errors(const String &p_path, const String &p_native_source, const std::vector<GDC::Result::NativeBody> &p_bodies, const String &error) {
	Array diagnostics;
	if (!error.is_empty()) {
		RegEx unix_location;
		RegEx msvc_location;
		unix_location.compile(R"(^(.+?):(\d+):(\d+):\s*((?:fatal )?error|warning):\s*(.*)$)");
		msvc_location.compile(R"(^(.+?)\((\d+)(?:,(\d+))?\)\s*:\s*((?:fatal )?error|warning)(?:\s+[^:]+)?:\s*(.*)$)");
		for (const String &entry : error.split("\n", false)) {
			Ref<RegExMatch> match = unix_location.search(entry.strip_edges());
			if (match.is_null()) {
				match = msvc_location.search(entry.strip_edges());
			}
			if (match.is_null() || match->get_string(4) == "warning") {
				continue;
			}
			int error_line = match->get_string(2).to_int();
			int error_column = MAX(1, int(match->get_string(3).to_int()));
			String message = match->get_string(5);
			const String filename = match->get_string(1);
			if (filename.begins_with("<cgd_native_") && filename.ends_with(">")) {
				int function_index = filename.left(filename.length() - 1).get_slice("_", 3).to_int();
				for (const auto &body : p_bodies) {
					if (!body.function_name.empty() && function_index-- == 0) {
						error_line = body.original.line;
						error_column = body.original.column;
						message = vformat("In generated wrapper for %s: %s", String(body.function_name.c_str()), message);
						break;
					}
				}
			} else if (filename == p_path) {
				for (const auto &body : p_bodies) {
					if (int(body.original.line) == error_line && !match->get_string(3).is_empty()) {
						const int start = p_native_source.rfind_char('\n', int(body.offset) - 1) + 1;
						error_column += int(body.original.column) - p_native_source.substr(start, int(body.offset) - start).utf8().length() - 1;
						break;
					}
				}
			} else {
				continue;
			}
			Dictionary positioned;
			positioned["line"] = error_line;
			positioned["column"] = error_column;
			positioned["error"] = message;
			diagnostics.push_back(positioned);
		}
	}
	return diagnostics;
}

void GDCNative::analyze(AnalysisRequest p_request) {
	Array diagnostics;
	Dictionary symbols;
	String generated;
	String native_source;
	String error;
	GDC::SourceMap map;
	int line = 1;
	int column = 1;
	NativeTarget frontend;
	List<String> args;
	std::vector<GDC::Result::NativeBody> bodies;
	if (p_request.check_build) {
		if (p_request.path.is_empty()) {
			const String platform = OS::get_singleton()->get_name().to_lower();
			const String arch = Engine::get_singleton()->get_architecture_name();
			HashMap<String, Dictionary> stamps;
			Vector<NativeUnit> units;
			for (const String &path : p_request.project_paths) {
				const String code = FileAccess::get_file_as_string(path);
				if (!code.contains("native")) {
					continue;
				}
				String id;
				String library;
				if (GDCFrontend::transpile(code, generated, map, error, line, column, false, { 0, 0 }, path, &native_source, nullptr, &id, &bodies) != OK || native_source.is_empty()) {
					continue;
				}
				units.push_back({ path, id, native_source });
				if (build(path, id, native_source, library, error, platform, arch, true, &stamps) == OK) {
					continue;
				}
				Dictionary failure;
				failure["path"] = path;
				failure["source"] = code;
				failure["line"] = bodies.empty() ? 1 : int(bodies.front().original.line);
				failure["column"] = bodies.empty() ? 1 : int(bodies.front().original.column);
				failure["positioned"] = positioned_build_errors(path, native_source, bodies, error);
				failure["error"] = error;
				diagnostics.push_back(failure);
			}
			if (!units.is_empty() && diagnostics.is_empty()) {
				String library;
				if (build_export(units, platform, arch, library, error) != OK) {
					Dictionary failure;
					failure["error"] = error;
					for (const NativeUnit &unit : units) {
						if (error.begins_with(unit.path + ":")) {
							failure["path"] = unit.path;
							failure["source"] = FileAccess::get_file_as_string(unit.path);
							failure["line"] = 1;
							failure["column"] = 1;
							break;
						}
					}
					diagnostics.push_back(failure);
				}
			}
			callable_mp(this, &GDCNative::finish_analysis).call_deferred(p_request.path, p_request.code, p_request.callback, diagnostics, symbols);
			return;
		}
		String id;
		String library;
		if (GDCFrontend::transpile(p_request.code, generated, map, error, line, column, false, { 0, 0 }, p_request.path, &native_source, nullptr, &id, &bodies) == OK && !native_source.is_empty()) {
			const Variant configured = setting("cgd_native/completion_target");
			const String selection = configured.get_type() == Variant::STRING ? String(configured) : String();
			const String platform = selection.is_empty() ? OS::get_singleton()->get_name().to_lower() : selection.get_slice("/", 0);
			const String arch = selection.is_empty() ? Engine::get_singleton()->get_architecture_name() : selection.get_slice("/", 1);
			if (build(p_request.path, id, native_source, library, error, platform, arch, true) != OK && !bodies.empty()) {
				line = bodies.front().original.line;
				column = bodies.front().original.column;
			}
		} else if (error.is_empty()) {
			error = "This script has no native C.";
		}
		Dictionary result;
		result["error"] = error;
		result["line"] = line;
		result["column"] = column;
		diagnostics.push_back(result);
		diagnostics.append_array(positioned_build_errors(p_request.path, native_source, bodies, error));
		callable_mp(this, &GDCNative::finish_analysis).call_deferred(p_request.path, p_request.code, p_request.callback, diagnostics, symbols);
		return;
	}
	String native_id;
	if (GDCFrontend::transpile(p_request.code, generated, map, error, line, column, false, { 0, 0 }, p_request.path, &native_source, nullptr, &native_id, &bodies) == OK &&
			!native_source.is_empty() && resolve_analysis_frontend(p_request.path, frontend, args, error)) {
		Array context;
		context.push_back(native_id);
		context.push_back(frontend.compiler);
		context.push_back(FileAccess::get_modified_time(frontend.compiler));
		for (const String &arg : args) {
			context.push_back(arg);
		}
		for (const char *env : { "INCLUDE", "CPATH", "C_INCLUDE_PATH", "SDKROOT", "DEVELOPER_DIR", "MACOSX_DEPLOYMENT_TARGET", "CL", "_CL_" }) {
			context.push_back(OS::get_singleton()->get_environment(env));
		}
		const String key = String(Variant(context)).sha256_text();
		Vector<int> body_lines;
		Vector<int> body_columns;
		for (const auto &body : bodies) {
			body_lines.push_back(body.original.line);
			body_columns.push_back(body.original.column);
		}
		if (const AnalysisResult *cached = analysis_cache.getptr(p_request.path); cached && cached->key == key && cached->body_lines.size() == body_lines.size()) {
			bool fresh = true;
			for (const KeyValue<Variant, Variant> &header : cached->headers) {
				const String path = header.key;
				if (!FileAccess::exists(path) || input_stamp(path, header.value, nullptr) != Dictionary(header.value)) {
					fresh = false;
					break;
				}
			}
			if (fresh) {
				print_verbose("CGD native analysis cache hit: " + p_request.path);
				diagnostics = cached->diagnostics.duplicate(true);
				for (int i = 0; i < diagnostics.size(); ++i) {
					Dictionary diagnostic = diagnostics[i];
					const int body = diagnostic.get("body", -1);
					if (body >= 0) {
						if (int(diagnostic["line"]) == cached->body_lines[body]) {
							diagnostic["column"] = int(diagnostic["column"]) + body_columns[body] - cached->body_columns[body];
						}
						diagnostic["line"] = int(diagnostic["line"]) + body_lines[body] - cached->body_lines[body];
					}
				}
				callable_mp(this, &GDCNative::finish_analysis).call_deferred(p_request.path, p_request.code, p_request.callback, diagnostics, cached->symbols);
				return;
			}
		}

		Error temp_error = OK;
		Ref<DirAccess> workspace = DirAccess::create_temp("cgd-diagnostics", false, &temp_error);
		if (workspace.is_valid()) {
			const String path = workspace->get_current_dir().path_join("diagnostics.c");
			Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
			if (file.is_valid() && file->store_string(native_source)) {
				file.unref();
				const String passthrough = frontend.uses_msvc_flags() ? "/clang:" : "";
				for (const char *flag : { "-fno-color-diagnostics", "-fno-caret-diagnostics", "-fdiagnostics-format=clang", "-ferror-limit=50" }) {
					args.push_back(passthrough + flag);
				}
				String output;
				print_verbose("Analyzing CGD native C: " + p_request.path);
				List<String> diagnostic_args(args);
				const String depfile = workspace->get_current_dir().path_join("diagnostics.d");
				diagnostic_args.push_back(passthrough + "-MD");
				diagnostic_args.push_back(passthrough + "-MF");
				diagnostic_args.push_back(passthrough + depfile);
				diagnostic_args.push_back(path);
				capture_compiler(frontend.compiler, diagnostic_args, output);
				const String prefix = p_request.path + ":";
				const String include_prefix = "In file included from " + prefix;
				int include_line = 0;
				int include_column = 1;
				for (const String &entry : output.split("\n", false)) {
					if (entry.begins_with(include_prefix)) {
						const PackedStringArray location = entry.substr(include_prefix.length()).split(":");
						include_line = location.is_empty() ? 0 : location[0].to_int();
						include_column = location.size() > 1 ? MAX(1, location[1].to_int()) : 1;
						continue;
					}
					if (entry.contains(": note: ") && !diagnostics.is_empty()) {
						Dictionary previous = diagnostics[diagnostics.size() - 1];
						previous["message"] = String(previous["message"]) + "\n" + entry.strip_edges();
						continue;
					}
					int generated_body = -1;
					if (entry.begins_with("<cgd_native_")) {
						const int end = entry.find(">");
						const String number = entry.left(end).get_slice("_", 3);
						if (end >= 0 && number.is_valid_int()) {
							const int index = int(number.to_int());
							int function = 0;
							for (int i = 0; i < int(bodies.size()); ++i) {
								if (!bodies[i].function_name.empty() && function++ == index) {
									generated_body = i;
									break;
								}
							}
						}
					}
					const bool own_source = entry.begins_with(prefix);
					if (own_source) {
						include_line = 0;
					}
					const int error_separator = own_source ? -1 : entry.find(": error: ");
					const int fatal_separator = own_source ? -1 : entry.find(": fatal error: ");
					const int warning_separator = own_source ? -1 : entry.find(": warning: ");
					const int separator = MAX(error_separator, MAX(fatal_separator, warning_separator));
					if (!own_source && generated_body < 0 && (include_line <= 0 || separator < 0)) {
						continue;
					}
					const String location = own_source ? entry.substr(prefix.length()) : vformat("%d:%d%s", generated_body >= 0 ? body_lines[generated_body] : include_line, include_column, entry.substr(separator));
					const PackedStringArray parts = location.split(":", true, 3);
					if (parts.size() != 4 || !parts[0].is_valid_int() || !parts[1].is_valid_int()) {
						continue;
					}
					const String severity = parts[2].strip_edges();
					if (severity != "error" && severity != "fatal error" && severity != "warning") {
						continue;
					}
					Dictionary diagnostic;
					const int diagnostic_line = parts[0].to_int();
					int diagnostic_column = parts[1].to_int();
					int body_index = generated_body;
					int nearest_line = 0;
					for (int i = 0; generated_body < 0 && i < int(bodies.size()); ++i) {
						const auto &body = bodies[i];
						if (int(body.original.line) <= diagnostic_line && int(body.original.line) >= nearest_line) {
							body_index = i;
							nearest_line = body.original.line;
						}
						if (int(body.original.line) == diagnostic_line) {
							const int line_start = native_source.rfind_char('\n', int(body.offset) - 1) + 1;
							const int generated_column = native_source.substr(line_start, int(body.offset) - line_start).utf8().length() + 1;
							diagnostic_column += int(body.original.column) - generated_column;
							break;
						}
					}
					diagnostic["line"] = diagnostic_line;
					diagnostic["column"] = generated_body >= 0 ? body_columns[generated_body] : diagnostic_column;
					diagnostic["error"] = severity != "warning";
					diagnostic["body"] = body_index;
					diagnostic["message"] = generated_body >= 0 ? vformat("In generated wrapper for %s: %s", String(bodies[generated_body].function_name.c_str()), parts[3].strip_edges()) : own_source ? parts[3].strip_edges()
																																																		: entry.strip_edges();
					diagnostics.push_back(diagnostic);
				}
				file = FileAccess::open(path, FileAccess::WRITE);
				if (file.is_valid() && file->store_string(native_source + "\nstatic void cgd_native_editor_scope(void) {\n    ;\n}\n")) {
					file.unref();
					args.push_back(passthrough + "-Xclang");
					args.push_back(passthrough + vformat("-code-completion-at=%s:%d:5", path, native_source.count("\n") + 3));
					args.push_back(passthrough + "-Xclang");
					args.push_back(passthrough + "-code-completion-macros");
					args.push_back(path);
					output = String();
					capture_compiler(frontend.compiler, args, output);
					for (const String &entry : output.split("\n", false)) {
						if (!entry.begins_with("COMPLETION: ")) {
							continue;
						}
						const String completion = entry.substr(12).strip_edges();
						const int split = completion.find(" : ");
						const String name = split < 0 ? completion : completion.left(split);
						if (!name.is_valid_unicode_identifier() || name == "Pattern" || name.begins_with("CGD_") || name.begins_with("cgd_native_")) {
							continue;
						}
						const String detail = split < 0 ? name : completion.substr(split + 3);
						symbols[name] = detail.replace("[#", "").replace("#]", " ").replace("<#", "").replace("#>", "").replace("{#", "").replace("#}", "");
					}
				}
				if (FileAccess::exists(depfile)) {
					Dictionary headers;
					for (const String &header : compiler_dependencies(depfile)) {
						if (header != path && FileAccess::exists(header)) {
							headers[header] = input_stamp(header, Dictionary(), nullptr);
						}
					}
					if (analysis_cache.size() >= 32 && !analysis_cache.has(p_request.path)) {
						analysis_cache.erase(analysis_cache.begin()->key);
					}
					analysis_cache.insert(p_request.path, { key, diagnostics, symbols, headers, body_lines, body_columns });
				}
			}
		}
	}
	callable_mp(this, &GDCNative::finish_analysis).call_deferred(p_request.path, p_request.code, p_request.callback, diagnostics, symbols);
}

bool GDCNative::is_native_position(const String &p_code, const String &p_path, int p_row, int p_column) {
	String generated;
	String error;
	GDC::SourceMap map;
	int line = 1;
	int column = 1;
	int offset = -1;
	return GDCFrontend::transpile(p_code, generated, map, error, line, column, true, { uint32_t(p_row + 1), uint32_t(p_column + 1) }, p_path, nullptr, &offset) == OK && offset >= 0;
}

bool GDCNative::complete_in_editor(ObjectID p_editor, const String &p_code, const String &p_path, const Callable &p_callback, List<ScriptLanguage::CodeCompletionOption> &r_options, String &r_hint) {
	const int caret = p_code.find_char(0xFFFF);
	if (caret < 0 || !p_code.contains("native")) {
		return false;
	}
	if (completion_result.symbol.is_empty() && completion_result.path == p_path && completion_result.code == p_code) {
		r_options = completion_result.options;
		r_hint = completion_result.hint;
		return true;
	}
	const String before = p_code.left(caret);
	if (!is_native_position(before + p_code.substr(caret + 1), p_path, before.count("\n"), caret - before.rfind_char('\n') - 1)) {
		return false;
	}
	pending_completions.insert(p_editor, { p_editor, p_path, p_code, p_callback });
	start_completion();
	return true;
}

void GDCNative::request_native_hover(ObjectID p_editor, const String &p_code, const String &p_path, const String &p_symbol, const Callable &p_callback) {
	if (completion_result.path == p_path && completion_result.code == p_code && completion_result.symbol == p_symbol) {
		p_callback.call(p_path, p_code, completion_result.prototype);
		return;
	}
	pending_completions.insert(p_editor, { p_editor, p_path, p_code, p_callback, p_symbol });
	start_completion();
}

void GDCNative::start_completion() {
	if (completion_task != WorkerThreadPool::INVALID_TASK_ID || pending_completions.is_empty()) {
		return;
	}
	const CompletionRequest request = pending_completions.begin()->value;
	pending_completions.erase(request.editor);
	completion_task = WorkerThreadPool::get_singleton()->add_template_task(this, &GDCNative::compute_completion, request, true, "CGD native completion");
}

void GDCNative::compute_completion(CompletionRequest p_request) {
	worker_completion_result = CompletionResult();
	worker_completion_result.path = p_request.path;
	worker_completion_result.code = p_request.code;
	worker_completion_result.symbol = p_request.symbol;
	complete_code(p_request.code, p_request.path, worker_completion_result.options, worker_completion_result.hint, p_request.symbol, &worker_completion_result.prototype);
	callable_mp(this, &GDCNative::finish_completion).call_deferred(p_request.callback);
}

void GDCNative::finish_completion(const Callable &p_callback) {
	WorkerThreadPool::get_singleton()->wait_for_task_completion(completion_task);
	completion_task = WorkerThreadPool::INVALID_TASK_ID;
	completion_result = std::move(worker_completion_result);
	if (p_callback.is_valid()) {
		if (completion_result.symbol.is_empty()) {
			p_callback.call(completion_result.path, completion_result.code);
		} else {
			p_callback.call(completion_result.path, completion_result.code, completion_result.prototype);
		}
	}
	start_completion();
}

bool GDCNative::complete_code(const String &p_code, const String &p_path, List<ScriptLanguage::CodeCompletionOption> &r_options, String &r_call_hint, const String &p_symbol, String *r_prototype) {
	const int caret = p_code.find_char(0xFFFF);
	if (caret < 0 || !p_code.contains("native")) {
		return false;
	}
	const String before = p_code.substr(0, caret);
	const int line_start = before.rfind_char('\n') + 1;
	const GDC::Position cursor = { uint32_t(before.count("\n") + 1), uint32_t(caret - line_start + 1) };
	const String source = before + p_code.substr(caret + 1);
	String generated;
	String native_source;
	GDC::SourceMap map;
	String error;
	int line = 1;
	int column = 1;
	int offset = -1;
	std::vector<GDC::Result::NativeBody> bodies;
	if (GDCFrontend::transpile(source, generated, map, error, line, column, true, cursor, p_path, &native_source, &offset, nullptr, &bodies) != OK || offset < 0) {
		return false;
	}
	r_call_hint = String();

	NativeTarget frontend;
	List<String> args;
	if (!resolve_analysis_frontend(p_path, frontend, args, error)) {
		return true;
	}
	const String current_line = before.substr(line_start);
	const String directive_line = current_line.strip_edges();
	const int quote = current_line.find_char('"');
	const int angle = current_line.find_char('<');
	const int delimiter = quote >= 0 && (angle < 0 || quote < angle) ? quote : angle;
	const String directive_name = delimiter >= 0 && directive_line.begins_with("#") ? current_line.left(delimiter).strip_edges().trim_prefix("#").strip_edges() : String();
	const bool include_directive = directive_name == "include" || directive_name == "include_next" || directive_name == "import";
	String include_directory;
	const int caret_offset = offset;
	if (include_directive && delimiter >= 0 && current_line.find_char(current_line[delimiter] == '"' ? '"' : '>', delimiter + 1) < 0) {
		const String include_prefix = current_line.substr(delimiter + 1);
		const int directory_end = include_prefix.rfind_char('/') + 1;
		include_directory = include_prefix.left(directory_end);
		offset -= include_prefix.length() - directory_end;
	} else {
		while (offset > 0 && is_unicode_identifier_continue(native_source[offset - 1])) {
			offset--;
		}
	}
	Error file_error = OK;
	Ref<DirAccess> workspace = DirAccess::create_temp("cgd-completion-" + itos(OS::get_singleton()->get_process_id()), false, &file_error);
	if (workspace.is_null()) {
		return true;
	}
	const String path = workspace->get_current_dir().path_join("completion.c");
	const String passthrough = frontend.uses_msvc_flags() ? "/clang:" : "";
	const String full_source = native_source;
	const int full_offset = offset;
	const List<String> full_args(args);
	String used_pch;
	// Keep declaration-block completion on the full translation unit. A completion
	// in a function can reuse the declarations compiled into the preamble.
	for (const auto &body : bodies) {
		if (body.function_name.empty()) {
			continue;
		}
		const int function_line = native_source.rfind_char('\n', int(body.offset) - 1);
		const int split = native_source.rfind_char('\n', function_line - 1) + 1;
		if (split <= 0 || split > offset) {
			break;
		}
		const String preamble = native_source.left(split);
		Array key;
		key.push_back(preamble);
		key.push_back(frontend.compiler);
		key.push_back(FileAccess::get_modified_time(frontend.compiler));
		for (const char *name : { "PATH", "INCLUDE", "CPATH", "C_INCLUDE_PATH", "SDKROOT", "DEVELOPER_DIR", "MACOSX_DEPLOYMENT_TARGET", "CL", "_CL_" }) {
			key.push_back(OS::get_singleton()->get_environment(name));
		}
		for (const String &arg : args) {
			key.push_back(arg);
		}
		const String cache = native_path(ProjectSettings::get_singleton()->get_project_data_path().path_join("cgd_native/pch"));
		DirAccess::make_dir_recursive_absolute(cache);
		const String base = cache.path_join(String(Variant(key)).sha256_text());
		const String header = base + ".h";
		const String pch = base + ".pch";
		static Mutex preamble_mutex;
		MutexLock lock(preamble_mutex);
		if (DirAccess::get_files_at(cache).size() > 128) {
			const uint64_t now = Time::get_singleton()->get_unix_time_from_system();
			for (const String &entry : DirAccess::get_files_at(cache)) {
				if (entry.get_basename().length() != 64 || !entry.get_basename().is_valid_hex_number(false) ||
						(entry.get_extension() != "h" && entry.get_extension() != "c" && entry.get_extension() != "pch")) {
					continue;
				}
				const String stale = cache.path_join(entry);
				if (FileAccess::get_modified_time(stale) + 86400 * 7 < now) {
					DirAccess::remove_absolute(stale);
				}
			}
		}
		if (!FileAccess::exists(pch)) {
			Ref<FileAccess> file = FileAccess::open(header, FileAccess::WRITE);
			if (file.is_null() || !file->store_string(preamble)) {
				break;
			}
			file.unref();
			List<String> flags;
			for (const String &arg : args) {
				if (arg != "/Zs" && arg != "-fsyntax-only") {
					flags.push_back(arg);
				}
			}
			if (frontend.uses_msvc_flags()) {
				const String stub = base + ".c";
				file = FileAccess::open(stub, FileAccess::WRITE);
				if (file.is_null() || !file->store_string("#include \"" + header + "\"\n")) {
					break;
				}
				file.unref();
				flags.push_back("/c");
				flags.push_back("/Yc" + header);
				flags.push_back("/Fp" + pch);
				flags.push_back("/Fo" + workspace->get_current_dir().path_join("preamble.obj"));
				flags.push_back(stub);
			} else {
				flags.push_back("-x");
				flags.push_back("c-header");
				flags.push_back("-o");
				flags.push_back(pch);
				flags.push_back(header);
			}
			String output;
			int status = -1;
			if (capture_compiler(frontend.compiler, flags, output, &status) != OK || status != 0) {
				DirAccess::remove_absolute(pch);
				break;
			}
		}
		if (FileAccess::exists(pch)) {
			used_pch = pch;
			if (frontend.uses_msvc_flags()) {
				const String include = "#include \"" + header + "\"\n";
				args.push_back("/Yu" + header);
				args.push_back("/Fp" + pch);
				native_source = include + native_source.substr(split);
				offset += include.length() - split;
			} else {
				args.push_back("-Xclang");
				args.push_back("-include-pch");
				args.push_back("-Xclang");
				args.push_back(pch);
				native_source = native_source.substr(split);
				offset -= split;
			}
		}
		break;
	}
	String output;
	const auto query = [&](const String &p_source, int p_offset, const List<String> &p_args) {
		Ref<FileAccess> temporary = FileAccess::open(path, FileAccess::WRITE);
		if (temporary.is_null() || !temporary->store_string(p_source)) {
			return false;
		}
		temporary.unref();
		const String native_before = p_source.left(p_offset);
		const String native_line = native_before.substr(native_before.rfind_char('\n') + 1);
		const String location = vformat("-code-completion-at=%s:%d:%d", path, native_before.count("\n") + 1, native_line.utf8().length() + 1);
		List<String> flags(p_args);
		for (const String &argument : { String("-Xclang"), location, String("-Xclang"), String("-code-completion-macros") }) {
			flags.push_back(passthrough + argument);
		}
		flags.push_back(path);
		int status = -1;
		output = String();
		return capture_compiler(frontend.compiler, flags, output, &status) == OK && status == 0;
	};
	if (!query(native_source, offset, args) && !used_pch.is_empty()) {
		print_verbose("CGD preamble rejected: " + output.get_slice("\n", 0));
		DirAccess::remove_absolute(used_pch);
		query(full_source, full_offset, full_args);
	}
	if (output.is_empty()) {
		return true;
	}
	HashSet<String> seen;
	const bool suggest_arguments = full_offset != caret_offset;
	const PackedStringArray entries = output.split("\n", false);
	for (const String &entry : entries) {
		if (entry.begins_with("OVERLOAD: ")) {
			// Clang marks the return type and the active argument.
			const String marker = String::chr(0xFFFF);
			const String signature = entry.trim_prefix("OVERLOAD: ").strip_edges().replace("[#", "").replace("#]", " ").replace("<#", marker).replace("#>", marker);
			if (!r_call_hint.is_empty()) {
				r_call_hint += "\n";
			}
			r_call_hint += signature;
		}
	}
	if (!r_call_hint.is_empty() && !suggest_arguments && p_symbol.is_empty()) {
		return true;
	}
	for (const String &entry : entries) {
		if (!entry.begins_with("COMPLETION: ")) {
			continue;
		}
		const String completion = entry.substr(12).strip_edges();
		const int separator = completion.find(" : ");
		String name = separator < 0 ? completion : completion.substr(0, separator);
		const String detail = separator < 0 ? String() : completion.substr(separator + 3);
		if (r_prototype && !p_symbol.is_empty() && name == p_symbol) {
			if (!r_prototype->is_empty()) {
				*r_prototype += "\n";
			}
			*r_prototype += detail.is_empty() ? name : detail.replace("[#", "").replace("#]", " ").replace("<#", "").replace("#>", "").replace("{#", "").replace("#}", "");
		}
		ScriptLanguage::CodeCompletionKind kind = ScriptLanguage::CODE_COMPLETION_KIND_PLAIN_TEXT;
		if (include_directive && name == "Pattern") {
			name = include_directory + detail;
			kind = ScriptLanguage::CODE_COMPLETION_KIND_FILE_PATH;
			if (delimiter >= 0 && current_line[delimiter] == '"') {
				name = "\"" + name;
			}
		} else if (!name.is_valid_unicode_identifier() || name.begins_with("CGD_") || name.begins_with("cgd_native_")) {
			continue;
		} else if (detail.contains(name + "(")) {
			kind = ScriptLanguage::CODE_COMPLETION_KIND_FUNCTION;
		}
		if (seen.has(name)) {
			continue;
		}
		seen.insert(name);
		ScriptLanguage::CodeCompletionOption option(name, kind);
		if (kind == ScriptLanguage::CODE_COMPLETION_KIND_FUNCTION) {
			option.insert_text += "(";
		}
		r_options.push_back(option);
	}
	return true;
}

bool GDCNative::prepare_editor_run() {
	EditorFileSystemDirectory *directory = EditorFileSystem::get_singleton()->get_filesystem();
	if (!directory) {
		return true;
	}
	String error;
	HashMap<String, Dictionary> stamps;
	const uint64_t started = Time::get_singleton()->get_ticks_usec();
	const Error result = prepare_directory(directory, OS::get_singleton()->get_name().to_lower(), Engine::get_singleton()->get_architecture_name(), error, stamps);
	if (!stamps.is_empty()) {
		print_verbose(vformat("CGD native preflight: %.1f ms", (Time::get_singleton()->get_ticks_usec() - started) / 1000.0));
	}
	if (result == OK) {
		return true;
	}
	ERR_PRINT(error);
	EditorNode::get_singleton()->show_warning(error, "CGD Native Build Failed");
	return false;
}
#endif

PackedStringArray GDCNative::runtime_libraries(const String &p_platform, const String &p_arch) {
	return target_array("runtime_libraries", p_platform, p_arch);
}

Error GDCNative::build(const String &p_path, const String &p_id, const String &p_native_source, String &r_dll, String &r_error, const String &p_platform, const String &p_arch, bool p_cache_for_run, HashMap<String, Dictionary> *p_run_stamps) {
#ifdef TOOLS_ENABLED
	const bool windows = p_platform == "windows";
	const bool android = p_platform == "android";
	const bool macos = p_platform == "macos";
	NativeTarget target;
	const Error target_error = resolve_target(p_platform, p_arch, target, r_error);
	if (target_error != OK) {
		r_error = p_path + ": " + r_error;
		return target_error;
	}
	const bool msvc = target.toolchain == "msvc";
	const String &compiler = target.compiler;
	const String prefix = target_prefix(p_platform, p_arch);
	const String library_name = library_filename(p_platform, p_id);
	const String cache = native_path(run_cache_path(p_platform, p_arch, p_id));
	String fingerprint;
	Dictionary inputs;
	const Dictionary previous = p_cache_for_run ? read_run_cache(cache) : Dictionary();
	const Dictionary previous_inputs = previous.get("inputs", Dictionary());
	const auto stamp = [&](const String &p_input) { return input_stamp(p_input, previous_inputs.get(p_input, Dictionary()), p_run_stamps); };
	if (p_cache_for_run) {
		OS *os = OS::get_singleton();
		Array configuration;
		configuration.push_back(NATIVE_BUILD_SCHEMA);
		configuration.push_back(p_id);
		configuration.push_back(target.toolchain);
		configuration.push_back(target.triple);
		configuration.push_back(compiler);
		inputs[compiler] = stamp(compiler);
		for (const char *name : { "defines", "include_dirs", "library_dirs", "libraries", "runtime_libraries" }) {
			configuration.push_back(setting("cgd_native/" + String(name)));
			configuration.push_back(setting(prefix + name));
		}
		for (const char *name : { "PATH", "INCLUDE", "LIB", "LIBPATH", "CPATH", "C_INCLUDE_PATH", "LIBRARY_PATH", "GCC_EXEC_PREFIX", "COMPILER_PATH", "SDKROOT", "DEVELOPER_DIR", "MACOSX_DEPLOYMENT_TARGET", "WindowsSdkDir", "WindowsSDKVersion", "VCToolsInstallDir", "CL", "_CL_", "LINK" }) {
			configuration.push_back(os->get_environment(name));
		}
		for (const String &entry : runtime_libraries(p_platform, p_arch)) {
			const String path = native_path(entry);
			inputs[path] = stamp(path);
		}
		for (const String &entry : target_array("libraries", p_platform, p_arch)) {
			const String path = native_path(entry);
			if (FileAccess::exists(path)) {
				inputs[path] = stamp(path);
			}
		}
		for (const String &entry : target_array("library_dirs", p_platform, p_arch)) {
			const String directory = native_path(entry);
			Dictionary listing;
			for (const String &file : DirAccess::get_files_at(directory)) {
				const String path = directory.path_join(file);
				listing[file] = vformat("%d:%d", FileAccess::get_size(path), FileAccess::get_modified_time(path));
			}
			configuration.push_back(listing);
		}
		configuration.push_back(inputs.keys());
		fingerprint = String(Variant(configuration)).sha256_text();
		const String previous_library = cached_library(cache, previous, library_name);
		if (!previous_library.is_empty() && previous.get("fingerprint", String()) == Variant(fingerprint)) {
			const Dictionary dependencies = previous.get("dependencies", Dictionary());
			bool current = previous.has("dependencies") && previous.has("inputs");
			Dictionary checked_dependencies;
			for (const KeyValue<Variant, Variant> &dependency : dependencies) {
				const String path = dependency.key;
				const Dictionary old_stamp = dependency.value;
				const Dictionary new_stamp = input_stamp(path, old_stamp, p_run_stamps);
				checked_dependencies[path] = new_stamp;
				if (!FileAccess::exists(path) || new_stamp["sha256"] != old_stamp["sha256"]) {
					current = false;
					break;
				}
			}
			for (const KeyValue<Variant, Variant> &input : inputs) {
				const Dictionary old_stamp = previous_inputs.get(input.key, Dictionary());
				const Dictionary new_stamp = input.value;
				if (new_stamp["sha256"] != old_stamp.get("sha256", Variant())) {
					current = false;
					break;
				}
			}
			if (current) {
				if (inputs != previous_inputs || checked_dependencies != dependencies) {
					MutexLock lock(build_mutex);
					if (read_run_cache(cache) == previous) {
						Dictionary refreshed = previous.duplicate();
						refreshed["inputs"] = inputs;
						refreshed["dependencies"] = checked_dependencies;
						const String pending = cache.path_join("refresh_" + itos(os->get_process_id()));
						Ref<FileAccess> file = FileAccess::open(pending, FileAccess::WRITE);
						if (file.is_valid() && file->store_var(refreshed)) {
							file.unref();
							DirAccess::rename_absolute(pending, cache.path_join("current"));
						}
					}
				}
				r_dll = previous_library;
				print_verbose("CGD native cache hit: " + p_path);
				return OK;
			}
		}
	}

	String dir;
	uint64_t build_index;
	{
		MutexLock lock(build_mutex);
		if (build_dir.is_null()) {
			Error temp_error = OK;
			build_dir = DirAccess::create_temp("godot-cgd-native-" + itos(OS::get_singleton()->get_process_id()), false, &temp_error);
		}
		if (build_dir.is_null()) {
			r_error = "Cannot create the CGD native build directory.";
			return ERR_CANT_CREATE;
		}
		static uint64_t next_build_index = 0;
		build_index = ++next_build_index;
		dir = build_dir->get_current_dir().path_join(itos(build_index));
	}
	// A fresh directory lets exports rebuild after header changes even while a previous DLL is loaded.
	if (DirAccess::make_dir_recursive_absolute(dir) != OK) {
		r_error = "Cannot create CGD native build directory: " + dir;
		return ERR_CANT_CREATE;
	}
	const String source_file = dir.path_join("script.c");
	r_dll = dir.path_join(library_name);
	Ref<FileAccess> source = FileAccess::open(source_file, FileAccess::WRITE);
	if (source.is_null() || !source->store_string(p_native_source)) {
		r_error = "Cannot write generated CGD C source: " + source_file;
		return ERR_CANT_CREATE;
	}
	source.unref();

	List<String> args = language_flags(target);
	args.push_back(target.uses_msvc_flags() ? "/O2" : "-O2");
	if (android) {
		args.push_back("-Wl,-soname," + library_name);
	} else if (windows) {
		args.push_back("/LD");
	} else if (macos) {
		args.push_back("-Wl,-install_name,@rpath/" + library_name);
		args.push_back("-Wl,-rpath,@loader_path");
	} else {
		args.push_back("-Wl,-soname," + library_name);
		args.push_back("-Wl,-rpath,$ORIGIN");
	}
	if (!windows) {
		args.push_back(macos ? "-dynamiclib" : "-shared");
		args.push_back("-fPIC");
		args.push_back("-fvisibility=hidden");
		if (!macos) {
			args.push_back("-Wl,--no-undefined");
		}
	}
	const String depfile = dir.path_join(msvc ? "script.json" : "script.d");
	if (p_cache_for_run) {
		if (msvc) {
			args.push_back("/sourceDependencies");
			args.push_back(depfile);
		} else {
			args.push_back(windows ? "/clang:-MD" : "-MD");
			args.push_back(windows ? "/clang:-MF" : "-MF");
			args.push_back((windows ? String("/clang:") : String()) + depfile);
		}
	}
	const Error inputs_error = add_compile_inputs(args, p_path, target, r_error);
	if (inputs_error != OK) {
		return inputs_error;
	}
	if (!windows) {
		args.push_back("-o");
		args.push_back(r_dll);
		args.push_back(source_file);
		args.push_back("-x");
		args.push_back("none");
	} else {
		args.push_back("/Fo" + dir.path_join("script.obj"));
		args.push_back(source_file);
		args.push_back("/link");
		args.push_back("/OUT:" + r_dll);
		args.push_back("/IMPLIB:" + dir.path_join("script.lib"));
	}
	for (const String &entry : target_array("library_dirs", p_platform, p_arch)) {
		args.push_back((windows ? "/LIBPATH:" : "-L") + native_path(entry));
	}
	for (const String &entry : target_array("libraries", p_platform, p_arch)) {
		if ((windows && entry.begins_with("/")) || entry.begins_with("-") || entry.begins_with("@")) {
			r_error = "Expected a library name or path in CGD native libraries: " + entry;
			return ERR_INVALID_PARAMETER;
		}
		args.push_back(!windows && entry.is_valid_ascii_identifier() ? "-l" + entry : native_path(entry));
	}
	int code = -1;
	String output;
	print_verbose("Compiling CGD native C: " + p_path);
	const Error execute_error = capture_compiler(compiler, args, output, &code);
	if (execute_error != OK || code != 0 || !FileAccess::exists(r_dll)) {
		r_error = vformat("%s: CGD native C build with '%s' failed (exit %d).\n%s", p_path, compiler, code, output);
		return ERR_COMPILATION_FAILED;
	}
	for (const String &entry : runtime_libraries(p_platform, p_arch)) {
		if (DirAccess::copy_absolute(native_path(entry), dir.path_join(entry.get_file())) != OK) {
			r_error = "Cannot copy CGD native runtime library: " + entry;
			return ERR_CANT_CREATE;
		}
	}
	if (p_cache_for_run) {
		Dictionary dependencies;
		const PackedStringArray headers = msvc ? msvc_dependencies(depfile) : compiler_dependencies(depfile);
		for (const String &header : headers) {
			const String path = header.replace_char('\\', '/');
			if (path != source_file && FileAccess::exists(path)) {
				dependencies[path] = input_stamp(path, Dictionary(), p_run_stamps);
			}
		}
		const String generation = "build_" + itos(OS::get_singleton()->get_process_id()) + "_" + itos(Time::get_singleton()->get_ticks_usec()) + "_" + itos(build_index);
		const String destination = cache.path_join(generation);
		if (!FileAccess::exists(depfile) || DirAccess::make_dir_recursive_absolute(destination) != OK ||
				DirAccess::copy_absolute(r_dll, destination.path_join(library_name)) != OK) {
			r_error = "Cannot publish the CGD native library and dependencies: " + p_path;
			return ERR_CANT_CREATE;
		}
		for (const String &entry : runtime_libraries(p_platform, p_arch)) {
			if (DirAccess::copy_absolute(dir.path_join(entry.get_file()), destination.path_join(entry.get_file())) != OK) {
				r_error = "Cannot publish CGD native runtime library: " + entry;
				return ERR_CANT_CREATE;
			}
		}
		Dictionary manifest;
		manifest["schema"] = NATIVE_BUILD_SCHEMA;
		manifest["generation"] = generation;
		manifest["fingerprint"] = fingerprint;
		manifest["inputs"] = inputs;
		manifest["dependencies"] = dependencies;
		const String pending = cache.path_join(generation + ".manifest");
		Ref<FileAccess> file = FileAccess::open(pending, FileAccess::WRITE);
		if (file.is_null() || !file->store_var(manifest)) {
			r_error = "Cannot write the CGD native cache manifest: " + p_path;
			return ERR_CANT_CREATE;
		}
		file.unref();
		MutexLock lock(build_mutex);
		if (DirAccess::rename_absolute(pending, cache.path_join("current")) != OK) {
			r_error = "Cannot publish the CGD native cache manifest: " + p_path;
			return ERR_CANT_CREATE;
		}
		r_dll = destination.path_join(library_name);
	}
	return OK;
#else
	r_error = "CGD native C builds require an editor build.";
	return ERR_UNAVAILABLE;
#endif
}

Error GDCNative::build_export(const Vector<NativeUnit> &p_units, const String &p_platform, const String &p_arch, String &r_library, String &r_error) {
#ifdef TOOLS_ENABLED
	NativeTarget target;
	const Error target_error = resolve_target(p_platform, p_arch, target, r_error);
	if (target_error != OK) {
		return target_error;
	}
	const bool windows = target.uses_msvc_flags();
	const bool macos = p_platform == "macos";
	if (macos && p_arch == "universal") {
		String x64;
		String arm64;
		Error error = build_export(p_units, p_platform, "x86_64", x64, r_error);
		if (error != OK) {
			return error;
		}
		error = build_export(p_units, p_platform, "arm64", arm64, r_error);
		if (error != OK) {
			return error;
		}
		const String output_dir = x64.get_base_dir().path_join("universal");
		DirAccess::make_dir_recursive_absolute(output_dir);
		r_library = output_dir.path_join("libcgd_native.dylib");
		int status = -1;
		if (capture_compiler("/usr/bin/lipo", { "-create", x64, arm64, "-output", r_library }, r_error, &status) != OK || status != 0) {
			return ERR_COMPILATION_FAILED;
		}
		return OK;
	}
	Error temp_error = OK;
	Ref<DirAccess> workspace = DirAccess::create_temp("cgd-export", false, &temp_error);
	if (workspace.is_null()) {
		r_error = "Cannot create CGD native export directory.";
		return ERR_CANT_CREATE;
	}
	const String directory = workspace->get_current_dir();
	const String filename = windows ? "cgd_native.dll" : macos ? "libcgd_native.dylib"
															   : "libcgd_native.so";
	List<String> objects;
	String symbol_tool;
#ifdef WINDOWS_ENABLED
	symbol_tool = find_clang_cl().get_base_dir().path_join("llvm-nm.exe");
	if (!FileAccess::exists(symbol_tool)) {
		symbol_tool = find_on_path("llvm-nm.exe");
	}
#else
	symbol_tool = macos ? "/usr/bin/nm" : find_on_path("nm");
#endif
	if (symbol_tool.is_empty()) {
		r_error = "CGD native exports need nm to validate native helper linkage.";
		return ERR_FILE_NOT_FOUND;
	}
	HashMap<String, Dictionary> stamps;
	const Dictionary compiler_stamp = input_stamp(target.compiler, Dictionary(), &stamps);
	const String cache_root = native_path(ProjectSettings::get_singleton()->get_project_data_path().path_join("cgd_native/export").path_join(p_platform).path_join(p_arch));
	DirAccess::make_dir_recursive_absolute(cache_root);
	String helper_errors;
	for (const NativeUnit &unit : p_units) {
		const String source = directory.path_join(unit.id + ".c");
		const String object = directory.path_join(unit.id + (windows ? ".obj" : ".o"));
		const String deps = directory.path_join(unit.id + ".d");
		const String manifest_path = cache_root.path_join(unit.id + ".manifest");
		const String cached_object = cache_root.path_join(object.get_file());
		List<String> args = language_flags(target);
		args.push_back(windows ? "/O2" : "-O2");
		args.push_back(windows ? "/DCGD_MERGED_EXPORT" : "-DCGD_MERGED_EXPORT");
		const Error input_error = add_compile_inputs(args, unit.path, target, r_error);
		if (input_error != OK) {
			return input_error;
		}
		Array configuration;
		configuration.push_back(NATIVE_BUILD_SCHEMA);
		configuration.push_back(target.compiler);
		configuration.push_back(compiler_stamp);
		for (const String &arg : args) {
			configuration.push_back(arg);
		}
		for (const char *name : { "PATH", "INCLUDE", "CPATH", "C_INCLUDE_PATH", "SDKROOT", "DEVELOPER_DIR", "MACOSX_DEPLOYMENT_TARGET", "CL", "_CL_" }) {
			configuration.push_back(OS::get_singleton()->get_environment(name));
		}
		const String fingerprint = String(Variant(configuration)).sha256_text();
		Ref<FileAccess> manifest = FileAccess::open(manifest_path, FileAccess::READ);
		if (manifest.is_valid() && FileAccess::exists(cached_object)) {
			const Variant data = manifest->get_var();
			if (data.get_type() == Variant::DICTIONARY) {
				const Dictionary entry = data;
				const Dictionary dependencies = entry.get("dependencies", Dictionary());
				bool fresh = entry.get("fingerprint", String()) == Variant(fingerprint);
				for (const KeyValue<Variant, Variant> &dependency : dependencies) {
					const String header = dependency.key;
					fresh = fresh && FileAccess::exists(header) && input_stamp(header, dependency.value, nullptr) == Dictionary(dependency.value);
				}
				if (fresh) {
					objects.push_back(cached_object);
					print_verbose("CGD native export object cache hit: " + unit.path);
					continue;
				}
			}
		}
		manifest.unref();
		Ref<FileAccess> file = FileAccess::open(source, FileAccess::WRITE);
		if (file.is_null() || !file->store_string(unit.source)) {
			r_error = "Cannot write native export source: " + unit.path;
			return ERR_CANT_CREATE;
		}
		file.unref();
		if (windows) {
			args.push_back("/c");
			args.push_back("/Fo" + object);
			if (target.toolchain == "msvc") {
				args.push_back("/sourceDependencies");
				args.push_back(deps);
			} else {
				args.push_back("/clang:-MD");
				args.push_back("/clang:-MF");
				args.push_back("/clang:" + deps);
			}
		} else {
			args.push_back("-c");
			args.push_back("-fPIC");
			args.push_back("-fvisibility=hidden");
			args.push_back("-MD");
			args.push_back("-MF");
			args.push_back(deps);
			args.push_back("-o");
			args.push_back(object);
		}
		args.push_back(source);
		int status = -1;
		String output;
		print_verbose("Compiling CGD native export object: " + unit.path);
		if (capture_compiler(target.compiler, args, output, &status) != OK || status != 0 || !FileAccess::exists(object)) {
			r_error = unit.path + ": " + output;
			return ERR_COMPILATION_FAILED;
		}
		String symbols;
		const List<String> symbol_args = macos ? List<String>{ "-g", "-U", "-j", object } : List<String>{ "--extern-only", "--defined-only", "--format=just-symbols", object };
		if (capture_compiler(symbol_tool, symbol_args, symbols, &status) != OK || status != 0) {
			r_error = unit.path + ": Cannot inspect native object symbols.\n" + symbols;
			return ERR_COMPILATION_FAILED;
		}
		bool invalid_helpers = false;
		for (const String &line : symbols.split("\n", false)) {
			const String symbol = line.strip_edges().trim_prefix(macos ? "_" : "");
			if (!symbol.is_empty() && !symbol.begins_with("cgd_native_" + unit.id.left(16) + "_") && symbol != "@feat.00" && symbol != "_fltused" && symbol != "__fltused") {
				helper_errors += unit.path + ": Native helpers must be static (scripts are linked together on export): " + symbol + "\n";
				invalid_helpers = true;
			}
		}
		if (invalid_helpers) {
			continue;
		}
		const PackedStringArray headers = target.toolchain == "msvc" ? msvc_dependencies(deps) : compiler_dependencies(deps);
		Dictionary dependencies;
		for (const String &header : headers) {
			if (header != source && FileAccess::exists(header)) {
				dependencies[header] = input_stamp(header, Dictionary(), &stamps);
			}
		}
		Dictionary entry;
		entry["fingerprint"] = fingerprint;
		entry["dependencies"] = dependencies;
		if (FileAccess::exists(deps) && DirAccess::copy_absolute(object, cached_object) == OK) {
			manifest = FileAccess::open(manifest_path, FileAccess::WRITE);
			if (manifest.is_valid() && manifest->store_var(entry)) {
				objects.push_back(cached_object);
				continue;
			}
		}
		objects.push_back(object);
	}
	if (!helper_errors.is_empty()) {
		r_error = helper_errors;
		return ERR_INVALID_DATA;
	}
	const String abi_source = directory.path_join("abi.c");
	const String abi_object = directory.path_join(windows ? "abi.obj" : "abi.o");
	const String preamble = p_units[0].source.get_slice("\n#line ", 0);
	Ref<FileAccess> abi_file = FileAccess::open(abi_source, FileAccess::WRITE);
	if (abi_file.is_null() || !abi_file->store_string(preamble)) {
		return ERR_CANT_CREATE;
	}
	abi_file.unref();
	List<String> abi_args = language_flags(target);
	if (windows) {
		abi_args.push_back("/c");
		abi_args.push_back("/Fo" + abi_object);
	} else {
		abi_args.push_back("-fPIC");
		abi_args.push_back("-c");
		abi_args.push_back("-o");
		abi_args.push_back(abi_object);
	}
	abi_args.push_back(abi_source);
	int abi_status = -1;
	if (capture_compiler(target.compiler, abi_args, r_error, &abi_status) != OK || abi_status != 0) {
		return ERR_COMPILATION_FAILED;
	}
	objects.push_back(abi_object);
	const String library = directory.path_join(filename);
	List<String> link;
	if (windows) {
		link = { "/nologo", "/LD", "/MT" };
		if (target.toolchain == "clang-cl") {
			link.push_back("--target=" + target.triple);
		}
	} else {
		link = { macos ? "-dynamiclib" : "-shared", "-o", library };
		if (!target.triple.is_empty()) {
			link.push_back("--target=" + target.triple);
		}
		if (macos) {
			link.push_back("-arch");
			link.push_back(p_arch);
			link.push_back("-Wl,-install_name,@rpath/" + filename);
			link.push_back("-Wl,-rpath,@loader_path");
		} else {
			link.push_back("-Wl,--no-undefined");
			link.push_back("-Wl,-soname," + filename);
			link.push_back("-Wl,-rpath,$ORIGIN");
		}
	}
	for (const String &object : objects) {
		link.push_back(object);
	}
	if (windows) {
		link.push_back("/link");
		link.push_back("/OUT:" + library);
		link.push_back("/IMPLIB:" + directory.path_join("cgd_native.lib"));
	}
	for (const String &entry : target_array("library_dirs", p_platform, p_arch)) {
		link.push_back((windows ? "/LIBPATH:" : "-L") + native_path(entry));
	}
	for (const String &entry : target_array("libraries", p_platform, p_arch)) {
		if ((windows && entry.begins_with("/")) || entry.begins_with("-") || entry.begins_with("@")) {
			r_error = "Expected a library name or path in CGD native libraries: " + entry;
			return ERR_INVALID_PARAMETER;
		}
		link.push_back(!windows && entry.is_valid_ascii_identifier() ? "-l" + entry : native_path(entry));
	}
	int status = -1;
	if (capture_compiler(target.compiler, link, r_error, &status) != OK || status != 0 || !FileAccess::exists(library)) {
		return ERR_COMPILATION_FAILED;
	}
	r_library = library;
	{
		MutexLock lock(build_mutex);
		export_directories.push_back(workspace);
	}
	return OK;
#else
	r_error = "CGD native exports require an editor build.";
	return ERR_UNAVAILABLE;
#endif
}

Variant GDCNative::invoke(const String &p_path, const StringName &p_id, const StringName &p_symbol, const StringName &p_signature, const Array &p_args) {
	OS *os = OS::get_singleton();
	NativeFunctionEntry function;
	bool cache_hit = false;
	{
		MutexLock lock(mutex);
		// Libraries stay loaded until shutdown. Clear their function caches before any future unload.
		const auto *library_functions = functions.getptr(p_id);
		const NativeFunctionEntry *cached_function = library_functions ? library_functions->getptr(p_symbol) : nullptr;
		if (cached_function) {
			function = *cached_function;
			cache_hit = true;
		} else {
			const String id = p_id;
			const String symbol_name = p_symbol;
			ERR_FAIL_COND_V_MSG(id.length() != 64 || !id.is_valid_hex_number(false), Variant(), "Invalid CGD native library identifier.");
			ERR_FAIL_COND_V_MSG(!symbol_name.begins_with("cgd_native_") || !symbol_name.get_slice("_", 3).is_valid_int() || !symbol_name.begins_with("cgd_native_" + id.left(16) + "_"), Variant(), "Invalid CGD native function identifier.");
			const CharString signature = p_signature.string().utf8();
			ERR_FAIL_COND_V_MSG(signature.length() != p_args.size() + 1, Variant(), "Invalid CGD native function signature.");
			void **cached = libraries.getptr(p_id);
			void *library = cached ? *cached : nullptr;
			if (!library) {
				String dll;
#ifdef ANDROID_ENABLED
				dll = "libcgd_native.so";
#else
				const String platform = os->get_name().to_lower();
				if (ProjectSettings::get_singleton()->is_using_datapack()) {
					const String directory = platform == "macos" ? "../Frameworks" : "cgd_native";
					dll = os->get_executable_path().get_base_dir().path_join(directory).path_join(platform == "windows" ? "cgd_native.dll" : platform == "macos" ? "libcgd_native.dylib"
																																								 : "libcgd_native.so")
								  .simplify_path();
				} else {
					const String arch = Engine::get_singleton()->get_architecture_name();
					const bool prebuilt = os->get_environment("GODOT_CGD_NATIVE_PREBUILT") == ProjectSettings::get_singleton()->get_resource_path();
					if (prebuilt) {
						const String cache = run_cache_path(platform, arch, p_id);
						dll = cached_library(cache, read_run_cache(cache), library_filename(platform, p_id));
					}
					if (dll.is_empty()) {
						Error read_error = OK;
						const String source = FileAccess::get_file_as_string(p_path, &read_error);
						ERR_FAIL_COND_V_MSG(read_error != OK, Variant(), "Cannot read CGD native source " + p_path + ".");
						String generated;
						String native_source;
						String native_id;
						GDC::SourceMap map;
						String error;
						int line = 1;
						int column = 1;
						const Error parse_error = GDCFrontend::transpile(source, generated, map, error, line, column, false, { 0, 0 }, p_path, &native_source, nullptr, &native_id);
						ERR_FAIL_COND_V_MSG(parse_error != OK || native_source.is_empty(), Variant(), vformat("%s:%d:%d: %s", p_path, line, column, error));
						ERR_FAIL_COND_V_MSG(native_id != p_id, Variant(), "CGD native source changed on disk. Save and reload " + p_path + ".");
						if (prebuilt) {
							print_line("Compiling CGD native C after reload: " + p_path);
						}
						lock.temp_unlock();
						const Error build_error = build(p_path, p_id, native_source, dll, error, platform, arch, true);
						lock.temp_relock();
						ERR_FAIL_COND_V_MSG(build_error != OK, Variant(), error);
					}
				}
#endif
				OS::GDExtensionData options;
				options.also_set_library_path = true;
#ifndef ANDROID_ENABLED
				ERR_FAIL_COND_V_MSG(!FileAccess::exists(dll), Variant(), "Cannot find CGD native library: " + dll);
#endif
#ifdef ANDROID_ENABLED
				const String library_key = dll;
#else
				const String library_key = ProjectSettings::get_singleton()->is_using_datapack() ? dll : id;
#endif
				if (void **loaded = libraries.getptr(library_key)) {
					library = *loaded;
				} else {
					ERR_FAIL_COND_V_MSG(os->open_dynamic_library(dll, library, &options) != OK, Variant(), "Cannot load CGD native library: " + dll);
					void *abi = nullptr;
					if (os->get_dynamic_library_symbol_handle(library, "cgd_native_abi_v3", abi, true) != OK ||
							!abi || reinterpret_cast<uint32_t (*)()>(abi)() != NATIVE_ABI_LAYOUT) {
						os->close_dynamic_library(library);
						ERR_FAIL_V_MSG(Variant(), "Incompatible CGD native ABI or engine precision. Rebuild the native library: " + dll);
					}
					libraries.insert(library_key, library);
				}
			}
			ERR_FAIL_COND_V_MSG(os->get_dynamic_library_symbol_handle(library, symbol_name, function.address) != OK, Variant(), "Missing CGD native function: " + symbol_name);
			function.signature = p_signature;
			function.signature_utf8 = signature;
		}
	}
	const CharString &signature = function.signature_utf8;
	ERR_FAIL_COND_V_MSG(function.signature != p_signature || signature.length() != p_args.size() + 1, Variant(), "Invalid CGD native function signature.");

	NativeValue stack_args[8];
	Vector<NativeValue> extra_args;
	PackedByteArray stack_byte_arrays[2];
	LocalVector<PackedByteArray> extra_byte_arrays;
	LocalVector<CharString> strings;
	int byte_array_count = 0;
	NativeValue *args = stack_args;
	if (p_args.size() > int(std::size(stack_args))) {
		ERR_FAIL_COND_V(extra_args.resize(p_args.size()) != OK, Variant());
		args = extra_args.ptrw();
	}
	for (int i = 0; i < p_args.size(); ++i) {
		NativeValue &value = args[i];
		switch (signature[i + 1]) {
			case 'i':
				value.integer = p_args[i];
				break;
			case 'f':
				value.floating = p_args[i];
				break;
			case 'b':
				value.boolean = p_args[i];
				break;
			case 'B': {
				const Variant &argument = p_args[i];
				ERR_FAIL_COND_V_MSG(argument.get_type() != Variant::PACKED_BYTE_ARRAY, Variant(), "Expected a PackedByteArray native argument.");
				PackedByteArray bytes = argument;
				value.span = { bytes.ptr(), bytes.size() };
				if (byte_array_count < int(std::size(stack_byte_arrays))) {
					stack_byte_arrays[byte_array_count++] = std::move(bytes);
				} else {
					extra_byte_arrays.push_back(std::move(bytes));
				}
			} break;
			case 'I':
			case 'L':
			case 'F':
			case 'D':
			case '2':
			case '3': {
				const Variant &argument = p_args[i];
				switch (signature[i + 1]) {
#define CGD_READ_ARRAY(letter, variant_type, array_type) \
	case letter: { \
		ERR_FAIL_COND_V_MSG(argument.get_type() != Variant::variant_type, Variant(), "Unexpected packed native argument type."); \
		const array_type array = argument; \
		value.span = { array.ptr(), array.size() }; \
	} break;
					CGD_READ_ARRAY('I', PACKED_INT32_ARRAY, PackedInt32Array)
					CGD_READ_ARRAY('L', PACKED_INT64_ARRAY, PackedInt64Array)
					CGD_READ_ARRAY('F', PACKED_FLOAT32_ARRAY, PackedFloat32Array)
					CGD_READ_ARRAY('D', PACKED_FLOAT64_ARRAY, PackedFloat64Array)
					CGD_READ_ARRAY('2', PACKED_VECTOR2_ARRAY, PackedVector2Array)
					CGD_READ_ARRAY('3', PACKED_VECTOR3_ARRAY, PackedVector3Array)
#undef CGD_READ_ARRAY
				}
			} break;
			case 's': {
				ERR_FAIL_COND_V_MSG(p_args[i].get_type() != Variant::STRING, Variant(), "Expected a String native argument.");
				strings.push_back(String(p_args[i]).utf8());
				const CharString &utf8 = strings[strings.size() - 1];
				value.span = { utf8.get_data(), utf8.length() };
			} break;
			default:
				ERR_FAIL_V_MSG(Variant(), "Invalid CGD native parameter type.");
		}
	}
	NativeValue result = {};
	NativeOutput output{ signature[0] };
	NativeAllocator allocator{ &output, allocate_native_output };
#ifdef ANDROID_ENABLED
	using NativeFunction = bool (*)(const char *, const NativeValue *, NativeValue *, NativeAllocator *, JNIEnv *, jobject);
	JNIEnv *env = get_jni_env();
	ERR_FAIL_NULL_V(env, Variant());
	ERR_FAIL_COND_V_MSG(env->PushLocalFrame(32) < 0, Variant(), "Cannot allocate a CGD native JNI frame.");
	jobject activity = OS_Android::get_singleton()->get_godot_java()->get_activity();
	const bool called = !env->ExceptionCheck() && reinterpret_cast<NativeFunction>(function.address)(signature.get_data(), args, &result, &allocator, env, activity);
	const bool exception = env->ExceptionCheck();
	if (exception) {
		env->ExceptionDescribe();
		env->ExceptionClear();
	}
	env->PopLocalFrame(nullptr);
	ERR_FAIL_COND_V_MSG(exception, Variant(), "CGD native Android call raised a Java exception.");
#else
	using NativeFunction = bool (*)(const char *, const NativeValue *, NativeValue *, NativeAllocator *);
	const bool called = reinterpret_cast<NativeFunction>(function.address)(signature.get_data(), args, &result, &allocator);
#endif
	ERR_FAIL_COND_V_MSG(!called, Variant(), "CGD native function signature mismatch.");
	if (!cache_hit) {
		MutexLock lock(mutex);
		functions[p_id].insert(p_symbol, function);
	}
	ERR_FAIL_COND_V_MSG(output.failed, Variant(), "CGD native output allocation failed.");
	switch (signature[0]) {
		case 's': {
			String value;
			if (!output.utf8.is_empty()) {
				ERR_FAIL_COND_V_MSG(value.append_utf8(reinterpret_cast<const char *>(output.utf8.ptr()), output.utf8.size()) != OK, Variant(), "CGD native String output is not valid UTF-8.");
			}
			return value;
		}
		case 'B':
		case 'I':
		case 'L':
		case 'F':
		case 'D':
		case '2':
		case '3':
			if (output.array.get_type() == Variant::NIL) {
				allocate_native_output(&output, 0);
			}
			return output.array;
		case 'i':
			return result.integer;
		case 'f':
			return result.floating;
		case 'b':
			return result.boolean;
		default:
			return Variant();
	}
}

void GDCNative::_bind_methods() {
	ClassDB::bind_method(D_METHOD("invoke_v3", "path", "library_id", "symbol", "signature", "args"), &GDCNative::invoke);
}

GDCNative::GDCNative() {
	singleton = this;
}

GDCNative::~GDCNative() {
#ifdef TOOLS_ENABLED
	if (completion_task != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(completion_task);
	}
	if (analysis_task != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(analysis_task);
	}
#endif
	for (const KeyValue<String, void *> &entry : libraries) {
		OS::get_singleton()->close_dynamic_library(entry.value);
	}
	singleton = nullptr;
}

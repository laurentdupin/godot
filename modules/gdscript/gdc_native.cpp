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

constexpr int NATIVE_BUILD_SCHEMA = 3;

union NativeValue {
	int64_t integer;
	double floating;
	bool boolean;
	struct {
		const uint8_t *data;
		int64_t size;
	} bytes;
};

constexpr uint32_t NATIVE_ABI_LAYOUT = (sizeof(real_t) * 8 << 16) | (sizeof(NativeValue) << 8) | alignof(NativeValue);

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

String input_stamp(const String &p_path) {
	return itos(FileAccess::get_size(p_path)) + ":" + itos(FileAccess::get_modified_time(p_path)) + ":" + FileAccess::get_sha256(p_path);
}

Error prepare_directory(EditorFileSystemDirectory *p_directory, const String &p_platform, const String &p_arch, String &r_error, HashMap<String, String> &r_stamps) {
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
		callable_mp(this, &GDCNative::finish_analysis).call_deferred(p_request.path, p_request.code, p_request.callback, diagnostics, symbols);
		return;
	}
	if (GDCFrontend::transpile(p_request.code, generated, map, error, line, column, false, { 0, 0 }, p_request.path, &native_source, nullptr, nullptr, &bodies) == OK &&
			!native_source.is_empty() && resolve_analysis_frontend(p_request.path, frontend, args, error)) {
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
				List<String> diagnostic_args(args);
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
					const bool own_source = entry.begins_with(prefix);
					if (own_source) {
						include_line = 0;
					}
					const int error_separator = own_source ? -1 : entry.find(": error: ");
					const int fatal_separator = own_source ? -1 : entry.find(": fatal error: ");
					const int warning_separator = own_source ? -1 : entry.find(": warning: ");
					const int separator = MAX(error_separator, MAX(fatal_separator, warning_separator));
					if (!own_source && (include_line <= 0 || separator < 0)) {
						continue;
					}
					const String location = own_source ? entry.substr(prefix.length()) : vformat("%d:%d%s", include_line, include_column, entry.substr(separator));
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
					for (const auto &body : bodies) {
						if (int(body.original.line) == diagnostic_line) {
							const int line_start = native_source.rfind_char('\n', int(body.offset) - 1) + 1;
							const int generated_column = native_source.substr(line_start, int(body.offset) - line_start).utf8().length() + 1;
							diagnostic_column += int(body.original.column) - generated_column;
							break;
						}
					}
					diagnostic["line"] = diagnostic_line;
					diagnostic["column"] = diagnostic_column;
					diagnostic["error"] = severity != "warning";
					diagnostic["message"] = own_source ? parts[3].strip_edges() : entry.strip_edges();
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

bool GDCNative::complete_code(const String &p_code, const String &p_path, List<ScriptLanguage::CodeCompletionOption> &r_options, String &r_call_hint) {
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
	if (GDCFrontend::transpile(source, generated, map, error, line, column, true, cursor, p_path, &native_source, &offset) != OK || offset < 0) {
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
	const String native_before = native_source.substr(0, offset);
	const String native_line = native_before.substr(native_before.rfind_char('\n') + 1);
	Error file_error = OK;
	Ref<DirAccess> workspace = DirAccess::create_temp("cgd-completion-" + itos(OS::get_singleton()->get_process_id()), false, &file_error);
	if (workspace.is_null()) {
		return true;
	}
	const String path = workspace->get_current_dir().path_join("completion.c");
	Ref<FileAccess> temporary = FileAccess::open(path, FileAccess::WRITE);
	if (temporary.is_null() || !temporary->store_string(native_source)) {
		return true;
	}
	temporary->close();
	const String location = vformat("-code-completion-at=%s:%d:%d", path, native_before.count("\n") + 1, native_line.utf8().length() + 1);
	const String passthrough = frontend.uses_msvc_flags() ? "/clang:" : "";
	for (const String &argument : { String("-Xclang"), location, String("-Xclang"), String("-code-completion-macros") }) {
		args.push_back(passthrough + argument);
	}
	args.push_back(path);
	String output;
	if (capture_compiler(frontend.compiler, args, output) != OK) {
		return true;
	}
	HashSet<String> seen;
	const bool suggest_arguments = offset != caret_offset;
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
	if (!r_call_hint.is_empty() && !suggest_arguments) {
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
	HashMap<String, String> stamps;
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

Error GDCNative::build(const String &p_path, const String &p_id, const String &p_native_source, String &r_dll, String &r_error, const String &p_platform, const String &p_arch, bool p_cache_for_run, HashMap<String, String> *p_run_stamps) {
#ifdef TOOLS_ENABLED
	const auto stamp = [p_run_stamps](const String &p_input) -> String {
		if (!p_run_stamps) {
			return input_stamp(p_input);
		}
		const String *previous = p_run_stamps->getptr(p_input);
		if (previous) {
			return *previous;
		}
		const String value = input_stamp(p_input);
		p_run_stamps->insert(p_input, value);
		return value;
	};
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
			for (const String &file : DirAccess::get_files_at(directory)) {
				const String path = directory.path_join(file);
				inputs[path] = stamp(path);
			}
		}
		configuration.push_back(inputs);
		fingerprint = String(Variant(configuration)).sha256_text();
		const Dictionary previous = read_run_cache(cache);
		const String previous_library = cached_library(cache, previous, library_name);
		if (!previous_library.is_empty() && previous.get("fingerprint", String()) == Variant(fingerprint)) {
			const Dictionary dependencies = previous.get("dependencies", Dictionary());
			bool current = previous.has("dependencies");
			for (const KeyValue<Variant, Variant> &dependency : dependencies) {
				const String path = dependency.key;
				if (!FileAccess::exists(path) || stamp(path) != String(dependency.value)) {
					current = false;
					break;
				}
			}
			if (current) {
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
				dependencies[path] = input_stamp(path);
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

Variant GDCNative::invoke(const String &p_path, const String &p_id, const String &p_symbol, const String &p_signature, const Array &p_args) {
	ERR_FAIL_COND_V_MSG(p_id.length() != 64 || !p_id.is_valid_hex_number(false), Variant(), "Invalid CGD native library identifier.");
	ERR_FAIL_COND_V_MSG(!p_symbol.begins_with("cgd_native_") || !p_symbol.substr(11).is_valid_int(), Variant(), "Invalid CGD native function identifier.");
	ERR_FAIL_COND_V_MSG(p_signature.length() != p_args.size() + 1, Variant(), "Invalid CGD native function signature.");

	OS *os = OS::get_singleton();
	void *symbol = nullptr;
	{
		MutexLock lock(mutex);
		// Libraries stay loaded until shutdown. Clear their function caches before any future unload.
		HashMap<String, void *> &library_functions = functions[p_id];
		void **cached_function = library_functions.getptr(p_symbol);
		if (cached_function) {
			symbol = *cached_function;
		} else {
			void **cached = libraries.getptr(p_id);
			void *library = cached ? *cached : nullptr;
			if (!library) {
				String dll;
#ifdef ANDROID_ENABLED
				dll = library_filename("android", p_id);
#else
				const String platform = os->get_name().to_lower();
				if (ProjectSettings::get_singleton()->is_using_datapack()) {
					const String directory = platform == "macos" ? "../Frameworks" : "cgd_native";
					dll = os->get_executable_path().get_base_dir().path_join(directory).path_join(library_filename(platform, p_id)).simplify_path();
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
				if (void **loaded = libraries.getptr(p_id)) {
					library = *loaded;
				} else {
					ERR_FAIL_COND_V_MSG(os->open_dynamic_library(dll, library, &options) != OK, Variant(), "Cannot load CGD native library: " + dll);
					void *abi = nullptr;
					if (os->get_dynamic_library_symbol_handle(library, "cgd_native_abi_v2", abi, true) != OK ||
							!abi || reinterpret_cast<uint32_t (*)()>(abi)() != NATIVE_ABI_LAYOUT) {
						os->close_dynamic_library(library);
						ERR_FAIL_V_MSG(Variant(), "Incompatible CGD native ABI or engine precision. Rebuild the native library: " + dll);
					}
					libraries.insert(p_id, library);
				}
			}
			ERR_FAIL_COND_V_MSG(os->get_dynamic_library_symbol_handle(library, p_symbol, symbol) != OK, Variant(), "Missing CGD native function: " + p_symbol);
			functions[p_id].insert(p_symbol, symbol);
		}
	}

	NativeValue stack_args[8];
	Vector<NativeValue> extra_args;
	LocalVector<PackedByteArray> byte_arrays;
	NativeValue *args = stack_args;
	if (p_args.size() > int(std::size(stack_args))) {
		ERR_FAIL_COND_V(extra_args.resize(p_args.size()) != OK, Variant());
		args = extra_args.ptrw();
	}
	for (int i = 0; i < p_args.size(); ++i) {
		NativeValue &value = args[i];
		switch (p_signature[i + 1]) {
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
				byte_arrays.push_back(PackedByteArray(argument));
				const PackedByteArray &bytes = byte_arrays[byte_arrays.size() - 1];
				value.bytes = { bytes.ptr(), bytes.size() };
			} break;
			default:
				ERR_FAIL_V_MSG(Variant(), "Invalid CGD native parameter type.");
		}
	}
	NativeValue result = {};
	const CharString signature = p_signature.utf8();
#ifdef ANDROID_ENABLED
	using NativeFunction = bool (*)(const char *, const NativeValue *, NativeValue *, JNIEnv *, jobject);
	JNIEnv *env = get_jni_env();
	ERR_FAIL_NULL_V(env, Variant());
	ERR_FAIL_COND_V_MSG(env->PushLocalFrame(32) < 0, Variant(), "Cannot allocate a CGD native JNI frame.");
	jobject activity = OS_Android::get_singleton()->get_godot_java()->get_activity();
	const bool called = !env->ExceptionCheck() && reinterpret_cast<NativeFunction>(symbol)(signature.get_data(), args, &result, env, activity);
	const bool exception = env->ExceptionCheck();
	if (exception) {
		env->ExceptionDescribe();
		env->ExceptionClear();
	}
	env->PopLocalFrame(nullptr);
	ERR_FAIL_COND_V_MSG(exception, Variant(), "CGD native Android call raised a Java exception.");
#else
	using NativeFunction = bool (*)(const char *, const NativeValue *, NativeValue *);
	const bool called = reinterpret_cast<NativeFunction>(symbol)(signature.get_data(), args, &result);
#endif
	ERR_FAIL_COND_V_MSG(!called, Variant(), "CGD native function signature mismatch.");
	switch (p_signature[0]) {
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
	ClassDB::bind_method(D_METHOD("invoke_v2", "path", "library_id", "symbol", "signature", "args"), &GDCNative::invoke);
}

GDCNative::GDCNative() {
	singleton = this;
}

GDCNative::~GDCNative() {
#ifdef TOOLS_ENABLED
	if (analysis_task != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(analysis_task);
	}
#endif
	for (const KeyValue<String, void *> &entry : libraries) {
		OS::get_singleton()->close_dynamic_library(entry.value);
	}
	singleton = nullptr;
}

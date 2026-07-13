/**************************************************************************/
/*  html_package_export_plugin.cpp                                        */
/**************************************************************************/

#include "html_package_export_plugin.h"

#ifdef TOOLS_ENABLED

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"

#include "hcsr_renderer.h"

void HTMLPackageExportPlugin::_export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) {
	debug_export = p_debug;
	_add_project_files("res://");
}

void HTMLPackageExportPlugin::_add_project_files(const String &p_directory) {
	for (const String &directory_name : DirAccess::get_directories_at(p_directory)) {
		if (!directory_name.begins_with(".")) {
			_add_project_files(p_directory.path_join(directory_name));
		}
	}

	for (const String &file_name : DirAccess::get_files_at(p_directory)) {
		const String source_path = p_directory.path_join(file_name);
		const String extension = file_name.get_extension().to_lower();
		if (extension != "html" && extension != "htm" && extension != "css") {
			continue;
		}
		if (debug_export) {
			const Vector<uint8_t> source = FileAccess::get_file_as_bytes(source_path);
			if (!source.is_empty()) {
				add_file(source_path, source, false);
			}
		} else if (extension == "html" || extension == "htm") {
			_compile_package(source_path);
		}
	}
}

void HTMLPackageExportPlugin::_compile_package(const String &p_path) {
	const String source_path = ProjectSettings::get_singleton()->globalize_path(p_path);
	const String source_root = ProjectSettings::get_singleton()->globalize_path("res://");
	const CharString source_path_utf8 = source_path.utf8();
	const CharString source_root_utf8 = source_root.utf8();
	uint8_t *package_bytes = nullptr;
	uint64_t package_byte_count = 0;
	const hcsr_status_t status = hcsr_compile_document_package(
			source_path_utf8.ptr(),
			source_root_utf8.ptr(),
			&package_bytes,
			&package_byte_count);
	if (status != HCSR_STATUS_OK || package_bytes == nullptr || package_byte_count == 0 || package_byte_count > INT32_MAX) {
		ERR_PRINT(vformat("Could not compile '%s' into an HCSR release package.", p_path));
		if (package_bytes != nullptr) {
			hcsr_release_document_package(package_bytes);
		}
		return;
	}

	Vector<uint8_t> package;
	package.resize((int)package_byte_count);
	memcpy(package.ptrw(), package_bytes, (size_t)package_byte_count);
	hcsr_release_document_package(package_bytes);
	add_file(p_path.get_basename() + ".hcsrpkg", package, false);
}

void HTMLPackageExportPlugin::_export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) {
	const String extension = p_path.get_extension().to_lower();
	if (!debug_export && (extension == "html" || extension == "htm" || extension == "css")) {
		skip();
	}
}

#endif

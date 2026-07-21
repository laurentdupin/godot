/**************************************************************************/
/*  html_package_export_plugin.cpp                                        */
/**************************************************************************/

#include "html_package_export_plugin.h"

#ifdef TOOLS_ENABLED

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"

#include "../html_document.h"

#include "hcsr_renderer.h"

void HTMLPackageExportPlugin::_export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) {
	debug_export = p_debug;
	compiled_documents.clear();
}

bool HTMLPackageExportPlugin::_compile_package(const String &p_path) {
	if (compiled_documents.has(p_path)) {
		return true;
	}
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
		const String message = vformat("Could not compile '%s' into an HCSR release package.", p_path);
		ERR_PRINT(message);
		get_export_platform()->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("HCSR HTML package"), message);
		if (package_bytes != nullptr) {
			hcsr_release_document_package(package_bytes);
		}
		fail_export(ERR_PARSE_ERROR);
		return false;
	}

	Vector<uint8_t> package;
	package.resize((int)package_byte_count);
	memcpy(package.ptrw(), package_bytes, (size_t)package_byte_count);
	hcsr_release_document_package(package_bytes);
	add_file(p_path.get_basename() + ".hcsrpkg", package, false);
	compiled_documents.insert(p_path);
	return true;
}

bool HTMLPackageExportPlugin::_is_complete_html_document(const String &p_path) {
	const String source = FileAccess::get_file_as_string(p_path).strip_edges();
	if (source.is_empty()) {
		return false;
	}
	const String lower_source = source.to_lower();
	return lower_source.begins_with("<html") ||
			(lower_source.begins_with("<!doctype html") && lower_source.find("<html") >= 0);
}

void HTMLPackageExportPlugin::_export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) {
	if (debug_export) {
		return;
	}
	const String extension = p_path.get_extension().to_lower();
	if ((extension == "html" || extension == "htm") && _is_complete_html_document(p_path)) {
		_compile_package(p_path);
		return;
	}
	if (p_type != HTMLDocument::get_class_static()) {
		return;
	}

	Ref<HTMLDocument> document = ResourceLoader::load(p_path, HTMLDocument::get_class_static());
	if (document.is_null()) {
		const String message = vformat("Could not load exported HTMLDocument resource '%s'.", p_path);
		ERR_PRINT(message);
		get_export_platform()->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("HCSR HTML package"), message);
		fail_export(ERR_CANT_OPEN);
		return;
	}

	const String html_file = document->get_html_file();
	if (!html_file.is_empty()) {
		_compile_package(html_file);
	}
}

void HTMLPackageExportPlugin::_export_end() {
	compiled_documents.clear();
}

#endif

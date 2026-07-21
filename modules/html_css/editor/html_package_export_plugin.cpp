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
	inspected_resources.clear();
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

static String _extract_html_resource_literal(const String &p_text) {
	for (int quote_index = 0; quote_index < p_text.length(); quote_index++) {
		const char32_t quote = p_text[quote_index];
		if (quote != '\'' && quote != '"') {
			continue;
		}
		const int end_quote = p_text.find_char(quote, quote_index + 1);
		if (end_quote < 0) {
			return String();
		}
		const String candidate = p_text.substr(quote_index + 1, end_quote - quote_index - 1);
		const String extension = candidate.get_extension().to_lower();
		if (candidate.begins_with("res://") && (extension == "html" || extension == "htm")) {
			return candidate;
		}
		quote_index = end_quote;
	}
	return String();
}

static String _extract_local_resource_literal(const String &p_text) {
	for (int quote_index = 0; quote_index < p_text.length(); quote_index++) {
		const char32_t quote = p_text[quote_index];
		if (quote != '\'' && quote != '"') {
			continue;
		}
		const int end_quote = p_text.find_char(quote, quote_index + 1);
		if (end_quote < 0) {
			return String();
		}
		const String candidate = p_text.substr(quote_index + 1, end_quote - quote_index - 1);
		if (candidate.begins_with("res://")) {
			return candidate;
		}
		quote_index = end_quote;
	}
	return String();
}

static String _extract_identifier(const String &p_text) {
	const String text = p_text.strip_edges();
	int length = 0;
	while (length < text.length()) {
		const char32_t character = text[length];
		if (!(character == '_' || (character >= 'a' && character <= 'z') ||
				(character >= 'A' && character <= 'Z') || (length > 0 && character >= '0' && character <= '9'))) {
			break;
		}
		length++;
	}
	return text.left(length);
}

void HTMLPackageExportPlugin::_compile_script_document_entries(const String &p_path) {
	const String source = FileAccess::get_file_as_string(p_path);
	const PackedStringArray lines = source.split("\n");
	HashMap<String, String> html_path_constants;

	for (const String &raw_line : lines) {
		const String line = raw_line.strip_edges();
		if (!line.begins_with("const ") && !line.begins_with("var ")) {
			continue;
		}
		int assignment = line.find(":=");
		if (assignment < 0) {
			assignment = line.find("=");
		}
		if (assignment < 0) {
			continue;
		}
		String declaration = line.substr(line.find(" ") + 1, assignment - line.find(" ") - 1).strip_edges();
		const int type_separator = declaration.find(":");
		if (type_separator >= 0) {
			declaration = declaration.left(type_separator).strip_edges();
		}
		const String path = _extract_html_resource_literal(line.substr(assignment + (line[assignment] == ':' ? 2 : 1)));
		if (!declaration.is_empty() && !path.is_empty()) {
			html_path_constants.insert(declaration, path);
		}
	}

	for (const String &raw_line : lines) {
		const String line = raw_line.strip_edges();
		int value_start = -1;
		const int property = line.find(".html_file");
		if (property >= 0) {
			const int assignment = line.find("=", property + 10);
			if (assignment >= 0) {
				value_start = assignment + 1;
			}
		} else {
			const int setter = line.find(".set_html_file(");
			if (setter >= 0) {
				value_start = setter + 15;
			}
		}
		if (value_start < 0) {
			continue;
		}
		const String value = line.substr(value_start);
		String path = _extract_html_resource_literal(value);
		if (path.is_empty()) {
			const String identifier = _extract_identifier(value);
			const String *constant_path = html_path_constants.getptr(identifier);
			if (constant_path != nullptr) {
				path = *constant_path;
			}
		}
		if (!path.is_empty()) {
			_compile_package(path);
		}
	}
}

void HTMLPackageExportPlugin::_inspect_resource_dependencies(const String &p_path) {
	if (inspected_resources.has(p_path)) {
		return;
	}
	inspected_resources.insert(p_path);
	const String extension = p_path.get_extension().to_lower();
	if (extension == "gd") {
		_compile_script_document_entries(p_path);
		const PackedStringArray lines = FileAccess::get_file_as_string(p_path).split("\n");
		for (const String &line : lines) {
			int load_call = line.find("preload(");
			if (load_call < 0) {
				load_call = line.find("load(");
			}
			if (load_call >= 0) {
				const String dependency = _extract_local_resource_literal(line.substr(load_call));
				if (!dependency.is_empty()) {
					_inspect_resource_dependencies(dependency);
				}
			}
		}
	}
	if (extension != "gd" && extension != "tscn" && extension != "tres" && extension != "scn" && extension != "res") {
		return;
	}
	List<String> dependencies;
	ResourceLoader::get_dependencies(p_path, &dependencies);
	for (const String &dependency : dependencies) {
		_inspect_resource_dependencies(dependency);
	}
}

void HTMLPackageExportPlugin::_export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) {
	if (debug_export) {
		return;
	}
	_inspect_resource_dependencies(p_path);
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
	inspected_resources.clear();
}

#endif

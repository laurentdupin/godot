/**************************************************************************/
/*  html_asset_provider.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "html_asset_provider.h"

#include "core/io/file_access.h"

static bool _is_local_asset_path(const String &p_path) {
	return p_path.begins_with("res://") || p_path.begins_with("user://");
}

static void _set_error(String *r_error, const String &p_message) {
	if (r_error != nullptr) {
		*r_error = p_message;
	}
}

static String _strip_uri_suffixes(const String &p_uri) {
	int end = p_uri.length();
	const int query = p_uri.find("?");
	const int fragment = p_uri.find("#");
	if (query >= 0) {
		end = MIN(end, query);
	}
	if (fragment >= 0) {
		end = MIN(end, fragment);
	}
	return p_uri.substr(0, end);
}

static bool _has_uri_scheme(const String &p_uri) {
	const int colon = p_uri.find(":");
	if (colon <= 0) {
		return false;
	}

	for (int i = 0; i < colon; i++) {
		const char32_t c = p_uri[i];
		const bool valid_scheme_char = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
		if (!valid_scheme_char) {
			return false;
		}
	}
	return true;
}

static String _get_document_base_path(const Ref<HTMLDocument> &p_document) {
	if (p_document.is_valid()) {
		const String html_file = p_document->get_html_file();
		if (!html_file.is_empty() && _is_local_asset_path(html_file)) {
			return html_file.get_base_dir();
		}

		const String resource_root = p_document->get_resource_root();
		if (!resource_root.is_empty()) {
			return resource_root;
		}
	}

	return "res://";
}

Error HTMLGodotAssetProvider::resolve_asset_path(const Ref<HTMLDocument> &p_document, const String &p_uri, String &r_path, String *r_error) {
	r_path = String();

	const String uri = _strip_uri_suffixes(p_uri.strip_edges());
	if (uri.is_empty()) {
		_set_error(r_error, "HTMLDocument resource URI cannot be empty.");
		return ERR_INVALID_PARAMETER;
	}

	String resolved_path;
	if (_is_local_asset_path(uri)) {
		resolved_path = uri;
	} else if (_has_uri_scheme(uri) || uri.begins_with("//")) {
		_set_error(r_error, "External URL schemes are not supported by HTMLDocument resources.");
		return ERR_UNAVAILABLE;
	} else {
		const String base_path = _get_document_base_path(p_document);
		const String relative_path = uri.begins_with("/") ? uri.substr(1) : uri;
		resolved_path = base_path.path_join(relative_path).simplify_path();
	}

	if (!_is_local_asset_path(resolved_path)) {
		_set_error(r_error, "Only res:// and user:// resource paths are supported.");
		return ERR_INVALID_PARAMETER;
	}

	r_path = resolved_path;
	return OK;
}

Error HTMLGodotAssetProvider::load_asset(const Ref<HTMLDocument> &p_document, const String &p_uri, HTMLAssetResource &r_asset, String *r_error) {
	r_asset = HTMLAssetResource();

	String path;
	Error err = resolve_asset_path(p_document, p_uri, path, r_error);
	if (err != OK) {
		return err;
	}

	Error file_error = OK;
	Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(path, &file_error);
	if (file_error != OK) {
		_set_error(r_error, vformat("Could not load HTMLDocument resource '%s'.", path));
		return file_error;
	}

	r_asset.path = path;
	r_asset.mime_type = get_mime_type_for_path(path);
	r_asset.bytes = bytes;
	return OK;
}

String HTMLGodotAssetProvider::get_mime_type_for_path(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	if (extension == "html" || extension == "htm") {
		return "text/html";
	}
	if (extension == "css") {
		return "text/css";
	}
	if (extension == "png") {
		return "image/png";
	}
	if (extension == "jpg" || extension == "jpeg") {
		return "image/jpeg";
	}
	if (extension == "webp") {
		return "image/webp";
	}
	if (extension == "gif") {
		return "image/gif";
	}
	if (extension == "svg") {
		return "image/svg+xml";
	}
	if (extension == "ttf") {
		return "font/ttf";
	}
	if (extension == "otf") {
		return "font/otf";
	}
	if (extension == "woff") {
		return "font/woff";
	}
	if (extension == "woff2") {
		return "font/woff2";
	}
	if (extension == "json") {
		return "application/json";
	}
	if (extension == "txt") {
		return "text/plain";
	}
	return "application/octet-stream";
}

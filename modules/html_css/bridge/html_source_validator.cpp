/**************************************************************************/
/*  html_source_validator.cpp                                             */
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

#include "html_source_validator.h"

static bool _is_attribute_boundary(char32_t p_char) {
	return p_char == 0 || p_char == '<' || p_char == '>' || p_char == '/' || p_char == '"' || p_char == '\'' || p_char == '=' || p_char <= 32;
}

static bool _has_inline_event_attribute(const String &p_lower_source) {
	const int length = p_lower_source.length();
	for (int i = 0; i < length - 2; i++) {
		if (p_lower_source[i] != 'o' || p_lower_source[i + 1] != 'n') {
			continue;
		}
		if (i > 0 && !_is_attribute_boundary(p_lower_source[i - 1])) {
			continue;
		}

		int cursor = i + 2;
		while (cursor < length) {
			const char32_t c = p_lower_source[cursor];
			if ((c >= 'a' && c <= 'z') || c == '-' || c == '_') {
				cursor++;
				continue;
			}
			break;
		}
		if (cursor == i + 2) {
			continue;
		}

		int whitespace = cursor;
		while (whitespace < length && p_lower_source[whitespace] <= 32) {
			whitespace++;
		}
		if (whitespace < length && p_lower_source[whitespace] == '=') {
			return true;
		}
	}
	return false;
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

PackedStringArray HTMLSourceValidator::validate_inline_source(const String &p_source) {
	PackedStringArray errors;
	if (p_source.is_empty()) {
		return errors;
	}

	const String lower_source = p_source.to_lower();
	if (lower_source.find("<script") != -1) {
		errors.push_back("Script elements are not supported by HTMLDocument.");
	}
	if (_has_inline_event_attribute(lower_source)) {
		errors.push_back("Inline event attributes are not supported by HTMLDocument.");
	}
	if (lower_source.find("javascript:") != -1) {
		errors.push_back("Executable URL schemes are not supported by HTMLDocument.");
	}
	if (lower_source.find(".wasm") != -1 || lower_source.find("application/wasm") != -1) {
		errors.push_back("WebAssembly resources are not supported by HTMLDocument.");
	}

	return errors;
}

PackedStringArray HTMLSourceValidator::validate_inline_css(const String &p_css) {
	PackedStringArray errors;
	if (p_css.is_empty()) {
		return errors;
	}

	const String lower_css = p_css.to_lower();
	if (lower_css.find("<script") != -1) {
		errors.push_back("Script elements are not supported in HTMLDocument CSS.");
	}
	if (lower_css.find("javascript:") != -1) {
		errors.push_back("Executable URL schemes are not supported in HTMLDocument CSS.");
	}
	if (lower_css.find(".wasm") != -1 || lower_css.find("application/wasm") != -1) {
		errors.push_back("WebAssembly resources are not supported in HTMLDocument CSS.");
	}

	return errors;
}

PackedStringArray HTMLSourceValidator::validate_resource_path(const String &p_path) {
	PackedStringArray errors;
	if (p_path.is_empty() || p_path.begins_with("res://") || p_path.begins_with("user://")) {
		return errors;
	}

	errors.push_back("Only res:// and user:// document paths are supported.");
	return errors;
}

PackedStringArray HTMLSourceValidator::validate_resource_uri(const String &p_uri) {
	PackedStringArray errors;
	const String uri = p_uri.strip_edges();
	if (uri.is_empty() || uri.begins_with("res://") || uri.begins_with("user://")) {
		return errors;
	}

	if (_has_uri_scheme(uri) || uri.begins_with("//")) {
		errors.push_back("External URL schemes are not supported by HTMLDocument resources.");
	}

	return errors;
}

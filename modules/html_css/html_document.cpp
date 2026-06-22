/**************************************************************************/
/*  html_document.cpp                                                     */
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

#include "html_document.h"

#include "bridge/html_source_validator.h"

#include "core/object/class_db.h"

void HTMLDocument::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_html", "html"), &HTMLDocument::set_html);
	ClassDB::bind_method(D_METHOD("get_html"), &HTMLDocument::get_html);
	ClassDB::bind_method(D_METHOD("set_html_file", "html_file"), &HTMLDocument::set_html_file);
	ClassDB::bind_method(D_METHOD("get_html_file"), &HTMLDocument::get_html_file);
	ClassDB::bind_method(D_METHOD("set_resource_root", "resource_root"), &HTMLDocument::set_resource_root);
	ClassDB::bind_method(D_METHOD("get_resource_root"), &HTMLDocument::get_resource_root);
	ClassDB::bind_method(D_METHOD("set_default_size", "default_size"), &HTMLDocument::set_default_size);
	ClassDB::bind_method(D_METHOD("get_default_size"), &HTMLDocument::get_default_size);
	ClassDB::bind_method(D_METHOD("set_transparent_background", "transparent_background"), &HTMLDocument::set_transparent_background);
	ClassDB::bind_method(D_METHOD("is_transparent_background"), &HTMLDocument::is_transparent_background);
	ClassDB::bind_method(D_METHOD("is_source_valid"), &HTMLDocument::is_source_valid);
	ClassDB::bind_method(D_METHOD("get_source_errors"), &HTMLDocument::get_source_errors);
	ClassDB::bind_method(D_METHOD("reload"), &HTMLDocument::reload);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "html", PROPERTY_HINT_MULTILINE_TEXT), "set_html", "get_html");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "html_file", PROPERTY_HINT_FILE, "*.html,*.htm"), "set_html_file", "get_html_file");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "resource_root", PROPERTY_HINT_DIR), "set_resource_root", "get_resource_root");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "default_size", PROPERTY_HINT_RANGE, "1,16384,1,or_greater,suffix:px"), "set_default_size", "get_default_size");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "transparent_background"), "set_transparent_background", "is_transparent_background");
}

bool HTMLDocument::_validate_source() {
	PackedStringArray new_errors = HTMLSourceValidator::validate_inline_source(html);

	PackedStringArray path_errors = HTMLSourceValidator::validate_resource_path(html_file);
	for (const String &error : path_errors) {
		new_errors.push_back(error);
	}

	PackedStringArray root_errors = HTMLSourceValidator::validate_resource_path(resource_root);
	for (const String &error : root_errors) {
		new_errors.push_back("Resource root: " + error);
	}

	if (source_errors == new_errors) {
		return false;
	}

	source_errors = new_errors;
	return true;
}

void HTMLDocument::set_html(const String &p_html) {
	if (html == p_html) {
		return;
	}
	html = p_html;
	_validate_source();
	emit_changed();
}

String HTMLDocument::get_html() const {
	return html;
}

void HTMLDocument::set_html_file(const String &p_html_file) {
	if (html_file == p_html_file) {
		return;
	}
	html_file = p_html_file;
	_validate_source();
	emit_changed();
}

String HTMLDocument::get_html_file() const {
	return html_file;
}

void HTMLDocument::set_resource_root(const String &p_resource_root) {
	if (resource_root == p_resource_root) {
		return;
	}
	resource_root = p_resource_root;
	_validate_source();
	emit_changed();
}

String HTMLDocument::get_resource_root() const {
	return resource_root;
}

void HTMLDocument::set_default_size(const Size2i &p_default_size) {
	Size2i new_size = Size2i(MAX(1, p_default_size.x), MAX(1, p_default_size.y));
	if (default_size == new_size) {
		return;
	}
	default_size = new_size;
	emit_changed();
}

Size2i HTMLDocument::get_default_size() const {
	return default_size;
}

void HTMLDocument::set_transparent_background(bool p_transparent_background) {
	if (transparent_background == p_transparent_background) {
		return;
	}
	transparent_background = p_transparent_background;
	emit_changed();
}

bool HTMLDocument::is_transparent_background() const {
	return transparent_background;
}

bool HTMLDocument::is_source_valid() const {
	return source_errors.is_empty();
}

PackedStringArray HTMLDocument::get_source_errors() const {
	return source_errors;
}

void HTMLDocument::reload() {
	_validate_source();
	emit_changed();
}

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
	ClassDB::bind_method(D_METHOD("set_css", "css"), &HTMLDocument::set_css);
	ClassDB::bind_method(D_METHOD("get_css"), &HTMLDocument::get_css);
	ClassDB::bind_method(D_METHOD("set_html_file", "html_file"), &HTMLDocument::set_html_file);
	ClassDB::bind_method(D_METHOD("get_html_file"), &HTMLDocument::get_html_file);
	ClassDB::bind_method(D_METHOD("set_css_files", "css_files"), &HTMLDocument::set_css_files);
	ClassDB::bind_method(D_METHOD("get_css_files"), &HTMLDocument::get_css_files);
	ClassDB::bind_method(D_METHOD("add_css_file", "css_file"), &HTMLDocument::add_css_file);
	ClassDB::bind_method(D_METHOD("remove_css_file", "css_file"), &HTMLDocument::remove_css_file);
	ClassDB::bind_method(D_METHOD("clear_css_files"), &HTMLDocument::clear_css_files);
	ClassDB::bind_method(D_METHOD("set_resource_root", "resource_root"), &HTMLDocument::set_resource_root);
	ClassDB::bind_method(D_METHOD("get_resource_root"), &HTMLDocument::get_resource_root);
	ClassDB::bind_method(D_METHOD("set_default_size", "default_size"), &HTMLDocument::set_default_size);
	ClassDB::bind_method(D_METHOD("get_default_size"), &HTMLDocument::get_default_size);
	ClassDB::bind_method(D_METHOD("set_background_color", "background_color"), &HTMLDocument::set_background_color);
	ClassDB::bind_method(D_METHOD("get_background_color"), &HTMLDocument::get_background_color);
	ClassDB::bind_method(D_METHOD("set_transparent_background", "transparent_background"), &HTMLDocument::set_transparent_background);
	ClassDB::bind_method(D_METHOD("is_transparent_background"), &HTMLDocument::is_transparent_background);
	ClassDB::bind_method(D_METHOD("is_source_valid"), &HTMLDocument::is_source_valid);
	ClassDB::bind_method(D_METHOD("get_source_errors"), &HTMLDocument::get_source_errors);
	ClassDB::bind_method(D_METHOD("reload"), &HTMLDocument::reload);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "html", PROPERTY_HINT_MULTILINE_TEXT), "set_html", "get_html");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "css", PROPERTY_HINT_MULTILINE_TEXT), "set_css", "get_css");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "html_file", PROPERTY_HINT_FILE, "*.html,*.htm"), "set_html_file", "get_html_file");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "css_files"), "set_css_files", "get_css_files");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "resource_root", PROPERTY_HINT_DIR), "set_resource_root", "get_resource_root");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "default_size", PROPERTY_HINT_RANGE, "1,16384,1,or_greater,suffix:px"), "set_default_size", "get_default_size");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "background_color"), "set_background_color", "get_background_color");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "transparent_background"), "set_transparent_background", "is_transparent_background");
}

bool HTMLDocument::_validate_source() {
	PackedStringArray new_errors = HTMLSourceValidator::validate_inline_source(html);

	PackedStringArray css_errors = HTMLSourceValidator::validate_inline_css(css);
	for (const String &error : css_errors) {
		new_errors.push_back("Inline CSS: " + error);
	}

	PackedStringArray path_errors = HTMLSourceValidator::validate_resource_path(html_file);
	for (const String &error : path_errors) {
		new_errors.push_back(error);
	}

	for (const String &css_file : css_files) {
		PackedStringArray css_file_errors = HTMLSourceValidator::validate_resource_uri(css_file);
		for (const String &error : css_file_errors) {
			new_errors.push_back(vformat("CSS file '%s': %s", css_file, error));
		}
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

void HTMLDocument::set_css(const String &p_css) {
	if (css == p_css) {
		return;
	}
	css = p_css;
	_validate_source();
	emit_changed();
}

String HTMLDocument::get_css() const {
	return css;
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

void HTMLDocument::set_css_files(const PackedStringArray &p_css_files) {
	PackedStringArray new_css_files;
	for (const String &css_file : p_css_files) {
		const String stripped = css_file.strip_edges();
		if (!stripped.is_empty()) {
			new_css_files.push_back(stripped);
		}
	}

	if (css_files == new_css_files) {
		return;
	}
	css_files = new_css_files;
	_validate_source();
	emit_changed();
}

PackedStringArray HTMLDocument::get_css_files() const {
	return css_files;
}

void HTMLDocument::add_css_file(const String &p_css_file) {
	const String stripped = p_css_file.strip_edges();
	if (stripped.is_empty()) {
		return;
	}

	PackedStringArray new_css_files = css_files;
	new_css_files.push_back(stripped);
	set_css_files(new_css_files);
}

void HTMLDocument::remove_css_file(const String &p_css_file) {
	const String stripped = p_css_file.strip_edges();
	PackedStringArray new_css_files;
	for (const String &css_file : css_files) {
		if (css_file != stripped) {
			new_css_files.push_back(css_file);
		}
	}
	set_css_files(new_css_files);
}

void HTMLDocument::clear_css_files() {
	set_css_files(PackedStringArray());
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

void HTMLDocument::set_background_color(const Color &p_background_color) {
	if (background_color == p_background_color) {
		return;
	}
	background_color = p_background_color;
	emit_changed();
}

Color HTMLDocument::get_background_color() const {
	return background_color;
}

void HTMLDocument::set_transparent_background(bool p_transparent_background) {
	Color new_color = background_color;
	new_color.a = p_transparent_background ? 0.0 : 1.0;
	if (background_color == new_color) {
		return;
	}
	background_color = new_color;
	emit_changed();
}

bool HTMLDocument::is_transparent_background() const {
	return background_color.a < 1.0;
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

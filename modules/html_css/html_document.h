/**************************************************************************/
/*  html_document.h                                                       */
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

#pragma once

#include "core/io/resource.h"
#include "core/variant/variant.h"

class HTMLDocument : public Resource {
	GDCLASS(HTMLDocument, Resource);
	RES_BASE_EXTENSION("htmlcss");

	String html;
	String css;
	String html_file;
	PackedStringArray css_files;
	String resource_root = "res://";
	Size2i default_size = Size2i(512, 512);
	Color background_color = Color(0, 0, 0, 0);
	PackedStringArray source_errors;

	bool _validate_source();

protected:
	static void _bind_methods();

public:
	void set_html(const String &p_html);
	String get_html() const;

	void set_css(const String &p_css);
	String get_css() const;

	void set_html_file(const String &p_html_file);
	String get_html_file() const;

	void set_css_files(const PackedStringArray &p_css_files);
	PackedStringArray get_css_files() const;
	void add_css_file(const String &p_css_file);
	void remove_css_file(const String &p_css_file);
	void clear_css_files();

	void set_resource_root(const String &p_resource_root);
	String get_resource_root() const;

	void set_default_size(const Size2i &p_default_size);
	Size2i get_default_size() const;

	void set_background_color(const Color &p_background_color);
	Color get_background_color() const;

	void set_transparent_background(bool p_transparent_background);
	bool is_transparent_background() const;

	bool is_source_valid() const;
	PackedStringArray get_source_errors() const;

	void reload();
};

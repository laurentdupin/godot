/**************************************************************************/
/*  test_html_document.h                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
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
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE       */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "../html_document.h"

#include "tests/test_macros.h"

namespace TestHTMLDocument {

TEST_CASE("[HTMLCSS] CSS file list preserves empty Inspector placeholders") {
	Ref<HTMLDocument> document;
	document.instantiate();

	document->set_css_files(PackedStringArray({ "", "  res://theme.css  ", "   " }));

	const PackedStringArray css_files = document->get_css_files();
	REQUIRE(css_files.size() == 3);
	CHECK(css_files[0].is_empty());
	CHECK(css_files[1] == "res://theme.css");
	CHECK(css_files[2].is_empty());
	CHECK(document->is_source_valid());
}

TEST_CASE("[HTMLCSS] CSS file helper methods retain their empty-path safeguards") {
	Ref<HTMLDocument> document;
	document.instantiate();

	document->add_css_file("   ");
	CHECK(document->get_css_files().is_empty());

	document->set_css_files(PackedStringArray({ "", "res://theme.css", "" }));
	document->remove_css_file("");
	CHECK(document->get_css_files() == PackedStringArray({ "res://theme.css" }));
}

} // namespace TestHTMLDocument

/**************************************************************************/
/*  html_render_surface.h                                                 */
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

#include "backend/html_surface_backend.h"
#include "html_document.h"

#include "core/object/ref_counted.h"
#include "core/variant/callable.h"

enum HTMLSurfaceBackendPreference {
	HTML_SURFACE_BACKEND_AUTO,
	HTML_SURFACE_BACKEND_CPU,
};

class HTMLRenderSurface : public RefCounted {
	Ref<HTMLDocument> document;
	HTMLSurfaceBackend *backend = nullptr;
	Size2i size = Size2i(512, 512);
	Color placeholder_background = Color(0.08, 0.09, 0.1, 1.0);
	String marker = "HTML";
	HTMLSurfaceBackendPreference backend_preference = HTML_SURFACE_BACKEND_AUTO;
	Callable changed_callback;

	void _ensure_backend();
	void _sync_backend_state();
	void _document_changed();
	void _notify_changed() const;

public:
	void set_document(const Ref<HTMLDocument> &p_document);
	Ref<HTMLDocument> get_document() const;

	void set_size(const Size2i &p_size);
	Size2i get_size() const;

	void set_placeholder_background(const Color &p_color);

	void set_backend_preference(HTMLSurfaceBackendPreference p_backend_preference);
	HTMLSurfaceBackendPreference get_backend_preference() const;

	void set_changed_callback(const Callable &p_callback);
	void render_now(const String &p_marker);
	Ref<Texture2D> get_texture() const;
	Ref<HTMLTexture2D> get_html_texture() const;

	HTMLRenderSurface();
	~HTMLRenderSurface();
};

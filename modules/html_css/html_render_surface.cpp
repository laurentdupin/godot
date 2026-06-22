/**************************************************************************/
/*  html_render_surface.cpp                                               */
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

#include "html_render_surface.h"

#include "backend/html_surface_cpu_backend.h"

#include "core/object/callable_mp.h"

void HTMLRenderSurface::_ensure_backend() {
	if (backend != nullptr) {
		return;
	}

	backend = memnew(HTMLSurfaceCPUBackend);
	_sync_backend_state();
}

void HTMLRenderSurface::_sync_backend_state() {
	ERR_FAIL_NULL(backend);
	backend->set_size(size);
	backend->set_transparent_background(document.is_valid() && document->is_transparent_background());
	backend->set_placeholder_background(placeholder_background);
}

void HTMLRenderSurface::_document_changed() {
	_sync_backend_state();
	render_now(marker);
}

void HTMLRenderSurface::_notify_changed() const {
	if (!changed_callback.is_valid()) {
		return;
	}

	Variant ret;
	Callable::CallError ce;
	changed_callback.callp(nullptr, 0, ret, ce);
	if (ce.error != Callable::CallError::CALL_OK) {
		ERR_PRINT(vformat("HTML render surface changed callback failed with error %d.", ce.error));
	}
}

void HTMLRenderSurface::set_document(const Ref<HTMLDocument> &p_document) {
	if (document == p_document) {
		return;
	}
	if (document.is_valid()) {
		document->disconnect_changed(callable_mp(this, &HTMLRenderSurface::_document_changed));
	}
	document = p_document;
	if (document.is_valid()) {
		document->connect_changed(callable_mp(this, &HTMLRenderSurface::_document_changed));
	}
	_sync_backend_state();
	render_now(marker);
}

Ref<HTMLDocument> HTMLRenderSurface::get_document() const {
	return document;
}

void HTMLRenderSurface::set_size(const Size2i &p_size) {
	Size2i new_size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
	if (size == new_size) {
		return;
	}
	size = new_size;
	_sync_backend_state();
	render_now(marker);
}

Size2i HTMLRenderSurface::get_size() const {
	return size;
}

void HTMLRenderSurface::set_placeholder_background(const Color &p_color) {
	if (placeholder_background == p_color) {
		return;
	}
	placeholder_background = p_color;
	_sync_backend_state();
	render_now(marker);
}

void HTMLRenderSurface::set_backend_preference(HTMLSurfaceBackendPreference p_backend_preference) {
	ERR_FAIL_INDEX((int)p_backend_preference, 2);
	if (backend_preference == p_backend_preference) {
		return;
	}
	backend_preference = p_backend_preference;
	// AUTO and CPU intentionally select the same portable backend for now.
	_sync_backend_state();
	render_now(marker);
}

HTMLSurfaceBackendPreference HTMLRenderSurface::get_backend_preference() const {
	return backend_preference;
}

void HTMLRenderSurface::set_changed_callback(const Callable &p_callback) {
	changed_callback = p_callback;
}

void HTMLRenderSurface::render_now(const String &p_marker) {
	marker = p_marker;
	_ensure_backend();
	_sync_backend_state();
	backend->render_placeholder(marker);
	_notify_changed();
}

Ref<Texture2D> HTMLRenderSurface::get_texture() const {
	return backend != nullptr ? backend->get_texture() : Ref<Texture2D>();
}

Ref<HTMLTexture2D> HTMLRenderSurface::get_html_texture() const {
	return backend != nullptr ? backend->get_html_texture() : Ref<HTMLTexture2D>();
}

HTMLRenderSurface::HTMLRenderSurface() {
	_ensure_backend();
	render_now(marker);
}

HTMLRenderSurface::~HTMLRenderSurface() {
	if (document.is_valid()) {
		document->disconnect_changed(callable_mp(this, &HTMLRenderSurface::_document_changed));
	}
	if (backend != nullptr) {
		memdelete(backend);
	}
}

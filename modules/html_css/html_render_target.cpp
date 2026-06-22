/**************************************************************************/
/*  html_render_target.cpp                                                */
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

#include "html_render_target.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

static HTMLSurfaceBackendPreference html_render_target_to_surface_backend_preference(HTMLView::BackendPreference p_backend_preference) {
	return p_backend_preference == HTMLView::BACKEND_CPU ? HTML_SURFACE_BACKEND_CPU : HTML_SURFACE_BACKEND_AUTO;
}

void HTMLRenderTarget::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_document", "document"), &HTMLRenderTarget::set_document);
	ClassDB::bind_method(D_METHOD("get_document"), &HTMLRenderTarget::get_document);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &HTMLRenderTarget::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &HTMLRenderTarget::get_size);
	ClassDB::bind_method(D_METHOD("set_backend_preference", "backend_preference"), &HTMLRenderTarget::set_backend_preference);
	ClassDB::bind_method(D_METHOD("get_backend_preference"), &HTMLRenderTarget::get_backend_preference);
	ClassDB::bind_method(D_METHOD("get_texture"), &HTMLRenderTarget::get_texture);
	ClassDB::bind_method(D_METHOD("render_now"), &HTMLRenderTarget::render_now);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "document", PROPERTY_HINT_RESOURCE_TYPE, HTMLDocument::get_class_static()), "set_document", "get_document");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "size", PROPERTY_HINT_RANGE, "1,16384,1,or_greater,suffix:px"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "backend_preference", PROPERTY_HINT_ENUM, "Auto,CPU"), "set_backend_preference", "get_backend_preference");

	ADD_SIGNAL(MethodInfo("rendered"));
	ADD_SIGNAL(MethodInfo("texture_changed"));
}

void HTMLRenderTarget::_surface_changed() {
	emit_signal(SNAME("texture_changed"));
}

void HTMLRenderTarget::set_document(const Ref<HTMLDocument> &p_document) {
	surface->set_document(p_document);
}

Ref<HTMLDocument> HTMLRenderTarget::get_document() const {
	return surface->get_document();
}

void HTMLRenderTarget::set_size(const Size2i &p_size) {
	Size2i new_size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
	if (size == new_size) {
		return;
	}
	size = new_size;
	surface->set_size(size);
}

Size2i HTMLRenderTarget::get_size() const {
	return size;
}

void HTMLRenderTarget::set_backend_preference(HTMLView::BackendPreference p_backend_preference) {
	ERR_FAIL_INDEX((int)p_backend_preference, 2);
	backend_preference = p_backend_preference;
	surface->set_backend_preference(html_render_target_to_surface_backend_preference(p_backend_preference));
}

HTMLView::BackendPreference HTMLRenderTarget::get_backend_preference() const {
	return backend_preference;
}

Ref<Texture2D> HTMLRenderTarget::get_texture() const {
	return surface->get_texture();
}

void HTMLRenderTarget::render_now() {
	surface->render_now("HTMLRenderTarget");
	emit_signal(SNAME("rendered"));
}

HTMLRenderTarget::HTMLRenderTarget() {
	surface.instantiate();
	surface->set_changed_callback(callable_mp(this, &HTMLRenderTarget::_surface_changed));
	surface->set_placeholder_background(Color(0.06, 0.07, 0.08, 1.0));
	surface->set_size(size);
	surface->set_backend_preference(html_render_target_to_surface_backend_preference(backend_preference));
	surface->render_now("HTMLRenderTarget");
}

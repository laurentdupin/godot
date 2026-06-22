/**************************************************************************/
/*  html_surface_3d.cpp                                                   */
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

#include "html_surface_3d.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

void HTMLSurface3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_document", "document"), &HTMLSurface3D::set_document);
	ClassDB::bind_method(D_METHOD("get_document"), &HTMLSurface3D::get_document);
	ClassDB::bind_method(D_METHOD("set_texture_size", "texture_size"), &HTMLSurface3D::set_texture_size);
	ClassDB::bind_method(D_METHOD("get_texture_size"), &HTMLSurface3D::get_texture_size);
	ClassDB::bind_method(D_METHOD("set_physical_size", "physical_size"), &HTMLSurface3D::set_physical_size);
	ClassDB::bind_method(D_METHOD("get_physical_size"), &HTMLSurface3D::get_physical_size);
	ClassDB::bind_method(D_METHOD("set_input_enabled", "input_enabled"), &HTMLSurface3D::set_input_enabled);
	ClassDB::bind_method(D_METHOD("is_input_enabled"), &HTMLSurface3D::is_input_enabled);
	ClassDB::bind_method(D_METHOD("set_backend_preference", "backend_preference"), &HTMLSurface3D::set_backend_preference);
	ClassDB::bind_method(D_METHOD("get_backend_preference"), &HTMLSurface3D::get_backend_preference);
	ClassDB::bind_method(D_METHOD("get_texture"), &HTMLSurface3D::get_texture);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "document", PROPERTY_HINT_RESOURCE_TYPE, HTMLDocument::get_class_static()), "set_document", "get_document");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "texture_size", PROPERTY_HINT_RANGE, "1,16384,1,or_greater,suffix:px"), "set_texture_size", "get_texture_size");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "physical_size", PROPERTY_HINT_RANGE, "0.001,1024,0.001,or_greater,suffix:m"), "set_physical_size", "get_physical_size");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "input_enabled"), "set_input_enabled", "is_input_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "backend_preference", PROPERTY_HINT_ENUM, "Auto,CPU"), "set_backend_preference", "get_backend_preference");

	ADD_SIGNAL(MethodInfo("texture_changed"));
}

void HTMLSurface3D::_document_changed() {
	_update_placeholder();
}

void HTMLSurface3D::_update_placeholder() {
	Color background = Color(0.05, 0.055, 0.065, document.is_valid() && document->is_transparent_background() ? 0.0 : 1.0);
	texture->update_placeholder(texture_size, background, "HTMLSurface3D");
	emit_signal(SNAME("texture_changed"));
}

void HTMLSurface3D::set_document(const Ref<HTMLDocument> &p_document) {
	if (document == p_document) {
		return;
	}
	if (document.is_valid()) {
		document->disconnect_changed(callable_mp(this, &HTMLSurface3D::_document_changed));
	}
	document = p_document;
	if (document.is_valid()) {
		document->connect_changed(callable_mp(this, &HTMLSurface3D::_document_changed));
	}
	_update_placeholder();
}

Ref<HTMLDocument> HTMLSurface3D::get_document() const {
	return document;
}

void HTMLSurface3D::set_texture_size(const Size2i &p_texture_size) {
	Size2i new_size = Size2i(MAX(1, p_texture_size.x), MAX(1, p_texture_size.y));
	if (texture_size == new_size) {
		return;
	}
	texture_size = new_size;
	_update_placeholder();
}

Size2i HTMLSurface3D::get_texture_size() const {
	return texture_size;
}

void HTMLSurface3D::set_physical_size(const Size2 &p_physical_size) {
	physical_size = Size2(MAX(0.001, p_physical_size.x), MAX(0.001, p_physical_size.y));
}

Size2 HTMLSurface3D::get_physical_size() const {
	return physical_size;
}

void HTMLSurface3D::set_input_enabled(bool p_input_enabled) {
	input_enabled = p_input_enabled;
}

bool HTMLSurface3D::is_input_enabled() const {
	return input_enabled;
}

void HTMLSurface3D::set_backend_preference(HTMLView::BackendPreference p_backend_preference) {
	backend_preference = p_backend_preference;
}

HTMLView::BackendPreference HTMLSurface3D::get_backend_preference() const {
	return backend_preference;
}

Ref<Texture2D> HTMLSurface3D::get_texture() const {
	return texture;
}

HTMLSurface3D::HTMLSurface3D() {
	texture.instantiate();
	_update_placeholder();
}

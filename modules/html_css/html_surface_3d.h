/**************************************************************************/
/*  html_surface_3d.h                                                     */
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

#include "html_document.h"
#include "html_texture.h"
#include "html_view.h"

#include "scene/3d/node_3d.h"

class HTMLSurface3D : public Node3D {
	GDCLASS(HTMLSurface3D, Node3D);

	Ref<HTMLDocument> document;
	Ref<HTMLTexture2D> texture;
	Size2i texture_size = Size2i(512, 512);
	Size2 physical_size = Size2(1, 1);
	bool input_enabled = true;
	HTMLView::BackendPreference backend_preference = HTMLView::BACKEND_AUTO;

	void _document_changed();
	void _update_placeholder();

protected:
	static void _bind_methods();

public:
	void set_document(const Ref<HTMLDocument> &p_document);
	Ref<HTMLDocument> get_document() const;

	void set_texture_size(const Size2i &p_texture_size);
	Size2i get_texture_size() const;

	void set_physical_size(const Size2 &p_physical_size);
	Size2 get_physical_size() const;

	void set_input_enabled(bool p_input_enabled);
	bool is_input_enabled() const;

	void set_backend_preference(HTMLView::BackendPreference p_backend_preference);
	HTMLView::BackendPreference get_backend_preference() const;

	Ref<Texture2D> get_texture() const;

	HTMLSurface3D();
};

/**************************************************************************/
/*  html_surface_blink_c_api_backend.h                                    */
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

#include "html_surface_cpu_backend.h"

#include "html_css_renderer/renderer_c_api.h"

class HTMLSurfaceExternalCApiBackend : public HTMLSurfaceCPUBackend {
	hcsr_renderer_t *renderer = nullptr;
	Ref<HTMLDocument> document;
	Size2i size = Size2i(512, 512);
	HTMLFrameMetadata frame_metadata;
	bool document_dirty = true;
	bool viewport_dirty = true;

	bool _ensure_renderer();
	bool _sync_document();
	bool _sync_viewport();
	bool _copy_latest_output();
	void _read_frame_metadata();
	String _load_document_html() const;
	String _get_document_base_path() const;
	void _clear_output();

public:
	virtual void set_size(const Size2i &p_size) override;
	virtual void set_document(const Ref<HTMLDocument> &p_document) override;
	virtual void render_placeholder(const String &p_marker) override;
	virtual void get_frame_metadata(HTMLFrameMetadata &r_metadata) const override;

	~HTMLSurfaceExternalCApiBackend();
};

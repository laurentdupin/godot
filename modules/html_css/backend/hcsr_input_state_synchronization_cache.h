/**************************************************************************/
/*  hcsr_input_state_synchronization_cache.h                             */
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

#include "core/math/vector2.h"

class HCSRInputStateSynchronizationCache {
	Point2 pointer_position;
	Vector2i scroll_offset;
	bool primary_button_pressed = false;
	bool synchronized = false;

public:
	bool needs_synchronization(const Point2 &p_pointer_position, bool p_primary_button_pressed, const Vector2i &p_scroll_offset) const {
		return !synchronized || pointer_position != p_pointer_position || primary_button_pressed != p_primary_button_pressed || scroll_offset != p_scroll_offset;
	}

	void mark_synchronized(const Point2 &p_pointer_position, bool p_primary_button_pressed, const Vector2i &p_scroll_offset) {
		pointer_position = p_pointer_position;
		primary_button_pressed = p_primary_button_pressed;
		scroll_offset = p_scroll_offset;
		synchronized = true;
	}

	void reset() {
		synchronized = false;
	}
};

/**************************************************************************/
/*  html_activation_engine.cpp                                            */
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

#include "html_activation_engine.h"

HTMLActionActivation HTMLActivationEngine::activate(const HTMLElementHit &p_hit, const Point2i &p_position, MouseButton p_button) {
	HTMLActionActivation activation;
	activation.element_id = p_hit.element_id;

	const String data_action = p_hit.get_attribute(SNAME("data-godot-action"));
	if (!data_action.is_empty()) {
		activation.action = StringName(data_action);
		activation.has_action = true;
	} else if (p_hit.element_id != StringName()) {
		// Deterministic fallback for authored metadata that omits an explicit action.
		activation.action = p_hit.element_id;
		activation.has_action = true;
	}

	activation.payload[SNAME("element_id")] = p_hit.element_id;
	activation.payload[SNAME("tag_name")] = p_hit.tag_name;
	activation.payload[SNAME("position")] = p_position;
	activation.payload[SNAME("button")] = (int)p_button;
	activation.payload[SNAME("bounds")] = p_hit.bounds;
	activation.payload[SNAME("disabled")] = p_hit.disabled;
	activation.payload[SNAME("editable")] = p_hit.editable;
	activation.payload[SNAME("checked")] = p_hit.checked;
	activation.payload[SNAME("focused")] = p_hit.focused;

	Dictionary attributes;
	for (const HTMLElementAttribute &attribute : p_hit.attributes) {
		attributes[attribute.name] = attribute.value;
	}
	activation.payload[SNAME("attributes")] = attributes;

	return activation;
}

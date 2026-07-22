/**************************************************************************/
/*  html_view_output.cpp                                                  */
/**************************************************************************/

#include "html_view_output.h"

#include "html_view.h"
#include "core/object/class_db.h"

void HTMLViewOutput::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &HTMLViewOutput::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &HTMLViewOutput::get_size);
	ClassDB::bind_method(D_METHOD("get_texture"), &HTMLViewOutput::get_texture);
	ClassDB::bind_method(D_METHOD("get_generation"), &HTMLViewOutput::get_generation);
	ClassDB::bind_method(D_METHOD("has_mipmaps"), &HTMLViewOutput::has_mipmaps);
	ClassDB::bind_method(D_METHOD("get_content_rect"), &HTMLViewOutput::get_content_rect);
	ClassDB::bind_method(D_METHOD("output_to_logical", "position"), &HTMLViewOutput::output_to_logical);
	ClassDB::bind_method(D_METHOD("logical_to_output", "position"), &HTMLViewOutput::logical_to_output);
	ClassDB::bind_method(D_METHOD("release"), &HTMLViewOutput::release);
	ClassDB::bind_method(D_METHOD("is_valid"), &HTMLViewOutput::is_valid);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "size", PROPERTY_HINT_NONE, "suffix:px"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D", PROPERTY_USAGE_READ_ONLY), "", "get_texture");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "generation", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_READ_ONLY), "", "get_generation");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mipmaps", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_READ_ONLY), "", "has_mipmaps");
	ADD_PROPERTY(PropertyInfo(Variant::RECT2I, "content_rect", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_READ_ONLY), "", "get_content_rect");
}

void HTMLViewOutput::set_size(const Size2i &p_size) {
	ERR_FAIL_COND_MSG(p_size.x <= 0 || p_size.y <= 0, "HTMLViewOutput dimensions must be positive.");
	if (size == p_size) {
		return;
	}
	ERR_FAIL_NULL_MSG(owner, "Cannot resize a released HTMLViewOutput.");
	if (owner->_resize_output(output_id, p_size) == OK) {
		size = p_size;
	}
}

Size2i HTMLViewOutput::get_size() const {
	return size;
}

Ref<Texture2D> HTMLViewOutput::get_texture() const {
	return owner != nullptr ? owner->_get_output_texture(output_id) : Ref<Texture2D>();
}

uint64_t HTMLViewOutput::get_generation() const {
	return owner != nullptr ? owner->_get_output_generation(output_id) : 0;
}

bool HTMLViewOutput::has_mipmaps() const {
	return mipmaps;
}

Rect2i HTMLViewOutput::get_content_rect() const {
	return Rect2i(Point2i(), size);
}

Vector2 HTMLViewOutput::output_to_logical(const Vector2 &p_position) const {
	if (owner == nullptr || size.x <= 0 || size.y <= 0) {
		return Vector2();
	}
	const Size2i logical_size = owner->get_logical_size();
	return Vector2(p_position.x * logical_size.x / size.x, p_position.y * logical_size.y / size.y);
}

Vector2 HTMLViewOutput::logical_to_output(const Vector2 &p_position) const {
	if (owner == nullptr) {
		return Vector2();
	}
	const Size2i logical_size = owner->get_logical_size();
	if (logical_size.x <= 0 || logical_size.y <= 0) {
		return Vector2();
	}
	return Vector2(p_position.x * size.x / logical_size.x, p_position.y * size.y / logical_size.y);
}

void HTMLViewOutput::release() {
	if (owner != nullptr) {
		HTMLView *previous_owner = owner;
		owner = nullptr;
		previous_owner->_release_output(output_id);
		output_id = 0;
	}
}

bool HTMLViewOutput::is_valid() const {
	return owner != nullptr && output_id != 0;
}

void HTMLViewOutput::initialize(HTMLView *p_owner, uint64_t p_output_id, const Size2i &p_size, bool p_mipmaps) {
	ERR_FAIL_NULL(p_owner);
	ERR_FAIL_COND(p_output_id == 0);
	owner = p_owner;
	output_id = p_output_id;
	size = p_size;
	mipmaps = p_mipmaps;
}

void HTMLViewOutput::detach_owner() {
	owner = nullptr;
	output_id = 0;
}

HTMLViewOutput::~HTMLViewOutput() {
	release();
}

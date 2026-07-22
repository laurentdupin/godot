/**************************************************************************/
/*  html_view_output.h                                                    */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"
#include "core/math/rect2.h"
#include "scene/resources/texture.h"

class HTMLView;

class HTMLViewOutput : public RefCounted {
	GDCLASS(HTMLViewOutput, RefCounted);

	HTMLView *owner = nullptr;
	uint64_t output_id = 0;
	Size2i size;

protected:
	static void _bind_methods();

public:
	void set_size(const Size2i &p_size);
	Size2i get_size() const;
	Ref<Texture2D> get_texture() const;
	uint64_t get_generation() const;
	Rect2i get_content_rect() const;
	Vector2 output_to_logical(const Vector2 &p_position) const;
	Vector2 logical_to_output(const Vector2 &p_position) const;
	void release();
	bool is_valid() const;

	void initialize(HTMLView *p_owner, uint64_t p_output_id, const Size2i &p_size);
	void detach_owner();

	~HTMLViewOutput();
};

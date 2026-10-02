#pragma once

#include "core/math/color.h"
#include "core/math/rect2.h"
#include "core/math/transform_2d.h"
#include "core/object/ref_counted.h"
#include "core/templates/rid.h"

// Native RD consumers can prepare the texture used by a canvas item after the
// preceding canvas batches have finished. This is deliberately not script-bound:
// preparation executes on the rendering thread and must not access scene nodes.
class CanvasRenderTargetPreparation : public RefCounted {
public:
	struct Input {
		RID color_texture; // Borrowed RD texture; never free, write, or retain it.
		Size2i size;
		Transform2D item_transform;
		Rect2 item_rect;
		Rect2 clip_rect;
		Color modulation;
		bool clipped = false;
		bool linear_colors = false;
		bool canvas_group = false;
	};

	// The input includes preceding items, excludes this and following items, and
	// is valid only during this invocation. Owned outputs may be updated here.
	virtual void prepare(const Input &p_input) = 0;
};

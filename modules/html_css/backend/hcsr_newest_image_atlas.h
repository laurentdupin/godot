#pragma once

#include "../html_document.h"
#include "hcsr_scene.h"

#include "core/io/image.h"
#include "servers/rendering/rendering_device.h"

// Per-view asset cache, shared by every presentation of the same scene packet.
// Godot owns GPU lifetime and barriers on both D3D12 and Vulkan.
class HCSRNewestImageAtlas {
	static constexpr int PAGE_SIZE = 4096;
	static constexpr int MAX_PAGES = 8;
	struct Entry {
		int page = -1;
		Rect2i rect;
		Size2i natural_size;
        Vector2 glyph_offset, glyph_size;
        int raster_size = 0;
        bool color_glyph = false;
	};
	struct Page {
        bool glyphs = false;
		Ref<Image> pixels;
		Vector<Rect2i> dirty;
		RID texture, uniform;
		int x = 0, y = 0, row_height = 0;
	};
	struct Vertex {
		float position_uv[4];
		float tint[4];
		float bounds[4];
	};
	struct Batch {
		int page;
		uint32_t first, count;
	};
	HashMap<String, Entry> entries;
	Vector<Page> pages;
	Vector<Vertex> vertices;
	Vector<Batch> batches;
	RID shader, sampler, buffer, pipeline;
	uint32_t buffer_capacity = 0;
	bool uploaded = false;
	uint64_t uploaded_bytes = 0, decoded_images = 0;
	Entry resolve(const Ref<HTMLDocument> &document, const String &source);
    Entry resolve_glyph(const hcsr_glyph_material_t &glyph, float scale);
    Entry rasterize_glyph(const hcsr_glyph_material_t &glyph, int level);
    struct PendingGlyph { hcsr_glyph_material_t glyph; int level; String key; };
    Vector<PendingGlyph> pending_glyphs;
    HashMap<String, Entry> last_glyphs;
    HashMap<String, bool> queued_glyphs;
    uint64_t rasterized_glyphs = 0;
    HashMap<String, int> glyph_levels;
	bool upload(RenderingDevice *device);

public:
	bool prepare(const hcsr_draw_packet_view_t &packet, const Ref<HTMLDocument> &document, float output_scale = 1);
	bool draw(RenderingDevice *device, RID target, const Color &background);
	void draw_cpu(Ref<Image> target, const Color &background);
	void release(RenderingDevice *device);
	Dictionary get_statistics() const;
    bool has_pending_glyphs() const { return !pending_glyphs.is_empty(); }
};

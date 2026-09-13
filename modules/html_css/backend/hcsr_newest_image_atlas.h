#pragma once

#include "../html_document.h"
#include "hcsr_scene.h"

#include "core/io/image.h"
#include "core/os/mutex.h"
#include "servers/rendering/rendering_device.h"

// Per-view asset cache, shared by every presentation of the same scene packet.
// Godot owns GPU lifetime and barriers on D3D12, Vulkan, and Metal.
class HCSRNewestImageAtlas {
	static constexpr int PAGE_SIZE = 4096;
	static constexpr int MAX_PAGES = 9;
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
        uint32_t state = UINT32_MAX, pad[3] = {};
	};
	struct Batch {
		int page;
		uint32_t first, count;
		uint32_t kind = 0, depth = 0;
		float opacity = 1;
        Rect2 bounds;
	};
    struct GroupTarget { RID texture, framebuffer, uniform; Size2i size; };
    struct GroupPool { RID output; Vector<GroupTarget> targets; };
    Vector<GroupPool> group_pools;
    uint32_t group_depth = 0;
    uint64_t group_allocations = 0;
    uint32_t last_render_passes = 0;
    bool last_disjoint_groups = false;
    void release_groups(RenderingDevice *device);
    struct GlyphKey {
        uint64_t face = 0;
        uint32_t glyph = 0, size = 0;
        bool operator==(const GlyphKey &other) const { return face == other.face && glyph == other.glyph && size == other.size; }
    };
    struct GlyphHasher {
        static uint32_t hash(const GlyphKey &key) {
            return hash_fmix32(hash_murmur3_one_32(key.size, hash_murmur3_one_32(key.glyph, hash_murmur3_one_64(key.face))));
        }
    };
    HashMap<GlyphKey, Entry, GlyphHasher> glyph_entries;
	HashMap<String, Entry> entries;
	Mutex image_mutex;
	HashMap<String, Ref<Image>> decoded_sources;
	HashMap<String, Size2i> source_sizes;
	Ref<Image> load_image(const Ref<HTMLDocument> &document, const String &source);
	Vector<Page> pages;
	Vector<Vertex> vertices;
    Vector<Vertex> uploaded_vertices;
    struct PreparedMesh { uint32_t source_first=0, prepared_first=0, count=0; };
    Vector<PreparedMesh> prepared_meshes;
    Vector<hcsr_paint_vertex_t> prepared_source;
    Vector<uint32_t> prepared_states;
    bool apply_color_patches(const hcsr_draw_packet_view_t &packet);
    uint64_t color_patch_updates = 0;
	Vector<Batch> batches;
    // Six vertex invocations per instance: triangle (0), quad (1), or triangle pair (2).
    struct Primitive { uint32_t first, topology, draw_index=UINT32_MAX, reserved=0; };
    Vector<Primitive> primitives, all_primitives;
    Vector<Batch> all_batches;
    void update_visible_instances(const hcsr_draw_packet_view_t &packet);
    Vector<hcsr_gpu_state_t> gpu_states;
    Vector<hcsr_gpu_clip_t> gpu_clips;
    Vector<hcsr_gpu_plane_t> gpu_planes;
    RID state_buffer, clip_buffer, plane_buffer, primitive_buffer;
    uint32_t state_capacity=0, clip_capacity=0, plane_capacity=0, primitive_capacity=0;
    uint64_t geometry_generation=0, geometry_uploaded_bytes=0, state_uploaded_bytes=0, instance_uploaded_bytes=0;
    bool gpu_geometry=false, geometry_dirty=true;
    float logical_width=1, logical_height=1, prepared_scale=0;
    uint32_t last_draw_calls=0;
    double last_gpu_ms=0;
    uint64_t last_gpu_frame=0;

	RID shader, sampler, buffer, pipeline;
	uint32_t buffer_capacity = 0;
	bool uploaded = false;
	uint64_t uploaded_bytes = 0, decoded_images = 0;
	Entry resolve(const Ref<HTMLDocument> &document, const String &source, const Size2i &natural, const Vector2 &physical_size);
    Entry resolve_glyph(const hcsr_glyph_material_t &glyph, float scale);
    Entry rasterize_glyph(const hcsr_glyph_material_t &glyph, int level);
    struct PendingGlyph { hcsr_glyph_material_t glyph; int level; GlyphKey key; };
    Vector<PendingGlyph> pending_glyphs;
    HashMap<GlyphKey, Entry, GlyphHasher> last_glyphs;
    HashMap<GlyphKey, bool, GlyphHasher> queued_glyphs;
    uint64_t rasterized_glyphs = 0;
    HashMap<GlyphKey, int, GlyphHasher> glyph_levels;
	bool upload(RenderingDevice *device);

public:
	Size2i resolve_size(const Ref<HTMLDocument> &document, const String &source);
	bool prepare(const hcsr_draw_packet_view_t &packet, const Ref<HTMLDocument> &document, float output_scale = 1);
	bool draw(RenderingDevice *device, RID target, const Color &background);
	void draw_cpu(Ref<Image> target, const Color &background);
	void release(RenderingDevice *device);
	Dictionary get_statistics() const;
    bool has_pending_glyphs() const { return !pending_glyphs.is_empty(); }
};

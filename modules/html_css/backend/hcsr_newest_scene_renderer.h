#pragma once

#include "hcsr_newest_raster_resources.h"
#include "hcsr_scene_submission.h"
#include "servers/rendering/rendering_device.h"

// Submits prepared scene drawing. Raster resource lifetime is owned by the view,
// not by this renderer. The remaining packet/mesh preparation is transitional.
class HCSRNewestSceneRenderer {
    HCSRNewestRasterResources &resources;
    using Entry = HCSRNewestRasterResources::Entry;
    struct GpuPage { RID texture, uniform; };
    Vector<GpuPage> gpu_pages;
    using Vertex = hcsr::render::scene_vertex;
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
	Vector<Vertex> vertices;
    Vector<Vertex> uploaded_vertices;
    struct PreparedMesh { uint32_t source_first=0, prepared_first=0, count=0; };
    Vector<PreparedMesh> prepared_meshes;
    Vector<hcsr_paint_vertex_t> prepared_source;
    Vector<uint32_t> prepared_states;
	Vector<Batch> batches;
    // Six vertex invocations per instance: triangle (0), quad (1), or triangle pair (2).
    using Primitive = hcsr::render::scene_primitive;
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
	uint64_t uploaded_bytes = 0;
	bool upload(RenderingDevice *device);

public:
    explicit HCSRNewestSceneRenderer(HCSRNewestRasterResources &p_resources) : resources(p_resources) {}
	bool prepare(const hcsr_draw_packet_view_t &packet, const Ref<HTMLDocument> &document, float output_scale = 1);
	bool draw(RenderingDevice *device, RID target, const Color &background);
	void draw_cpu(Ref<Image> target, const Color &background);
	void release(RenderingDevice *device);
	Dictionary get_statistics() const;
};

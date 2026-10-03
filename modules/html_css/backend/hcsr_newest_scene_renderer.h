#pragma once

#include "hcsr_newest_raster_resources.h"
#include "hcsr_scene_submission.h"
#include "hcsr_backdrop_submission.h"
#include "hcsr_scene_hierarchy.h"
#include "hcsr_gpu_coverage.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/canvas_render_target_preparation.h"

// Submits prepared scene drawing. Raster resource lifetime is owned by the view,
// not by this renderer. The remaining packet/mesh preparation is transitional.
class HCSRNewestSceneRenderer {
    HCSRNewestRasterResources &resources;
    using Entry = HCSRNewestRasterResources::Entry;
    struct GpuPage { RID texture, uniform, mask_uniform,blend_uniform; };
    Vector<GpuPage> gpu_pages;
    Vector<Entry> backdrop_entries;
    Vector<uint64_t> backdrop_identities;
    uint32_t backdrop_first_primitive=0;
    using Vertex = hcsr::render::scene_vertex;
	struct Batch {
		int page;
		uint32_t first, count;
		uint32_t kind = 0, depth = 0;
		float opacity = 1;
        Rect2 bounds;
        size_t event_index=0;
        int mask_page=0;
        uint32_t blend_mode=0;
        bool source_scratch=false;
        uint32_t backdrop_source=0,backdrop_destination=0;
        size_t backdrop_source_event=SIZE_MAX;
        bool backdrop=false,backdrop_mask=false,backdrop_merge=false,backdrop_first=false;
		bool document_source=false, backdrop_prefix=false;
        uint32_t opacity_state_plus_one=0;
        bool hidden=false;
	};
    struct GroupTarget { RID texture, framebuffer, uniform; Size2i size; };
    struct GroupPool { RID output,blend_texture,blend_uniform; Vector<GroupTarget> targets,underlays; };
    Vector<GroupPool> group_pools;
    hcsr::render::compositing_bounds_program bounds_program;
    struct CoverageGpu {
        hcsr::render::gpu_coverage_program program;
        std::vector<uint32_t> words;
        RID data,output,controls,arguments,uniform,region_uniform,clear_uniform,shaders[3],pipelines[3];
        uint32_t capacities[4]{};
        uint64_t revision=0,evaluations=0,uploaded_bytes=0,clears=0;
        Size2i target;
        Vector2 logical;
        bool dirty=true;
    } coverage;
    bool prepare_coverage(RenderingDevice *,const Size2i &);
    bool clear_group(RenderingDevice *,RenderingDevice::DrawListID,RID,const Size2i &,size_t event);
    uint32_t group_depth = 0;
    bool ordered_backdrops=false;
	bool document_backdrops=false, snapshot_target=false;
	RID document_shader, document_pipeline;
	bool snapshot_document(RenderingDevice *, RID prefix, RID snapshot, const CanvasRenderTargetPreparation::Input *);
    uint64_t group_allocations = 0;
    uint32_t last_render_passes = 0;
    bool last_disjoint_groups = false;
    void release_groups(RenderingDevice *device);
	Vector<hcsr::render::scene_word> vertices;
    Vector<hcsr::render::scene_word> uploaded_vertices;
	Vector<Batch> batches;
    // Six vertex invocations per instance: triangle (0), quad (1), or triangle pair (2).
    using Primitive = hcsr::render::scene_primitive;
    Vector<Primitive> primitives;
    void update_compositing_bounds(const hcsr_draw_packet_view_t &packet);
    Vector<hcsr_gpu_state_t> gpu_states;
    Vector<hcsr_gpu_clip_t> gpu_clips;
    Vector<hcsr_gpu_plane_t> gpu_planes;
    RID state_buffer, clip_buffer, plane_buffer, primitive_buffer;
    hcsr::render::hierarchy_input hierarchy;
    RID hierarchy_buffer,hierarchy_shader,hierarchy_pipeline,hierarchy_uniform;
    uint32_t hierarchy_capacity=0,gpu_state_count=0;
    uint64_t hierarchy_uploaded_revision=0,hierarchy_uploaded_bytes=0,hierarchy_evaluations=0,resolved_state_uploaded_bytes=0;
    bool local_hierarchy=false;
    uint32_t state_capacity=0, clip_capacity=0, plane_capacity=0, primitive_capacity=0;
    uint64_t geometry_generation=0, geometry_uploaded_bytes=0, state_uploaded_bytes=0, instance_uploaded_bytes=0, clip_definition_uploaded_bytes=0;
    bool gpu_geometry=false, geometry_dirty=true;
    float logical_width=1, logical_height=1, prepared_scale=0;
    uint32_t last_draw_calls=0;
    double last_gpu_ms=0;
    uint64_t last_gpu_frame=0;

	RID shader, sampler, buffer, pipeline, shadow_pipeline;
	uint32_t buffer_capacity = 0;
	bool uploaded = false;
	uint64_t uploaded_bytes = 0;
	bool upload(RenderingDevice *device);

public:
    explicit HCSRNewestSceneRenderer(HCSRNewestRasterResources &p_resources) : resources(p_resources) {}
	bool prepare(const hcsr_draw_packet_view_t &packet, const Ref<HTMLDocument> &document, float output_scale = 1, const hcsr_backdrop_view_t &backdrop = {},const hcsr_hierarchy_view_t &local = {});
    hcsr_gpu_state_t reference_state(const hcsr_draw_packet_view_t &packet,uint32_t index) const;
    // Borrowed atlas bindings: lifetime and upload remain with this renderer.
    HCSRNewestRasterResources::Entry backdrop_entry(uint32_t index) const { return backdrop_entries[index]; }
    bool draw_backdrop_mask(RenderingDevice *device,RID target,const hcsr_backdrop_view_t &view,const Size2i &logical,const Size2i &physical);
	bool prepare_gpu_resources(RenderingDevice *device);
	bool has_document_backdrops() const { return document_backdrops; }
	bool draw(RenderingDevice *device, RID target, const Color &background, const CanvasRenderTargetPreparation::Input *input = nullptr);
	void draw_cpu(Ref<Image> target, const Color &background);
	void release(RenderingDevice *device);
	Dictionary get_statistics() const;
};

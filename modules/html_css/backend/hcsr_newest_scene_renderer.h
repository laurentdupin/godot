#pragma once

#include "hcsr_newest_raster_resources.h"
#include "hcsr_scene_submission.h"
#include "hcsr_backdrop_submission.h"
#include "hcsr_scene_hierarchy.h"
#include "hcsr_gpu_coverage.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/canvas_render_target_preparation.h"

// Shares compact surface submission and local hierarchy across GPU adapters.
// The view owns raster validity/allocation; this renderer owns GPU mirrors and
// presentation scratch resources, never a second appearance cache.
class HCSRNewestSceneRenderer {
    HCSRNewestRasterResources &resources;
    using Entry = HCSRNewestRasterResources::Entry;
    struct GpuPage { RID texture, uniform, mask_uniform,blend_uniform; };
    Vector<GpuPage> gpu_pages;
    static void release_page(RenderingDevice *,GpuPage &);
    uint64_t retired_atlas_pages=0;
    uint64_t uploaded_atlas_revision=UINT64_MAX;
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
        bool backdrop_fused=false;
		bool document_source=false, backdrop_prefix=false;
        uint32_t document_region=0;
        uint32_t opacity_state_plus_one=0;
        bool hidden=false;
	};
    struct GroupTarget { RID texture, framebuffer, uniform,foreground_uniform; Size2i size; };
    struct BlendCopy { RID source,uniform; };
    struct DocumentBindings {RID prefix,host,snapshot,uniform;};
    struct GroupPool { RID output,blend_texture,blend_uniform; Size2i blend_size; DocumentBindings document_bindings; Vector<GroupTarget> targets,underlays; Vector<BlendCopy> blend_copies; };
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
    RID blend_copy_shader,blend_copy_pipeline;
    uint64_t gpu_blend_region_copies=0;
    uint64_t gpu_backdrop_prefix_draws=0;
    bool copy_group_region(RenderingDevice *,GroupPool &,RID source,size_t event,uint32_t depth);
    uint32_t group_depth = 0;
    bool host_coverage_required = true;
    std::vector<std::vector<size_t>> nested_group_passes;
    uint64_t cpu_compositing_bounds_evaluations = 0;
    bool ordered_backdrops=false;
	bool document_backdrops=false, snapshot_target=false;
	RID document_shader, document_pipeline,document_region_shader,document_region_pipeline;
    RID document_region_buffer,document_arguments,document_region_uniform,document_descriptors,document_mapping;
    struct DocumentRegion { uint32_t geometry,coverage,first,count; };
    Vector<DocumentRegion> document_regions;
    uint32_t document_snapshot_cohorts=0;
    uint32_t document_region_capacity=0;
    bool document_regions_dirty=true;
    bool prepare_document_regions(RenderingDevice *,const Size2i &);
    uint64_t gpu_document_snapshots=0;
	bool snapshot_document(RenderingDevice *,GroupPool &, RID prefix, RID snapshot, uint32_t region, const CanvasRenderTargetPreparation::Input *);
    uint64_t group_allocations = 0;
    uint32_t last_render_passes = 0;
    bool last_disjoint_groups = false;
    void release_groups(RenderingDevice *device);
    static void release_group_target(RenderingDevice *,GroupTarget &);
    static void release_group_pool(RenderingDevice *,GroupPool &);
    uint64_t retired_group_targets = 0;
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
    bool opengl_submission=false;
    float logical_width=1, logical_height=1, prepared_scale=0;
    uint32_t last_draw_calls=0;
    double last_gpu_ms=0;
    uint64_t last_gpu_frame=0;

	RID shader, sampler, buffer, pipeline, fused_pipeline, shadow_pipeline;
	uint32_t buffer_capacity = 0;
	bool uploaded = false;
	uint64_t uploaded_bytes = 0;
	bool upload(RenderingDevice *device);

#ifdef GLES3_ENABLED
    struct GLData {uint32_t texture=0;int width=0,height=0;};
    struct GLTarget {uint32_t texture=0,framebuffer=0;};
    struct GLPool {RID output;Size2i size;Vector<GLTarget> groups;GLTarget blend;};
    struct GLResources {
        uint32_t program=0,hierarchy_program=0,vao=0,framebuffer=0;
        GLData geometry,states,clips,planes,primitives,nodes,outputs;
        Vector<uint32_t> pages;
        Vector<GLPool> pools;
        uint64_t hierarchy_geometry=0,hierarchy_revision=0;
    } gl;
    void release_gl();
#endif
public:
    explicit HCSRNewestSceneRenderer(HCSRNewestRasterResources &p_resources) : resources(p_resources) {}
    bool prepare(const hcsr_draw_packet_view_t &packet, const Ref<HTMLDocument> &document, float output_scale = 1, const hcsr_backdrop_view_t &backdrop = {},const hcsr_hierarchy_view_t &local = {},const hcsr_raster_demand_view_t &raster_demand = {},bool gpu_compositing=true,bool opengl=false);
    bool draw_gl(RID target,const Size2i &size,const Color &background,bool mipmaps);
    void release_gl_output(RID target);
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

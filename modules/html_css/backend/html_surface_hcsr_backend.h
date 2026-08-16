/**************************************************************************/
/*  html_surface_hcsr_backend.h                                          */
/**************************************************************************/

#pragma once

#include "html_surface_cpu_backend.h"
#include "hcsr_performance_monitor.h"

#include "core/templates/hash_map.h"
#include "core/templates/vector.h"

#include "hcsr_runtime.h"

#include <cstring>

class HTMLSurfaceHCSRBackend : public HTMLSurfaceCPUBackend {
	struct OutputState {
		int32_t runtime_output_id = 0;
		Size2i requested_size;
		bool mipmaps = false;
		Ref<HTMLTexture2D> texture;
		uint64_t generation = 0;
		uint64_t copied_tile_bytes = 0;
	};
	struct PresentationOutputCandidate {
		bool primary = false;
		OutputState *output_state = nullptr;
		Ref<HTMLTexture2D> target_texture;
		hcsr_runtime_frame_t *frame = nullptr;
		hcsr_runtime_frame_info_t frame_info = {};
		int32_t next_tile = 0;
		int32_t publication_output_index = -1;
		bool sync_initialized = false;
	};
	struct PresentationCandidate {
		hcsr_runtime_publication_t *publication = nullptr;
		hcsr_runtime_publication_info_t publication_info = {};
		uint64_t author_submission_generation = 0;
		uint64_t configuration_generation = 0;
		uint64_t staging_base_runtime_generation = 0;
		uint64_t sync_base_runtime_generation = 0;
		Vector<PresentationOutputCandidate> outputs;
		int32_t next_output_to_acquire = 0;
		int32_t active_output = 0;
		bool activated = false;
	};

	Ref<HTMLDocument> document;
	hcsr_runtime_document_t *compiled_document = nullptr;
	hcsr_runtime_session_t *session = nullptr;
	PresentationCandidate *presentation_candidate = nullptr;
	HashMap<uint64_t, OutputState *> presentation_outputs;
	uint64_t next_presentation_output_id = 2;
	uint64_t next_document_request_id = 1;
	uint64_t queued_generation = 0;
	uint64_t active_generation = 0;
	uint64_t next_activation_generation = 1;
	uint64_t publication_cursor_runtime_generation = 0;
	uint64_t visible_runtime_generation = 0;
	uint64_t standby_runtime_generation = 0;
	uint64_t author_submission_generation = 0;
	uint64_t configuration_generation = 1;
	Size2i physical_size = Size2i(512, 512);
	float device_scale_factor = 1.0f;
	bool document_dirty = false;
	bool session_configuration_dirty = true;
	bool derivation_pending = false;
	bool publication_probe_pending = false;
	bool terminal_failure = false;
	String terminal_failure_reason;
	HTMLFrameMetadata frame_metadata;
	uint64_t primary_copied_tile_bytes = 0;
	double last_step_milliseconds = 0.0;
	double last_presentation_slice_milliseconds = 0.0;
	double last_tile_copy_milliseconds = 0.0;
	double last_texture_upload_milliseconds = 0.0;
	int64_t last_step_work_units = 0;
	uint64_t last_texture_upload_bytes = 0;
	uint64_t staged_tile_count = 0;

	template <typename T>
	static void _initialize_abi(T &r_value) {
		std::memset(&r_value, 0, sizeof(T));
		r_value.struct_size = sizeof(T);
		r_value.abi_version = HCSR_RUNTIME_ABI_VERSION;
	}

	void _set_terminal_failure(const String &p_message);
	bool _load_document_source(String &r_html, String &r_css) const;
	bool _compile_document();
	bool _recreate_session();
	void _begin_session_retirement();
	bool _step_session(uint64_t p_budget_usec);
	bool _consume_publication();
	bool _begin_presentation_candidate();
	bool _advance_presentation_candidate();
	bool _copy_candidate_tile(PresentationOutputCandidate &p_output);
	void _release_presentation_candidate(bool p_keep_standby);
	Error _submit_attribute_mutations(const Array &p_mutations);
	Error _submit_attribute_mutation(const StringName &p_id, const StringName &p_name, const String &p_value);
	void _report_compilation(hcsr_runtime_compilation_report_t *p_report);
	void _publish_metrics();

public:
	virtual void mark_document_dirty() override;
	virtual void set_size(const Size2i &p_size) override;
	virtual void set_device_scale_factor(float p_device_scale_factor) override;
	virtual void set_physical_size(const Size2i &p_physical_size) override;
	virtual void set_document(const Ref<HTMLDocument> &p_document) override;
	virtual void set_background_color(const Color &p_background_color) override;
	virtual Error update_compositor(double p_timeline_time_seconds, bool *r_needs_output, bool *r_needs_begin_frame) override;
	virtual void render_placeholder(const String &p_marker) override;
	virtual bool poll_pending_output(bool *r_waiting_for_completion = nullptr) override;
	virtual HTMLPendingOutputState consume_pending_output_state() override;
	virtual void schedule_retirement_service() override;
	virtual bool has_pending_output() const override;
	virtual bool has_pending_frame_request() const override;
	virtual uint64_t get_last_queued_frame_generation() const override;
	virtual uint64_t get_active_frame_generation() const override;
	virtual bool uses_generation_bound_input() const override;
	virtual bool has_terminal_render_failure() const override;
	virtual String get_terminal_render_failure_reason() const override;
	virtual Error apply_element_mutations(const Array &p_mutations) override;
	virtual Error set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value) override;
	virtual Error set_element_style(const StringName &p_id, const String &p_css_text) override;
	virtual void get_frame_metadata(HTMLFrameMetadata &r_metadata) const override;
	virtual uint64_t create_presentation_output(const Size2i &p_size, bool p_mipmaps) override;
	virtual Error resize_presentation_output(uint64_t p_output_id, const Size2i &p_size) override;
	virtual void destroy_presentation_output(uint64_t p_output_id) override;
	virtual Ref<Texture2D> get_presentation_output_texture(uint64_t p_output_id) const override;
	virtual uint64_t get_presentation_output_generation(uint64_t p_output_id) const override;

	HTMLSurfaceHCSRBackend();
	~HTMLSurfaceHCSRBackend();
};

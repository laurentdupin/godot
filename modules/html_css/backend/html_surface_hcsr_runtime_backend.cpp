/**************************************************************************/
/*  html_surface_hcsr_runtime_backend.cpp                                 */
/**************************************************************************/

#include "html_surface_hcsr_runtime_backend.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

namespace {
enum RuntimeMutationKind {
	RUNTIME_MUTATION_TEXT,
	RUNTIME_MUTATION_ATTRIBUTE,
	RUNTIME_MUTATION_STYLE,
	RUNTIME_MUTATION_CHECKED,
};

static constexpr int RUNTIME_INTERACTIVE_STEP_SLICE_UNITS = 256;
static constexpr int RUNTIME_PRESENTER_STEP_SLICE_UNITS = 256;

struct RuntimeMutation {
	int kind = RUNTIME_MUTATION_TEXT;
	String id;
	String name;
	String value;
};

struct RuntimePublicationLineage {
	bool valid = false;
	uint64_t request_serial = 0;
	uint64_t runtime_generation = 0;
	uint64_t semantic_frame_generation = 0;
	uint64_t target_author_revision = 0;
	uint64_t interactive_submission_id = 0;
	uint64_t interactive_frame_id = 0;
};

struct RuntimeExternalSurfaceSlot {
	uint64_t hcsr_slot_id = 0;
	int32_t godot_slot = -1;
	hcsr_runtime_d3d12_surface_registration_t *registration = nullptr;
	hcsr_runtime_d3d12_surface_t *generation_surface = nullptr;
	RID rd_texture;
	RID canvas_texture;
	uint64_t runtime_generation = 0;
};

struct RuntimePresentationBinding {
	hcsr_runtime_d3d12_presenter_t *presenter = nullptr;
	RID external_texture_pool;
	Vector<RuntimeExternalSurfaceSlot> slots;
	int32_t active_slot = -1;
	bool presenter_pending = false;
	bool pool_stopped = false;
	bool presenter_shutdown_started = false;
	RuntimePublicationLineage pending_lineage;
};

static void initialize_abi(void *p_value, size_t p_size) {
	memset(p_value, 0, p_size);
	uint32_t *header = static_cast<uint32_t *>(p_value);
	header[0] = (uint32_t)p_size;
	header[1] = HCSR_RUNTIME_ABI_VERSION;
}
} // namespace

struct HTMLSurfaceHCSRRuntimeBackend::RuntimeState {
	Mutex mutex;
	Ref<HTMLTexture2D> texture;
	String html;
	String css;
	Size2i logical_size = Size2i(512, 512);
	Size2i physical_size = Size2i(512, 512);
	Vector<RuntimeMutation> mutations;
	bool document_dirty = false;
	bool configuration_dirty = false;
	bool work_scheduled = false;
	bool pending_work = false;
	bool interactive_pending = false;
	bool activation_pending = false;
	bool cutoff_scheduled = false;
	bool presentation_changed = false;
	bool closing = false;
	bool terminal = false;
	bool session_shutdown_started = false;
	String terminal_reason;
	uint64_t document_request_id = 0;
	uint64_t consumed_runtime_generation = 0;
	uint64_t queued_generation = 0;
	uint64_t active_generation = 0;
	uint64_t mutation_request_process_frame = 0;
	uint64_t request_serial = 0;
	uint64_t submitted_request_serial = 0;
	uint64_t newest_requested_submission_id = 0;
	uint64_t newest_requested_author_revision = 0;
	uint64_t newest_requested_frame_id = 0;
	uint64_t newest_requested_cutoff_timestamp_microseconds = 0;
	uint64_t cutoff_process_frame = 0;
	uint64_t active_request_process_frame = 0;
	uint64_t last_activation_process_frame = UINT64_MAX;
	HTMLFrameMetadata frame_metadata;
	hcsr_runtime_document_t *compiled_document = nullptr;
	hcsr_runtime_session_t *session = nullptr;
	RuntimePresentationBinding *active_binding = nullptr;
	RuntimePresentationBinding *successor_binding = nullptr;
	RuntimePresentationBinding *retiring_binding = nullptr;
	RuntimePresentationBinding *staged_binding = nullptr;
	RuntimePublicationLineage staged_lineage;
	Vector<RID> owned_canvas_textures;
	RID active_imported_texture;
	uint64_t active_semantic_frame_generation = 0;
	int64_t active_configuration_id = 0;
	int32_t active_output_id = 0;
	int32_t active_pixel_width = 0;
	int32_t active_pixel_height = 0;
	hcsr_runtime_d3d12_surface_format_t active_pixel_format = HCSR_RUNTIME_D3D12_SURFACE_FORMAT_RGBA8_UNORM;
};

static bool runtime_track_canvas_texture(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		const RID &p_texture) {
	MutexLock lock(p_state->mutex);
	if (p_state->closing) {
		return false;
	}
	p_state->owned_canvas_textures.push_back(p_texture);
	return true;
}

static void runtime_release_canvas_texture(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		RenderingServer *p_rendering_server,
		RuntimeExternalSurfaceSlot &p_slot) {
	if (!p_slot.canvas_texture.is_valid()) {
		return;
	}
	bool owns_texture = false;
	{
		MutexLock lock(p_state->mutex);
		const int texture_index = p_state->owned_canvas_textures.find(p_slot.canvas_texture);
		if (texture_index >= 0) {
			p_state->owned_canvas_textures.remove_at(texture_index);
			owns_texture = true;
		}
	}
	if (owns_texture) {
		p_rendering_server->free_rid(p_slot.canvas_texture);
	}
	p_slot.canvas_texture = RID();
}

static void runtime_set_terminal(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		const String &p_reason) {
	MutexLock lock(p_state->mutex);
	p_state->terminal = true;
	p_state->pending_work = false;
	p_state->terminal_reason = p_reason;
	ERR_PRINT(p_reason);
}

static String runtime_compilation_failure(
		hcsr_runtime_compilation_report_t *p_report,
		const hcsr_runtime_compilation_report_info_t &p_info) {
	if (p_info.diagnostic_count <= 0) {
		return "HCSR replacement compilation failed without a diagnostic.";
	}
	hcsr_runtime_compilation_diagnostic_info_t diagnostic;
	initialize_abi(&diagnostic, sizeof(diagnostic));
	if (hcsr_runtime_compilation_report_get_diagnostic(p_report, 0, &diagnostic) != HCSR_RUNTIME_OK) {
		return "HCSR replacement compilation diagnostics could not be read.";
	}
	Vector<char> message;
	message.resize(MAX(1, diagnostic.message_utf8_bytes + 1));
	if (hcsr_runtime_compilation_report_copy_diagnostic_text(
			p_report, 0, 1, message.ptrw(), message.size()) != HCSR_RUNTIME_OK) {
		return "HCSR replacement compilation diagnostic text could not be read.";
	}
	return "HCSR replacement document is unsupported: " + String::utf8(message.ptr());
}

static bool runtime_schedule_state(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state) {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		runtime_set_terminal(p_state, "RenderingServer is unavailable for HCSR replacement work.");
		return false;
	}
	{
		MutexLock lock(p_state->mutex);
		if (p_state->work_scheduled) {
			return true;
		}
		p_state->work_scheduled = true;
	}
	rendering_server->call_on_render_thread(
			callable_mp_static(&HTMLSurfaceHCSRRuntimeBackend::_step_on_render_thread_callback)
					.bind((uint64_t)p_state));
	return true;
}

static bool runtime_schedule_frame_cutoff(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state) {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		runtime_set_terminal(p_state, "RenderingServer is unavailable for the HCSR frame cutoff.");
		return false;
	}
	const uint64_t process_frame = Engine::get_singleton()->get_process_frames();
	{
		MutexLock lock(p_state->mutex);
		if (p_state->cutoff_scheduled) {
			p_state->cutoff_process_frame = MAX(p_state->cutoff_process_frame, process_frame);
			return true;
		}
		p_state->cutoff_scheduled = true;
		p_state->cutoff_process_frame = process_frame;
	}
	rendering_server->call_on_render_thread(
			callable_mp_static(&HTMLSurfaceHCSRRuntimeBackend::_activate_frame_cutoff_on_render_thread_callback)
					.bind((uint64_t)p_state));
	return true;
}

static RuntimePublicationLineage runtime_lineage_from_publication(
		uint64_t p_request_serial,
		const hcsr_runtime_publication_info_t &p_info) {
	RuntimePublicationLineage lineage;
	lineage.valid = true;
	lineage.request_serial = p_request_serial;
	lineage.runtime_generation = p_info.generation;
	lineage.semantic_frame_generation = p_info.semantic_frame_generation;
	lineage.target_author_revision = p_info.target_author_revision;
	lineage.interactive_submission_id = p_info.interactive_submission_id;
	lineage.interactive_frame_id = p_info.interactive_frame_id;
	return lineage;
}

static RuntimePresentationBinding *runtime_create_presentation_binding(
		void *p_device,
		void *p_queue,
		RenderingDevice *p_rendering_device) {
	RuntimePresentationBinding *binding = memnew(RuntimePresentationBinding);
	if (hcsr_runtime_d3d12_presenter_create(
			p_device, p_queue, &binding->presenter) != HCSR_RUNTIME_OK
			|| binding->presenter == nullptr) {
		memdelete(binding);
		return nullptr;
	}
	binding->external_texture_pool = p_rendering_device->external_texture_pool_create();
	if (!binding->external_texture_pool.is_valid()) {
		hcsr_runtime_d3d12_presenter_begin_shutdown(binding->presenter);
		hcsr_runtime_step_info_t step;
		initialize_abi(&step, sizeof(step));
		if (hcsr_runtime_d3d12_presenter_step_shutdown(binding->presenter, 4096, &step)
				== HCSR_RUNTIME_CLOSED) {
			hcsr_runtime_d3d12_presenter_destroy(binding->presenter);
		}
		memdelete(binding);
		return nullptr;
	}
	return binding;
}

static bool runtime_ensure_initialized(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state) {
	if (p_state->session != nullptr) {
		return true;
	}
	if (hcsr_runtime_get_abi_version() != HCSR_RUNTIME_ABI_VERSION
			|| HCSR_RUNTIME_ABI_VERSION != 5) {
		runtime_set_terminal(p_state, "HCSR replacement ABI mismatch; Godot requires runtime ABI v5.");
		return false;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	RenderingDevice *rendering_device = rendering_server != nullptr
			? rendering_server->get_rendering_device()
			: nullptr;
	if (rendering_device == nullptr) {
		runtime_set_terminal(p_state, "Godot did not expose a RenderingDevice to HCSR replacement.");
		return false;
	}
	void *device = (void *)rendering_device->get_driver_resource(
			RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE);
	void *queue = (void *)rendering_device->get_driver_resource(
			RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE);
	if (device == nullptr || queue == nullptr) {
		runtime_set_terminal(p_state, "Godot did not expose its D3D12 device and direct queue to HCSR replacement.");
		return false;
	}
	hcsr_runtime_output_configuration_t output;
	initialize_abi(&output, sizeof(output));
	output.output_id = 1;
	output.pixel_width = p_state->physical_size.x;
	output.pixel_height = p_state->physical_size.y;
	output.logical_width = p_state->logical_size.x;
	output.logical_height = p_state->logical_size.y;
	output.tile_size = 64;
	if (hcsr_runtime_session_create(
			p_state->logical_size.x,
			p_state->logical_size.y,
			&output,
			1,
			&p_state->session) != HCSR_RUNTIME_OK) {
			runtime_set_terminal(p_state, "HCSR replacement could not create its RuntimeSession D3D12 presenter.");
		return false;
	}
	p_state->active_binding = runtime_create_presentation_binding(device, queue, rendering_device);
	if (p_state->active_binding == nullptr) {
		runtime_set_terminal(p_state, "Godot could not create the HCSR configuration presentation binding.");
		return false;
	}
	return true;
}

static bool runtime_submit_document(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		const String &p_html,
		const String &p_css) {
	CharString html_utf8 = p_html.utf8();
	CharString css_utf8 = p_css.utf8();
	hcsr_runtime_document_t *document = nullptr;
	hcsr_runtime_compilation_report_t *report = nullptr;
	const hcsr_runtime_status_t compile_status = hcsr_runtime_document_compile(
			html_utf8.get_data(), css_utf8.get_data(), &document, &report);
	if (report == nullptr) {
		runtime_set_terminal(p_state, "HCSR replacement did not return a compilation report.");
		return false;
	}
	hcsr_runtime_compilation_report_info_t info;
	initialize_abi(&info, sizeof(info));
	const bool report_valid = hcsr_runtime_compilation_report_get_info(report, &info) == HCSR_RUNTIME_OK;
	if (!report_valid || compile_status != HCSR_RUNTIME_OK || info.success == 0
			|| info.diagnostic_count != 0 || document == nullptr) {
		const String failure = report_valid
				? runtime_compilation_failure(report, info)
				: String("HCSR replacement compilation report was invalid.");
		hcsr_runtime_compilation_report_release(report);
		if (document != nullptr) {
			hcsr_runtime_document_release(document);
		}
		runtime_set_terminal(p_state, failure);
		return false;
	}
	hcsr_runtime_compilation_report_release(report);
	if (hcsr_runtime_session_submit_document(
			p_state->session, ++p_state->document_request_id, document) != HCSR_RUNTIME_OK) {
		hcsr_runtime_document_release(document);
		runtime_set_terminal(p_state, "HCSR replacement rejected the compiled document publication.");
		return false;
	}
	if (p_state->compiled_document != nullptr) {
		hcsr_runtime_document_release(p_state->compiled_document);
	}
	p_state->compiled_document = document;
	p_state->pending_work = true;
	return true;
}

static bool runtime_submit_configuration(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state) {
	hcsr_runtime_output_configuration_t output;
	initialize_abi(&output, sizeof(output));
	output.output_id = 1;
	output.pixel_width = p_state->physical_size.x;
	output.pixel_height = p_state->physical_size.y;
	output.logical_width = p_state->logical_size.x;
	output.logical_height = p_state->logical_size.y;
	output.tile_size = 64;
	const hcsr_runtime_status_t configuration_status = hcsr_runtime_session_submit_configuration(
			p_state->session,
			p_state->logical_size.x,
			p_state->logical_size.y,
			&output,
			1);
	if (configuration_status != HCSR_RUNTIME_OK) {
		runtime_set_terminal(p_state, vformat(
				"HCSR replacement rejected the Godot output configuration (status %d, logical %dx%d, physical %dx%d).",
				(int)configuration_status,
				p_state->logical_size.x,
				p_state->logical_size.y,
				p_state->physical_size.x,
				p_state->physical_size.y));
		return false;
	}
	p_state->pending_work = true;
	return true;
}

static bool runtime_submit_mutations(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		const Vector<RuntimeMutation> &p_mutations,
		uint64_t p_request_process_frame,
		uint64_t p_request_serial) {
	if (p_mutations.is_empty()) {
		return true;
	}
	hcsr_runtime_mutation_t *journal = nullptr;
	if (hcsr_runtime_mutation_begin(p_state->session, &journal) != HCSR_RUNTIME_OK
			|| journal == nullptr) {
		runtime_set_terminal(p_state, "HCSR replacement could not begin an author mutation journal.");
		return false;
	}
	for (const RuntimeMutation &mutation : p_mutations) {
		CharString id = mutation.id.utf8();
		CharString name = mutation.name.utf8();
		CharString value = mutation.value.utf8();
		hcsr_runtime_status_t status = HCSR_RUNTIME_INVALID_ARGUMENT;
		switch (mutation.kind) {
			case RUNTIME_MUTATION_TEXT:
				status = hcsr_runtime_mutation_set_text_by_id(journal, id.get_data(), value.get_data());
				break;
			case RUNTIME_MUTATION_ATTRIBUTE:
				status = hcsr_runtime_mutation_set_attribute_by_id(journal, id.get_data(), name.get_data(), value.get_data());
				break;
			case RUNTIME_MUTATION_STYLE:
				status = hcsr_runtime_mutation_set_inline_style_by_id(journal, id.get_data(), value.get_data());
				break;
			case RUNTIME_MUTATION_CHECKED:
				status = hcsr_runtime_mutation_set_control_checked_by_id(journal, id.get_data(), mutation.value == "1" ? 1 : 0);
				break;
		}
		if (status != HCSR_RUNTIME_OK) {
			hcsr_runtime_mutation_release(journal);
			runtime_set_terminal(p_state, "HCSR replacement rejected a Godot mutation journal operation.");
			return false;
		}
	}
	hcsr_runtime_submission_info_t submission;
	initialize_abi(&submission, sizeof(submission));
	const uint64_t now = hcsr_runtime_get_monotonic_timestamp_microseconds();
	const uint64_t cutoff_timestamp = now + 16667;
	const hcsr_runtime_status_t status = hcsr_runtime_session_submit_mutation_with_priority(
			p_state->session,
			journal,
			HCSR_RUNTIME_MUTATION_PRIORITY_INTERACTIVE,
			MAX((uint64_t)1, p_request_process_frame),
			cutoff_timestamp,
			&submission);
	if (status != HCSR_RUNTIME_OK) {
		runtime_set_terminal(p_state, "HCSR replacement rejected the Godot interactive mutation journal.");
		return false;
	}
	p_state->pending_work = true;
	p_state->interactive_pending = true;
	p_state->submitted_request_serial = p_request_serial;
	p_state->newest_requested_submission_id = submission.submission_id;
	p_state->newest_requested_author_revision = submission.target_author_revision;
	p_state->newest_requested_frame_id = submission.frame_id;
	p_state->newest_requested_cutoff_timestamp_microseconds = cutoff_timestamp;
	return true;
}

static hcsr_runtime_status_t runtime_step_presenter_sliced(
		hcsr_runtime_d3d12_presenter_t *p_presenter,
		uint64_t p_cutoff_timestamp_microseconds,
		hcsr_runtime_step_info_t *r_step) {
	hcsr_runtime_status_t status = HCSR_RUNTIME_PENDING;
	do {
		initialize_abi(r_step, sizeof(*r_step));
		status = hcsr_runtime_d3d12_presenter_step(
				p_presenter, RUNTIME_PRESENTER_STEP_SLICE_UNITS, r_step);
	} while (status == HCSR_RUNTIME_PENDING
			&& p_cutoff_timestamp_microseconds != 0
			&& hcsr_runtime_get_monotonic_timestamp_microseconds() < p_cutoff_timestamp_microseconds);
	return status;
}

static RuntimeExternalSurfaceSlot *runtime_find_external_slot(
		RuntimePresentationBinding *p_binding,
		uint64_t p_hcsr_slot_id) {
	for (RuntimeExternalSurfaceSlot &slot : p_binding->slots) {
		if (slot.hcsr_slot_id == p_hcsr_slot_id) {
			return &slot;
		}
	}
	return nullptr;
}

static bool runtime_release_completed_external_slots(
		RuntimePresentationBinding *p_binding,
		RenderingDevice *p_device) {
	for (RuntimeExternalSurfaceSlot &slot : p_binding->slots) {
		if (slot.generation_surface == nullptr) {
			continue;
		}
		const Dictionary status = p_device->external_texture_pool_get_slot_status(
				p_binding->external_texture_pool, slot.godot_slot);
		if (status.is_empty()) {
			return false;
		}
		const bool current = bool(status.get("current", false));
		const bool retired = bool(status.get("retired", false));
		const bool release_complete = bool(status.get("release_complete", false));
		if (!current && retired && release_complete) {
			hcsr_runtime_d3d12_surface_release(slot.generation_surface);
			slot.generation_surface = nullptr;
		}
	}
	return true;
}

static RuntimeExternalSurfaceSlot *runtime_register_external_slot(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		RuntimePresentationBinding *p_binding,
		RenderingServer *p_rendering_server,
		RenderingDevice *p_device,
		hcsr_runtime_d3d12_surface_t *p_surface,
		const hcsr_runtime_d3d12_surface_info_t &p_surface_info,
		const hcsr_runtime_d3d12_surface_synchronization_info_t &p_sync) {
	hcsr_runtime_d3d12_surface_registration_t *registration = nullptr;
	hcsr_runtime_d3d12_surface_synchronization_info_t registered_sync;
	initialize_abi(&registered_sync, sizeof(registered_sync));
	if (hcsr_runtime_d3d12_surface_register(
			p_surface, &registration, &registered_sync) != HCSR_RUNTIME_OK
			|| registration == nullptr
			|| registered_sync.surface_slot_id != p_sync.surface_slot_id
			|| registered_sync.texture != p_sync.texture
			|| registered_sync.producer_timeline != p_sync.producer_timeline) {
		if (registration != nullptr) {
			hcsr_runtime_d3d12_surface_registration_release(registration);
		}
		return nullptr;
	}
	const int32_t godot_slot = p_device->external_texture_pool_add_local_slot(
			p_binding->external_texture_pool,
			RenderingDevice::TEXTURE_TYPE_2D,
			RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
			RenderingDevice::TEXTURE_SAMPLES_1,
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT,
			(uint64_t)registered_sync.texture,
			(uint64_t)registered_sync.producer_timeline,
			p_surface_info.pixel_width,
			p_surface_info.pixel_height,
			1,
			1,
			1,
			RenderingDevice::EXTERNAL_TEXTURE_STATE_SHADER_READ);
	if (godot_slot < 0) {
		hcsr_runtime_d3d12_surface_registration_release(registration);
		return nullptr;
	}
	const Dictionary status = p_device->external_texture_pool_get_slot_status(
			p_binding->external_texture_pool, godot_slot);
	const RID rd_texture = status.get("texture", RID());
	const RID canvas_texture = rd_texture.is_valid()
			? p_rendering_server->texture_rd_create(rd_texture)
			: RID();
	if (!rd_texture.is_valid() || !canvas_texture.is_valid()) {
		if (canvas_texture.is_valid()) {
			p_rendering_server->free_rid(canvas_texture);
		}
		hcsr_runtime_d3d12_surface_registration_release(registration);
		return nullptr;
	}
	if (!runtime_track_canvas_texture(p_state, canvas_texture)) {
		p_rendering_server->free_rid(canvas_texture);
		hcsr_runtime_d3d12_surface_registration_release(registration);
		return nullptr;
	}
	RuntimeExternalSurfaceSlot slot;
	slot.hcsr_slot_id = registered_sync.surface_slot_id;
	slot.godot_slot = godot_slot;
	slot.registration = registration;
	slot.rd_texture = rd_texture;
	slot.canvas_texture = canvas_texture;
	p_binding->slots.push_back(slot);
	return &p_binding->slots.write[p_binding->slots.size() - 1];
}

static bool runtime_activate_surface(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		RuntimePresentationBinding *p_binding) {
	ERR_FAIL_NULL_V(p_binding, false);
	hcsr_runtime_d3d12_surface_info_t info;
	initialize_abi(&info, sizeof(info));
	if (hcsr_runtime_d3d12_presenter_query_surface(p_binding->presenter, &info)
			!= HCSR_RUNTIME_OK) {
		return false;
	}
	if (info.runtime_generation == p_state->active_generation) {
		return true;
	}
	hcsr_runtime_d3d12_surface_t *surface = nullptr;
	hcsr_runtime_d3d12_surface_info_t acquired;
	initialize_abi(&acquired, sizeof(acquired));
	if (hcsr_runtime_d3d12_presenter_acquire_surface(
			p_binding->presenter,
			info.runtime_generation,
			info.semantic_frame_generation,
			info.configuration_id,
			info.output_id,
			&surface,
			&acquired) != HCSR_RUNTIME_OK
			|| surface == nullptr || acquired.texture == nullptr) {
		runtime_set_terminal(p_state, "HCSR replacement could not acquire its exact D3D12 texture generation.");
		return false;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	RenderingDevice *rendering_device = rendering_server != nullptr
			? rendering_server->get_rendering_device()
			: nullptr;
	if (rendering_server == nullptr || rendering_device == nullptr) {
		hcsr_runtime_d3d12_surface_release(surface);
		runtime_set_terminal(p_state, "Godot rendering disappeared during HCSR texture activation.");
		return false;
	}
	if (!runtime_release_completed_external_slots(p_binding, rendering_device)) {
		hcsr_runtime_d3d12_surface_release(surface);
		runtime_set_terminal(p_state, "Godot could not reconcile released HCSR external texture slots.");
		return false;
	}
	hcsr_runtime_d3d12_surface_synchronization_info_t sync;
	initialize_abi(&sync, sizeof(sync));
	if (hcsr_runtime_d3d12_surface_query_synchronization(surface, &sync) != HCSR_RUNTIME_OK
			|| sync.surface_slot_id == 0 || sync.texture != acquired.texture
			|| sync.producer_timeline == nullptr || sync.producer_value == 0) {
		hcsr_runtime_d3d12_surface_release(surface);
		runtime_set_terminal(p_state, "HCSR replacement returned an invalid D3D12 surface synchronization contract.");
		return false;
	}
	RuntimeExternalSurfaceSlot *slot = runtime_find_external_slot(p_binding, sync.surface_slot_id);
	if (slot == nullptr) {
		slot = runtime_register_external_slot(
				p_state, p_binding, rendering_server, rendering_device, surface, acquired, sync);
	}
	if (slot == nullptr) {
		hcsr_runtime_d3d12_surface_release(surface);
		runtime_set_terminal(p_state, "Godot could not register the stable HCSR external texture slot.");
		return false;
	}
	const bool runtime_only_current = slot->generation_surface != nullptr
			&& p_state->active_binding == p_binding
			&& p_binding->active_slot == slot->godot_slot
			&& p_state->active_semantic_frame_generation == acquired.semantic_frame_generation
			&& p_state->active_configuration_id == acquired.configuration_id
			&& p_state->active_output_id == acquired.output_id;
	if (runtime_only_current) {
		hcsr_runtime_d3d12_surface_release(slot->generation_surface);
		slot->generation_surface = surface;
		slot->runtime_generation = acquired.runtime_generation;
	} else {
		if (slot->generation_surface != nullptr) {
			hcsr_runtime_d3d12_surface_release(surface);
			runtime_set_terminal(p_state, "HCSR attempted to republish an external texture slot before Godot released it.");
			return false;
		}
		const Error publish_error = rendering_device->external_texture_pool_publish(
				p_binding->external_texture_pool,
				slot->godot_slot,
				sync.producer_value,
				acquired.runtime_generation,
				RenderingDevice::EXTERNAL_TEXTURE_STATE_SHADER_READ);
		if (publish_error != OK) {
			hcsr_runtime_d3d12_surface_release(surface);
			runtime_set_terminal(p_state, "Godot rejected the HCSR external texture slot publication.");
			return false;
		}
		slot->generation_surface = surface;
		slot->runtime_generation = acquired.runtime_generation;
		const RID acquired_rd_texture = rendering_device->external_texture_pool_acquire_latest(
				p_binding->external_texture_pool);
		RuntimeExternalSurfaceSlot *active_slot = nullptr;
		for (RuntimeExternalSurfaceSlot &candidate : p_binding->slots) {
			if (candidate.rd_texture == acquired_rd_texture) {
				active_slot = &candidate;
				break;
			}
		}
		if (active_slot == nullptr) {
			runtime_set_terminal(p_state, "Godot did not acquire the exact newest HCSR external texture slot.");
			return false;
		}
		p_binding->active_slot = active_slot->godot_slot;
		if (p_state->active_imported_texture != active_slot->canvas_texture) {
			p_state->texture->set_external_texture(
					active_slot->canvas_texture,
					Size2i(acquired.pixel_width, acquired.pixel_height),
					true);
		}
		p_state->active_imported_texture = active_slot->canvas_texture;
		p_state->presentation_changed = true;
	}
	{
		MutexLock lock(p_state->mutex);
		p_state->active_semantic_frame_generation = acquired.semantic_frame_generation;
		p_state->active_configuration_id = acquired.configuration_id;
		p_state->active_output_id = acquired.output_id;
		p_state->active_pixel_width = acquired.pixel_width;
		p_state->active_pixel_height = acquired.pixel_height;
		p_state->active_pixel_format = acquired.pixel_format;
		p_state->active_generation = acquired.runtime_generation;
		p_state->queued_generation = acquired.runtime_generation;
		p_state->frame_metadata.logical_size = p_state->logical_size;
		p_state->frame_metadata.physical_size = Size2i(acquired.pixel_width, acquired.pixel_height);
		p_state->frame_metadata.generation = acquired.runtime_generation;
		p_state->frame_metadata.host_frame_number = p_state->active_request_process_frame;
	}
	return true;
}

static bool runtime_step_retiring_binding(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		RuntimePresentationBinding *p_binding,
		RenderingServer *p_rendering_server,
		RenderingDevice *p_rendering_device) {
	ERR_FAIL_NULL_V(p_binding, true);
	if (p_binding->external_texture_pool.is_valid()) {
		if (!p_binding->pool_stopped) {
			p_rendering_device->external_texture_pool_stop(p_binding->external_texture_pool);
			p_binding->pool_stopped = true;
		}
		if (!runtime_release_completed_external_slots(p_binding, p_rendering_device)) {
			runtime_set_terminal(p_state, "Godot could not reconcile a retiring HCSR texture pool.");
			return false;
		}
		for (const RuntimeExternalSurfaceSlot &slot : p_binding->slots) {
			if (slot.generation_surface != nullptr) {
				return false;
			}
		}
		for (RuntimeExternalSurfaceSlot &slot : p_binding->slots) {
			runtime_release_canvas_texture(p_state, p_rendering_server, slot);
			if (slot.registration != nullptr) {
				hcsr_runtime_d3d12_surface_registration_release(slot.registration);
				slot.registration = nullptr;
			}
		}
		p_binding->slots.clear();
		p_rendering_server->free_rid(p_binding->external_texture_pool);
		p_binding->external_texture_pool = RID();
		p_binding->active_slot = -1;
	}
	if (p_binding->presenter != nullptr) {
		if (!p_binding->presenter_shutdown_started) {
			if (hcsr_runtime_d3d12_presenter_begin_shutdown(p_binding->presenter)
					!= HCSR_RUNTIME_OK) {
				runtime_set_terminal(p_state, "HCSR replacement could not begin retiring a configuration presenter.");
				return false;
			}
			p_binding->presenter_shutdown_started = true;
		}
		hcsr_runtime_step_info_t step;
		initialize_abi(&step, sizeof(step));
		const hcsr_runtime_status_t status = hcsr_runtime_d3d12_presenter_step_shutdown(
				p_binding->presenter, 4096, &step);
		if (status == HCSR_RUNTIME_PENDING_CLEANUP) {
			return false;
		}
		if (status != HCSR_RUNTIME_CLOSED
				|| hcsr_runtime_d3d12_presenter_destroy(p_binding->presenter) != HCSR_RUNTIME_OK) {
			runtime_set_terminal(p_state, "HCSR replacement could not retire a configuration presenter.");
			return false;
		}
		p_binding->presenter = nullptr;
	}
	return true;
}

static bool runtime_step_active(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state) {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	RenderingDevice *rendering_device = rendering_server != nullptr
			? rendering_server->get_rendering_device()
			: nullptr;
	if (rendering_device == nullptr || p_state->active_binding == nullptr
			|| !runtime_release_completed_external_slots(p_state->active_binding, rendering_device)) {
		runtime_set_terminal(p_state, "Godot could not advance HCSR external texture release ownership.");
		return false;
	}
	if (p_state->retiring_binding != nullptr
			&& runtime_step_retiring_binding(
					p_state, p_state->retiring_binding, rendering_server, rendering_device)) {
		memdelete(p_state->retiring_binding);
		p_state->retiring_binding = nullptr;
	}
	if (p_state->terminal) {
		return false;
	}
	hcsr_runtime_step_info_t step;
	initialize_abi(&step, sizeof(step));
	bool semantic_publication_ready = false;
	bool semantic_pending = false;
	hcsr_runtime_status_t status = HCSR_RUNTIME_OK;
	if (p_state->interactive_pending) {
		hcsr_runtime_interactive_step_info_t interactive;
		do {
			initialize_abi(&interactive, sizeof(interactive));
			status = hcsr_runtime_session_step_interactive(
					p_state->session, RUNTIME_INTERACTIVE_STEP_SLICE_UNITS, &interactive);
		} while (status == HCSR_RUNTIME_PENDING);
		p_state->interactive_pending = false;
		semantic_publication_ready = status == HCSR_RUNTIME_OK
				&& interactive.runtime_generation != 0;
		semantic_pending = status == HCSR_RUNTIME_OK && !semantic_publication_ready;
	} else {
		status = hcsr_runtime_session_step(p_state->session, 4096, &step);
		semantic_publication_ready = status == HCSR_RUNTIME_OK;
		semantic_pending = status == HCSR_RUNTIME_PENDING;
	}
	if (status != HCSR_RUNTIME_OK && status != HCSR_RUNTIME_PENDING) {
		runtime_set_terminal(p_state, "HCSR replacement semantic derivation failed.");
		return false;
	}
	if (semantic_publication_ready) {
		hcsr_runtime_publication_t *publication = nullptr;
		hcsr_runtime_publication_info_t publication_info;
		initialize_abi(&publication_info, sizeof(publication_info));
		uint64_t acquire_after_generation = p_state->consumed_runtime_generation;
		for (int publication_index = 0; publication_index < 64; publication_index++) {
			hcsr_runtime_publication_t *next_publication = nullptr;
			hcsr_runtime_publication_info_t next_info;
			initialize_abi(&next_info, sizeof(next_info));
			const hcsr_runtime_status_t acquire_status = hcsr_runtime_session_acquire_publication(
					p_state->session,
					acquire_after_generation,
					&next_publication,
					&next_info);
			if (acquire_status != HCSR_RUNTIME_OK || next_publication == nullptr) {
				break;
			}
			if (publication != nullptr) {
				hcsr_runtime_publication_release(p_state->session, publication);
			}
			publication = next_publication;
			publication_info = next_info;
			acquire_after_generation = next_info.generation;
		}
		p_state->consumed_runtime_generation = acquire_after_generation;
		if (publication != nullptr) {
			const bool has_interactive_authority = p_state->newest_requested_submission_id != 0;
			const bool exact_interactive_authority = !has_interactive_authority
					|| (publication_info.interactive_submission_id == p_state->newest_requested_submission_id
							&& publication_info.target_author_revision == p_state->newest_requested_author_revision
							&& publication_info.interactive_frame_id == p_state->newest_requested_frame_id);
			if (!exact_interactive_authority) {
				const bool proven_obsolete = publication_info.interactive_submission_id < p_state->newest_requested_submission_id
						|| publication_info.target_author_revision < p_state->newest_requested_author_revision;
				hcsr_runtime_publication_release(p_state->session, publication);
				if (!proven_obsolete) {
					runtime_set_terminal(p_state, "HCSR replacement produced a publication outside the newest requested interactive lineage.");
					return false;
				}
				semantic_pending = true;
				publication = nullptr;
			}
		}
		if (publication != nullptr) {
			hcsr_runtime_output_info_t output_info;
			initialize_abi(&output_info, sizeof(output_info));
			const hcsr_runtime_status_t output_status = hcsr_runtime_publication_get_output(
					publication,
					publication_info.generation,
					0,
					&output_info);
			if (output_status != HCSR_RUNTIME_OK) {
				hcsr_runtime_publication_release(p_state->session, publication);
				runtime_set_terminal(p_state, vformat("HCSR replacement publication validation failed for handle=%d generation=%d with status %d.", (uint64_t)publication, publication_info.generation, (int)output_status));
				return false;
			}
			RuntimePresentationBinding *target_binding = p_state->successor_binding != nullptr
					? p_state->successor_binding
					: p_state->active_binding;
			hcsr_runtime_status_t submit_status = hcsr_runtime_d3d12_presenter_submit(
					target_binding->presenter,
					publication,
					publication_info.generation,
					0);
			bool submitted_to_successor = false;
			if (submit_status == HCSR_RUNTIME_RECONFIGURATION_REQUIRED
					&& target_binding == p_state->active_binding) {
				RenderingServer *rendering_server = RenderingServer::get_singleton();
				RenderingDevice *rendering_device = rendering_server != nullptr
						? rendering_server->get_rendering_device()
						: nullptr;
				void *device = rendering_device != nullptr
						? (void *)rendering_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE)
						: nullptr;
				void *queue = rendering_device != nullptr
						? (void *)rendering_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE)
						: nullptr;
				if (p_state->successor_binding != nullptr || device == nullptr || queue == nullptr) {
					hcsr_runtime_publication_release(p_state->session, publication);
					runtime_set_terminal(p_state, "HCSR replacement could not create its successor D3D12 presenter.");
					return false;
				}
				p_state->successor_binding = runtime_create_presentation_binding(
						device, queue, rendering_device);
				if (p_state->successor_binding == nullptr) {
					hcsr_runtime_publication_release(p_state->session, publication);
					runtime_set_terminal(p_state, "HCSR replacement could not create its successor presentation binding.");
					return false;
				}
				target_binding = p_state->successor_binding;
				submit_status = hcsr_runtime_d3d12_presenter_submit(
						p_state->successor_binding->presenter,
						publication,
						publication_info.generation,
						0);
				submitted_to_successor = submit_status == HCSR_RUNTIME_OK;
			}
			hcsr_runtime_publication_release(p_state->session, publication);
			if (submit_status == HCSR_RUNTIME_STALE_REQUEST) {
				const bool proven_superseded = publication_info.interactive_submission_id < p_state->newest_requested_submission_id
						|| publication_info.target_author_revision < p_state->newest_requested_author_revision
						|| publication_info.generation < p_state->queued_generation;
				if (!proven_superseded) {
					runtime_set_terminal(p_state, "HCSR replacement reported a stale presenter request without a proven newer authority.");
					return false;
				}
				semantic_pending = true;
			} else if (submit_status != HCSR_RUNTIME_OK) {
				runtime_set_terminal(p_state, vformat("HCSR replacement D3D12 presenter rejected runtime generation %d semantic generation %d with status %d.", publication_info.generation, publication_info.semantic_frame_generation, (int)submit_status));
				return false;
			} else {
				const RuntimePublicationLineage lineage = runtime_lineage_from_publication(
						p_state->submitted_request_serial, publication_info);
				p_state->queued_generation = publication_info.generation;
				(void)submitted_to_successor;
				target_binding->pending_lineage = lineage;
				target_binding->presenter_pending = true;
			}
		}
	}
	if (p_state->active_binding->presenter_pending) {
		const uint64_t presenter_cutoff = p_state->active_binding->pending_lineage.interactive_submission_id
				== p_state->newest_requested_submission_id
			? p_state->newest_requested_cutoff_timestamp_microseconds
			: 0;
		status = runtime_step_presenter_sliced(
				p_state->active_binding->presenter, presenter_cutoff, &step);
		if (status != HCSR_RUNTIME_OK && status != HCSR_RUNTIME_PENDING) {
			runtime_set_terminal(p_state, "HCSR replacement D3D12 presenter failed.");
			return false;
		}
		if (status == HCSR_RUNTIME_OK) {
			p_state->active_binding->presenter_pending = false;
			p_state->staged_binding = p_state->active_binding;
			p_state->staged_lineage = p_state->active_binding->pending_lineage;
			p_state->activation_pending = true;
		}
	}
	if (p_state->successor_binding != nullptr
			&& p_state->successor_binding->presenter_pending) {
		const uint64_t presenter_cutoff = p_state->successor_binding->pending_lineage.interactive_submission_id
				== p_state->newest_requested_submission_id
			? p_state->newest_requested_cutoff_timestamp_microseconds
			: 0;
		status = runtime_step_presenter_sliced(
				p_state->successor_binding->presenter, presenter_cutoff, &step);
		if (status != HCSR_RUNTIME_OK && status != HCSR_RUNTIME_PENDING) {
			runtime_set_terminal(p_state, "HCSR replacement successor D3D12 presenter failed.");
			return false;
		}
		if (status == HCSR_RUNTIME_OK) {
			p_state->successor_binding->presenter_pending = false;
			p_state->staged_binding = p_state->successor_binding;
			p_state->staged_lineage = p_state->successor_binding->pending_lineage;
			p_state->activation_pending = true;
		}
	}
	p_state->pending_work = p_state->active_binding->presenter_pending
			|| (p_state->successor_binding != nullptr && p_state->successor_binding->presenter_pending)
			|| p_state->activation_pending
			|| p_state->retiring_binding != nullptr
			|| semantic_pending;
	return true;
}

static bool runtime_step_shutdown(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state) {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	RenderingDevice *rendering_device = rendering_server != nullptr
			? rendering_server->get_rendering_device()
			: nullptr;
	if (rendering_server == nullptr || rendering_device == nullptr) {
		return false;
	}
	if (p_state->active_imported_texture.is_valid()) {
		if (p_state->texture.is_valid()) {
			p_state->texture->clear_external_texture();
		}
		p_state->active_imported_texture = RID();
	}
	bool bindings_complete = true;
	auto step_binding = [&](RuntimePresentationBinding *&p_binding) {
		if (p_binding == nullptr) {
			return;
		}
		if (!runtime_step_retiring_binding(
				p_state, p_binding, rendering_server, rendering_device)) {
			bindings_complete = false;
			return;
		}
		if (p_state->staged_binding == p_binding) {
			p_state->staged_binding = nullptr;
			p_state->staged_lineage = RuntimePublicationLineage();
		}
		memdelete(p_binding);
		p_binding = nullptr;
	};
	step_binding(p_state->successor_binding);
	step_binding(p_state->retiring_binding);
	step_binding(p_state->active_binding);
	if (!bindings_complete) {
		return false;
	}
	hcsr_runtime_step_info_t step;
	initialize_abi(&step, sizeof(step));
	if (p_state->session != nullptr) {
		if (!p_state->session_shutdown_started) {
			hcsr_runtime_session_begin_shutdown(p_state->session);
			p_state->session_shutdown_started = true;
		}
		initialize_abi(&step, sizeof(step));
		const hcsr_runtime_status_t status = hcsr_runtime_session_step_retirement(
				p_state->session, 4096, &step);
		if (status == HCSR_RUNTIME_PENDING_CLEANUP
				|| status == HCSR_RUNTIME_WAITING_FOR_LEASES) {
			return false;
		}
		if (status != HCSR_RUNTIME_CLOSED
				|| hcsr_runtime_session_destroy(p_state->session) != HCSR_RUNTIME_OK) {
			return false;
		}
		p_state->session = nullptr;
	}
	if (p_state->compiled_document != nullptr) {
		hcsr_runtime_document_release(p_state->compiled_document);
		p_state->compiled_document = nullptr;
	}
	return true;
}

void HTMLSurfaceHCSRRuntimeBackend::_step_on_render_thread_callback(uint64_t p_state_ptr) {
	RuntimeState *runtime = (RuntimeState *)p_state_ptr;
	String html;
	String css;
	Vector<RuntimeMutation> mutations;
	uint64_t mutation_request_process_frame = 0;
	uint64_t request_serial = 0;
	bool document_dirty = false;
	bool configuration_dirty = false;
	bool closing = false;
	{
		MutexLock lock(runtime->mutex);
		runtime->work_scheduled = false;
		closing = runtime->closing;
		if (!closing && !runtime->terminal) {
			document_dirty = runtime->document_dirty;
			configuration_dirty = runtime->configuration_dirty;
			runtime->document_dirty = false;
			runtime->configuration_dirty = false;
			html = runtime->html;
			css = runtime->css;
			mutations = runtime->mutations;
			mutation_request_process_frame = runtime->mutation_request_process_frame;
			request_serial = runtime->request_serial;
			runtime->mutations.clear();
		}
	}
	if (closing) {
		if (runtime_step_shutdown(runtime)) {
			memdelete(runtime);
			return;
		}
		RenderingDevice *device = RenderingServer::get_singleton()->get_rendering_device();
		device->external_resource_defer_release(
				callable_mp_static(&HTMLSurfaceHCSRRuntimeBackend::_destroy_state_on_render_thread_callback)
						.bind((uint64_t)runtime));
		return;
	}
	if (runtime->session == nullptr && !document_dirty) {
		// Do not freeze the constructor's placeholder dimensions into a session.
		// The first real document snapshot and the final control size establish
		// the initial atomic configuration together.
		return;
	}
	const bool initialized_before_step = runtime->session != nullptr;
	if (runtime->terminal || !runtime_ensure_initialized(runtime)) {
		return;
	}
	if (!initialized_before_step) {
		// Session creation consumed the latest logical and physical dimensions.
		configuration_dirty = false;
	}
	if (document_dirty && !runtime_submit_document(runtime, html, css)) {
		return;
	}
	if (configuration_dirty && !runtime_submit_configuration(runtime)) {
		return;
	}
	if (!runtime_submit_mutations(runtime, mutations, mutation_request_process_frame, request_serial)) {
		return;
	}
	if (mutations.is_empty() && (document_dirty || configuration_dirty)) {
		runtime->submitted_request_serial = request_serial;
	}
	if (runtime->pending_work) {
		runtime_step_active(runtime);
	}
}

void HTMLSurfaceHCSRRuntimeBackend::_activate_frame_cutoff_on_render_thread_callback(uint64_t p_state_ptr) {
	RuntimeState *runtime = (RuntimeState *)p_state_ptr;
	uint64_t cutoff_process_frame = 0;
	uint64_t cutoff_request_serial = 0;
	{
		MutexLock lock(runtime->mutex);
		runtime->cutoff_scheduled = false;
		cutoff_process_frame = runtime->cutoff_process_frame;
		cutoff_request_serial = runtime->request_serial;
		if (runtime->closing || runtime->terminal) {
			return;
		}
	}
	const uint64_t current_process_frame = Engine::get_singleton()->get_process_frames();
	const bool exact_requested_authority = runtime->staged_lineage.valid
			&& runtime->staged_lineage.request_serial == cutoff_request_serial
			&& (runtime->newest_requested_submission_id == 0
					|| (runtime->staged_lineage.interactive_submission_id == runtime->newest_requested_submission_id
							&& runtime->staged_lineage.target_author_revision == runtime->newest_requested_author_revision
							&& runtime->staged_lineage.interactive_frame_id == runtime->newest_requested_frame_id));
	if (runtime->activation_pending
			&& exact_requested_authority
			&& runtime->staged_binding != nullptr
			&& !runtime->staged_binding->presenter_pending
			&& (runtime->staged_binding != runtime->successor_binding
					|| runtime->retiring_binding == nullptr)
			&& current_process_frame == cutoff_process_frame
			&& cutoff_process_frame != runtime->last_activation_process_frame) {
		runtime->active_request_process_frame = runtime->staged_lineage.interactive_frame_id;
		RuntimePresentationBinding *activated_binding = runtime->staged_binding;
		if (!runtime_activate_surface(runtime, activated_binding)) {
			return;
		}
		if (activated_binding == runtime->successor_binding) {
			runtime->retiring_binding = runtime->active_binding;
			runtime->active_binding = runtime->successor_binding;
			runtime->successor_binding = nullptr;
		}
		runtime->activation_pending = false;
		runtime->staged_binding = nullptr;
		runtime->staged_lineage = RuntimePublicationLineage();
		runtime->last_activation_process_frame = cutoff_process_frame;
	}
	runtime->pending_work = (runtime->active_binding != nullptr && runtime->active_binding->presenter_pending)
			|| (runtime->successor_binding != nullptr && runtime->successor_binding->presenter_pending)
			|| runtime->activation_pending
			|| runtime->retiring_binding != nullptr;
}

void HTMLSurfaceHCSRRuntimeBackend::_destroy_state_on_render_thread_callback(uint64_t p_state_ptr) {
	_step_on_render_thread_callback(p_state_ptr);
}

void HTMLSurfaceHCSRRuntimeBackend::_schedule_work() {
	if (state != nullptr) {
		runtime_schedule_state(state);
	}
}

void HTMLSurfaceHCSRRuntimeBackend::_queue_document_snapshot() {
	if (state == nullptr) {
		return;
	}
	String html;
	String css;
	if (document.is_valid()) {
		html = document->get_html();
		css = document->get_css();
	}
	if (html.strip_edges().is_empty()) {
		return;
	}
	{
		MutexLock lock(state->mutex);
		state->html = html;
		state->css = css;
		state->document_dirty = true;
		state->request_serial++;
		state->pending_work = true;
	}
	_schedule_work();
}

Error HTMLSurfaceHCSRRuntimeBackend::_queue_mutation(
		int p_kind,
		const StringName &p_id,
		const StringName &p_name,
		const String &p_value) {
	ERR_FAIL_NULL_V(state, ERR_UNAVAILABLE);
	RuntimeMutation mutation;
	mutation.kind = p_kind;
	mutation.id = String(p_id);
	mutation.name = String(p_name);
	mutation.value = p_value;
	{
		MutexLock lock(state->mutex);
		ERR_FAIL_COND_V(state->terminal || state->closing, ERR_UNAVAILABLE);
		state->mutations.push_back(mutation);
		state->mutation_request_process_frame = Engine::get_singleton()->get_process_frames();
		state->request_serial++;
		state->pending_work = true;
	}
	_schedule_work();
	return OK;
}

void HTMLSurfaceHCSRRuntimeBackend::mark_document_dirty() {
	_queue_document_snapshot();
}

void HTMLSurfaceHCSRRuntimeBackend::set_size(const Size2i &p_size) {
	ERR_FAIL_NULL(state);
	MutexLock lock(state->mutex);
	const Size2i requested_size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
	if (state->logical_size == requested_size) {
		return;
	}
	state->logical_size = requested_size;
	state->configuration_dirty = state->session != nullptr;
	state->request_serial++;
	state->pending_work = true;
}

void HTMLSurfaceHCSRRuntimeBackend::set_device_scale_factor(float p_device_scale_factor) {
	(void)p_device_scale_factor;
}

void HTMLSurfaceHCSRRuntimeBackend::set_physical_size(const Size2i &p_physical_size) {
	ERR_FAIL_NULL(state);
	MutexLock lock(state->mutex);
	const Size2i requested_size = Size2i(MAX(1, p_physical_size.x), MAX(1, p_physical_size.y));
	if (state->physical_size == requested_size) {
		return;
	}
	state->physical_size = requested_size;
	state->configuration_dirty = state->session != nullptr;
	state->request_serial++;
	state->pending_work = true;
}

void HTMLSurfaceHCSRRuntimeBackend::set_document(const Ref<HTMLDocument> &p_document) {
	if (document == p_document) {
		return;
	}
	document = p_document;
	_queue_document_snapshot();
}

void HTMLSurfaceHCSRRuntimeBackend::set_transparent_background(bool p_transparent_background) {
	(void)p_transparent_background;
}

void HTMLSurfaceHCSRRuntimeBackend::set_background_color(const Color &p_background_color) {
	(void)p_background_color;
}

void HTMLSurfaceHCSRRuntimeBackend::set_placeholder_background(const Color &p_color) {
	(void)p_color;
}

Error HTMLSurfaceHCSRRuntimeBackend::update_compositor(
		double p_timeline_time_seconds,
		bool *r_needs_output,
		bool *r_needs_begin_frame) {
	(void)p_timeline_time_seconds;
	if (r_needs_output != nullptr) {
		*r_needs_output = has_pending_output();
	}
	if (r_needs_begin_frame != nullptr) {
		*r_needs_begin_frame = has_pending_output();
	}
	return OK;
}

void HTMLSurfaceHCSRRuntimeBackend::render_placeholder(const String &p_marker) {
	(void)p_marker;
	_schedule_work();
	if (state != nullptr) {
		runtime_schedule_frame_cutoff(state);
	}
}

bool HTMLSurfaceHCSRRuntimeBackend::poll_pending_output(bool *r_waiting_for_completion) {
	if (state == nullptr) {
		return false;
	}
	bool changed = false;
	bool pending = false;
	{
		MutexLock lock(state->mutex);
		changed = state->presentation_changed;
		state->presentation_changed = false;
		pending = state->pending_work || state->work_scheduled;
	}
	if (pending) {
		_schedule_work();
	}
	if (r_waiting_for_completion != nullptr) {
		*r_waiting_for_completion = pending;
	}
	return changed;
}

HTMLPendingOutputState HTMLSurfaceHCSRRuntimeBackend::consume_pending_output_state() {
	HTMLPendingOutputState result;
	bool pending = false;
	result.presentation_changed = poll_pending_output(&pending);
	// Pending replacement work is a resumable producer state, not evidence that
	// physical surface capacity blocked forward progress.
	result.producer_blocked = false;
	result.retirement_pending = false;
	return result;
}

bool HTMLSurfaceHCSRRuntimeBackend::has_pending_output() const {
	if (state == nullptr) {
		return false;
	}
	MutexLock lock(state->mutex);
	return state->pending_work || state->work_scheduled;
}

bool HTMLSurfaceHCSRRuntimeBackend::has_pending_frame_request() const {
	return has_pending_output();
}

uint64_t HTMLSurfaceHCSRRuntimeBackend::get_last_queued_frame_generation() const {
	if (state == nullptr) {
		return 0;
	}
	MutexLock lock(state->mutex);
	return state->queued_generation;
}

uint64_t HTMLSurfaceHCSRRuntimeBackend::get_active_frame_generation() const {
	if (state == nullptr) {
		return 0;
	}
	MutexLock lock(state->mutex);
	return state->active_generation;
}

bool HTMLSurfaceHCSRRuntimeBackend::uses_generation_bound_input() const {
	return true;
}

bool HTMLSurfaceHCSRRuntimeBackend::has_terminal_render_failure() const {
	if (state == nullptr) {
		return true;
	}
	MutexLock lock(state->mutex);
	return state->terminal;
}

String HTMLSurfaceHCSRRuntimeBackend::get_terminal_render_failure_reason() const {
	if (state == nullptr) {
		return "HCSR replacement runtime state is unavailable.";
	}
	MutexLock lock(state->mutex);
	return state->terminal_reason;
}

Error HTMLSurfaceHCSRRuntimeBackend::submit_cpu_frame(const HTMLCPUFrame &p_frame) {
	(void)p_frame;
	return ERR_UNAVAILABLE;
}

Error HTMLSurfaceHCSRRuntimeBackend::apply_element_mutations(const Array &p_mutations) {
	ERR_FAIL_NULL_V(state, ERR_UNAVAILABLE);
	if (p_mutations.is_empty()) {
		return OK;
	}
	Vector<RuntimeMutation> parsed;
	parsed.resize(p_mutations.size());
	for (int index = 0; index < p_mutations.size(); index++) {
		if (p_mutations[index].get_type() != Variant::DICTIONARY) {
			return ERR_INVALID_PARAMETER;
		}
		const Dictionary value = p_mutations[index];
		const String operation = value.get("operation", String());
		RuntimeMutation &mutation = parsed.write[index];
		mutation.id = value.get("id", String());
		if (mutation.id.is_empty()) {
			return ERR_INVALID_PARAMETER;
		}
		if (operation == "set_text") {
			mutation.kind = RUNTIME_MUTATION_TEXT;
			mutation.value = value.get("value", String());
		} else if (operation == "set_attribute") {
			mutation.kind = RUNTIME_MUTATION_ATTRIBUTE;
			mutation.name = value.get("name", String());
			mutation.value = value.get("value", String());
			if (mutation.name.is_empty()) {
				return ERR_INVALID_PARAMETER;
			}
		} else if (operation == "set_style") {
			mutation.kind = RUNTIME_MUTATION_STYLE;
			mutation.value = value.get("value", String());
		} else if (operation == "set_checked") {
			mutation.kind = RUNTIME_MUTATION_CHECKED;
			mutation.value = bool(value.get("value", false)) ? "1" : "0";
		} else {
			return ERR_UNAVAILABLE;
		}
	}
	{
		MutexLock lock(state->mutex);
		ERR_FAIL_COND_V(state->terminal || state->closing, ERR_UNAVAILABLE);
		state->mutations.append_array(parsed);
		state->mutation_request_process_frame = Engine::get_singleton()->get_process_frames();
		state->request_serial++;
		state->pending_work = true;
	}
	_schedule_work();
	return OK;
}

Error HTMLSurfaceHCSRRuntimeBackend::set_element_text(
		const StringName &p_id, const String &p_text) {
	return _queue_mutation(RUNTIME_MUTATION_TEXT, p_id, StringName(), p_text);
}

Error HTMLSurfaceHCSRRuntimeBackend::set_element_attribute(
		const StringName &p_id, const StringName &p_name, const String &p_value) {
	return _queue_mutation(RUNTIME_MUTATION_ATTRIBUTE, p_id, p_name, p_value);
}

Error HTMLSurfaceHCSRRuntimeBackend::set_element_style(
		const StringName &p_id, const String &p_css_text) {
	return _queue_mutation(RUNTIME_MUTATION_STYLE, p_id, StringName(), p_css_text);
}

Error HTMLSurfaceHCSRRuntimeBackend::set_form_control_checked(
		const StringName &p_id, bool p_checked) {
	return _queue_mutation(
			RUNTIME_MUTATION_CHECKED, p_id, StringName(), p_checked ? "1" : "0");
}

void HTMLSurfaceHCSRRuntimeBackend::get_frame_metadata(HTMLFrameMetadata &r_metadata) const {
	if (state == nullptr) {
		r_metadata = HTMLFrameMetadata();
		return;
	}
	MutexLock lock(state->mutex);
	r_metadata = state->frame_metadata;
}

Ref<Texture2D> HTMLSurfaceHCSRRuntimeBackend::get_texture() const {
	return texture;
}

Ref<HTMLTexture2D> HTMLSurfaceHCSRRuntimeBackend::get_html_texture() const {
	return texture;
}

HTMLSurfaceHCSRRuntimeBackend::HTMLSurfaceHCSRRuntimeBackend() {
	texture.instantiate();
	state = memnew(RuntimeState);
	state->texture = texture;
	if (hcsr_runtime_get_abi_version() != 5) {
		runtime_set_terminal(state, "HCSR replacement ABI mismatch during Godot module initialization.");
	}
}

HTMLSurfaceHCSRRuntimeBackend::~HTMLSurfaceHCSRRuntimeBackend() {
	RuntimeState *retiring = state;
	state = nullptr;
	document.unref();
	if (retiring == nullptr) {
		texture.unref();
		return;
	}
	Ref<HTMLTexture2D> retiring_texture;
	Vector<RID> canvas_textures;
	{
		MutexLock lock(retiring->mutex);
		retiring->closing = true;
		retiring->pending_work = true;
		retiring_texture = retiring->texture;
		retiring->texture.unref();
		canvas_textures = retiring->owned_canvas_textures;
		retiring->owned_canvas_textures.clear();
	}
	if (retiring_texture.is_valid()) {
		retiring_texture->clear_external_texture();
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server != nullptr) {
		for (const RID &canvas_texture : canvas_textures) {
			rendering_server->free_rid(canvas_texture);
		}
	}
	texture.unref();
	retiring_texture.unref();
	runtime_schedule_state(retiring);
}

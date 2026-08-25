/**************************************************************************/
/*  html_surface_hcsr_runtime_backend.cpp                                 */
/**************************************************************************/

#include "html_surface_hcsr_runtime_backend.h"

#include "../bridge/html_asset_provider.h"
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
	RUNTIME_MUTATION_INNER_HTML,
};

static constexpr int RUNTIME_INTERACTIVE_STEP_SLICE_UNITS = 65536;
static constexpr int RUNTIME_PRESENTER_STEP_SLICE_UNITS = 4096;
static constexpr int RUNTIME_SEMANTIC_STEP_SLICE_UNITS = 4096;
static constexpr uint64_t RUNTIME_SEMANTIC_FRAME_BUDGET_MICROSECONDS = 8000;
static constexpr uint64_t RUNTIME_INTERACTIVE_FRAME_BUDGET_MICROSECONDS = 16667;
static constexpr uint32_t RUNTIME_REQUIRED_ABI_VERSION = 11;

static_assert(HCSR_RUNTIME_ABI_VERSION == RUNTIME_REQUIRED_ABI_VERSION,
		"The Godot HCSR replacement backend must be compiled against runtime ABI v11.");

struct RuntimeMutation {
	int kind = RUNTIME_MUTATION_TEXT;
	String id;
	String name;
	String value;
};

struct RuntimePointerRequest {
	hcsr_runtime_pointer_event_kind_t kind = HCSR_RUNTIME_POINTER_MOVE;
	Point2 position;
	uint32_t buttons = 0;
	bool focus_on_primary_down = false;
};

struct RuntimeScrollRequest {
	int32_t kind = HCSR_RUNTIME_SCROLL_INPUT_WHEEL;
	Point2 position;
	Vector2 delta;
	int32_t source = HCSR_RUNTIME_SCROLL_SOURCE_MOUSE_WHEEL;
	int32_t orientation = HCSR_RUNTIME_SCROLL_VERTICAL;
};

struct RuntimeResourceToken {
	uint64_t document_request_id = 0;
	uint64_t author_epoch = 0;
	int64_t resource_world_epoch = 0;
	int64_t parent_resource_revision = 0;
	int64_t request_generation = 0;
	int64_t resource_id = 0;

	bool matches(const hcsr_runtime_pending_resource_info_t &p_pending) const {
		return document_request_id == p_pending.document_request_id
				&& author_epoch == p_pending.author_epoch
				&& resource_world_epoch == p_pending.resource_world_epoch
				&& parent_resource_revision == p_pending.parent_resource_revision
				&& request_generation == p_pending.resource_request_generation
				&& resource_id == p_pending.resource_id;
	}
};

// Resource completions are admitted before semantic stepping so one host turn
// observes a coherent resource frontier instead of repeatedly superseding the
// same expensive layout candidate. The bound is host policy, not document
// semantics; larger inventories resume on the next scheduled turn.
static constexpr int RUNTIME_RESOURCE_COMPLETION_SLICE = 256;

struct RuntimeResolvedStylesheet {
	String reference;
	String content;
};

struct RuntimePlatformFontSource {
	String family;
	String reference;
	int32_t weight = 400;
	bool italic = false;
	int32_t face_index = 0;
};

static Vector<RuntimePlatformFontSource> runtime_discover_platform_fonts() {
	static const char *families[] = {
		"Arial", "sans-serif", "Segoe UI", "system-ui", "ui-sans-serif",
		"Times New Roman", "serif", "ui-serif",
		"Consolas", "monospace", "ui-monospace",
	};
	Vector<RuntimePlatformFontSource> result;
	for (const char *family : families) {
		String reference;
		if (!HTMLGodotAssetProvider::resolve_platform_font(family, 400, false, reference)) {
			continue;
		}
		RuntimePlatformFontSource source;
		source.family = family;
		source.reference = reference;
		result.push_back(source);
	}
	return result;
}

struct RuntimePublicationLineage {
	bool valid = false;
	bool configuration_only = false;
	uint64_t request_serial = 0;
	uint64_t runtime_generation = 0;
	uint64_t semantic_frame_generation = 0;
	uint64_t target_author_revision = 0;
	uint64_t interactive_submission_id = 0;
	uint64_t interactive_frame_id = 0;
	uint64_t interaction_input_id = 0;
	bool has_interaction_state = false;
};

static void initialize_abi(void *p_value, size_t p_size);

static bool runtime_load_document_source(
		const Ref<HTMLDocument> &p_document,
		String &r_html,
		String &r_css,
		Vector<RuntimeResolvedStylesheet> &r_stylesheets,
		String &r_error) {
	r_html = String();
	r_css = String();
	r_stylesheets.clear();
	r_error = String();
	if (p_document.is_null() || !p_document->is_source_valid()) {
		r_error = "HCSR replacement received an invalid HTMLDocument source.";
		return false;
	}

	r_html = p_document->get_html();
	const String html_file = p_document->get_html_file();
	if (r_html.is_empty() && !html_file.is_empty()) {
		HTMLAssetResource asset;
		if (HTMLGodotAssetProvider::load_asset(p_document, html_file, asset, &r_error) != OK) {
			return false;
		}
		r_html = String::utf8((const char *)asset.bytes.ptr(), asset.bytes.size());
	}
	if (r_html.strip_edges().is_empty()) {
		r_error = "HCSR replacement requires non-empty HTML source.";
		return false;
	}

	for (const String &css_file : p_document->get_css_files()) {
		HTMLAssetResource asset;
		if (HTMLGodotAssetProvider::load_asset(p_document, css_file, asset, &r_error) != OK) {
			return false;
		}
		r_css += "\n" + String::utf8((const char *)asset.bytes.ptr(), asset.bytes.size()) + "\n";
	}
	r_css += p_document->get_css();

	CharString html_utf8 = r_html.utf8();
	CharString css_utf8 = r_css.utf8();
	hcsr_runtime_document_t *provisional_document = nullptr;
	hcsr_runtime_compilation_report_t *report = nullptr;
	const hcsr_runtime_status_t discovery_status = hcsr_runtime_document_compile_with_stylesheets(
			html_utf8.get_data(), css_utf8.get_data(), nullptr, 0, &provisional_document, &report);
	if (report == nullptr) {
		r_error = vformat("HCSR replacement could not discover stylesheet resources (status %d).", (int)discovery_status);
		return false;
	}
	hcsr_runtime_compilation_report_info_t report_info;
	initialize_abi(&report_info, sizeof(report_info));
	if (hcsr_runtime_compilation_report_get_info(report, &report_info) != HCSR_RUNTIME_OK) {
		hcsr_runtime_compilation_report_release(report);
		if (provisional_document != nullptr) {
			hcsr_runtime_document_release(provisional_document);
		}
		r_error = "HCSR replacement could not inspect stylesheet resource intents.";
		return false;
	}
	for (int32_t index = 0; index < report_info.resource_intent_count; index++) {
		hcsr_runtime_compilation_resource_info_t resource_info;
		initialize_abi(&resource_info, sizeof(resource_info));
		if (hcsr_runtime_compilation_report_get_resource(report, index, &resource_info) != HCSR_RUNTIME_OK
				|| resource_info.kind != HCSR_RUNTIME_RESOURCE_STYLESHEET) {
			continue;
		}
		Vector<char> reference_utf8;
		reference_utf8.resize(resource_info.reference_utf8_bytes);
		if (hcsr_runtime_compilation_report_copy_resource_reference(
					report, index, reference_utf8.ptrw(), reference_utf8.size()) != HCSR_RUNTIME_OK) {
			continue;
		}
		RuntimeResolvedStylesheet stylesheet;
		stylesheet.reference = String::utf8(reference_utf8.ptr());
		bool already_resolved = false;
		for (const RuntimeResolvedStylesheet &resolved : r_stylesheets) {
			if (resolved.reference == stylesheet.reference) {
				already_resolved = true;
				break;
			}
		}
		if (already_resolved) {
			continue;
		}
		HTMLAssetResource asset;
		if (HTMLGodotAssetProvider::load_asset(p_document, stylesheet.reference, asset, &r_error) != OK) {
			hcsr_runtime_compilation_report_release(report);
			if (provisional_document != nullptr) {
				hcsr_runtime_document_release(provisional_document);
			}
			return false;
		}
		stylesheet.content = String::utf8((const char *)asset.bytes.ptr(), asset.bytes.size());
		r_stylesheets.push_back(stylesheet);
	}
	hcsr_runtime_compilation_report_release(report);
	if (provisional_document != nullptr) {
		hcsr_runtime_document_release(provisional_document);
	}
	return true;
}

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

struct RuntimeOutputState {
	uint64_t output_id = 0;
	Size2i requested_size;
	bool mipmaps = false;
	Ref<HTMLTexture2D> texture;
	RID mipmapped_texture;
	Size2i mipmapped_texture_size;
	uint64_t active_semantic_frame_generation = 0;
	int64_t active_configuration_id = 0;
	int32_t active_output_id = 0;
	int32_t active_pixel_width = 0;
	int32_t active_pixel_height = 0;
	uint64_t active_generation = 0;
	RuntimePresentationBinding *active_binding = nullptr;
	RuntimePresentationBinding *successor_binding = nullptr;
	RuntimePresentationBinding *retiring_binding = nullptr;
	RuntimePresentationBinding *staged_binding = nullptr;
	RuntimePublicationLineage staged_lineage;
	bool activation_ready = false;
	bool retains_active_surface = false;
	uint32_t topology_owner_count = 0;
	bool retirement_complete = false;
};

struct RuntimeOutputSnapshotEntry {
	RuntimeOutputState *state = nullptr;
	uint64_t output_id = 0;
	Size2i size;
};

struct RuntimeOutputTopologySnapshot {
	uint64_t revision = 0;
	Vector<RuntimeOutputSnapshotEntry> outputs;
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
	Ref<HTMLDocument> document;
	String html;
	String css;
	Vector<RuntimeResolvedStylesheet> stylesheets;
	Size2i logical_size = Size2i(512, 512);
	Size2i physical_size = Size2i(512, 512);
	Size2i submitted_logical_size;
	Size2i submitted_physical_size;
	Vector<RuntimeMutation> mutations;
	Vector<RuntimePointerRequest> pointer_requests;
	Vector<RuntimeScrollRequest> scroll_requests;
	Vector<HTMLPointerEvent> pointer_events;
	Vector<RuntimeOutputState *> outputs;
	Vector<RuntimeOutputState *> retiring_outputs;
	uint64_t next_output_id = 2;
	uint64_t requested_topology_revision = 1;
	RuntimeOutputTopologySnapshot candidate_topology;
	RuntimeOutputTopologySnapshot submitted_topology;
	RuntimeOutputTopologySnapshot staged_topology;
	bool submitted_request_is_configuration_only = false;
	bool document_dirty = false;
	bool configuration_dirty = false;
	bool work_scheduled = false;
	bool pending_work = false;
	bool semantic_pending = false;
	bool interactive_pending = false;
	bool activation_pending = false;
	bool activation_callback_scheduled = false;
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
	uint64_t active_interaction_input_id = 0;
	bool active_has_interaction_state = false;
	uint64_t active_scroll_input_id = 0;
	uint64_t next_host_input_id = 1;
	Vector<RuntimeResourceToken> completed_resource_tokens;
	uint64_t next_host_frame_id = 1;
	uint64_t active_pointer_submission_id = 0;
	RuntimePointerRequest active_pointer_request;
	bool active_pointer_request_valid = false;
	uint64_t active_pointer_cutoff_timestamp_microseconds = 0;
	uint64_t pointer_event_sequence = 0;
	uint32_t pointer_buttons = 0;
	Point2 pointer_position;
	bool scrollbar_interaction_active = false;
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

static String runtime_copy_last_error(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		const String &p_fallback) {
	Vector<char> message;
	message.resize(64 * 1024);
	if (hcsr_runtime_session_copy_last_error(
			p_state->session, message.ptrw(), message.size()) != HCSR_RUNTIME_OK) {
		return p_fallback;
	}
	const String native_error = String::utf8(message.ptr());
	return native_error.is_empty() ? p_fallback : native_error;
}

static bool runtime_has_completed_resource_token(
		const HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		const hcsr_runtime_pending_resource_info_t &p_pending) {
	for (const RuntimeResourceToken &token : p_state->completed_resource_tokens) {
		if (token.matches(p_pending)) {
			return true;
		}
	}
	return false;
}

static bool runtime_complete_pending_resources(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		int &r_completed_count) {
	r_completed_count = 0;
	int64_t after_resource_id = 0;
	for (int admission = 0; admission < RUNTIME_RESOURCE_COMPLETION_SLICE; admission++) {
		hcsr_runtime_pending_resource_info_t pending;
		initialize_abi(&pending, sizeof(pending));
		const hcsr_runtime_status_t find_status = hcsr_runtime_session_find_pending_resource(
				p_state->session, after_resource_id, &pending);
		if (find_status == HCSR_RUNTIME_NO_NEW_PUBLICATION) {
			return true;
		}
		if (find_status != HCSR_RUNTIME_OK) {
			runtime_set_terminal(p_state, vformat("HCSR replacement could not enumerate a pending resource (status %d).", (int)find_status));
			return false;
		}
		after_resource_id = pending.resource_id;
		if (runtime_has_completed_resource_token(p_state, pending)) {
			continue;
		}
		Vector<char> reference_utf8;
		reference_utf8.resize(MAX(1, pending.reference_utf8_bytes));
		if (hcsr_runtime_session_copy_pending_resource_reference(
				p_state->session, &pending, reference_utf8.ptrw(), reference_utf8.size()) != HCSR_RUNTIME_OK) {
			runtime_set_terminal(p_state, "HCSR replacement could not copy a pending resource reference.");
			return false;
		}
		String reference = String::utf8(reference_utf8.ptr());
		Vector<char> base_utf8;
		base_utf8.resize(4096);
		const hcsr_runtime_status_t base_status = hcsr_runtime_session_copy_pending_resource_base_reference(
				p_state->session, &pending, base_utf8.ptrw(), base_utf8.size());
		if (base_status == HCSR_RUNTIME_OK) {
			String base_reference = String::utf8(base_utf8.ptr());
			String resolved_base;
			String resolve_error;
			if (HTMLGodotAssetProvider::resolve_asset_path(p_state->document, base_reference, resolved_base, &resolve_error) != OK) {
				runtime_set_terminal(p_state, resolve_error);
				return false;
			}
			reference = resolved_base.get_base_dir().path_join(reference).simplify_path();
		} else if (base_status != HCSR_RUNTIME_NO_NEW_PUBLICATION) {
			runtime_set_terminal(p_state, vformat("HCSR replacement could not qualify a pending resource base (status %d).", (int)base_status));
			return false;
		}
		HTMLAssetResource asset;
		String asset_error;
		hcsr_runtime_resource_completion_t completion;
		initialize_abi(&completion, sizeof(completion));
		completion.document_request_id = pending.document_request_id;
		completion.author_epoch = pending.author_epoch;
		completion.resource_world_epoch = pending.resource_world_epoch;
		completion.parent_resource_revision = pending.parent_resource_revision;
		completion.resource_request_generation = pending.resource_request_generation;
		completion.resource_id = pending.resource_id;
		if (HTMLGodotAssetProvider::load_asset(p_state->document, reference, asset, &asset_error) != OK) {
			completion.status = HCSR_RUNTIME_RESOURCE_COMPLETION_FAILED;
			CharString failure = asset_error.utf8();
			const hcsr_runtime_status_t complete_status = hcsr_runtime_session_complete_resource(
					p_state->session, &completion, nullptr, nullptr, nullptr, 0, failure.get_data());
			if (complete_status != HCSR_RUNTIME_OK) {
				runtime_set_terminal(p_state, vformat("HCSR replacement rejected a failed resource completion (status %d).", (int)complete_status));
				return false;
			}
		} else {
			completion.status = HCSR_RUNTIME_RESOURCE_COMPLETION_READY;
			// Image dimensions are decoded by the runtime codec when zero; fonts and
			// other byte resources have no intrinsic dimensions.
			completion.intrinsic_width = 0;
			completion.intrinsic_height = 0;
			CharString identity = asset.path.utf8();
			CharString mime = asset.mime_type.utf8();
			const hcsr_runtime_status_t complete_status = hcsr_runtime_session_complete_resource(
					p_state->session, &completion, identity.get_data(), mime.get_data(), asset.bytes.ptr(), asset.bytes.size(), nullptr);
			if (complete_status != HCSR_RUNTIME_OK) {
				runtime_set_terminal(p_state, vformat("HCSR replacement rejected resource '%s' (status %d).", reference, (int)complete_status));
				return false;
			}
		}
		p_state->completed_resource_tokens.push_back({
			pending.document_request_id,
			pending.author_epoch,
			pending.resource_world_epoch,
			pending.parent_resource_revision,
			pending.resource_request_generation,
			pending.resource_id,
		});
		r_completed_count++;
	}
	return true;
}

static String runtime_compilation_failure(
		hcsr_runtime_compilation_report_t *p_report,
		const hcsr_runtime_compilation_report_info_t &p_info) {
	if (p_info.diagnostic_count <= 0) {
		return "HCSR replacement compilation failed without a diagnostic.";
	}
	int32_t diagnostic_index = 0;
	hcsr_runtime_compilation_diagnostic_info_t diagnostic;
	for (int32_t index = 0; index < p_info.diagnostic_count; index++) {
		initialize_abi(&diagnostic, sizeof(diagnostic));
		if (hcsr_runtime_compilation_report_get_diagnostic(p_report, index, &diagnostic) != HCSR_RUNTIME_OK) {
			return "HCSR replacement compilation diagnostics could not be read.";
		}
		if (diagnostic.severity == HCSR_RUNTIME_DIAGNOSTIC_ERROR) {
			diagnostic_index = index;
			break;
		}
	}
	initialize_abi(&diagnostic, sizeof(diagnostic));
	if (hcsr_runtime_compilation_report_get_diagnostic(p_report, diagnostic_index, &diagnostic) != HCSR_RUNTIME_OK) {
		return "HCSR replacement compilation diagnostics could not be read.";
	}
	Vector<char> message;
	message.resize(MAX(1, diagnostic.message_utf8_bytes + 1));
	if (hcsr_runtime_compilation_report_copy_diagnostic_text(
			p_report, diagnostic_index, 1, message.ptrw(), message.size()) != HCSR_RUNTIME_OK) {
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
		bool p_configuration_only,
		const hcsr_runtime_publication_info_t &p_info,
		const hcsr_runtime_interaction_publication_info_t &p_interaction) {
	RuntimePublicationLineage lineage;
	lineage.valid = true;
	lineage.configuration_only = p_configuration_only;
	lineage.request_serial = p_request_serial;
	lineage.runtime_generation = p_info.generation;
	lineage.semantic_frame_generation = p_info.semantic_frame_generation;
	lineage.target_author_revision = p_info.target_author_revision;
	lineage.interactive_submission_id = p_info.interactive_submission_id;
	lineage.interactive_frame_id = p_info.interactive_frame_id;
	lineage.interaction_input_id = p_interaction.input_id;
	lineage.has_interaction_state = p_interaction.has_interaction_world != 0
			&& p_interaction.has_interaction_state != 0;
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

static RuntimePresentationBinding *runtime_create_presentation_binding_from_device(
		RenderingDevice *p_rendering_device) {
	if (p_rendering_device == nullptr) {
		return nullptr;
	}
	void *device = (void *)p_rendering_device->get_driver_resource(
			RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE);
	void *queue = (void *)p_rendering_device->get_driver_resource(
			RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE);
	return device != nullptr && queue != nullptr
			? runtime_create_presentation_binding(device, queue, p_rendering_device)
			: nullptr;
}

static RuntimeOutputTopologySnapshot runtime_capture_output_topology(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state) {
	RuntimeOutputTopologySnapshot snapshot;
	MutexLock lock(p_state->mutex);
	snapshot.revision = p_state->requested_topology_revision;
	snapshot.outputs.resize(p_state->outputs.size());
	for (int index = 0; index < p_state->outputs.size(); index++) {
		RuntimeOutputState *output = p_state->outputs[index];
		snapshot.outputs.write[index].state = output;
		snapshot.outputs.write[index].output_id = output->output_id;
		snapshot.outputs.write[index].size = output->requested_size;
		output->topology_owner_count++;
	}
	return snapshot;
}

static void runtime_release_output_topology(RuntimeOutputTopologySnapshot &p_snapshot) {
	for (const RuntimeOutputSnapshotEntry &entry : p_snapshot.outputs) {
		ERR_CONTINUE(entry.state == nullptr || entry.state->topology_owner_count == 0);
		entry.state->topology_owner_count--;
	}
	p_snapshot = RuntimeOutputTopologySnapshot();
}

static void runtime_replace_output_topology(RuntimeOutputTopologySnapshot &p_target,
		const RuntimeOutputTopologySnapshot &p_source) {
	runtime_release_output_topology(p_target);
	p_target = p_source;
	for (const RuntimeOutputSnapshotEntry &entry : p_target.outputs) {
		ERR_CONTINUE(entry.state == nullptr);
		entry.state->topology_owner_count++;
	}
}

static bool runtime_output_topologies_match(
		const RuntimeOutputTopologySnapshot &p_left,
		const RuntimeOutputTopologySnapshot &p_right) {
	if (p_left.outputs.size() != p_right.outputs.size()) {
		return false;
	}
	for (int index = 0; index < p_left.outputs.size(); index++) {
		if (p_left.outputs[index].output_id != p_right.outputs[index].output_id
				|| p_left.outputs[index].size != p_right.outputs[index].size) {
			return false;
		}
	}
	return true;
}

static Vector<hcsr_runtime_output_configuration_t> runtime_create_output_configurations(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		const RuntimeOutputTopologySnapshot &p_topology) {
	Vector<hcsr_runtime_output_configuration_t> outputs;
	outputs.resize(1 + p_topology.outputs.size());
	initialize_abi(&outputs.write[0], sizeof(hcsr_runtime_output_configuration_t));
	outputs.write[0].output_id = 1;
	outputs.write[0].pixel_width = p_state->physical_size.x;
	outputs.write[0].pixel_height = p_state->physical_size.y;
	outputs.write[0].logical_width = p_state->logical_size.x;
	outputs.write[0].logical_height = p_state->logical_size.y;
	outputs.write[0].tile_size = 64;
	for (int index = 0; index < p_topology.outputs.size(); index++) {
		const RuntimeOutputSnapshotEntry &output = p_topology.outputs[index];
		initialize_abi(&outputs.write[index + 1], sizeof(hcsr_runtime_output_configuration_t));
		outputs.write[index + 1].output_id = (int32_t)output.output_id;
		outputs.write[index + 1].pixel_width = output.size.x;
		outputs.write[index + 1].pixel_height = output.size.y;
		outputs.write[index + 1].logical_width = p_state->logical_size.x;
		outputs.write[index + 1].logical_height = p_state->logical_size.y;
		outputs.write[index + 1].tile_size = 64;
	}
	return outputs;
}

static bool runtime_ensure_initialized(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state) {
	if (p_state->session != nullptr) {
		return true;
	}
	if (hcsr_runtime_get_abi_version() != RUNTIME_REQUIRED_ABI_VERSION) {
		runtime_set_terminal(p_state, "HCSR replacement ABI mismatch; Godot requires runtime ABI v11.");
		return false;
	}
	CharString codec_directory = OS::get_singleton()->get_executable_path().get_base_dir().utf8();
	if (hcsr_runtime_set_native_dependency_directory(codec_directory.get_data()) != HCSR_RUNTIME_OK) {
		runtime_set_terminal(p_state, "Godot could not configure the HCSR native codec directory.");
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
	runtime_release_output_topology(p_state->candidate_topology);
	p_state->candidate_topology = runtime_capture_output_topology(p_state);
	runtime_replace_output_topology(p_state->submitted_topology, p_state->candidate_topology);
	p_state->submitted_logical_size = p_state->logical_size;
	p_state->submitted_physical_size = p_state->physical_size;
	p_state->submitted_request_is_configuration_only = true;
	Vector<hcsr_runtime_output_configuration_t> outputs = runtime_create_output_configurations(p_state, p_state->candidate_topology);
	if (hcsr_runtime_session_create_with_presentation_mode(
			p_state->logical_size.x,
			p_state->logical_size.y,
			outputs.ptr(),
			outputs.size(),
			HCSR_RUNTIME_PRESENTATION_SEMANTIC_ONLY,
			&p_state->session) != HCSR_RUNTIME_OK) {
			runtime_set_terminal(p_state, "HCSR replacement could not create its RuntimeSession D3D12 presenter.");
		return false;
	}
	p_state->active_binding = runtime_create_presentation_binding(device, queue, rendering_device);
	if (p_state->active_binding == nullptr) {
		runtime_set_terminal(p_state, "Godot could not create the HCSR configuration presentation binding.");
		return false;
	}
	for (const RuntimeOutputSnapshotEntry &entry : p_state->candidate_topology.outputs) {
		RuntimeOutputState *output = entry.state;
		output->active_binding = runtime_create_presentation_binding(device, queue, rendering_device);
		if (output->active_binding == nullptr) {
			runtime_set_terminal(p_state, "Godot could not create an HCSR secondary output presenter.");
			return false;
		}
	}
	return true;
}

static bool runtime_submit_document(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		const String &p_html,
		const String &p_css,
		const Vector<RuntimeResolvedStylesheet> &p_stylesheets) {
	CharString html_utf8 = p_html.utf8();
	CharString css_utf8 = p_css.utf8();
	Vector<CharString> stylesheet_references;
	Vector<CharString> stylesheet_contents;
	stylesheet_references.resize(p_stylesheets.size());
	stylesheet_contents.resize(p_stylesheets.size());
	for (int index = 0; index < p_stylesheets.size(); index++) {
		stylesheet_references.write[index] = p_stylesheets[index].reference.utf8();
		stylesheet_contents.write[index] = p_stylesheets[index].content.utf8();
	}
	Vector<hcsr_runtime_resolved_stylesheet_t> stylesheet_inputs;
	stylesheet_inputs.resize(p_stylesheets.size());
	for (int index = 0; index < p_stylesheets.size(); index++) {
		initialize_abi(&stylesheet_inputs.write[index], sizeof(hcsr_runtime_resolved_stylesheet_t));
		stylesheet_inputs.write[index].reference_utf8 = stylesheet_references[index].get_data();
		stylesheet_inputs.write[index].content_utf8 = stylesheet_contents[index].get_data();
	}
	const Vector<RuntimePlatformFontSource> platform_fonts = runtime_discover_platform_fonts();
	Vector<CharString> font_families;
	Vector<CharString> font_references;
	font_families.resize(platform_fonts.size());
	font_references.resize(platform_fonts.size());
	Vector<hcsr_runtime_document_font_source_t> font_inputs;
	font_inputs.resize(platform_fonts.size());
	for (int index = 0; index < platform_fonts.size(); index++) {
		font_families.write[index] = platform_fonts[index].family.utf8();
		font_references.write[index] = platform_fonts[index].reference.utf8();
		initialize_abi(&font_inputs.write[index], sizeof(hcsr_runtime_document_font_source_t));
		font_inputs.write[index].family_utf8 = font_families[index].get_data();
		font_inputs.write[index].reference_utf8 = font_references[index].get_data();
		font_inputs.write[index].weight = platform_fonts[index].weight;
		font_inputs.write[index].italic = platform_fonts[index].italic ? 1 : 0;
		font_inputs.write[index].face_index = platform_fonts[index].face_index;
	}
	hcsr_runtime_document_t *document = nullptr;
	hcsr_runtime_compilation_report_t *report = nullptr;
	const hcsr_runtime_status_t compile_status = hcsr_runtime_document_compile_with_stylesheets_and_fonts(
			html_utf8.get_data(), css_utf8.get_data(), stylesheet_inputs.ptr(), stylesheet_inputs.size(),
			font_inputs.ptr(), font_inputs.size(), &document, &report);
	if (report == nullptr) {
		runtime_set_terminal(p_state, "HCSR replacement did not return a compilation report.");
		return false;
	}
	hcsr_runtime_compilation_report_info_t info;
	initialize_abi(&info, sizeof(info));
	const bool report_valid = hcsr_runtime_compilation_report_get_info(report, &info) == HCSR_RUNTIME_OK;
	if (!report_valid || compile_status != HCSR_RUNTIME_OK || info.success == 0 || document == nullptr) {
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
	p_state->completed_resource_tokens.clear();
	if (p_state->compiled_document != nullptr) {
		hcsr_runtime_document_release(p_state->compiled_document);
	}
	p_state->compiled_document = document;
	p_state->semantic_pending = true;
	p_state->pending_work = true;
	return true;
}

static bool runtime_submit_configuration(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		uint64_t p_request_serial) {
	runtime_release_output_topology(p_state->candidate_topology);
	p_state->candidate_topology = runtime_capture_output_topology(p_state);
	if (p_state->logical_size == p_state->submitted_logical_size
			&& p_state->physical_size == p_state->submitted_physical_size
			&& runtime_output_topologies_match(
					p_state->candidate_topology, p_state->submitted_topology)) {
		return true;
	}
	Vector<hcsr_runtime_output_configuration_t> outputs = runtime_create_output_configurations(p_state, p_state->candidate_topology);
	const hcsr_runtime_status_t configuration_status = hcsr_runtime_session_submit_configuration(
			p_state->session,
			p_state->logical_size.x,
			p_state->logical_size.y,
			outputs.ptr(),
			outputs.size());
	if (configuration_status != HCSR_RUNTIME_OK) {
		runtime_set_terminal(p_state, runtime_copy_last_error(p_state, vformat(
				"HCSR replacement rejected the Godot output configuration (status %d, logical %dx%d, physical %dx%d).",
				(int)configuration_status,
				p_state->logical_size.x,
				p_state->logical_size.y,
				p_state->physical_size.x,
				p_state->physical_size.y)));
		return false;
	}
	runtime_replace_output_topology(p_state->submitted_topology, p_state->candidate_topology);
	p_state->submitted_logical_size = p_state->logical_size;
	p_state->submitted_physical_size = p_state->physical_size;
	p_state->submitted_request_is_configuration_only = true;
	p_state->submitted_request_serial = p_request_serial;
	p_state->semantic_pending = true;
	p_state->pending_work = true;
	return true;
}

static bool runtime_submit_mutations(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		const Vector<RuntimeMutation> &p_mutations,
		uint64_t p_request_process_frame,
		uint64_t p_request_serial,
		int &r_consumed_count) {
	r_consumed_count = 0;
	if (p_mutations.is_empty()) {
		return true;
	}
	hcsr_runtime_mutation_t *journal = nullptr;
	if (hcsr_runtime_mutation_begin(p_state->session, &journal) != HCSR_RUNTIME_OK
			|| journal == nullptr) {
		runtime_set_terminal(p_state, "HCSR replacement could not begin an author mutation journal.");
		return false;
	}
	const bool structural_journal = p_mutations[0].kind == RUNTIME_MUTATION_INNER_HTML;
	for (const RuntimeMutation &mutation : p_mutations) {
		if (r_consumed_count > 0
				&& (structural_journal || mutation.kind == RUNTIME_MUTATION_INNER_HTML)) {
			break;
		}
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
			case RUNTIME_MUTATION_INNER_HTML:
				status = hcsr_runtime_mutation_set_inner_html_by_id(journal, id.get_data(), value.get_data());
				break;
		}
		if (status != HCSR_RUNTIME_OK) {
			Vector<char> mutation_error_buffer;
			mutation_error_buffer.resize(16384);
			String mutation_error;
			if (hcsr_runtime_mutation_copy_last_error(
					journal,
					mutation_error_buffer.ptrw(),
					mutation_error_buffer.size()) == HCSR_RUNTIME_OK) {
				mutation_error = String::utf8(mutation_error_buffer.ptr());
			}
			hcsr_runtime_mutation_release(journal);
			runtime_set_terminal(p_state, vformat(
					"HCSR replacement rejected a Godot mutation journal operation "
					"(kind %d, id '%s', name '%s', status %d): %s",
					mutation.kind, mutation.id, mutation.name, (int)status,
					mutation_error.is_empty() ? "no detailed diagnostic" : mutation_error));
			return false;
		}
		r_consumed_count++;
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
	p_state->semantic_pending = true;
	p_state->pending_work = true;
	p_state->interactive_pending = true;
	p_state->submitted_request_serial = p_request_serial;
	p_state->submitted_request_is_configuration_only = false;
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
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT
					| RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT,
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
		RuntimePresentationBinding *p_binding,
		RuntimeOutputState *p_output = nullptr) {
	ERR_FAIL_NULL_V(p_binding, false);
	hcsr_runtime_d3d12_surface_info_t info;
	initialize_abi(&info, sizeof(info));
	if (hcsr_runtime_d3d12_presenter_query_surface(p_binding->presenter, &info)
			!= HCSR_RUNTIME_OK) {
		return false;
	}
	if (p_output != nullptr && p_output->active_generation != 0
			&& p_output->staged_lineage.configuration_only
			&& p_binding == p_output->active_binding
			&& !p_binding->presenter_pending) {
		p_output->active_generation = p_output->staged_lineage.runtime_generation;
		return true;
	}
	const uint64_t active_generation = p_output != nullptr ? p_output->active_generation : p_state->active_generation;
	if (info.runtime_generation == active_generation) {
		return true;
	}
	if (p_output == nullptr && p_state->active_generation != 0
			&& p_state->staged_lineage.configuration_only
			&& p_binding == p_state->active_binding
			&& !p_binding->presenter_pending) {
		MutexLock lock(p_state->mutex);
		p_state->active_generation = p_state->staged_lineage.runtime_generation;
		p_state->queued_generation = p_state->staged_lineage.runtime_generation;
		p_state->frame_metadata.generation = p_state->staged_lineage.runtime_generation;
		p_state->presentation_changed = true;
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
	const uint64_t active_semantic_frame_generation = p_output != nullptr
			? p_output->active_semantic_frame_generation
			: p_state->active_semantic_frame_generation;
	const int64_t active_configuration_id = p_output != nullptr
			? p_output->active_configuration_id
			: p_state->active_configuration_id;
	const int32_t active_output_id = p_output != nullptr
			? p_output->active_output_id
			: p_state->active_output_id;
	const bool runtime_only_current = slot->generation_surface != nullptr
			&& ((p_output == nullptr && p_state->active_binding == p_binding)
					|| (p_output != nullptr && p_output->active_binding == p_binding))
			&& p_binding->active_slot == slot->godot_slot
			&& active_semantic_frame_generation == acquired.semantic_frame_generation
			&& active_configuration_id == acquired.configuration_id
			&& active_output_id == acquired.output_id;
	if (runtime_only_current) {
		hcsr_runtime_d3d12_surface_release(slot->generation_surface);
		slot->generation_surface = surface;
		slot->runtime_generation = acquired.runtime_generation;
		if (p_output != nullptr) {
			p_output->active_generation = acquired.runtime_generation;
			p_output->active_semantic_frame_generation = acquired.semantic_frame_generation;
			p_output->active_configuration_id = acquired.configuration_id;
			p_output->active_output_id = acquired.output_id;
			p_output->active_pixel_width = acquired.pixel_width;
			p_output->active_pixel_height = acquired.pixel_height;
		}
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
				p_binding->external_texture_pool, true);
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
		if (p_output == nullptr) {
			if (p_state->active_imported_texture != active_slot->canvas_texture) {
				p_state->texture->set_external_texture(active_slot->canvas_texture,
						Size2i(acquired.pixel_width, acquired.pixel_height), true);
			}
			p_state->active_imported_texture = active_slot->canvas_texture;
		} else {
			RID published_texture = active_slot->canvas_texture;
			if (p_output->mipmaps) {
				const Size2i acquired_size(acquired.pixel_width, acquired.pixel_height);
				if (p_output->mipmapped_texture.is_valid()
						&& p_output->mipmapped_texture_size != acquired_size) {
					rendering_server->free_rid(p_output->mipmapped_texture);
					p_output->mipmapped_texture = RID();
					p_output->mipmapped_texture_size = Size2i();
				}
				if (!p_output->mipmapped_texture.is_valid()) {
					p_output->mipmapped_texture = rendering_server->texture_drawable_create(
							acquired.pixel_width, acquired.pixel_height,
							RenderingServerEnums::TEXTURE_DRAWABLE_FORMAT_RGBA8_SRGB,
							Color(0, 0, 0, 0), true);
					p_output->mipmapped_texture_size = acquired_size;
				}
				if (!p_output->mipmapped_texture.is_valid()) {
					runtime_set_terminal(p_state, "Godot could not allocate the mipmapped HCSR secondary output.");
					return false;
				}
				rendering_server->texture_drawable_copy_level_zero(active_slot->canvas_texture, p_output->mipmapped_texture);
				rendering_server->texture_drawable_generate_mipmaps(p_output->mipmapped_texture, true);
				published_texture = p_output->mipmapped_texture;
			}
			p_output->texture->set_external_texture(published_texture,
					Size2i(acquired.pixel_width, acquired.pixel_height), true);
			p_output->active_generation = acquired.runtime_generation;
			p_output->active_semantic_frame_generation = acquired.semantic_frame_generation;
			p_output->active_configuration_id = acquired.configuration_id;
			p_output->active_output_id = acquired.output_id;
			p_output->active_pixel_width = acquired.pixel_width;
			p_output->active_pixel_height = acquired.pixel_height;
		}
		p_state->presentation_changed = true;
	}
	{
		MutexLock lock(p_state->mutex);
		if (p_output == nullptr) {
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

static bool runtime_step_retiring_output(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		RuntimeOutputState *p_output,
		RenderingServer *p_rendering_server,
		RenderingDevice *p_rendering_device) {
	auto step_binding = [&](RuntimePresentationBinding *&p_binding) {
		if (p_binding == nullptr) {
			return true;
		}
		if (!runtime_step_retiring_binding(p_state, p_binding, p_rendering_server, p_rendering_device)) {
			return false;
		}
		memdelete(p_binding);
		p_binding = nullptr;
		return true;
	};
	if (!step_binding(p_output->successor_binding)
			|| !step_binding(p_output->retiring_binding)
			|| !step_binding(p_output->active_binding)) {
		return false;
	}
	if (p_output->mipmapped_texture.is_valid()) {
		p_rendering_server->free_rid(p_output->mipmapped_texture);
		p_output->mipmapped_texture = RID();
	}
	p_output->texture.unref();
	return true;
}

static bool runtime_step_active(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state) {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	RenderingDevice *rendering_device = rendering_server != nullptr
			? rendering_server->get_rendering_device()
			: nullptr;
	if (rendering_device == nullptr || p_state->active_binding == nullptr) {
		runtime_set_terminal(p_state, "Godot could not advance HCSR external texture release ownership.");
		return false;
	}
	// Active pools are allowed to retain their current slot while a successor
	// presenter is prepared. Release reconciliation is strict only once a pool
	// enters retirement; otherwise resize would wait on the very texture it is
	// replacing and could never create its successor revision.
	(void)runtime_release_completed_external_slots(p_state->active_binding, rendering_device);
	if (p_state->retiring_binding != nullptr
			&& runtime_step_retiring_binding(
					p_state, p_state->retiring_binding, rendering_server, rendering_device)) {
		memdelete(p_state->retiring_binding);
		p_state->retiring_binding = nullptr;
	}
	for (RuntimeOutputState *output : p_state->outputs) {
		if (output->active_binding != nullptr) {
			(void)runtime_release_completed_external_slots(output->active_binding, rendering_device);
		}
		if (output->retiring_binding != nullptr
				&& runtime_step_retiring_binding(
						p_state, output->retiring_binding, rendering_server, rendering_device)) {
			memdelete(output->retiring_binding);
			output->retiring_binding = nullptr;
		}
	}
	for (int index = p_state->retiring_outputs.size() - 1; index >= 0; index--) {
		RuntimeOutputState *output = p_state->retiring_outputs[index];
		if (!output->retirement_complete) {
			output->retirement_complete = runtime_step_retiring_output(
					p_state, output, rendering_server, rendering_device);
		}
		if (output->retirement_complete && output->topology_owner_count == 0) {
			p_state->retiring_outputs.remove_at(index);
			memdelete(output);
		}
	}
	if (p_state->terminal) {
		return false;
	}
	int completed_resource_count = 0;
	if (!runtime_complete_pending_resources(p_state, completed_resource_count)) {
		return false;
	}
	hcsr_runtime_step_info_t step;
	initialize_abi(&step, sizeof(step));
	bool semantic_publication_ready = false;
	bool semantic_pending = false;
	int32_t interactive_work_units = 0;
	hcsr_runtime_status_t status = HCSR_RUNTIME_OK;
	if (p_state->interactive_pending) {
		hcsr_runtime_interactive_step_info_t interactive;
		initialize_abi(&interactive, sizeof(interactive));
		status = hcsr_runtime_session_step_interactive(
				p_state->session, RUNTIME_INTERACTIVE_STEP_SLICE_UNITS, &interactive);
		interactive_work_units = interactive.work_units;
		if (status != HCSR_RUNTIME_OK && status != HCSR_RUNTIME_PENDING) {
			runtime_set_terminal(p_state, runtime_copy_last_error(
					p_state, vformat("HCSR replacement interactive derivation failed with status %d.", (int)status)));
			return false;
		}
		if (interactive.submission_id != p_state->newest_requested_submission_id
				|| interactive.frame_id != p_state->newest_requested_frame_id) {
			runtime_set_terminal(p_state, vformat(
					"HCSR replacement interactive stepping lost the newest input submission/frame identity (actual %d/%d, expected %d/%d, status %d).",
					interactive.submission_id, interactive.frame_id,
					p_state->newest_requested_submission_id, p_state->newest_requested_frame_id,
					(int)status));
			return false;
		}
		if (interactive.target_author_revision != 0) {
			p_state->newest_requested_author_revision = interactive.target_author_revision;
		}
		if (status == HCSR_RUNTIME_PENDING) {
			semantic_pending = true;
		} else {
			p_state->interactive_pending = false;
			if (status == HCSR_RUNTIME_OK
					&& interactive.outcome != HCSR_RUNTIME_INTERACTIVE_SEMANTIC_PUBLISHED_BEFORE_CUTOFF) {
				runtime_set_terminal(p_state, vformat(
						"HCSR replacement interaction missed its mandatory next-frame semantic publication (outcome %d).",
						(int)interactive.outcome));
				return false;
			}
			semantic_publication_ready = status == HCSR_RUNTIME_OK
					&& interactive.runtime_generation != 0;
			semantic_pending = status == HCSR_RUNTIME_OK && !semantic_publication_ready;
		}
	} else {
		const uint64_t semantic_cutoff = hcsr_runtime_get_monotonic_timestamp_microseconds()
				+ RUNTIME_SEMANTIC_FRAME_BUDGET_MICROSECONDS;
		do {
			initialize_abi(&step, sizeof(step));
			status = hcsr_runtime_session_step(
					p_state->session, RUNTIME_SEMANTIC_STEP_SLICE_UNITS, &step);
		} while (status == HCSR_RUNTIME_PENDING
				&& hcsr_runtime_get_monotonic_timestamp_microseconds() < semantic_cutoff);
			semantic_publication_ready = status == HCSR_RUNTIME_OK
					&& step.state == HCSR_RUNTIME_STEP_PUBLISHED;
			semantic_pending = status == HCSR_RUNTIME_PENDING;
		}
	if (status != HCSR_RUNTIME_OK && status != HCSR_RUNTIME_PENDING) {
		runtime_set_terminal(p_state, runtime_copy_last_error(
				p_state, vformat("HCSR replacement semantic derivation failed with status %d and state %d.", (int)status, step.state)));
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
			const bool exact_interactive_authority = p_state->submitted_request_is_configuration_only
					|| !has_interactive_authority
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
			if (publication_info.output_count != 1 + p_state->submitted_topology.outputs.size()) {
				hcsr_runtime_publication_release(p_state->session, publication);
				runtime_set_terminal(p_state, "HCSR replacement publication output topology differs from the requested atomic configuration.");
				return false;
			}
			hcsr_runtime_interaction_publication_info_t interaction_info;
			initialize_abi(&interaction_info, sizeof(interaction_info));
			if (hcsr_runtime_publication_get_interaction_info(
					publication, publication_info.generation, &interaction_info) != HCSR_RUNTIME_OK) {
				hcsr_runtime_publication_release(p_state->session, publication);
				runtime_set_terminal(p_state, "HCSR replacement publication lost its interaction input authority.");
				return false;
			}
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
			bool retained_primary_surface = p_state->submitted_request_is_configuration_only
					&& target_binding == p_state->active_binding
					&& p_state->active_generation != 0
					&& output_info.output_id == p_state->active_output_id
					&& output_info.pixel_width == p_state->active_pixel_width
					&& output_info.pixel_height == p_state->active_pixel_height;
			hcsr_runtime_status_t submit_status = retained_primary_surface
					? HCSR_RUNTIME_OK
					: hcsr_runtime_d3d12_presenter_submit(
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
			if (submit_status == HCSR_RUNTIME_STALE_REQUEST) {
				if (p_state->submitted_request_is_configuration_only
						&& p_state->active_generation != 0
						&& output_info.output_id == p_state->active_output_id
						&& output_info.pixel_width == p_state->active_pixel_width
						&& output_info.pixel_height == p_state->active_pixel_height) {
					submit_status = HCSR_RUNTIME_OK;
					target_binding->pending_lineage = runtime_lineage_from_publication(
							p_state->submitted_request_serial, true, publication_info, interaction_info);
					p_state->staged_binding = target_binding;
					p_state->staged_lineage = target_binding->pending_lineage;
					retained_primary_surface = true;
				}
			}
			if (submit_status == HCSR_RUNTIME_STALE_REQUEST) {
				const bool proven_superseded = publication_info.interactive_submission_id < p_state->newest_requested_submission_id
						|| publication_info.target_author_revision < p_state->newest_requested_author_revision
						|| publication_info.generation < p_state->queued_generation;
				if (!proven_superseded) {
					hcsr_runtime_publication_release(p_state->session, publication);
					runtime_set_terminal(p_state, "HCSR replacement reported a stale presenter request without a proven newer authority.");
					return false;
				}
				semantic_pending = true;
			} else if (submit_status != HCSR_RUNTIME_OK) {
				hcsr_runtime_publication_release(p_state->session, publication);
				runtime_set_terminal(p_state, vformat("HCSR replacement D3D12 presenter rejected runtime generation %d semantic generation %d with status %d.", publication_info.generation, publication_info.semantic_frame_generation, (int)submit_status));
				return false;
			} else {
				const RuntimePublicationLineage lineage = runtime_lineage_from_publication(
						p_state->submitted_request_serial,
						p_state->submitted_request_is_configuration_only,
						publication_info,
						interaction_info);
				p_state->queued_generation = publication_info.generation;
				(void)submitted_to_successor;
				target_binding->pending_lineage = lineage;
				if (retained_primary_surface) {
					target_binding->presenter_pending = false;
					p_state->staged_binding = target_binding;
					p_state->staged_lineage = lineage;
				} else if (p_state->staged_binding != target_binding
						|| p_state->staged_lineage.runtime_generation != publication_info.generation) {
					target_binding->presenter_pending = true;
				}
				for (int output_index = 0; output_index < p_state->submitted_topology.outputs.size(); output_index++) {
					const RuntimeOutputSnapshotEntry &entry = p_state->submitted_topology.outputs[output_index];
					RuntimeOutputState *output = entry.state;
					hcsr_runtime_output_info_t secondary_info;
					initialize_abi(&secondary_info, sizeof(secondary_info));
					if (hcsr_runtime_publication_get_output(publication, publication_info.generation,
							output_index + 1, &secondary_info) != HCSR_RUNTIME_OK
							|| secondary_info.output_id != (int32_t)entry.output_id
							|| secondary_info.pixel_width != entry.size.x
							|| secondary_info.pixel_height != entry.size.y) {
						hcsr_runtime_publication_release(p_state->session, publication);
						runtime_set_terminal(p_state, "HCSR replacement secondary output metadata is not the requested projection.");
						return false;
					}
					RuntimePresentationBinding *secondary_binding = output->successor_binding != nullptr
							? output->successor_binding : output->active_binding;
					if (secondary_binding == nullptr) {
						RenderingDevice *rendering_device = RenderingServer::get_singleton()->get_rendering_device();
						output->active_binding = runtime_create_presentation_binding_from_device(rendering_device);
						secondary_binding = output->active_binding;
					}
					bool retained_secondary_surface = p_state->submitted_request_is_configuration_only
							&& secondary_binding == output->active_binding
							&& output->active_generation != 0
							&& secondary_info.output_id == output->active_output_id
							&& secondary_info.pixel_width == output->active_pixel_width
							&& secondary_info.pixel_height == output->active_pixel_height;
					hcsr_runtime_status_t secondary_submit_status = retained_secondary_surface
							? HCSR_RUNTIME_OK
							: secondary_binding != nullptr
									? hcsr_runtime_d3d12_presenter_submit(secondary_binding->presenter,
											publication, publication_info.generation, output_index + 1)
									: HCSR_RUNTIME_INTERNAL_ERROR;
					if (secondary_submit_status == HCSR_RUNTIME_STALE_REQUEST
							&& p_state->submitted_request_is_configuration_only
							&& output->active_generation != 0
							&& secondary_info.output_id == output->active_output_id
							&& secondary_info.pixel_width == output->active_pixel_width
							&& secondary_info.pixel_height == output->active_pixel_height) {
						secondary_submit_status = HCSR_RUNTIME_OK;
						retained_secondary_surface = true;
					}
					if (secondary_submit_status == HCSR_RUNTIME_RECONFIGURATION_REQUIRED
							&& secondary_binding == output->active_binding
							&& output->successor_binding == nullptr
							&& output->retiring_binding == nullptr) {
						RenderingDevice *rendering_device = RenderingServer::get_singleton()->get_rendering_device();
						output->successor_binding = runtime_create_presentation_binding_from_device(rendering_device);
						secondary_binding = output->successor_binding;
						secondary_submit_status = secondary_binding != nullptr
								? hcsr_runtime_d3d12_presenter_submit(secondary_binding->presenter,
										publication, publication_info.generation, output_index + 1)
								: HCSR_RUNTIME_INTERNAL_ERROR;
					}
					if (secondary_submit_status != HCSR_RUNTIME_OK) {
						hcsr_runtime_publication_release(p_state->session, publication);
						runtime_set_terminal(p_state, vformat("HCSR replacement could not submit atomic secondary D3D12 output %d with status %d.", entry.output_id, (int)secondary_submit_status));
						return false;
					}
					secondary_binding->pending_lineage = lineage;
					secondary_binding->presenter_pending = !retained_secondary_surface;
					output->staged_binding = retained_secondary_surface ? secondary_binding : nullptr;
					output->staged_lineage = retained_secondary_surface ? lineage : RuntimePublicationLineage();
					output->activation_ready = retained_secondary_surface;
				}
				runtime_replace_output_topology(p_state->staged_topology, p_state->submitted_topology);
			}
			hcsr_runtime_publication_release(p_state->session, publication);
		}
	}
	if (p_state->active_binding->presenter_pending) {
		const uint64_t presenter_cutoff = p_state->active_binding->pending_lineage.interactive_submission_id
				== p_state->newest_requested_submission_id
			? p_state->newest_requested_cutoff_timestamp_microseconds
			: 0;
		status = runtime_step_presenter_sliced(
				p_state->active_binding->presenter, presenter_cutoff, &step);
		if (status == HCSR_RUNTIME_PENDING && presenter_cutoff != 0
				&& hcsr_runtime_get_monotonic_timestamp_microseconds() >= presenter_cutoff) {
			runtime_set_terminal(p_state, vformat(
					"HCSR replacement missed the mandatory next-frame primary GPU presentation cutoff (semantic work units %d).",
					interactive_work_units));
			return false;
		}
		if (status != HCSR_RUNTIME_OK && status != HCSR_RUNTIME_PENDING) {
			runtime_set_terminal(p_state, "HCSR replacement D3D12 presenter failed.");
			return false;
		}
		if (status == HCSR_RUNTIME_OK) {
			p_state->active_binding->presenter_pending = false;
			p_state->staged_binding = p_state->active_binding;
			p_state->staged_lineage = p_state->active_binding->pending_lineage;
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
		if (status == HCSR_RUNTIME_PENDING && presenter_cutoff != 0
				&& hcsr_runtime_get_monotonic_timestamp_microseconds() >= presenter_cutoff) {
			runtime_set_terminal(p_state, "HCSR replacement missed the mandatory next-frame successor GPU presentation cutoff.");
			return false;
		}
		if (status != HCSR_RUNTIME_OK && status != HCSR_RUNTIME_PENDING) {
			runtime_set_terminal(p_state, "HCSR replacement successor D3D12 presenter failed.");
			return false;
		}
		if (status == HCSR_RUNTIME_OK) {
			p_state->successor_binding->presenter_pending = false;
			p_state->staged_binding = p_state->successor_binding;
			p_state->staged_lineage = p_state->successor_binding->pending_lineage;
		}
	}
	for (const RuntimeOutputSnapshotEntry &entry : p_state->staged_topology.outputs) {
		RuntimeOutputState *output = entry.state;
		RuntimePresentationBinding *binding = output->successor_binding != nullptr
				? output->successor_binding : output->active_binding;
		if (binding == nullptr || !binding->presenter_pending) {
			continue;
		}
		const uint64_t presenter_cutoff = binding->pending_lineage.interactive_submission_id
				== p_state->newest_requested_submission_id
			? p_state->newest_requested_cutoff_timestamp_microseconds : 0;
		status = runtime_step_presenter_sliced(binding->presenter, presenter_cutoff, &step);
		if (status == HCSR_RUNTIME_PENDING && presenter_cutoff != 0
				&& hcsr_runtime_get_monotonic_timestamp_microseconds() >= presenter_cutoff) {
			runtime_set_terminal(p_state, "HCSR replacement missed the mandatory next-frame secondary GPU presentation cutoff.");
			return false;
		}
		if (status != HCSR_RUNTIME_OK && status != HCSR_RUNTIME_PENDING) {
			runtime_set_terminal(p_state, "HCSR replacement secondary D3D12 presenter failed.");
			return false;
		}
		if (status == HCSR_RUNTIME_OK) {
			binding->presenter_pending = false;
			output->staged_binding = binding;
			output->staged_lineage = binding->pending_lineage;
			output->activation_ready = true;
		}
	}
	const bool primary_ready = p_state->staged_binding != nullptr
			&& !p_state->staged_binding->presenter_pending
			&& p_state->staged_lineage.valid;
	bool all_outputs_ready = primary_ready;
	for (const RuntimeOutputSnapshotEntry &entry : p_state->staged_topology.outputs) {
		RuntimeOutputState *output = entry.state;
		all_outputs_ready = all_outputs_ready && output->activation_ready
				&& output->staged_lineage.runtime_generation == p_state->staged_lineage.runtime_generation;
	}
	if (all_outputs_ready && !p_state->activation_pending) {
		p_state->activation_pending = true;
		p_state->activation_callback_scheduled = runtime_schedule_frame_cutoff(p_state);
	}
	{
		MutexLock lock(p_state->mutex);
		p_state->semantic_pending = semantic_pending;
		p_state->pending_work = p_state->active_binding->presenter_pending
				|| (p_state->successor_binding != nullptr && p_state->successor_binding->presenter_pending)
				|| p_state->activation_pending
				|| p_state->retiring_binding != nullptr
				|| !p_state->retiring_outputs.is_empty()
				|| p_state->semantic_pending;
		for (const RuntimeOutputSnapshotEntry &entry : p_state->staged_topology.outputs) {
			RuntimeOutputState *output = entry.state;
			p_state->pending_work = p_state->pending_work
					|| (output->active_binding != nullptr && output->active_binding->presenter_pending)
					|| (output->successor_binding != nullptr && output->successor_binding->presenter_pending)
					|| (output->activation_ready && !p_state->activation_pending);
		}
	}
	return true;
}

static bool runtime_step_pointer_input(HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state) {
	if (p_state->active_pointer_submission_id == 0) {
		// Inputs may arrive while the first document generation is still deriving.
		// They are not admitted until an exact interaction authority is active.
		if (p_state->active_generation == 0) {
			return true;
		}
		if (!p_state->active_has_interaction_state) {
			MutexLock lock(p_state->mutex);
			p_state->pointer_requests.clear();
			return true;
		}
		RuntimePointerRequest request;
		{
			MutexLock lock(p_state->mutex);
			if (p_state->pointer_requests.is_empty()) {
				return true;
			}
			request = p_state->pointer_requests[0];
			p_state->pointer_requests.remove_at(0);
		}
		hcsr_runtime_pointer_input_t input;
		initialize_abi(&input, sizeof(input));
		input.source_runtime_generation = p_state->active_generation;
		input.configuration_id = p_state->active_configuration_id;
		input.source_input_id = p_state->active_interaction_input_id;
		input.input_id = p_state->next_host_input_id++;
		input.frame_id = p_state->next_host_frame_id++;
		input.cutoff_timestamp_microseconds = hcsr_runtime_get_monotonic_timestamp_microseconds()
				+ RUNTIME_INTERACTIVE_FRAME_BUDGET_MICROSECONDS;
		const bool pointer_needs_position = request.kind == HCSR_RUNTIME_POINTER_MOVE
				|| request.kind == HCSR_RUNTIME_POINTER_PRIMARY_DOWN
				|| request.kind == HCSR_RUNTIME_POINTER_PRIMARY_UP;
		input.logical_x = pointer_needs_position ? request.position.x : 0.0;
		input.logical_y = pointer_needs_position ? request.position.y : 0.0;
		input.buttons = request.buttons;
		input.kind = request.kind;
		input.focus_on_primary_down = request.focus_on_primary_down ? 1 : 0;
		hcsr_runtime_pointer_submission_info_t submission;
		initialize_abi(&submission, sizeof(submission));
		const hcsr_runtime_status_t submit_status = hcsr_runtime_session_submit_pointer_input(
				p_state->session, &input, &submission);
		if (submit_status == HCSR_RUNTIME_STALE_REQUEST || submit_status == HCSR_RUNTIME_GENERATION_MISMATCH) {
			MutexLock lock(p_state->mutex);
			p_state->pointer_requests.insert(0, request);
			p_state->pending_work = true;
			return true;
		}
		if (submit_status != HCSR_RUNTIME_OK) {
			runtime_set_terminal(p_state, runtime_copy_last_error(p_state, vformat(
					"HCSR replacement pointer submission failed (status %d, kind %d, buttons %d, generation %d, configuration %d, source input %d, position %s).",
					(int)submit_status, input.kind, input.buttons, input.source_runtime_generation, input.configuration_id,
					input.source_input_id, request.position)));
			return false;
		}
		p_state->active_pointer_submission_id = submission.submission_id;
		p_state->active_pointer_request = request;
		p_state->active_pointer_request_valid = true;
		p_state->active_pointer_cutoff_timestamp_microseconds = input.cutoff_timestamp_microseconds;
	}

	hcsr_runtime_pointer_step_info_t step;
	initialize_abi(&step, sizeof(step));
	const hcsr_runtime_status_t step_status = hcsr_runtime_session_step_pointer_input(
			p_state->session, p_state->active_pointer_submission_id,
			RUNTIME_INTERACTIVE_STEP_SLICE_UNITS, &step);
	if (step_status == HCSR_RUNTIME_PENDING) {
		return true;
	}
	if (step_status == HCSR_RUNTIME_STALE_REQUEST || step_status == HCSR_RUNTIME_GENERATION_MISMATCH) {
		p_state->active_pointer_submission_id = 0;
		if (p_state->active_pointer_request_valid) {
			MutexLock lock(p_state->mutex);
			p_state->pointer_requests.insert(0, p_state->active_pointer_request);
			p_state->pending_work = true;
		}
		p_state->active_pointer_request_valid = false;
		p_state->active_pointer_cutoff_timestamp_microseconds = 0;
		return true;
	}
	if (step_status != HCSR_RUNTIME_OK || step.state != HCSR_RUNTIME_POINTER_STEP_STATE_SUBMITTED) {
		runtime_set_terminal(p_state, vformat("HCSR replacement pointer interpretation failed (status %d).", (int)step_status));
		return false;
	}
	if (step.interactive_submission_id == 0) {
		runtime_set_terminal(p_state, "HCSR replacement pointer interpretation did not create a deadline-qualified runtime submission.");
		return false;
	}
	p_state->active_pointer_submission_id = 0;
	p_state->active_pointer_request_valid = false;
	p_state->newest_requested_submission_id = step.interactive_submission_id;
	p_state->newest_requested_frame_id = step.frame_id;
	p_state->newest_requested_cutoff_timestamp_microseconds = p_state->active_pointer_cutoff_timestamp_microseconds;
	p_state->active_pointer_cutoff_timestamp_microseconds = 0;
	p_state->submitted_request_serial = p_state->request_serial;
	p_state->submitted_request_is_configuration_only = false;
	p_state->interactive_pending = true;
	p_state->semantic_pending = true;
	return true;
}

static bool runtime_submit_one_scroll_input(HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state) {
	RuntimeScrollRequest request;
	{
		MutexLock lock(p_state->mutex);
		if (p_state->scroll_requests.is_empty()) {
			return true;
		}
		request = p_state->scroll_requests[0];
		p_state->scroll_requests.remove_at(0);
	}
	hcsr_runtime_scroll_input_t input;
	initialize_abi(&input, sizeof(input));
	input.source_runtime_generation = p_state->active_generation;
	input.configuration_id = p_state->active_configuration_id;
	input.source_input_id = p_state->active_scroll_input_id;
	input.input_id = p_state->next_host_input_id++;
	input.frame_id = p_state->next_host_frame_id++;
	input.logical_x = request.position.x;
	input.logical_y = request.position.y;
	input.delta_x = request.delta.x;
	input.delta_y = request.delta.y;
	input.kind = request.kind;
	input.granularity = HCSR_RUNTIME_SCROLL_GRANULARITY_PRECISE_PIXEL;
	input.source = request.source;
	input.orientation = request.orientation;
	hcsr_runtime_scroll_submission_info_t submission;
	initialize_abi(&submission, sizeof(submission));
	const hcsr_runtime_status_t status = hcsr_runtime_session_submit_scroll_input(
			p_state->session, &input, &submission);
	if (status == HCSR_RUNTIME_STALE_REQUEST || status == HCSR_RUNTIME_GENERATION_MISMATCH) {
		return true;
	}
	if (status != HCSR_RUNTIME_OK) {
		runtime_set_terminal(p_state, vformat("HCSR replacement scroll submission failed (status %d).", (int)status));
		return false;
	}
	p_state->semantic_pending = true;
	return true;
}

static bool runtime_step_shutdown(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state) {
	runtime_release_output_topology(p_state->staged_topology);
	runtime_release_output_topology(p_state->submitted_topology);
	runtime_release_output_topology(p_state->candidate_topology);
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	RenderingDevice *rendering_device = rendering_server != nullptr
			? rendering_server->get_rendering_device()
			: nullptr;
	if (rendering_server == nullptr || rendering_device == nullptr) {
		if (p_state->session == nullptr && p_state->active_binding == nullptr
				&& p_state->successor_binding == nullptr && p_state->retiring_binding == nullptr) {
			for (RuntimeOutputState *output : p_state->outputs) {
				output->texture.unref();
				memdelete(output);
			}
			p_state->outputs.clear();
			for (RuntimeOutputState *output : p_state->retiring_outputs) {
				output->texture.unref();
				memdelete(output);
			}
			p_state->retiring_outputs.clear();
			if (p_state->compiled_document != nullptr) {
				hcsr_runtime_document_release(p_state->compiled_document);
				p_state->compiled_document = nullptr;
			}
			return true;
		}
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
	for (RuntimeOutputState *output : p_state->outputs) {
		if (!runtime_step_retiring_output(p_state, output, rendering_server, rendering_device)) {
			bindings_complete = false;
			continue;
		}
		memdelete(output);
	}
	if (bindings_complete) {
		p_state->outputs.clear();
	}
	for (RuntimeOutputState *output : p_state->retiring_outputs) {
		if (!runtime_step_retiring_output(p_state, output, rendering_server, rendering_device)) {
			bindings_complete = false;
			continue;
		}
		memdelete(output);
	}
	if (bindings_complete) {
		p_state->retiring_outputs.clear();
	}
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
	Vector<RuntimeResolvedStylesheet> stylesheets;
	Vector<RuntimeMutation> mutations;
	uint64_t mutation_request_process_frame = 0;
	uint64_t request_serial = 0;
	bool document_dirty = false;
	bool configuration_dirty = false;
	bool closing = false;
	auto finish_scheduled_work = [runtime]() {
		MutexLock lock(runtime->mutex);
		runtime->work_scheduled = false;
	};
	{
		MutexLock lock(runtime->mutex);
		closing = runtime->closing;
		if (!closing && !runtime->terminal) {
			document_dirty = runtime->document_dirty;
			configuration_dirty = runtime->configuration_dirty;
			runtime->document_dirty = false;
			runtime->configuration_dirty = false;
			html = runtime->html;
			css = runtime->css;
			stylesheets = runtime->stylesheets;
			// Mutation journals are qualified to the exact durable author
			// revision at MutationBegin. Keep host descriptions queued while a
			// semantic/resource successor is deriving; otherwise the journal is
			// stale before SubmitMutation and cannot be retried.
			if (!runtime->semantic_pending && !runtime->interactive_pending) {
				mutations = runtime->mutations;
				mutation_request_process_frame = runtime->mutation_request_process_frame;
				runtime->mutations.clear();
			}
			request_serial = runtime->request_serial;
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
		finish_scheduled_work();
		return;
	}
	const bool initialized_before_step = runtime->session != nullptr;
	if (runtime->terminal || !runtime_ensure_initialized(runtime)) {
		finish_scheduled_work();
		return;
	}
	if (!initialized_before_step) {
		// Session creation consumed the latest logical and physical dimensions.
		configuration_dirty = false;
	}
	if (document_dirty && !runtime_submit_document(runtime, html, css, stylesheets)) {
		finish_scheduled_work();
		return;
	}
	if (configuration_dirty && !runtime_submit_configuration(runtime, request_serial)) {
		finish_scheduled_work();
		return;
	}
	if (!mutations.is_empty() && (runtime->semantic_pending || runtime->interactive_pending)) {
		MutexLock lock(runtime->mutex);
		// Preserve chronological order ahead of mutations queued since this
		// callback captured its snapshot.
		Vector<RuntimeMutation> combined = mutations;
		combined.append_array(runtime->mutations);
		runtime->mutations = combined;
		mutations.clear();
	}
	int consumed_mutation_count = 0;
	if (!runtime_submit_mutations(runtime, mutations, mutation_request_process_frame, request_serial, consumed_mutation_count)) {
		finish_scheduled_work();
		return;
	}
	if (consumed_mutation_count < mutations.size()) {
		MutexLock lock(runtime->mutex);
		Vector<RuntimeMutation> remaining;
		for (int index = consumed_mutation_count; index < mutations.size(); index++) {
			remaining.push_back(mutations[index]);
		}
		remaining.append_array(runtime->mutations);
		runtime->mutations = remaining;
		runtime->pending_work = true;
	}
	if (!runtime_step_pointer_input(runtime) || !runtime_submit_one_scroll_input(runtime)) {
		finish_scheduled_work();
		return;
	}
	bool has_pending_work = false;
	{
		MutexLock lock(runtime->mutex);
		has_pending_work = runtime->pending_work
				|| runtime->active_pointer_submission_id != 0
				|| !runtime->mutations.is_empty()
				|| !runtime->pointer_requests.is_empty()
				|| !runtime->scroll_requests.is_empty();
		runtime->pending_work = has_pending_work;
	}
	if (has_pending_work) {
		runtime_step_active(runtime);
	}
	finish_scheduled_work();
	{
		MutexLock lock(runtime->mutex);
		has_pending_work = !runtime->closing && !runtime->terminal
				&& (runtime->pending_work || !runtime->mutations.is_empty());
		runtime->pending_work = has_pending_work;
	}
}

void HTMLSurfaceHCSRRuntimeBackend::_activate_frame_cutoff_on_render_thread_callback(uint64_t p_state_ptr) {
	RuntimeState *runtime = (RuntimeState *)p_state_ptr;
	uint64_t cutoff_process_frame = 0;
	uint64_t cutoff_request_serial = 0;
	{
		MutexLock lock(runtime->mutex);
		runtime->cutoff_scheduled = false;
		runtime->activation_callback_scheduled = false;
		cutoff_process_frame = runtime->cutoff_process_frame;
		cutoff_request_serial = runtime->request_serial;
		if (runtime->closing || runtime->terminal) {
			return;
		}
	}
	const uint64_t current_process_frame = Engine::get_singleton()->get_process_frames();
	const bool exact_request_serial = runtime->staged_lineage.configuration_only
			|| runtime->staged_lineage.request_serial == cutoff_request_serial;
	const bool exact_requested_authority = runtime->staged_lineage.valid
			&& exact_request_serial
			&& (runtime->staged_lineage.configuration_only
					|| runtime->newest_requested_submission_id == 0
					|| (runtime->staged_lineage.interactive_submission_id == runtime->newest_requested_submission_id
							&& runtime->staged_lineage.target_author_revision == runtime->newest_requested_author_revision
							&& runtime->staged_lineage.interactive_frame_id == runtime->newest_requested_frame_id));
	const bool exact_interactive_frame = runtime->activation_pending
			&& runtime->staged_lineage.interactive_submission_id != 0
			&& runtime->staged_lineage.interactive_submission_id == runtime->newest_requested_submission_id;
	if (exact_interactive_frame
			&& (current_process_frame != cutoff_process_frame
					|| hcsr_runtime_get_monotonic_timestamp_microseconds()
							> runtime->newest_requested_cutoff_timestamp_microseconds)) {
		runtime_set_terminal(runtime, "HCSR replacement could not activate the input-qualified output in its mandatory next presented frame.");
		return;
	}
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
		if (!runtime_activate_surface(runtime, activated_binding, nullptr)) {
			return;
		}
		for (const RuntimeOutputSnapshotEntry &entry : runtime->staged_topology.outputs) {
			RuntimeOutputState *output = entry.state;
			if (!output->activation_ready || output->staged_binding == nullptr
					|| output->staged_lineage.runtime_generation != runtime->staged_lineage.runtime_generation
					|| !runtime_activate_surface(runtime, output->staged_binding, output)) {
				runtime_set_terminal(runtime, "Godot could not atomically activate the complete HCSR output set.");
				return;
			}
			if (output->staged_binding == output->successor_binding) {
				output->retiring_binding = output->active_binding;
				output->active_binding = output->successor_binding;
				output->successor_binding = nullptr;
			}
			output->activation_ready = false;
			output->staged_binding = nullptr;
			output->staged_lineage = RuntimePublicationLineage();
		}
		if (activated_binding == runtime->successor_binding) {
			runtime->retiring_binding = runtime->active_binding;
			runtime->active_binding = runtime->successor_binding;
			runtime->successor_binding = nullptr;
		}
		{
			MutexLock lock(runtime->mutex);
			runtime->active_generation = runtime->staged_lineage.runtime_generation;
			runtime->active_interaction_input_id = runtime->staged_lineage.interaction_input_id;
			runtime->active_has_interaction_state = runtime->staged_lineage.has_interaction_state;
			runtime->queued_generation = runtime->staged_lineage.runtime_generation;
			runtime->frame_metadata.generation = runtime->staged_lineage.runtime_generation;
			for (const RuntimeOutputSnapshotEntry &entry : runtime->staged_topology.outputs) {
				entry.state->active_generation = runtime->staged_lineage.runtime_generation;
			}
			runtime->presentation_changed = true;
		}
		runtime->activation_pending = false;
		runtime->activation_callback_scheduled = false;
		runtime->staged_binding = nullptr;
		runtime->staged_lineage = RuntimePublicationLineage();
		runtime_release_output_topology(runtime->staged_topology);
		runtime_release_output_topology(runtime->candidate_topology);
		runtime->last_activation_process_frame = cutoff_process_frame;
	}
	{
		MutexLock lock(runtime->mutex);
		runtime->pending_work = (runtime->active_binding != nullptr && runtime->active_binding->presenter_pending)
				|| (runtime->successor_binding != nullptr && runtime->successor_binding->presenter_pending)
				|| runtime->activation_pending
				|| runtime->retiring_binding != nullptr
				|| runtime->semantic_pending;
		for (const RuntimeOutputSnapshotEntry &entry : runtime->staged_topology.outputs) {
			RuntimeOutputState *output = entry.state;
			runtime->pending_work = runtime->pending_work
					|| (output->active_binding != nullptr && output->active_binding->presenter_pending)
					|| (output->successor_binding != nullptr && output->successor_binding->presenter_pending);
		}
	}
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
	Vector<RuntimeResolvedStylesheet> stylesheets;
	String error;
	if (!runtime_load_document_source(document, html, css, stylesheets, error)) {
		if (document.is_valid() && (!document->get_html().is_empty() || !document->get_html_file().is_empty())) {
			runtime_set_terminal(state, error);
		}
		return;
	}
	{
		MutexLock lock(state->mutex);
		state->html = html;
		state->css = css;
		state->stylesheets = stylesheets;
		state->document = document;
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
		*r_needs_output = has_pending_frame_request();
	}
	if (r_needs_begin_frame != nullptr) {
		*r_needs_begin_frame = false;
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

void HTMLSurfaceHCSRRuntimeBackend::schedule_retirement_service() {
	if (has_pending_output()) {
		_schedule_work();
	}
}

bool HTMLSurfaceHCSRRuntimeBackend::has_pending_output() const {
	if (state == nullptr) {
		return false;
	}
	MutexLock lock(state->mutex);
	return state->pending_work || state->work_scheduled;
}

bool HTMLSurfaceHCSRRuntimeBackend::has_pending_frame_request() const {
	if (state == nullptr) {
		return false;
	}
	MutexLock lock(state->mutex);
	return state->document_dirty || state->configuration_dirty
			|| !state->mutations.is_empty() || !state->pointer_requests.is_empty()
			|| !state->scroll_requests.is_empty();
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

static Error runtime_queue_pointer_request(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		hcsr_runtime_pointer_event_kind_t p_kind,
		const Point2 &p_position,
		uint32_t p_buttons,
		bool p_focus_on_primary_down) {
	ERR_FAIL_NULL_V(p_state, ERR_UNAVAILABLE);
	{
		MutexLock lock(p_state->mutex);
		if (p_state->closing || p_state->terminal || p_state->active_generation == 0) {
			return ERR_UNAVAILABLE;
		}
		RuntimePointerRequest request;
		request.kind = p_kind;
		request.position = p_position;
		request.buttons = p_buttons;
		request.focus_on_primary_down = p_focus_on_primary_down;
		p_state->pointer_requests.push_back(request);
		p_state->pointer_position = p_position;
		p_state->pending_work = true;
	}
	runtime_schedule_state(p_state);
	return OK;
}

Error HTMLSurfaceHCSRRuntimeBackend::mouse_move(const Point2 &p_position, int p_modifiers, bool &r_visual_state_changed) {
	(void)p_modifiers;
	r_visual_state_changed = true;
	uint32_t buttons = 0;
	if (state != nullptr) {
		MutexLock lock(state->mutex);
		buttons = state->pointer_buttons;
	}
	return runtime_queue_pointer_request(state, HCSR_RUNTIME_POINTER_MOVE, p_position, buttons, false);
}

Error HTMLSurfaceHCSRRuntimeBackend::mouse_down(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) {
	(void)p_modifiers;
	(void)p_click_count;
	if (p_button != HTML_SURFACE_MOUSE_BUTTON_LEFT || state == nullptr) {
		return ERR_UNAVAILABLE;
	}
	{
		MutexLock lock(state->mutex);
		state->pointer_buttons |= HCSR_RUNTIME_POINTER_BUTTON_PRIMARY;
	}
	return runtime_queue_pointer_request(state, HCSR_RUNTIME_POINTER_PRIMARY_DOWN, p_position,
			HCSR_RUNTIME_POINTER_BUTTON_PRIMARY, true);
}

Error HTMLSurfaceHCSRRuntimeBackend::mouse_up(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) {
	(void)p_modifiers;
	(void)p_click_count;
	if (p_button != HTML_SURFACE_MOUSE_BUTTON_LEFT || state == nullptr) {
		return ERR_UNAVAILABLE;
	}
	{
		MutexLock lock(state->mutex);
		state->pointer_buttons &= ~HCSR_RUNTIME_POINTER_BUTTON_PRIMARY;
	}
	return runtime_queue_pointer_request(state, HCSR_RUNTIME_POINTER_PRIMARY_UP, p_position, 0, false);
}

Error HTMLSurfaceHCSRRuntimeBackend::pointer_cancel(const Point2 &p_position, int p_pointer_id) {
	(void)p_pointer_id;
	if (state != nullptr) {
		MutexLock lock(state->mutex);
		state->pointer_buttons = 0;
	}
	return runtime_queue_pointer_request(state, HCSR_RUNTIME_POINTER_CANCEL, p_position, 0, false);
}

Error HTMLSurfaceHCSRRuntimeBackend::notify_pointer_leave(const Point2 &p_position, bool p_cancel_pressed_interaction, int p_pointer_id) {
	(void)p_pointer_id;
	if (state != nullptr && p_cancel_pressed_interaction) {
		MutexLock lock(state->mutex);
		state->pointer_buttons = 0;
	}
	return runtime_queue_pointer_request(state,
			p_cancel_pressed_interaction ? HCSR_RUNTIME_POINTER_CANCEL : HCSR_RUNTIME_POINTER_LEAVE,
			p_position, 0, false);
}

static Error runtime_queue_scroll_request(
		HTMLSurfaceHCSRRuntimeBackend::RuntimeState *p_state,
		int32_t p_kind,
		const Point2 &p_position,
		const Vector2 &p_delta,
		int32_t p_source,
		int32_t p_orientation) {
	ERR_FAIL_NULL_V(p_state, ERR_UNAVAILABLE);
	{
		MutexLock lock(p_state->mutex);
		if (p_state->closing || p_state->terminal || p_state->active_generation == 0) {
			return ERR_UNAVAILABLE;
		}
		RuntimeScrollRequest request;
		request.kind = p_kind;
		request.position = p_position;
		request.delta = p_delta;
		request.source = p_source;
		request.orientation = p_orientation;
		p_state->scroll_requests.push_back(request);
		p_state->pending_work = true;
	}
	runtime_schedule_state(p_state);
	return OK;
}

Error HTMLSurfaceHCSRRuntimeBackend::begin_scrollbar_interaction(const Point2 &p_position, double p_event_time_seconds, bool &r_consumed) {
	(void)p_event_time_seconds;
	r_consumed = false;
	if (state == nullptr) {
		return ERR_UNAVAILABLE;
	}
	{
		MutexLock lock(state->mutex);
		state->scrollbar_interaction_active = true;
	}
	const Error result = runtime_queue_scroll_request(state, HCSR_RUNTIME_SCROLL_INPUT_SCROLLBAR_BEGIN,
			p_position, Vector2(), HCSR_RUNTIME_SCROLL_SOURCE_SCROLLBAR, HCSR_RUNTIME_SCROLL_VERTICAL);
	r_consumed = result == OK;
	return result;
}

Error HTMLSurfaceHCSRRuntimeBackend::update_scrollbar_interaction(const Point2 &p_position, bool &r_consumed) {
	r_consumed = false;
	if (state == nullptr) {
		return ERR_UNAVAILABLE;
	}
	{
		MutexLock lock(state->mutex);
		if (!state->scrollbar_interaction_active) {
			return OK;
		}
	}
	const Error result = runtime_queue_scroll_request(state, HCSR_RUNTIME_SCROLL_INPUT_SCROLLBAR_UPDATE,
			p_position, Vector2(), HCSR_RUNTIME_SCROLL_SOURCE_SCROLLBAR, HCSR_RUNTIME_SCROLL_VERTICAL);
	r_consumed = result == OK;
	return result;
}

Error HTMLSurfaceHCSRRuntimeBackend::end_scrollbar_interaction(bool &r_consumed) {
	r_consumed = false;
	if (state == nullptr) {
		return ERR_UNAVAILABLE;
	}
	Point2 position;
	{
		MutexLock lock(state->mutex);
		if (!state->scrollbar_interaction_active) {
			return OK;
		}
		state->scrollbar_interaction_active = false;
		position = state->pointer_position;
	}
	const Error result = runtime_queue_scroll_request(state, HCSR_RUNTIME_SCROLL_INPUT_SCROLLBAR_END,
			position, Vector2(), HCSR_RUNTIME_SCROLL_SOURCE_SCROLLBAR, HCSR_RUNTIME_SCROLL_VERTICAL);
	r_consumed = result == OK;
	return result;
}

bool HTMLSurfaceHCSRRuntimeBackend::poll_pointer_event(HTMLPointerEvent &r_event) {
	if (state == nullptr) {
		return false;
	}
	MutexLock lock(state->mutex);
	if (state->pointer_events.is_empty()) {
		return false;
	}
	r_event = state->pointer_events[0];
	state->pointer_events.remove_at(0);
	return true;
}

Error HTMLSurfaceHCSRRuntimeBackend::wheel(const Point2 &p_position, const Vector2 &p_delta) {
	if (p_delta.is_zero_approx()) {
		return OK;
	}
	return runtime_queue_scroll_request(state, HCSR_RUNTIME_SCROLL_INPUT_WHEEL,
			p_position, p_delta, HCSR_RUNTIME_SCROLL_SOURCE_TOUCHPAD,
			Math::abs(p_delta.x) > Math::abs(p_delta.y) ? HCSR_RUNTIME_SCROLL_HORIZONTAL : HCSR_RUNTIME_SCROLL_VERTICAL);
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

Error HTMLSurfaceHCSRRuntimeBackend::set_element_inner_html(
		const StringName &p_id, const String &p_html_fragment) {
	return _queue_mutation(RUNTIME_MUTATION_INNER_HTML, p_id, StringName(), p_html_fragment);
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

uint64_t HTMLSurfaceHCSRRuntimeBackend::create_presentation_output(const Size2i &p_size, bool p_mipmaps) {
	if (state == nullptr || p_size.x <= 0 || p_size.y <= 0) {
		return 0;
	}
	RuntimeOutputState *output = memnew(RuntimeOutputState);
	output->requested_size = p_size;
	output->mipmaps = p_mipmaps;
	output->texture.instantiate();
	uint64_t output_id = 0;
	{
		MutexLock lock(state->mutex);
		if (state->closing || state->terminal || state->next_output_id > INT32_MAX) {
			memdelete(output);
			return 0;
		}
		output_id = state->next_output_id++;
		output->output_id = output_id;
		state->outputs.push_back(output);
		state->requested_topology_revision++;
		state->configuration_dirty = true;
		state->request_serial++;
		state->pending_work = true;
	}
	_schedule_work();
	return output_id;
}

Error HTMLSurfaceHCSRRuntimeBackend::resize_presentation_output(uint64_t p_output_id, const Size2i &p_size) {
	ERR_FAIL_COND_V(p_size.x <= 0 || p_size.y <= 0, ERR_INVALID_PARAMETER);
	ERR_FAIL_NULL_V(state, ERR_UNAVAILABLE);
	{
		MutexLock lock(state->mutex);
		RuntimeOutputState *output = nullptr;
		for (RuntimeOutputState *candidate : state->outputs) {
			if (candidate->output_id == p_output_id) {
				output = candidate;
				break;
			}
		}
		ERR_FAIL_NULL_V(output, ERR_DOES_NOT_EXIST);
		if (output->requested_size == p_size) {
			return OK;
		}
		output->requested_size = p_size;
		state->requested_topology_revision++;
		state->configuration_dirty = true;
		state->request_serial++;
		state->pending_work = true;
	}
	_schedule_work();
	return OK;
}

void HTMLSurfaceHCSRRuntimeBackend::destroy_presentation_output(uint64_t p_output_id) {
	if (state == nullptr) {
		return;
	}
	RuntimeOutputState *removed = nullptr;
	{
		MutexLock lock(state->mutex);
		for (int index = 0; index < state->outputs.size(); index++) {
			if (state->outputs[index]->output_id == p_output_id) {
				removed = state->outputs[index];
				state->outputs.remove_at(index);
				break;
			}
		}
		if (removed == nullptr) {
			return;
		}
		state->requested_topology_revision++;
		state->configuration_dirty = true;
		state->request_serial++;
		state->pending_work = true;
	}
	if (removed->texture.is_valid()) {
		removed->texture->clear_external_texture();
	}
	// Presenter and external-texture ownership are retired on the render thread.
	// Keep the detached state reachable until that bounded retirement completes.
	{
		MutexLock lock(state->mutex);
		state->retiring_outputs.push_back(removed);
	}
	_schedule_work();
}

Ref<Texture2D> HTMLSurfaceHCSRRuntimeBackend::get_presentation_output_texture(uint64_t p_output_id) const {
	if (state == nullptr) {
		return Ref<Texture2D>();
	}
	MutexLock lock(state->mutex);
	for (RuntimeOutputState *output : state->outputs) {
		if (output->output_id == p_output_id) {
			return output->texture;
		}
	}
	return Ref<Texture2D>();
}

uint64_t HTMLSurfaceHCSRRuntimeBackend::get_presentation_output_generation(uint64_t p_output_id) const {
	if (state == nullptr) {
		return 0;
	}
	MutexLock lock(state->mutex);
	for (RuntimeOutputState *output : state->outputs) {
		if (output->output_id == p_output_id) {
			return output->active_generation;
		}
	}
	return 0;
}

HTMLSurfaceHCSRRuntimeBackend::HTMLSurfaceHCSRRuntimeBackend() {
	texture.instantiate();
	state = memnew(RuntimeState);
	state->texture = texture;
	if (hcsr_runtime_get_abi_version() != RUNTIME_REQUIRED_ABI_VERSION) {
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

/**************************************************************************/
/*  html_surface_hcsr_backend.cpp                                        */
/**************************************************************************/

#include "html_surface_hcsr_backend.h"

#include "../bridge/html_asset_provider.h"
#include "hcsr_frame_budget_service.h"
#include "hcsr_session_retirement_service.h"

#include "core/os/os.h"
#include "core/string/print_string.h"

namespace {

constexpr uint64_t HCSR_RUNTIME_FRAME_STEP_BUDGET_USEC = 1000;
constexpr int32_t HCSR_RUNTIME_TILE_SIZE = 64;

static String copy_report_text(hcsr_runtime_compilation_report_t *p_report, int p_index, bool p_message, int p_byte_count) {
	if (p_byte_count <= 1) {
		return String();
	}
	Vector<char> bytes;
	bytes.resize(p_byte_count);
	if (hcsr_runtime_compilation_report_copy_diagnostic_text(
			p_report,
			p_index,
			p_message ? 1 : 0,
			bytes.ptrw(),
			bytes.size()) != HCSR_RUNTIME_OK) {
		return String();
	}
	return String::utf8(bytes.ptr(), bytes.size() - 1);
}

} // namespace

void HTMLSurfaceHCSRBackend::_set_terminal_failure(const String &p_message) {
	terminal_failure = true;
	terminal_failure_reason = p_message;
	ERR_PRINT(p_message);
}

bool HTMLSurfaceHCSRBackend::_load_document_source(String &r_html, String &r_css) const {
	if (document.is_null() || !document->is_source_valid()) {
		return false;
	}

	r_html = document->get_html();
	if (r_html.is_empty() && !document->get_html_file().is_empty()) {
		HTMLAssetResource asset;
		String error;
		if (HTMLGodotAssetProvider::load_asset(document, document->get_html_file(), asset, &error) != OK) {
			ERR_PRINT(error);
			return false;
		}
		r_html = String::utf8(reinterpret_cast<const char *>(asset.bytes.ptr()), asset.bytes.size());
	}
	if (r_html.strip_edges().is_empty()) {
		return false;
	}

	r_css = String();
	for (const String &css_file : document->get_css_files()) {
		HTMLAssetResource asset;
		String error;
		if (HTMLGodotAssetProvider::load_asset(document, css_file, asset, &error) != OK) {
			ERR_PRINT(error);
			return false;
		}
		r_css += "\n" + String::utf8(reinterpret_cast<const char *>(asset.bytes.ptr()), asset.bytes.size()) + "\n";
	}
	r_css += document->get_css();
	return true;
}

void HTMLSurfaceHCSRBackend::_report_compilation(hcsr_runtime_compilation_report_t *p_report) {
	if (p_report == nullptr) {
		return;
	}

	hcsr_runtime_compilation_report_info_t report_info;
	_initialize_abi(report_info);
	if (hcsr_runtime_compilation_report_get_info(p_report, &report_info) != HCSR_RUNTIME_OK) {
		ERR_PRINT("HCSR RuntimeSession compilation report could not be inspected.");
		return;
	}

	for (int index = 0; index < report_info.diagnostic_count; index++) {
		hcsr_runtime_compilation_diagnostic_info_t diagnostic;
		_initialize_abi(diagnostic);
		if (hcsr_runtime_compilation_report_get_diagnostic(p_report, index, &diagnostic) != HCSR_RUNTIME_OK) {
			continue;
		}
		const String code = copy_report_text(p_report, index, false, diagnostic.code_utf8_bytes);
		const String message = copy_report_text(p_report, index, true, diagnostic.message_utf8_bytes);
		const String formatted = vformat("HCSR %s at %d:%d: %s", code, diagnostic.line, diagnostic.column, message);
		if (diagnostic.severity == HCSR_RUNTIME_DIAGNOSTIC_ERROR) {
			ERR_PRINT(formatted);
		} else {
			WARN_PRINT(formatted);
		}
	}

	for (int index = 0; index < report_info.resource_intent_count; index++) {
		hcsr_runtime_compilation_resource_info_t resource;
		_initialize_abi(resource);
		if (hcsr_runtime_compilation_report_get_resource(p_report, index, &resource) != HCSR_RUNTIME_OK || resource.reference_utf8_bytes <= 1) {
			continue;
		}
		Vector<char> reference;
		reference.resize(resource.reference_utf8_bytes);
		if (hcsr_runtime_compilation_report_copy_resource_reference(p_report, index, reference.ptrw(), reference.size()) == HCSR_RUNTIME_OK) {
			WARN_PRINT(vformat(
					"HCSR RuntimeSession resource intent kind %d for '%s' is unresolved by the CPU integration slice.",
					(int)resource.kind,
					String::utf8(reference.ptr(), reference.size() - 1)));
		}
	}
}

bool HTMLSurfaceHCSRBackend::_compile_document() {
	if (document.is_null()) {
		document_dirty = false;
		return false;
	}
	if (!document_dirty) {
		return compiled_document != nullptr;
	}
	document_dirty = false;

	String html;
	String css;
	if (!_load_document_source(html, css)) {
		_set_terminal_failure("HCSR RuntimeSession could not load the HTMLDocument source.");
		return false;
	}

	const CharString html_utf8 = html.utf8();
	const CharString css_utf8 = css.utf8();
	hcsr_runtime_document_t *candidate_document = nullptr;
	hcsr_runtime_compilation_report_t *report = nullptr;
	const hcsr_runtime_status_t status = hcsr_runtime_document_compile(
			html_utf8.ptr(),
			css_utf8.ptr(),
			&candidate_document,
			&report);
	_report_compilation(report);
	if (report != nullptr) {
		hcsr_runtime_compilation_report_release(report);
	}
	if (status != HCSR_RUNTIME_OK || candidate_document == nullptr) {
		_set_terminal_failure("HCSR RuntimeSession rejected the HTMLDocument.");
		return false;
	}

	hcsr_runtime_document_t *previous_document = compiled_document;
	compiled_document = candidate_document;
	terminal_failure = false;
	terminal_failure_reason = String();

	if (session != nullptr) {
		const uint64_t request_id = next_document_request_id++;
		if (hcsr_runtime_session_submit_document(session, request_id, compiled_document) != HCSR_RUNTIME_OK) {
			compiled_document = previous_document;
			hcsr_runtime_document_release(candidate_document);
			_set_terminal_failure("HCSR RuntimeSession rejected the compiled document submission.");
			return false;
		}
		queued_generation++;
		author_submission_generation++;
		derivation_pending = true;
	}
	if (previous_document != nullptr) {
		hcsr_runtime_document_release(previous_document);
	}
	return true;
}

void HTMLSurfaceHCSRBackend::_begin_session_retirement() {
	if (session == nullptr) {
		return;
	}
	_release_presentation_candidate(false);
	HCSRSessionRetirementService::enqueue(session);
	session = nullptr;
	derivation_pending = false;
	publication_probe_pending = false;
	publication_cursor_runtime_generation = 0;
	visible_runtime_generation = 0;
	standby_runtime_generation = 0;
	pending_interactive_submission_id = 0;
	pending_interactive_author_revision = 0;
	pending_interactive_frame_id = 0;
	pending_interactive_activation_cutoff_usec = 0;
	pending_interactive_step = false;
}

bool HTMLSurfaceHCSRBackend::_recreate_session() {
	if (!session_configuration_dirty && session != nullptr) {
		return true;
	}

	Vector<hcsr_runtime_output_configuration_t> outputs;
	outputs.resize(1 + presentation_outputs.size());
	_initialize_abi(outputs.write[0]);
	outputs.write[0].output_id = 1;
	outputs.write[0].pixel_width = MAX(1, physical_size.x);
	outputs.write[0].pixel_height = MAX(1, physical_size.y);
	outputs.write[0].logical_width = MAX(1, size.x);
	outputs.write[0].logical_height = MAX(1, size.y);
	outputs.write[0].tile_size = HCSR_RUNTIME_TILE_SIZE;

	int output_index = 1;
	for (const KeyValue<uint64_t, OutputState *> &entry : presentation_outputs) {
		_initialize_abi(outputs.write[output_index]);
		outputs.write[output_index].output_id = entry.value->runtime_output_id;
		outputs.write[output_index].pixel_width = MAX(1, entry.value->requested_size.x);
		outputs.write[output_index].pixel_height = MAX(1, entry.value->requested_size.y);
		outputs.write[output_index].logical_width = MAX(1, size.x);
		outputs.write[output_index].logical_height = MAX(1, size.y);
		outputs.write[output_index].tile_size = HCSR_RUNTIME_TILE_SIZE;
		output_index++;
	}

	if (session != nullptr) {
		_release_presentation_candidate(false);
		pending_interactive_submission_id = 0;
		pending_interactive_author_revision = 0;
		pending_interactive_frame_id = 0;
		pending_interactive_activation_cutoff_usec = 0;
		pending_interactive_step = false;
		const hcsr_runtime_status_t configuration_status = hcsr_runtime_session_submit_configuration(
				session,
				MAX(1, size.x),
				MAX(1, size.y),
				outputs.ptr(),
				outputs.size());
		if (configuration_status != HCSR_RUNTIME_OK) {
			_set_terminal_failure(vformat("HCSR RuntimeSession reconfiguration failed with status %d.", (int)configuration_status));
			return false;
		}
		session_configuration_dirty = false;
		queued_generation++;
		derivation_pending = true;
		publication_probe_pending = false;
		return true;
	}

	const hcsr_runtime_status_t create_status = hcsr_runtime_session_create(
			MAX(1, size.x),
			MAX(1, size.y),
			outputs.ptr(),
			outputs.size(),
			&session);
	if (create_status != HCSR_RUNTIME_OK || session == nullptr) {
		_set_terminal_failure(vformat("HCSR RuntimeSession creation failed with status %d.", (int)create_status));
		return false;
	}

	session_configuration_dirty = false;
	publication_cursor_runtime_generation = 0;
	visible_runtime_generation = 0;
	standby_runtime_generation = 0;
	if (compiled_document != nullptr) {
		const uint64_t request_id = next_document_request_id++;
		if (hcsr_runtime_session_submit_document(session, request_id, compiled_document) != HCSR_RUNTIME_OK) {
			_set_terminal_failure("HCSR RuntimeSession rejected the initial document submission.");
			return false;
		}
		queued_generation++;
		author_submission_generation++;
		derivation_pending = true;
	}
	return true;
}

bool HTMLSurfaceHCSRBackend::_step_session(uint64_t p_budget_usec) {
	if (session == nullptr || !derivation_pending) {
		HCSRFrameBudgetService::set_semantic_pending((uint64_t)this, false);
		return false;
	}
	HCSRFrameBudgetService::set_semantic_pending((uint64_t)this, true);
	if (pending_interactive_step) {
		const uint64_t interactive_budget_usec = HCSRFrameBudgetService::claim_interactive_semantic((uint64_t)this, 8000);
		if (interactive_budget_usec == 0) {
			return false;
		}
		const uint64_t start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
		hcsr_runtime_interactive_step_info_t interactive_info;
		_initialize_abi(interactive_info);
		const hcsr_runtime_status_t interactive_status = hcsr_runtime_session_step_interactive(
				session,
				INT32_MAX,
				&interactive_info);
		const uint64_t elapsed_usec = start_usec == 0 ? 0 : OS::get_singleton()->get_ticks_usec() - start_usec;
		HCSRFrameBudgetService::consume_interactive_semantic((uint64_t)this, elapsed_usec);
		last_step_milliseconds = elapsed_usec / 1000.0;
		last_step_work_units = interactive_info.work_units;
		last_interactive_outcome = interactive_info.outcome;
		if (interactive_info.submission_id != pending_interactive_submission_id
				|| interactive_info.target_author_revision != pending_interactive_author_revision
				|| interactive_info.frame_id != pending_interactive_frame_id) {
			_set_terminal_failure("HCSR RuntimeSession interactive result did not match its submitted author authority.");
			derivation_pending = false;
			pending_interactive_step = false;
			return false;
		}
		if (interactive_status == HCSR_RUNTIME_OK
				&& interactive_info.outcome == HCSR_RUNTIME_INTERACTIVE_CPU_PUBLISHED_BEFORE_CUTOFF) {
			derivation_pending = false;
			pending_interactive_step = false;
			publication_probe_pending = true;
			HCSRFrameBudgetService::set_semantic_pending((uint64_t)this, false);
			return true;
		}
		if (interactive_status != HCSR_RUNTIME_OK) {
			_set_terminal_failure(vformat("HCSR RuntimeSession interactive step failed with status %d.", (int)interactive_status));
			derivation_pending = false;
		} else {
			// A missed or structural interactive candidate remains valid normal-priority work.
			derivation_pending = true;
		}
		pending_interactive_step = false;
		HCSRFrameBudgetService::set_semantic_pending((uint64_t)this, derivation_pending);
		return false;
	}
	const uint64_t allowed_budget_usec = HCSRFrameBudgetService::claim_semantic((uint64_t)this, p_budget_usec);
	if (allowed_budget_usec == 0) {
		return false;
	}
	const uint64_t start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	last_step_work_units = 0;
	bool completed = false;
	uint32_t step_count = 0;
	do {
		hcsr_runtime_step_info_t info;
		_initialize_abi(info);
		const hcsr_runtime_status_t status = hcsr_runtime_session_step(session, 1, &info);
		last_step_work_units += info.work_units;
		if (status == HCSR_RUNTIME_OK) {
			derivation_pending = false;
			publication_probe_pending = true;
			completed = true;
			break;
		}
		if (status != HCSR_RUNTIME_PENDING) {
			_set_terminal_failure(vformat("HCSR RuntimeSession frame step failed with status %d.", (int)status));
			derivation_pending = false;
			break;
		}
		step_count++;
	} while ((start_usec != 0 && OS::get_singleton()->get_ticks_usec() - start_usec < allowed_budget_usec) || (start_usec == 0 && step_count < 1024));
	const uint64_t elapsed_usec = start_usec == 0 ? 0 : OS::get_singleton()->get_ticks_usec() - start_usec;
	HCSRFrameBudgetService::consume_semantic((uint64_t)this, elapsed_usec);
	HCSRFrameBudgetService::set_semantic_pending((uint64_t)this, derivation_pending);
	last_step_milliseconds = elapsed_usec / 1000.0;
	return completed;
}

bool HTMLSurfaceHCSRBackend::_begin_presentation_candidate() {
	ERR_FAIL_COND_V(presentation_candidate != nullptr, true);
	if (session == nullptr) {
		return false;
	}
	hcsr_runtime_publication_t *publication = nullptr;
	hcsr_runtime_publication_info_t publication_info;
	_initialize_abi(publication_info);
	const hcsr_runtime_status_t status = hcsr_runtime_session_acquire_publication(
			session,
			publication_cursor_runtime_generation,
			&publication,
			&publication_info);
	if (status == HCSR_RUNTIME_NO_NEW_PUBLICATION) {
		publication_probe_pending = false;
		return false;
	}
	if (status != HCSR_RUNTIME_OK || publication == nullptr || publication_info.output_count <= 0) {
		_set_terminal_failure(vformat("HCSR RuntimeSession publication acquisition failed with status %d.", (int)status));
		return false;
	}
	presentation_candidate = memnew(PresentationCandidate);
	publication_probe_pending = false;
	presentation_candidate->publication = publication;
	presentation_candidate->publication_info = publication_info;
	presentation_candidate->author_submission_generation = author_submission_generation;
	presentation_candidate->configuration_generation = configuration_generation;
	presentation_candidate->staging_base_runtime_generation = standby_runtime_generation;
	presentation_candidate->outputs.resize(publication_info.output_count);
	last_presentation_slice_milliseconds = 0.0;
	last_texture_upload_bytes = 0;
	staged_tile_count = 0;
	last_tile_copy_milliseconds = 0.0;
	last_texture_upload_milliseconds = 0.0;
	primary_copied_tile_bytes = 0;
	for (const KeyValue<uint64_t, OutputState *> &entry : presentation_outputs) {
		entry.value->copied_tile_bytes = 0;
	}
	return true;
}

bool HTMLSurfaceHCSRBackend::_copy_candidate_tile(PresentationOutputCandidate &p_output) {
	if (p_output.next_tile >= p_output.frame_info.changed_tile_count) {
		return true;
	}
	const uint64_t copy_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	hcsr_runtime_changed_tile_info_t tile;
	_initialize_abi(tile);
	if (hcsr_runtime_frame_get_changed_tile(
			p_output.frame,
			p_output.frame_info.runtime_generation,
			p_output.next_tile,
			&tile) != HCSR_RUNTIME_OK) {
		return false;
	}
	PackedByteArray bgra_tile;
	bgra_tile.resize(tile.minimum_destination_stride_bytes * tile.pixel_height);
	if (hcsr_runtime_frame_copy_changed_tile(
			p_output.frame,
			p_output.frame_info.runtime_generation,
			p_output.next_tile,
			bgra_tile.ptrw(),
			bgra_tile.size(),
			tile.minimum_destination_stride_bytes) != HCSR_RUNTIME_OK) {
		return false;
	}
	PackedByteArray rgba_tile;
	rgba_tile.resize(tile.pixel_width * tile.pixel_height * 4);
	const uint8_t *source = bgra_tile.ptr();
	uint8_t *destination = rgba_tile.ptrw();
	for (int y = 0; y < tile.pixel_height; y++) {
		for (int x = 0; x < tile.pixel_width; x++) {
			const int source_offset = y * tile.minimum_destination_stride_bytes + x * 4;
			const int destination_offset = (y * tile.pixel_width + x) * 4;
			destination[destination_offset + 0] = source[source_offset + 2];
			destination[destination_offset + 1] = source[source_offset + 1];
			destination[destination_offset + 2] = source[source_offset + 0];
			destination[destination_offset + 3] = source[source_offset + 3];
		}
	}
	Ref<Image> image = Image::create_from_data(tile.pixel_width, tile.pixel_height, false, Image::FORMAT_RGBA8, rgba_tile);
	const uint64_t upload_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	p_output.target_texture->update_candidate_region(Rect2i(tile.pixel_x, tile.pixel_y, tile.pixel_width, tile.pixel_height), image);
	const uint64_t tile_bytes = uint64_t(tile.pixel_width) * tile.pixel_height * 4;
	if (!presentation_candidate->activated) {
		uint64_t &copied_bytes = p_output.primary ? primary_copied_tile_bytes : p_output.output_state->copied_tile_bytes;
		copied_bytes += tile_bytes;
		staged_tile_count++;
	}
	last_texture_upload_bytes += tile_bytes;
	p_output.next_tile++;
	if (copy_start_usec != 0) {
		last_tile_copy_milliseconds += (upload_start_usec - copy_start_usec) / 1000.0;
		last_texture_upload_milliseconds += (OS::get_singleton()->get_ticks_usec() - upload_start_usec) / 1000.0;
	}
	return true;
}

bool HTMLSurfaceHCSRBackend::_advance_presentation_candidate() {
	if (presentation_candidate == nullptr && !_begin_presentation_candidate()) {
		return false;
	}
	PresentationCandidate &candidate = *presentation_candidate;
	if (candidate.next_output_to_acquire < candidate.outputs.size()) {
		const int output_index = candidate.next_output_to_acquire;
		hcsr_runtime_output_info_t output_info;
		_initialize_abi(output_info);
		if (hcsr_runtime_publication_get_output(candidate.publication, candidate.publication_info.generation, output_index, &output_info) != HCSR_RUNTIME_OK) {
			_set_terminal_failure("HCSR RuntimeSession could not inspect a presentation output.");
			_release_presentation_candidate(false);
			return false;
		}
		PresentationOutputCandidate &output = candidate.outputs.write[output_index];
		output.publication_output_index = output_index;
		output.primary = output_info.output_id == 1;
		if (output.primary) {
			output.target_texture = texture;
		} else {
			for (const KeyValue<uint64_t, OutputState *> &entry : presentation_outputs) {
				if (entry.value->runtime_output_id == output_info.output_id) {
					output.output_state = entry.value;
					output.target_texture = entry.value->texture;
					break;
				}
			}
		}
		_initialize_abi(output.frame_info);
		if (output.target_texture.is_null()
				|| hcsr_runtime_publication_acquire_output_frame_for_base(
						candidate.publication,
						candidate.publication_info.generation,
						output_index,
						candidate.staging_base_runtime_generation,
						&output.frame,
						&output.frame_info) != HCSR_RUNTIME_OK
				|| output.frame == nullptr
				|| output.frame_info.pixel_format != HCSR_RUNTIME_PIXEL_FORMAT_BGRA32_STRAIGHT_SRGB) {
			_set_terminal_failure("HCSR RuntimeSession could not stage a complete presentation output.");
			_release_presentation_candidate(false);
			return false;
		}
		const uint64_t initialization_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
		output.target_texture->begin_region_candidate(Size2i(output.frame_info.pixel_width, output.frame_info.pixel_height), true);
		if (initialization_start_usec != 0) {
			last_texture_upload_milliseconds += (OS::get_singleton()->get_ticks_usec() - initialization_start_usec) / 1000.0;
		}
		candidate.next_output_to_acquire++;
		return false;
	}

	if (!candidate.activated) {
		while (candidate.active_output < candidate.outputs.size()) {
			PresentationOutputCandidate &output = candidate.outputs.write[candidate.active_output];
			if (output.next_tile < output.frame_info.changed_tile_count) {
				const String failure_output = OS::get_singleton() != nullptr ? OS::get_singleton()->get_environment("HCSR_RUNTIME_TEST_FAIL_PRESENTATION_OUTPUT") : String();
				if (!failure_output.is_empty() && failure_output.to_int() == candidate.active_output && output.next_tile == 0) {
					publication_cursor_runtime_generation = candidate.publication_info.generation;
					standby_runtime_generation = 0;
					_release_presentation_candidate(false);
					return false;
				}
				if (!_copy_candidate_tile(output)) {
					_set_terminal_failure("HCSR RuntimeSession changed-tile staging failed.");
					_release_presentation_candidate(false);
				}
				return false;
			}
			candidate.active_output++;
		}
		if (candidate.configuration_generation != configuration_generation) {
			_release_presentation_candidate(false);
			return false;
		}
		if (candidate.author_submission_generation != author_submission_generation) {
			publication_cursor_runtime_generation = candidate.publication_info.generation;
			standby_runtime_generation = candidate.publication_info.generation;
			_release_presentation_candidate(true);
			_publish_metrics();
			return false;
		}
		PresentationOutputCandidate *primary = nullptr;
		for (PresentationOutputCandidate &output : candidate.outputs) {
			if (output.primary) {
				primary = &output;
				break;
			}
		}
		if (primary == nullptr) {
			_set_terminal_failure("HCSR RuntimeSession publication omitted its primary output.");
			_release_presentation_candidate(false);
			return false;
		}
		candidate.sync_base_runtime_generation = visible_runtime_generation;
		const uint64_t visible_generation = next_activation_generation++;
		bool interactive_presented_same_frame = false;
		if (candidate.publication_info.interactive_submission_id != 0) {
			const Engine *engine = Engine::get_singleton();
			const OS *os = OS::get_singleton();
			const bool exact_lineage = candidate.publication_info.interactive_submission_id == pending_interactive_submission_id
					&& candidate.publication_info.target_author_revision == pending_interactive_author_revision
					&& candidate.publication_info.interactive_frame_id == pending_interactive_frame_id;
			if (!exact_lineage) {
				// A newer interactive submission may supersede a publication after it
				// was acquired. Retain it only as the next staging base; never expose it.
				publication_cursor_runtime_generation = candidate.publication_info.generation;
				standby_runtime_generation = candidate.publication_info.generation;
				_release_presentation_candidate(true);
				_publish_metrics();
				return false;
			}
			interactive_presented_same_frame = engine != nullptr
					&& engine->get_process_frames() == pending_interactive_frame_id
					&& os != nullptr
					&& os->get_ticks_usec() <= pending_interactive_activation_cutoff_usec;
		}
		for (PresentationOutputCandidate &output : candidate.outputs) {
			if (output.frame != nullptr) {
				if (hcsr_runtime_frame_release(candidate.publication, output.frame) != HCSR_RUNTIME_OK) {
					_set_terminal_failure("HCSR RuntimeSession could not release a staged output frame.");
					_release_presentation_candidate(false);
					return false;
				}
				output.frame = nullptr;
			}
			output.next_tile = 0;
		}
		for (PresentationOutputCandidate &output : candidate.outputs) {
			output.target_texture->activate_region_candidate(false);
			if (!output.primary) {
				output.output_state->generation = visible_generation;
			}
		}
		frame_metadata.logical_size = size;
		frame_metadata.physical_size = Size2i(primary->frame_info.pixel_width, primary->frame_info.pixel_height);
		frame_metadata.device_scale_factor = device_scale_factor;
		frame_metadata.generation = visible_generation;
		frame_metadata.host_frame_number = Engine::get_singleton() != nullptr ? Engine::get_singleton()->get_process_frames() : 0;
		frame_metadata.hits.clear();
		frame_metadata.backdrop_filter_regions.clear();
		active_generation = visible_generation;
		visible_runtime_generation = candidate.publication_info.generation;
		if (candidate.publication_info.interactive_submission_id != 0) {
			last_interactive_outcome = interactive_presented_same_frame
					? HCSR_RUNTIME_INTERACTIVE_CPU_PUBLISHED_BEFORE_CUTOFF
					: HCSR_RUNTIME_INTERACTIVE_MISSED_CUTOFF;
			pending_interactive_submission_id = 0;
			pending_interactive_author_revision = 0;
			pending_interactive_frame_id = 0;
			pending_interactive_activation_cutoff_usec = 0;
		}
		candidate.activated = true;
		candidate.active_output = 0;
		for (PresentationOutputCandidate &output : candidate.outputs) {
			output.target_texture->notify_region_candidate_activation();
		}
		_publish_metrics();
		return true;
	}

	while (candidate.active_output < candidate.outputs.size()) {
		PresentationOutputCandidate &output = candidate.outputs.write[candidate.active_output];
		if (!output.sync_initialized) {
			_initialize_abi(output.frame_info);
			if (hcsr_runtime_publication_acquire_output_frame_for_base(
					candidate.publication,
					candidate.publication_info.generation,
					output.publication_output_index,
					candidate.sync_base_runtime_generation,
					&output.frame,
					&output.frame_info) != HCSR_RUNTIME_OK
					|| output.frame == nullptr) {
				_set_terminal_failure("HCSR RuntimeSession could not acquire standby synchronization authority.");
				_release_presentation_candidate(false);
				return false;
			}
			const uint64_t initialization_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
			output.target_texture->begin_region_candidate(Size2i(output.frame_info.pixel_width, output.frame_info.pixel_height), true);
			if (initialization_start_usec != 0) {
				last_texture_upload_milliseconds += (OS::get_singleton()->get_ticks_usec() - initialization_start_usec) / 1000.0;
			}
			output.sync_initialized = true;
			return false;
		}
		if (output.next_tile < output.frame_info.changed_tile_count) {
			if (!_copy_candidate_tile(output)) {
				_set_terminal_failure("HCSR RuntimeSession standby-texture synchronization failed.");
				_release_presentation_candidate(false);
			}
			return false;
		}
		candidate.active_output++;
	}
	publication_cursor_runtime_generation = candidate.publication_info.generation;
	standby_runtime_generation = candidate.publication_info.generation;
	_release_presentation_candidate(true);
	_publish_metrics();
	return false;
}

void HTMLSurfaceHCSRBackend::_release_presentation_candidate(bool p_keep_standby) {
	if (presentation_candidate == nullptr) {
		return;
	}
	bool release_succeeded = true;
	for (PresentationOutputCandidate &output : presentation_candidate->outputs) {
		if (output.frame != nullptr) {
			release_succeeded = hcsr_runtime_frame_release(presentation_candidate->publication, output.frame) == HCSR_RUNTIME_OK && release_succeeded;
			output.frame = nullptr;
		}
		if (!p_keep_standby && output.target_texture.is_valid()) {
			output.target_texture->cancel_region_candidate();
		}
	}
	if (!p_keep_standby) {
		standby_runtime_generation = 0;
	}
	if (presentation_candidate->publication != nullptr && session != nullptr) {
		release_succeeded = hcsr_runtime_publication_release(session, presentation_candidate->publication) == HCSR_RUNTIME_OK && release_succeeded;
	}
	memdelete(presentation_candidate);
	presentation_candidate = nullptr;
	if (!release_succeeded) {
		_set_terminal_failure("HCSR RuntimeSession could not release its presentation candidate leases.");
	}
}

bool HTMLSurfaceHCSRBackend::_consume_publication() {
	HCSRFrameBudgetService::set_presentation_pending((uint64_t)this, true);
	if (presentation_candidate == nullptr && publication_probe_pending) {
		_begin_presentation_candidate();
	}
	const bool interactive = presentation_candidate != nullptr
			&& presentation_candidate->publication_info.interactive_submission_id != 0;
	const uint64_t allowed_budget_usec = interactive
			? HCSRFrameBudgetService::claim_interactive_presentation((uint64_t)this, 6000)
			: HCSRFrameBudgetService::claim_presentation((uint64_t)this, 1000);
	if (allowed_budget_usec == 0) {
		return false;
	}
	const uint64_t start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	bool changed = false;
	uint32_t work_units = 0;
	uint32_t test_work_unit_limit = UINT32_MAX;
	if (OS::get_singleton() != nullptr) {
		const String configured_limit = OS::get_singleton()->get_environment("HCSR_RUNTIME_TEST_PRESENTATION_UNIT_LIMIT");
		if (!configured_limit.is_empty()) {
			test_work_unit_limit = MAX(1, configured_limit.to_int());
		}
	}
	do {
		changed = _advance_presentation_candidate();
		work_units++;
		if (changed || (presentation_candidate == nullptr && !publication_probe_pending)) {
			break;
		}
	} while (work_units < test_work_unit_limit
			&& ((start_usec != 0 && OS::get_singleton()->get_ticks_usec() - start_usec < allowed_budget_usec) || (start_usec == 0 && work_units < 1024)));
	const uint64_t elapsed_usec = start_usec == 0 ? 0 : OS::get_singleton()->get_ticks_usec() - start_usec;
	if (interactive) {
		HCSRFrameBudgetService::consume_interactive_presentation((uint64_t)this, elapsed_usec);
	} else {
		HCSRFrameBudgetService::consume_presentation((uint64_t)this, elapsed_usec);
	}
	HCSRFrameBudgetService::set_presentation_pending((uint64_t)this, presentation_candidate != nullptr || publication_probe_pending);
	const double elapsed_milliseconds = elapsed_usec / 1000.0;
	last_presentation_slice_milliseconds = MAX(last_presentation_slice_milliseconds, elapsed_milliseconds);
	return changed;
}

void HTMLSurfaceHCSRBackend::_publish_metrics() {
	HCSRPerformanceMonitor::IntegrationCounters counters;
	counters.cpu_primary_publication_milliseconds = last_step_milliseconds;
	counters.cpu_primary_conversion_milliseconds = last_tile_copy_milliseconds;
	counters.cpu_primary_upload_milliseconds = last_texture_upload_milliseconds;
	counters.runtime_session_step_milliseconds = last_step_milliseconds;
	counters.runtime_presentation_slice_milliseconds = last_presentation_slice_milliseconds;
	counters.runtime_session_work_units = last_step_work_units;
	counters.runtime_changed_tile_bytes = primary_copied_tile_bytes;
	for (const KeyValue<uint64_t, OutputState *> &entry : presentation_outputs) {
		counters.runtime_changed_tile_bytes += entry.value->copied_tile_bytes;
	}
	counters.runtime_texture_upload_bytes = last_texture_upload_bytes;
	counters.runtime_staged_tiles = staged_tile_count;
	counters.runtime_retiring_sessions = HCSRSessionRetirementService::pending_count();
	HCSRPerformanceMonitor::update_integration((uint64_t)this, counters);
}

Error HTMLSurfaceHCSRBackend::_submit_attribute_mutations(const Array &p_mutations) {
	if (p_mutations.is_empty()) {
		return OK;
	}
	if (!_compile_document() || !_recreate_session() || session == nullptr) {
		return ERR_UNAVAILABLE;
	}

	hcsr_runtime_mutation_t *mutation = nullptr;
	if (hcsr_runtime_mutation_begin(session, &mutation) != HCSR_RUNTIME_OK || mutation == nullptr) {
		return FAILED;
	}
	bool valid = true;
	for (int mutation_index = 0; mutation_index < p_mutations.size(); mutation_index++) {
		if (p_mutations[mutation_index].get_type() != Variant::DICTIONARY) {
			valid = false;
			break;
		}
		const Dictionary entry = p_mutations[mutation_index];
		const String operation = entry.get("operation", String());
		const String id = entry.get("id", String());
		const String name = entry.get("name", String());
		const String value = entry.get("value", String());
		if (operation != "set_attribute" || id.is_empty() || name.is_empty()) {
			valid = false;
			break;
		}
		const CharString id_utf8 = id.utf8();
		const CharString name_utf8 = name.utf8();
		const CharString value_utf8 = value.utf8();
		if (hcsr_runtime_mutation_set_attribute_by_id(mutation, id_utf8.ptr(), name_utf8.ptr(), value_utf8.ptr()) != HCSR_RUNTIME_OK) {
			valid = false;
			break;
		}
	}
	if (!valid) {
		hcsr_runtime_mutation_release(mutation);
		return ERR_INVALID_PARAMETER;
	}

	hcsr_runtime_submission_info_t submission_info;
	_initialize_abi(submission_info);
	const hcsr_runtime_status_t submit_status = hcsr_runtime_session_submit_mutation_with_priority(
			session,
			mutation,
			HCSR_RUNTIME_MUTATION_PRIORITY_INTERACTIVE,
			Engine::get_singleton() != nullptr ? Engine::get_singleton()->get_process_frames() : 0,
			hcsr_runtime_get_monotonic_timestamp_microseconds() + 8000,
			&submission_info);
	if (submit_status != HCSR_RUNTIME_OK) {
		hcsr_runtime_mutation_release(mutation);
		return submit_status == HCSR_RUNTIME_LIMIT_EXCEEDED ? ERR_OUT_OF_MEMORY : FAILED;
	}
	queued_generation++;
	author_submission_generation++;
	pending_interactive_submission_id = submission_info.submission_id;
	pending_interactive_author_revision = submission_info.target_author_revision;
	pending_interactive_frame_id = submission_info.frame_id;
	pending_interactive_activation_cutoff_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() + 12000 : 0;
	pending_interactive_step = true;
	derivation_pending = true;
	return OK;
}

Error HTMLSurfaceHCSRBackend::_submit_attribute_mutation(const StringName &p_id, const StringName &p_name, const String &p_value) {
	Dictionary mutation;
	mutation["operation"] = "set_attribute";
	mutation["id"] = String(p_id);
	mutation["name"] = String(p_name);
	mutation["value"] = p_value;
	Array mutations;
	mutations.push_back(mutation);
	return _submit_attribute_mutations(mutations);
}

void HTMLSurfaceHCSRBackend::mark_document_dirty() {
	document_dirty = true;
}

void HTMLSurfaceHCSRBackend::set_size(const Size2i &p_size) {
	const Size2i new_size(MAX(1, p_size.x), MAX(1, p_size.y));
	if (size == new_size) {
		return;
	}
	size = new_size;
	session_configuration_dirty = true;
	configuration_generation++;
}

void HTMLSurfaceHCSRBackend::set_device_scale_factor(float p_device_scale_factor) {
	const float new_scale = CLAMP(p_device_scale_factor, 0.01f, 8.0f);
	if (Math::is_equal_approx(device_scale_factor, new_scale)) {
		return;
	}
	device_scale_factor = new_scale;
	session_configuration_dirty = true;
	configuration_generation++;
}

void HTMLSurfaceHCSRBackend::set_physical_size(const Size2i &p_physical_size) {
	const Size2i new_size(MAX(1, p_physical_size.x), MAX(1, p_physical_size.y));
	if (physical_size == new_size) {
		return;
	}
	physical_size = new_size;
	session_configuration_dirty = true;
	configuration_generation++;
}

void HTMLSurfaceHCSRBackend::set_document(const Ref<HTMLDocument> &p_document) {
	if (document != p_document) {
		document = p_document;
		document_dirty = document.is_valid();
	}
	if (document_dirty && document.is_valid()) {
		_compile_document();
	}
}

void HTMLSurfaceHCSRBackend::set_background_color(const Color &p_background_color) {
	HTMLSurfaceCPUBackend::set_background_color(p_background_color);
}

Error HTMLSurfaceHCSRBackend::update_compositor(double p_timeline_time_seconds, bool *r_needs_output, bool *r_needs_begin_frame) {
	frame_metadata.timeline_time_seconds = p_timeline_time_seconds;
	bool ready = false;
	if (!terminal_failure && document.is_valid() && _compile_document() && _recreate_session()) {
		ready = _step_session(HCSR_RUNTIME_FRAME_STEP_BUDGET_USEC);
	}
	_publish_metrics();
	if (r_needs_output != nullptr) {
		*r_needs_output = ready;
	}
	if (r_needs_begin_frame != nullptr) {
		*r_needs_begin_frame = derivation_pending || presentation_candidate != nullptr || publication_probe_pending;
	}
	return terminal_failure ? FAILED : OK;
}

void HTMLSurfaceHCSRBackend::render_placeholder(const String &p_marker) {
	(void)p_marker;
	if (terminal_failure || document.is_null()) {
		return;
	}
	if (!_compile_document() || !_recreate_session()) {
		return;
	}
	if (derivation_pending) {
		_step_session(HCSR_RUNTIME_FRAME_STEP_BUDGET_USEC);
	}
	_consume_publication();
}

bool HTMLSurfaceHCSRBackend::poll_pending_output(bool *r_waiting_for_completion) {
	bool changed = false;
	if (!terminal_failure && document.is_valid() && _compile_document() && _recreate_session()) {
		if (derivation_pending) {
			_step_session(HCSR_RUNTIME_FRAME_STEP_BUDGET_USEC);
		}
		changed = _consume_publication();
	}
	_publish_metrics();
	if (r_waiting_for_completion != nullptr) {
		*r_waiting_for_completion = derivation_pending || presentation_candidate != nullptr || publication_probe_pending;
	}
	return changed;
}

HTMLPendingOutputState HTMLSurfaceHCSRBackend::consume_pending_output_state() {
	HTMLPendingOutputState state;
	state.presentation_changed = poll_pending_output(&state.producer_blocked);
	state.producer_blocked = false;
	state.retirement_pending = HCSRSessionRetirementService::pending_count() > 0;
	return state;
}

void HTMLSurfaceHCSRBackend::schedule_retirement_service() {
	// Retirement is serviced once per SceneTree frame by the module-owned queue.
}

bool HTMLSurfaceHCSRBackend::has_pending_output() const {
	return derivation_pending || presentation_candidate != nullptr || publication_probe_pending;
}

bool HTMLSurfaceHCSRBackend::has_pending_frame_request() const {
	return derivation_pending || presentation_candidate != nullptr || publication_probe_pending || document_dirty || session_configuration_dirty;
}

uint64_t HTMLSurfaceHCSRBackend::get_last_queued_frame_generation() const {
	return queued_generation;
}

uint64_t HTMLSurfaceHCSRBackend::get_active_frame_generation() const {
	return active_generation;
}

bool HTMLSurfaceHCSRBackend::uses_generation_bound_input() const {
	return true;
}

bool HTMLSurfaceHCSRBackend::has_terminal_render_failure() const {
	return terminal_failure;
}

String HTMLSurfaceHCSRBackend::get_terminal_render_failure_reason() const {
	return terminal_failure_reason;
}

Error HTMLSurfaceHCSRBackend::apply_element_mutations(const Array &p_mutations) {
	return _submit_attribute_mutations(p_mutations);
}

Error HTMLSurfaceHCSRBackend::set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value) {
	return _submit_attribute_mutation(p_id, p_name, p_value);
}

Error HTMLSurfaceHCSRBackend::set_element_style(const StringName &p_id, const String &p_css_text) {
	return _submit_attribute_mutation(p_id, SNAME("style"), p_css_text);
}

void HTMLSurfaceHCSRBackend::get_frame_metadata(HTMLFrameMetadata &r_metadata) const {
	r_metadata = frame_metadata;
}

uint64_t HTMLSurfaceHCSRBackend::create_presentation_output(const Size2i &p_size, bool p_mipmaps) {
	const uint64_t output_id = next_presentation_output_id++;
	ERR_FAIL_COND_V_MSG(output_id > uint64_t(INT32_MAX), 0, "HCSR RuntimeSession exhausted its presentation-output identifier range.");
	OutputState *state = memnew(OutputState);
	state->runtime_output_id = int32_t(output_id);
	state->requested_size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
	state->mipmaps = p_mipmaps;
	state->texture.instantiate();
	presentation_outputs.insert(output_id, state);
	session_configuration_dirty = true;
	configuration_generation++;
	return output_id;
}

Error HTMLSurfaceHCSRBackend::resize_presentation_output(uint64_t p_output_id, const Size2i &p_size) {
	OutputState **state = presentation_outputs.getptr(p_output_id);
	ERR_FAIL_NULL_V(state, ERR_DOES_NOT_EXIST);
	const Size2i new_size(MAX(1, p_size.x), MAX(1, p_size.y));
	if ((*state)->requested_size != new_size) {
		(*state)->requested_size = new_size;
		session_configuration_dirty = true;
		configuration_generation++;
	}
	return OK;
}

void HTMLSurfaceHCSRBackend::destroy_presentation_output(uint64_t p_output_id) {
	OutputState **state = presentation_outputs.getptr(p_output_id);
	if (state == nullptr) {
		return;
	}
	if (presentation_candidate != nullptr) {
		if (presentation_candidate->activated) {
			publication_cursor_runtime_generation = presentation_candidate->publication_info.generation;
		}
		_release_presentation_candidate(false);
	}
	memdelete(*state);
	presentation_outputs.erase(p_output_id);
	session_configuration_dirty = true;
	configuration_generation++;
}

Ref<Texture2D> HTMLSurfaceHCSRBackend::get_presentation_output_texture(uint64_t p_output_id) const {
	const OutputState *const *state = presentation_outputs.getptr(p_output_id);
	if (state == nullptr) {
		return Ref<Texture2D>();
	}
	return (*state)->texture;
}

uint64_t HTMLSurfaceHCSRBackend::get_presentation_output_generation(uint64_t p_output_id) const {
	const OutputState *const *state = presentation_outputs.getptr(p_output_id);
	return state != nullptr ? (*state)->generation : 0;
}

HTMLSurfaceHCSRBackend::HTMLSurfaceHCSRBackend() {
	HCSRFrameBudgetService::register_owner((uint64_t)this);
	if (hcsr_runtime_get_abi_version() != HCSR_RUNTIME_ABI_VERSION) {
		_set_terminal_failure(vformat(
				"HCSR RuntimeSession ABI mismatch: Godot expects %d, library reports %d.",
				HCSR_RUNTIME_ABI_VERSION,
				hcsr_runtime_get_abi_version()));
	}
}

HTMLSurfaceHCSRBackend::~HTMLSurfaceHCSRBackend() {
	HCSRFrameBudgetService::unregister_owner((uint64_t)this);
	_begin_session_retirement();
	if (compiled_document != nullptr) {
		hcsr_runtime_document_release(compiled_document);
		compiled_document = nullptr;
	}
	for (const KeyValue<uint64_t, OutputState *> &entry : presentation_outputs) {
		memdelete(entry.value);
	}
	presentation_outputs.clear();
	HCSRPerformanceMonitor::remove((uint64_t)this);
}

/**************************************************************************/
/*  hcsr_frame_budget_service.cpp                                         */
/**************************************************************************/

#include "hcsr_frame_budget_service.h"

#include "core/config/engine.h"

namespace {
constexpr uint64_t HCSR_SEMANTIC_FRAME_BUDGET_USEC = 2000;
constexpr uint64_t HCSR_PRESENTATION_FRAME_BUDGET_USEC = 2000;
}

uint64_t HCSRFrameBudgetService::process_frame = UINT64_MAX;
uint64_t HCSRFrameBudgetService::semantic_used_usec = 0;
uint64_t HCSRFrameBudgetService::presentation_used_usec = 0;

void HCSRFrameBudgetService::_refresh_frame() {
	const Engine *engine = Engine::get_singleton();
	const uint64_t current_frame = engine != nullptr ? engine->get_process_frames() : 0;
	if (process_frame == current_frame) {
		return;
	}
	process_frame = current_frame;
	semantic_used_usec = 0;
	presentation_used_usec = 0;
}

uint64_t HCSRFrameBudgetService::claim_semantic(uint64_t p_maximum_usec) {
	_refresh_frame();
	return MIN(p_maximum_usec, HCSR_SEMANTIC_FRAME_BUDGET_USEC - MIN(semantic_used_usec, HCSR_SEMANTIC_FRAME_BUDGET_USEC));
}

void HCSRFrameBudgetService::consume_semantic(uint64_t p_elapsed_usec) {
	_refresh_frame();
	semantic_used_usec = MIN(HCSR_SEMANTIC_FRAME_BUDGET_USEC, semantic_used_usec + p_elapsed_usec);
}

uint64_t HCSRFrameBudgetService::claim_presentation(uint64_t p_maximum_usec) {
	_refresh_frame();
	return MIN(p_maximum_usec, HCSR_PRESENTATION_FRAME_BUDGET_USEC - MIN(presentation_used_usec, HCSR_PRESENTATION_FRAME_BUDGET_USEC));
}

void HCSRFrameBudgetService::consume_presentation(uint64_t p_elapsed_usec) {
	_refresh_frame();
	presentation_used_usec = MIN(HCSR_PRESENTATION_FRAME_BUDGET_USEC, presentation_used_usec + p_elapsed_usec);
}

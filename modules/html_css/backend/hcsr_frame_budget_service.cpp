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
int HCSRFrameBudgetService::semantic_cursor = 0;
int HCSRFrameBudgetService::presentation_cursor = 0;
Vector<HCSRFrameBudgetService::OwnerState> HCSRFrameBudgetService::owners;
Vector<uint64_t> HCSRFrameBudgetService::semantic_grants;
Vector<uint64_t> HCSRFrameBudgetService::presentation_grants;

HCSRFrameBudgetService::OwnerState *HCSRFrameBudgetService::_find_owner(uint64_t p_owner) {
	for (OwnerState &owner : owners) {
		if (owner.id == p_owner) {
			return &owner;
		}
	}
	return nullptr;
}

bool HCSRFrameBudgetService::_has_grant(const Vector<uint64_t> &p_grants, uint64_t p_owner) {
	for (uint64_t owner : p_grants) {
		if (owner == p_owner) {
			return true;
		}
	}
	return false;
}

void HCSRFrameBudgetService::_build_grants(bool p_semantic, Vector<uint64_t> &r_grants, int &r_cursor) {
	r_grants.clear();
	if (owners.is_empty()) {
		r_cursor = 0;
		return;
	}
	r_cursor %= owners.size();
	int inspected = 0;
	int index = r_cursor;
	while (inspected < owners.size() && r_grants.size() < 2) {
		const OwnerState &owner = owners[index];
		if (p_semantic ? owner.semantic_pending : owner.presentation_pending) {
			r_grants.push_back(owner.id);
		}
		index = (index + 1) % owners.size();
		inspected++;
	}
	r_cursor = index;
}

void HCSRFrameBudgetService::_refresh_frame() {
	const Engine *engine = Engine::get_singleton();
	const uint64_t current_frame = engine != nullptr ? engine->get_process_frames() : 0;
	if (process_frame == current_frame) {
		return;
	}
	process_frame = current_frame;
	semantic_used_usec = 0;
	presentation_used_usec = 0;
	for (OwnerState &owner : owners) {
		owner.semantic_used_usec = 0;
		owner.presentation_used_usec = 0;
	}
	_build_grants(true, semantic_grants, semantic_cursor);
	_build_grants(false, presentation_grants, presentation_cursor);
}

void HCSRFrameBudgetService::register_owner(uint64_t p_owner) {
	if (p_owner != 0 && _find_owner(p_owner) == nullptr) {
		OwnerState owner;
		owner.id = p_owner;
		owners.push_back(owner);
	}
}

void HCSRFrameBudgetService::unregister_owner(uint64_t p_owner) {
	for (int index = 0; index < owners.size(); index++) {
		if (owners[index].id == p_owner) {
			owners.remove_at(index);
			semantic_cursor = owners.is_empty() ? 0 : semantic_cursor % owners.size();
			presentation_cursor = owners.is_empty() ? 0 : presentation_cursor % owners.size();
			return;
		}
	}
}

void HCSRFrameBudgetService::set_semantic_pending(uint64_t p_owner, bool p_pending) {
	OwnerState *owner = _find_owner(p_owner);
	if (owner == nullptr) {
		return;
	}
	owner->semantic_pending = p_pending;
	_refresh_frame();
}

void HCSRFrameBudgetService::set_presentation_pending(uint64_t p_owner, bool p_pending) {
	OwnerState *owner = _find_owner(p_owner);
	if (owner == nullptr) {
		return;
	}
	owner->presentation_pending = p_pending;
	_refresh_frame();
}

uint64_t HCSRFrameBudgetService::claim_semantic(uint64_t p_owner, uint64_t p_maximum_usec) {
	_refresh_frame();
	OwnerState *owner = _find_owner(p_owner);
	if (owner == nullptr || !owner->semantic_pending || !_has_grant(semantic_grants, p_owner)) {
		return 0;
	}
	const uint64_t owner_remaining = 1000 - MIN(owner->semantic_used_usec, uint64_t(1000));
	const uint64_t global_remaining = HCSR_SEMANTIC_FRAME_BUDGET_USEC - MIN(semantic_used_usec, HCSR_SEMANTIC_FRAME_BUDGET_USEC);
	return MIN(p_maximum_usec, MIN(owner_remaining, global_remaining));
}

void HCSRFrameBudgetService::consume_semantic(uint64_t p_owner, uint64_t p_elapsed_usec) {
	_refresh_frame();
	semantic_used_usec = MIN(HCSR_SEMANTIC_FRAME_BUDGET_USEC, semantic_used_usec + p_elapsed_usec);
	OwnerState *owner = _find_owner(p_owner);
	if (owner != nullptr) {
		owner->semantic_used_usec = MIN(uint64_t(1000), owner->semantic_used_usec + p_elapsed_usec);
	}
}

uint64_t HCSRFrameBudgetService::claim_presentation(uint64_t p_owner, uint64_t p_maximum_usec) {
	_refresh_frame();
	OwnerState *owner = _find_owner(p_owner);
	if (owner == nullptr || !owner->presentation_pending || !_has_grant(presentation_grants, p_owner)) {
		return 0;
	}
	const uint64_t owner_remaining = 1000 - MIN(owner->presentation_used_usec, uint64_t(1000));
	const uint64_t global_remaining = HCSR_PRESENTATION_FRAME_BUDGET_USEC - MIN(presentation_used_usec, HCSR_PRESENTATION_FRAME_BUDGET_USEC);
	return MIN(p_maximum_usec, MIN(owner_remaining, global_remaining));
}

void HCSRFrameBudgetService::consume_presentation(uint64_t p_owner, uint64_t p_elapsed_usec) {
	_refresh_frame();
	presentation_used_usec = MIN(HCSR_PRESENTATION_FRAME_BUDGET_USEC, presentation_used_usec + p_elapsed_usec);
	OwnerState *owner = _find_owner(p_owner);
	if (owner != nullptr) {
		owner->presentation_used_usec = MIN(uint64_t(1000), owner->presentation_used_usec + p_elapsed_usec);
	}
}

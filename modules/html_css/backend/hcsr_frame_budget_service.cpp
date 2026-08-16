/**************************************************************************/
/*  hcsr_frame_budget_service.cpp                                         */
/**************************************************************************/

#include "hcsr_frame_budget_service.h"

#include "core/config/engine.h"

namespace {
constexpr uint64_t HCSR_SEMANTIC_FRAME_BUDGET_USEC = 2000;
constexpr uint64_t HCSR_PRESENTATION_FRAME_BUDGET_USEC = 2000;
constexpr uint64_t HCSR_INTERACTIVE_FRAME_BUDGET_USEC = 12000;
}

uint64_t HCSRFrameBudgetService::process_frame = UINT64_MAX;
uint64_t HCSRFrameBudgetService::semantic_used_usec = 0;
uint64_t HCSRFrameBudgetService::presentation_used_usec = 0;
uint64_t HCSRFrameBudgetService::interactive_used_usec = 0;
uint64_t HCSRFrameBudgetService::semantic_head = 0;
uint64_t HCSRFrameBudgetService::presentation_head = 0;
uint64_t HCSRFrameBudgetService::semantic_cursor = 0;
uint64_t HCSRFrameBudgetService::presentation_cursor = 0;
HashMap<uint64_t, HCSRFrameBudgetService::OwnerState> HCSRFrameBudgetService::owners;
Vector<uint64_t> HCSRFrameBudgetService::semantic_grants;
Vector<uint64_t> HCSRFrameBudgetService::presentation_grants;
uint64_t HCSRFrameBudgetService::owner_records_inspected = 0;

HCSRFrameBudgetService::OwnerState *HCSRFrameBudgetService::_find_owner(uint64_t p_owner) {
	owner_records_inspected++;
	return owners.getptr(p_owner);
}

bool HCSRFrameBudgetService::_has_grant(const Vector<uint64_t> &p_grants, uint64_t p_owner) {
	for (uint64_t owner : p_grants) {
		if (owner == p_owner) {
			return true;
		}
	}
	return false;
}

void HCSRFrameBudgetService::_build_grants(bool p_semantic, Vector<uint64_t> &r_grants, uint64_t &r_cursor, uint64_t p_head) {
	r_grants.clear();
	if (p_head == 0) {
		r_cursor = 0;
		return;
	}
	uint64_t owner_id = r_cursor != 0 ? r_cursor : p_head;
	while (r_grants.size() < 2) {
		OwnerState *owner = _find_owner(owner_id);
		if (owner == nullptr) {
			break;
		}
		r_grants.push_back(owner_id);
		const uint64_t next = p_semantic ? owner->semantic_next : owner->presentation_next;
		owner_id = next;
		if (owner_id == (r_cursor != 0 ? r_cursor : p_head)) {
			break;
		}
	}
	r_cursor = owner_id;
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
	interactive_used_usec = 0;
	_build_grants(true, semantic_grants, semantic_cursor, semantic_head);
	_build_grants(false, presentation_grants, presentation_cursor, presentation_head);
}

void HCSRFrameBudgetService::register_owner(uint64_t p_owner) {
	if (p_owner != 0 && _find_owner(p_owner) == nullptr) {
		OwnerState owner;
		owner.id = p_owner;
		owners.insert(p_owner, owner);
	}
}

void HCSRFrameBudgetService::unregister_owner(uint64_t p_owner) {
	OwnerState *owner = _find_owner(p_owner);
	if (owner == nullptr) {
		return;
	}
	_set_pending(p_owner, false, true);
	_set_pending(p_owner, false, false);
	owners.erase(p_owner);
}

void HCSRFrameBudgetService::_set_pending(uint64_t p_owner, bool p_pending, bool p_semantic) {
	OwnerState *owner = _find_owner(p_owner);
	if (owner == nullptr || (p_semantic ? owner->semantic_pending : owner->presentation_pending) == p_pending) {
		return;
	}
	bool &pending = p_semantic ? owner->semantic_pending : owner->presentation_pending;
	uint64_t &previous = p_semantic ? owner->semantic_previous : owner->presentation_previous;
	uint64_t &next = p_semantic ? owner->semantic_next : owner->presentation_next;
	uint64_t &head = p_semantic ? semantic_head : presentation_head;
	uint64_t &cursor = p_semantic ? semantic_cursor : presentation_cursor;
	if (p_pending) {
		if (head == 0) {
			head = p_owner;
			previous = p_owner;
			next = p_owner;
		} else {
			OwnerState *head_owner = _find_owner(head);
			OwnerState *tail_owner = head_owner != nullptr ? _find_owner(p_semantic ? head_owner->semantic_previous : head_owner->presentation_previous) : nullptr;
			if (head_owner == nullptr || tail_owner == nullptr) {
				return;
			}
			previous = tail_owner->id;
			next = head;
			if (p_semantic) {
				tail_owner->semantic_next = p_owner;
				head_owner->semantic_previous = p_owner;
			} else {
				tail_owner->presentation_next = p_owner;
				head_owner->presentation_previous = p_owner;
			}
		}
		pending = true;
	} else {
		if (next == p_owner) {
			head = 0;
			cursor = 0;
		} else {
			OwnerState *previous_owner = _find_owner(previous);
			OwnerState *next_owner = _find_owner(next);
			if (previous_owner != nullptr && next_owner != nullptr) {
				if (p_semantic) {
					previous_owner->semantic_next = next;
					next_owner->semantic_previous = previous;
				} else {
					previous_owner->presentation_next = next;
					next_owner->presentation_previous = previous;
				}
			}
			if (head == p_owner) {
				head = next;
			}
			if (cursor == p_owner) {
				cursor = next;
			}
		}
		previous = 0;
		next = 0;
		pending = false;
	}
}

void HCSRFrameBudgetService::set_semantic_pending(uint64_t p_owner, bool p_pending) {
	_set_pending(p_owner, p_pending, true);
	_refresh_frame();
}

void HCSRFrameBudgetService::set_presentation_pending(uint64_t p_owner, bool p_pending) {
	_set_pending(p_owner, p_pending, false);
	_refresh_frame();
}

uint64_t HCSRFrameBudgetService::claim_semantic(uint64_t p_owner, uint64_t p_maximum_usec) {
	_refresh_frame();
	OwnerState *owner = _find_owner(p_owner);
	if (owner == nullptr || !owner->semantic_pending || !_has_grant(semantic_grants, p_owner)) {
		return 0;
	}
	if (owner->semantic_usage_frame != process_frame) {
		owner->semantic_usage_frame = process_frame;
		owner->semantic_used_usec = 0;
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
		if (owner->semantic_usage_frame != process_frame) {
			owner->semantic_usage_frame = process_frame;
			owner->semantic_used_usec = 0;
		}
		owner->semantic_used_usec = MIN(uint64_t(1000), owner->semantic_used_usec + p_elapsed_usec);
	}
}

uint64_t HCSRFrameBudgetService::claim_presentation(uint64_t p_owner, uint64_t p_maximum_usec) {
	_refresh_frame();
	OwnerState *owner = _find_owner(p_owner);
	if (owner == nullptr || !owner->presentation_pending || !_has_grant(presentation_grants, p_owner)) {
		return 0;
	}
	if (owner->presentation_usage_frame != process_frame) {
		owner->presentation_usage_frame = process_frame;
		owner->presentation_used_usec = 0;
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
		if (owner->presentation_usage_frame != process_frame) {
			owner->presentation_usage_frame = process_frame;
			owner->presentation_used_usec = 0;
		}
		owner->presentation_used_usec = MIN(uint64_t(1000), owner->presentation_used_usec + p_elapsed_usec);
	}
}

uint64_t HCSRFrameBudgetService::claim_interactive_semantic(uint64_t p_owner, uint64_t p_maximum_usec) {
	_refresh_frame();
	OwnerState *owner = _find_owner(p_owner);
	if (owner == nullptr || !owner->semantic_pending) {
		return 0;
	}
	if (owner->interactive_usage_frame != process_frame) {
		owner->interactive_usage_frame = process_frame;
		owner->interactive_used_usec = 0;
	}
	const uint64_t owner_remaining = HCSR_INTERACTIVE_FRAME_BUDGET_USEC - MIN(owner->interactive_used_usec, HCSR_INTERACTIVE_FRAME_BUDGET_USEC);
	const uint64_t global_remaining = HCSR_INTERACTIVE_FRAME_BUDGET_USEC - MIN(interactive_used_usec, HCSR_INTERACTIVE_FRAME_BUDGET_USEC);
	return MIN(p_maximum_usec, MIN(owner_remaining, global_remaining));
}

void HCSRFrameBudgetService::consume_interactive_semantic(uint64_t p_owner, uint64_t p_elapsed_usec) {
	_refresh_frame();
	interactive_used_usec = MIN(HCSR_INTERACTIVE_FRAME_BUDGET_USEC, interactive_used_usec + p_elapsed_usec);
	OwnerState *owner = _find_owner(p_owner);
	if (owner != nullptr) {
		if (owner->interactive_usage_frame != process_frame) {
			owner->interactive_usage_frame = process_frame;
			owner->interactive_used_usec = 0;
		}
		owner->interactive_used_usec = MIN(HCSR_INTERACTIVE_FRAME_BUDGET_USEC, owner->interactive_used_usec + p_elapsed_usec);
	}
}

uint64_t HCSRFrameBudgetService::claim_interactive_presentation(uint64_t p_owner, uint64_t p_maximum_usec) {
	_refresh_frame();
	OwnerState *owner = _find_owner(p_owner);
	if (owner == nullptr || !owner->presentation_pending) {
		return 0;
	}
	if (owner->interactive_usage_frame != process_frame) {
		owner->interactive_usage_frame = process_frame;
		owner->interactive_used_usec = 0;
	}
	const uint64_t owner_remaining = HCSR_INTERACTIVE_FRAME_BUDGET_USEC - MIN(owner->interactive_used_usec, HCSR_INTERACTIVE_FRAME_BUDGET_USEC);
	const uint64_t global_remaining = HCSR_INTERACTIVE_FRAME_BUDGET_USEC - MIN(interactive_used_usec, HCSR_INTERACTIVE_FRAME_BUDGET_USEC);
	return MIN(p_maximum_usec, MIN(owner_remaining, global_remaining));
}

void HCSRFrameBudgetService::consume_interactive_presentation(uint64_t p_owner, uint64_t p_elapsed_usec) {
	_refresh_frame();
	interactive_used_usec = MIN(HCSR_INTERACTIVE_FRAME_BUDGET_USEC, interactive_used_usec + p_elapsed_usec);
	OwnerState *owner = _find_owner(p_owner);
	if (owner != nullptr) {
		if (owner->interactive_usage_frame != process_frame) {
			owner->interactive_usage_frame = process_frame;
			owner->interactive_used_usec = 0;
		}
		owner->interactive_used_usec = MIN(HCSR_INTERACTIVE_FRAME_BUDGET_USEC, owner->interactive_used_usec + p_elapsed_usec);
	}
}

uint64_t HCSRFrameBudgetService::get_owner_records_inspected() {
	return owner_records_inspected;
}

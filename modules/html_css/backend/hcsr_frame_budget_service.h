/**************************************************************************/
/*  hcsr_frame_budget_service.h                                           */
/**************************************************************************/

#pragma once

#include "core/typedefs.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"

class HCSRFrameBudgetService {
	struct OwnerState {
		uint64_t id = 0;
		bool semantic_pending = false;
		bool presentation_pending = false;
		uint64_t semantic_previous = 0;
		uint64_t semantic_next = 0;
		uint64_t presentation_previous = 0;
		uint64_t presentation_next = 0;
		uint64_t semantic_usage_frame = UINT64_MAX;
		uint64_t presentation_usage_frame = UINT64_MAX;
		uint64_t semantic_used_usec = 0;
		uint64_t presentation_used_usec = 0;
	};

	static uint64_t process_frame;
	static uint64_t semantic_used_usec;
	static uint64_t presentation_used_usec;
	static uint64_t semantic_head;
	static uint64_t presentation_head;
	static uint64_t semantic_cursor;
	static uint64_t presentation_cursor;
	static HashMap<uint64_t, OwnerState> owners;
	static Vector<uint64_t> semantic_grants;
	static Vector<uint64_t> presentation_grants;
	static uint64_t owner_records_inspected;

	static void _refresh_frame();
	static void _build_grants(bool p_semantic, Vector<uint64_t> &r_grants, uint64_t &r_cursor, uint64_t p_head);
	static OwnerState *_find_owner(uint64_t p_owner);
	static bool _has_grant(const Vector<uint64_t> &p_grants, uint64_t p_owner);
	static void _set_pending(uint64_t p_owner, bool p_pending, bool p_semantic);

public:
	static void register_owner(uint64_t p_owner);
	static void unregister_owner(uint64_t p_owner);
	static void set_semantic_pending(uint64_t p_owner, bool p_pending);
	static void set_presentation_pending(uint64_t p_owner, bool p_pending);
	static uint64_t claim_semantic(uint64_t p_owner, uint64_t p_maximum_usec);
	static void consume_semantic(uint64_t p_owner, uint64_t p_elapsed_usec);
	static uint64_t claim_presentation(uint64_t p_owner, uint64_t p_maximum_usec);
	static void consume_presentation(uint64_t p_owner, uint64_t p_elapsed_usec);
	static uint64_t get_owner_records_inspected();
};

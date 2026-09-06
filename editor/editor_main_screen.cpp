/**************************************************************************/
/*  editor_main_screen.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "editor_main_screen.h"

#include "core/object/callable_mp.h"
#include "core/object/object.h"
#include "editor/docks/editor_dock.h"
#include "editor/docks/editor_dock_manager.h"
#include "editor/editor_node.h"
#include "editor/plugins/editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/script/script_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"

#ifndef DISABLE_DEPRECATED
void LegacyMainScreenContainer::_force_dock_visible(EditorDock *p_dock, CanvasItem *p_child) {
	if (p_dock->is_visible_in_tree()) {
		p_child->show();
	}
}

void LegacyMainScreenContainer::add_child_notify(Node *p_child) {
	EditorMainScreen *ms = EditorNode::get_editor_main_screen();
	if (!ms->adding_plugin || !ms->adding_plugin->has_main_screen()) {
		return;
	}

	EditorDock *dock = memnew(EditorDock);
	dock->set_default_slot(EditorDock::DOCK_SLOT_MAIN_SCREEN);
	dock->set_available_layouts(EditorDock::DOCK_LAYOUT_MAIN_SCREEN);
	dock->set_title(ms->adding_plugin->get_plugin_name());
	dock->set_dock_icon(ms->adding_plugin->get_plugin_icon());
	dock->set_icon_name(ms->adding_plugin->get_plugin_name());
	EditorDockManager::get_singleton()->add_dock(dock);

	ms->adding_plugin->set_meta("_dock", dock);

	CanvasItem *ci_child = Object::cast_to<CanvasItem>(p_child);
	if (ci_child) {
		ci_child->show();
		dock->connect(SceneStringName(visibility_changed), callable_mp(this, &LegacyMainScreenContainer::_force_dock_visible).bind(dock, ci_child));
	}

	callable_mp(p_child, &Node::reparent).call_deferred(dock, false);
}
#endif

void EditorMainScreen::_on_tab_changed(int p_tab) {
	EditorNode::get_singleton()->update_distraction_free_mode();

	EditorDock *dock = get_dock(p_tab);
	if (!dock) {
		return;
	}
	const String new_main_screen = dock->get_display_title();

	EditorData &editor_data = EditorNode::get_editor_data();
	int plugin_count = editor_data.get_editor_plugin_count();
	for (int i = 0; i < plugin_count; i++) {
		editor_data.get_editor_plugin(i)->notify_main_screen_changed(new_main_screen);
	}
}

void EditorMainScreen::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_POSTINITIALIZE: {
			set_tab_alignment(TabBar::ALIGNMENT_CENTER);
			get_internal_container()->set_alignment(BoxContainer::ALIGNMENT_CENTER);
			connect("tab_changed", callable_mp(this, &EditorMainScreen::_on_tab_changed));
		} break;

		case NOTIFICATION_READY: {
			set_popup(nullptr);

			for (int i = 0; i < get_tab_count(); i++) {
				EditorDock *dock = get_dock(i);
				if (dock == Node3DEditor::get_singleton()) {
					dock->make_visible();
					break;
				}
			}
		} break;
	}
}

DockTabContainer::TabStyle EditorMainScreen::get_tab_style() const {
	return (TabStyle)EDITOR_GET("interface/editor/docks/main_screen_dock_tab_style").operator int();
}

Rect2 EditorMainScreen::get_drag_hint_rect() const {
	const Rect2 tab_rect = get_internal_container()->get_global_rect();
	const Rect2 content_rect = get_global_rect();
	Rect2 final_rect = tab_rect.merge(content_rect);
	float tab_x = get_tab_bar()->get_global_transform().xform(get_tab_bar()->get_tab_rect(0).position).x;
	final_rect.position.x = MIN(tab_x, content_rect.position.x);
	tab_x = get_tab_bar()->get_global_transform().xform(get_tab_bar()->get_tab_rect(-1).get_end()).x;
	final_rect.set_end(Vector2(MAX(tab_x, content_rect.get_end().x), final_rect.get_end().y));
	return final_rect;
}

void EditorMainScreen::edit(Object *p_object) {
	const ObjectID requested_object = p_object ? p_object->get_instance_id() : ObjectID();
	if (edit_in_progress) {
		// Coalesce nested requests, but finish the current deactivation first.
		// Repeating the active request must not recursively edit the same object.
		pending_edit_object = requested_object;
		has_pending_edit = requested_object != editing_object;
		return;
	}

	edit_in_progress = true;
	ObjectID next_object = requested_object;
	int transition_count = 0;
	do {
		has_pending_edit = false;
		editing_object = next_object;
		Object *object = next_object.is_valid() ? ObjectDB::get_instance(next_object) : nullptr;
		if (next_object.is_valid() && object == nullptr) {
			break; // A queued object was freed by a plugin callback.
		}
		EditorPlugin *handling_plugin = EditorNode::get_editor_data().get_handling_main_editor(object);
		EditorPlugin *previous_plugin = selected_plugin;
		const ObjectID handling_id = handling_plugin ? handling_plugin->get_instance_id() : ObjectID();
		const ObjectID previous_id = previous_plugin ? previous_plugin->get_instance_id() : ObjectID();
		selected_plugin = handling_plugin;
		if (previous_plugin && previous_plugin != handling_plugin) {
			previous_plugin->edit(nullptr);
			previous_plugin = Object::cast_to<EditorPlugin>(ObjectDB::get_instance(previous_id));
			if (previous_plugin && handling_id.is_valid()) {
				previous_plugin->make_visible(false);
			}
		}
		handling_plugin = Object::cast_to<EditorPlugin>(ObjectDB::get_instance(handling_id));
		if (handling_plugin && selected_plugin == handling_plugin) {
			// edit(nullptr) may also have freed the originally requested object.
			object = next_object.is_valid() ? ObjectDB::get_instance(next_object) : nullptr;
			handling_plugin->edit(object);
			handling_plugin = Object::cast_to<EditorPlugin>(ObjectDB::get_instance(handling_id));
			if (handling_plugin && selected_plugin == handling_plugin) {
				handling_plugin->make_visible(true);
			}
		} else if (!handling_plugin && handling_id.is_valid()) {
			selected_plugin = nullptr;
		}
		if (handling_id.is_valid() && ObjectDB::get_instance(handling_id) == nullptr) {
			selected_plugin = nullptr; // A visibility/edit callback unloaded the plugin.
		}
		next_object = pending_edit_object;
		transition_count++;
	} while (has_pending_edit && transition_count < 64);

	const bool transition_limit_reached = has_pending_edit;
	has_pending_edit = false;
	pending_edit_object = ObjectID();
	editing_object = ObjectID();
	edit_in_progress = false;
	ERR_FAIL_COND_MSG(transition_limit_reached, "Main-screen plugins repeatedly requested conflicting editor selections.");
}

void EditorMainScreen::select_next() {
	if (get_tab_count() == 0) {
		return;
	}
	if (get_current_tab() < get_tab_count() - 1) {
		set_current_tab(get_current_tab() + 1);
	} else {
		set_current_tab(0);
	}
}

void EditorMainScreen::select_prev() {
	if (get_tab_count() == 0) {
		return;
	}
	if (get_current_tab() > 0) {
		set_current_tab(get_current_tab() - 1);
	} else {
		set_current_tab(get_tab_count() - 1);
	}
}

EditorPlugin *EditorMainScreen::get_selected_plugin() const {
	return selected_plugin;
}

bool EditorMainScreen::can_auto_switch_screens() const {
	if (get_current_tab() == -1) {
		return true;
	}
	EditorDock *dock = get_dock(get_current_tab());
	return dock->is_allow_switch_screen();
}

#ifndef DISABLE_DEPRECATED
VBoxContainer *EditorMainScreen::get_control() const {
	return main_screen_vbox;
}

void EditorMainScreen::add_main_plugin(EditorPlugin *p_editor) {
	editor_table.push_back(p_editor);
}

void EditorMainScreen::remove_main_plugin(EditorPlugin *p_editor) {
	if (selected_plugin == p_editor) {
		selected_plugin = nullptr;
	}
	if (p_editor->has_meta("_dock")) {
		EditorDock *dock = Object::cast_to<EditorDock>(p_editor->get_meta("_dock").get_validated_object());
		if (dock) {
			EditorDockManager::get_singleton()->remove_dock(dock);
		}
	}

	editor_table.erase(p_editor);
}
#endif

EditorMainScreen::EditorMainScreen() :
		DockTabContainer(EditorDock::DOCK_SLOT_MAIN_SCREEN) {
	layout = EditorDock::DOCK_LAYOUT_MAIN_SCREEN;
	grid_rect = Rect2i(2, 0, 4, 4);

	set_theme_type_variation("MainScreenContainer");
	set_custom_minimum_size(Size2(0, 80) * EDSCALE);
	set_v_size_flags(Control::SIZE_EXPAND_FILL);
	set_draw_behind_parent(true);
	get_tab_bar()->set_mouse_filter(Control::MOUSE_FILTER_PASS);

#ifndef DISABLE_DEPRECATED
	main_screen_vbox = memnew(LegacyMainScreenContainer);
	main_screen_vbox->hide();
	EditorNode::get_singleton()->get_gui_base()->add_child(main_screen_vbox);
#endif

	Ref<StyleBoxEmpty> sb;
	sb.instantiate();
	add_theme_style_override(SceneStringName(panel), sb);
}

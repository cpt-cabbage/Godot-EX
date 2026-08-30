/**************************************************************************/
/*  light_3d_editor_plugin.cpp                                            */
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

#include "light_3d_editor_plugin.h"

#include "core/object/callable_mp.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/3d/light_3d.h"
#include "scene/gui/button.h"

void EditorInspectorPluginLight3D::_update_visibility_icon(Button *p_button, bool p_visible) {
	p_button->set_button_icon(EditorNode::get_singleton()->get_editor_theme()->get_icon(p_visible ? SNAME("GuiVisibilityVisible") : SNAME("GuiVisibilityHidden"), EditorStringName(EditorIcons)));
}

void EditorInspectorPluginLight3D::_toggle_range_visibility(ObjectID p_light_id, Button *p_button) {
	Light3D *light = ObjectDB::get_instance<Light3D>(p_light_id);
	if (!light) {
		return;
	}
	const bool was_visible = light->get_meta("_edit_range_visible_", true);

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(was_visible ? TTR("Hide Light Range") : TTR("Show Light Range"));
	if (was_visible) {
		ur->add_do_method(light, "set_meta", "_edit_range_visible_", false);
		ur->add_undo_method(light, "remove_meta", "_edit_range_visible_");
	} else {
		ur->add_do_method(light, "remove_meta", "_edit_range_visible_");
		ur->add_undo_method(light, "set_meta", "_edit_range_visible_", false);
	}
	ur->add_do_method(light, "update_gizmos");
	ur->add_undo_method(light, "update_gizmos");
	ur->commit_action();

	_update_visibility_icon(p_button, !was_visible);
}

bool EditorInspectorPluginLight3D::can_handle(Object *p_object) {
	return Object::cast_to<Light3D>(p_object) != nullptr;
}

bool EditorInspectorPluginLight3D::parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) {
	if (instantiating_editor) {
		// instantiate_property_editor() below runs every plugin again; let the
		// default float editor be created instead of recursing.
		return false;
	}
	if (p_path != "omni_range" && p_path != "spot_range" && p_path != "area_range") {
		return false;
	}
	Light3D *light = Object::cast_to<Light3D>(p_object);
	if (!light) {
		return false;
	}

	instantiating_editor = true;
	EditorProperty *editor = EditorInspector::instantiate_property_editor(p_object, p_type, p_path, p_hint, p_hint_text, p_usage, p_wide);
	instantiating_editor = false;
	if (!editor) {
		return false;
	}

	Button *visibility_button = memnew(Button);
	visibility_button->set_theme_type_variation(SNAME("FlatButton"));
	visibility_button->set_accessibility_name(TTR("Range Visibility"));
	visibility_button->set_tooltip_text(TTR("Show or hide the light's range in the 3D viewport."));
	_update_visibility_icon(visibility_button, light->get_meta("_edit_range_visible_", true));
	visibility_button->connect(SceneStringName(pressed), callable_mp(this, &EditorInspectorPluginLight3D::_toggle_range_visibility).bind(light->get_instance_id(), visibility_button));

	editor->add_inline_control(visibility_button, EditorProperty::INLINE_CONTROL_RIGHT);
	add_property_editor(p_path, editor);
	return true;
}

Light3DEditorPlugin::Light3DEditorPlugin() {
	Ref<EditorInspectorPluginLight3D> plugin;
	plugin.instantiate();
	add_inspector_plugin(plugin);
}

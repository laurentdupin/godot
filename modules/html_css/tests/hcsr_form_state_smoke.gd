extends SceneTree

func _initialize() -> void:
	var document := HTMLDocument.new()
	document.html = "<!DOCTYPE html><html><body><input id='name' value='old'><input id='check' type='checkbox'><input id='radio-a' type='radio' name='mode' checked><input id='radio-b' type='radio' name='mode'><textarea id='notes'>initial</textarea><select id='choice'><option value='a'>Alpha</option><option id='option-b' value='b'>Beta</option></select></body></html>"
	document.resource_root = "res://"

	var view := HTMLView.new()
	view.backend_preference = HTMLView.BACKEND_CPU
	view.size = Vector2(640, 240)
	view.document = document
	root.add_child(view)
	await process_frame

	if view.set_form_control_value("name", "updated") != OK \
			or view.set_form_control_value("notes", "new notes") != OK \
			or view.set_form_control_value("choice", "b") != OK \
			or view.set_form_control_checked("check", true) != OK \
			or view.set_form_control_checked("radio-b", true) != OK:
		push_error("HTMLView rejected an HCSR form-control state update.")
		quit(1)
		return

	var name := view.get_form_control_state("name")
	var notes := view.get_form_control_state("notes")
	var choice := view.get_form_control_state("choice")
	var option := view.get_form_control_state("option-b")
	var check := view.get_form_control_state("check")
	var radio_a := view.get_form_control_state("radio-a")
	var radio_b := view.get_form_control_state("radio-b")
	if name.get("value") != "updated" or name.get("focused") != false \
			or notes.get("value") != "new notes" \
			or choice.get("value") != "b" or choice.get("selected_index") != 1 \
			or option.get("selected") != true or option.get("selected_index") != 1 \
			or check.get("checked") != true \
			or radio_a.get("checked") != false or radio_b.get("checked") != true:
		push_error("HTMLView returned inconsistent HCSR form-control state.")
		quit(1)
		return

	print("HCSR HTMLView form-control state smoke passed.")
	quit()

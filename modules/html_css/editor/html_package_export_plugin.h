/**************************************************************************/
/*  html_package_export_plugin.h                                          */
/**************************************************************************/

#pragma once

#ifdef TOOLS_ENABLED

#include "editor/export/editor_export_plugin.h"

class HTMLPackageExportPlugin : public EditorExportPlugin {
	GDCLASS(HTMLPackageExportPlugin, EditorExportPlugin);

	bool debug_export = false;
	HashSet<String> compiled_documents;
	HashSet<String> inspected_resources;
	bool _compile_package(const String &p_path);
	void _compile_script_document_entries(const String &p_path);
	void _inspect_resource_dependencies(const String &p_path);

protected:
	virtual void _export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) override;
	virtual void _export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) override;
	virtual void _export_end() override;

public:
	virtual String get_name() const override { return "HCSR HTML package compiler"; }
};

#endif

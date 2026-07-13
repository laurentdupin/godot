/**************************************************************************/
/*  html_package_export_plugin.h                                          */
/**************************************************************************/

#pragma once

#ifdef TOOLS_ENABLED

#include "editor/export/editor_export_plugin.h"

class HTMLPackageExportPlugin : public EditorExportPlugin {
	GDCLASS(HTMLPackageExportPlugin, EditorExportPlugin);

	bool debug_export = false;
	void _add_project_files(const String &p_directory);
	void _compile_package(const String &p_path);

protected:
	virtual void _export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) override;
	virtual void _export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) override;

public:
	virtual String get_name() const override { return "HCSR HTML package compiler"; }
};

#endif

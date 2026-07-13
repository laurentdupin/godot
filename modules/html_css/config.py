def get_opts(platform):
    from SCons.Variables import BoolVariable, EnumVariable, PathVariable

    default_external_lib = "blink_standalone_renderer_c_api.lib" if platform == "windows" else "libblink_standalone_renderer_c_api.a"

    return [
        EnumVariable(
            "module_html_css_renderer",
            "HTML/CSS renderer implementation compiled into the module. 'none' keeps only the raw CPU frame receiver; 'blink' and 'hcsr' are mutually exclusive providers.",
            "none",
            allowed_values=("none", "blink", "hcsr"),
        ),
        BoolVariable(
            "module_html_css_blink_enabled",
            "Legacy alias for module_html_css_renderer=blink. Cannot be combined with module_html_css_renderer=hcsr.",
            False,
        ),
        PathVariable(
            "module_html_css_hcsr_lib_path",
            "Optional path to a prebuilt static HCSR NativeAOT library. Defaults to the nested thirdparty/hcsr publish output.",
            "",
            PathVariable.PathAccept,
        ),
        BoolVariable(
            "module_html_css_hcsr_auto_build",
            "Build the nested HCSR static NativeAOT library with dotnet publish when its archive is missing.",
            True,
        ),
        PathVariable(
            "module_html_css_blink_lib_path",
            "Path to an external HTML/CSS renderer C API package directory. Defaults to the nested thirdparty Blink generated package directory for the selected link mode.",
            "",
            PathVariable.PathAccept,
        ),
        PathVariable(
            "module_html_css_blink_package_root",
            "Optional root directory for local/release Blink package artifacts, used only after generated nested package output when module_html_css_blink_lib_path is empty.",
            "thirdparty/blink-standalone-ui/prebuilt",
            PathVariable.PathAccept,
        ),
        EnumVariable(
            "module_html_css_blink_link_mode",
            "How to consume the external HTML/CSS renderer C API package: static links the provided static archive(s) and is the default; dynamic links an import/shared library and is an explicit diagnostic/development override",
            "static",
            allowed_values=("dynamic", "static"),
        ),
        (
            "module_html_css_blink_lib",
            "External HTML/CSS renderer C API library file name or linker input",
            default_external_lib,
        ),
        (
            "module_html_css_blink_static_libs",
            "Fallback semicolon-separated linker inputs required by a static Blink C API package when no static link manifest is used.",
            "",
        ),
        (
            "module_html_css_blink_static_manifest",
            "Path to a Blink static C API package link manifest. If empty in static mode, SCsub looks for blink_standalone_renderer_c_api_static_link_manifest.json in module_html_css_blink_lib_path.",
            "",
        ),
        BoolVariable(
            "module_html_css_blink_auto_build",
            "Opt in to building the nested Blink C API package when the expected artifact is missing. This may perform networked V8/depot_tools/CIPD bootstrap work.",
            False,
        ),
        BoolVariable(
            "module_html_css_blink_copy_runtime_sidecars",
            "Copy Blink C API runtime sidecars from the selected dynamic or static package directory to the Godot bin directory.",
            True,
        ),
        EnumVariable(
            "module_html_css_blink_package_profile",
            "Nested Blink package profile used when module_html_css_blink_lib_path is empty. auto prefers an existing nested package, then selects the default expected package path for the platform.",
            "auto",
            allowed_values=("auto", "msvc", "generated_v8_chromium_llvm"),
        ),
        BoolVariable(
            "module_html_css_blink_static_whole_archive",
            "Force whole-archive linking for Blink static archives listed as manifest candidates. Off by default because Godot may already provide libraries such as libpng, ICU, VMA, or Embree.",
            False,
        ),
        BoolVariable(
            "module_html_css_blink_static_allow_unsupported_host",
            "Allow linking a Blink static package whose manifest declares full host/editor static linking unsupported. Experimental and unsafe; off by default.",
            False,
        ),
    ]


def can_build(env, platform):
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "HTMLDocument",
        "HTMLRenderTarget",
        "HTMLSurface3D",
        "HTMLTexture2D",
        "HTMLView",
    ]


def get_doc_path():
    return "doc_classes"

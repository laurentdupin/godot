def get_opts(platform):
    from SCons.Variables import BoolVariable, EnumVariable, PathVariable

    default_external_lib = "blink_standalone_renderer_c_api.lib" if platform == "windows" else "libblink_standalone_renderer_c_api.a"

    return [
        BoolVariable(
            "module_html_css_blink_enabled",
            "Enable the external HTML/CSS renderer C API backend for the html_css module",
            False,
        ),
        PathVariable(
            "module_html_css_blink_lib_path",
            "Path to the prebuilt external HTML/CSS renderer C API package directory, such as thirdparty/blink-standalone-ui/build/cmake-generated-v8-chromium-llvm/package/c_api_runtime",
            "",
            PathVariable.PathAccept,
        ),
        EnumVariable(
            "module_html_css_blink_link_mode",
            "How to consume the external HTML/CSS renderer C API package: dynamic links an import/shared library and requires runtime sidecars; static links the provided static archive(s) but may still require package data/sidecars",
            "dynamic",
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
            "module_html_css_blink_static_whole_archive",
            "Force whole-archive linking for Blink static archives listed as manifest candidates. Off by default because Godot may already provide libraries such as libpng, ICU, VMA, or Embree.",
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

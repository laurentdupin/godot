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
            "Additional semicolon-separated linker inputs required by a static Blink C API package. Leave empty until the Blink static package manifest provides the transitive list.",
            "",
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

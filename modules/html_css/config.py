def get_opts(platform):
    from SCons.Variables import BoolVariable, PathVariable

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
        (
            "module_html_css_blink_lib",
            "External HTML/CSS renderer C API library file name or linker input",
            default_external_lib,
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

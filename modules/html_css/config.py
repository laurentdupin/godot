def get_opts(platform):
    from SCons.Variables import BoolVariable, EnumVariable, PathVariable

    return [
        EnumVariable(
            "module_html_css_renderer",
            "HTML/CSS renderer implementation compiled into the module. 'none' keeps only the raw CPU frame receiver.",
            "none",
            allowed_values=("none", "hcsr"),
        ),
        PathVariable(
            "module_html_css_hcsr_lib_path",
            "Optional path to a prebuilt static HCSR NativeAOT library. Defaults to the nested thirdparty/hcsr publish output.",
            "",
            PathVariable.PathAccept,
        ),
        BoolVariable(
            "module_html_css_hcsr_auto_build",
            "Build the nested platform-specific HCSR static NativeAOT package when its archive is missing.",
            True,
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

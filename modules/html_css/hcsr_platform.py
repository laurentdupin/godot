"""Shared Unix NativeAOT host linking for old and newest HCSR."""

def unix_link_flags(platform, initializer, archive):
    if platform == "linuxbsd":
        return ["-Wl,-z,nostart-stop-gc", initializer, archive, "-ldl", "-lpthread", "-lm", "-lz", "-lrt"]
    if platform == "macos":
        flags = ["-mmacosx-version-min=12.0", initializer, archive]
        for framework in ("CoreFoundation", "Foundation", "GSS", "Metal", "Network", "Security"):
            flags += ["-framework", framework]
        return flags + ["-lz"]
    raise ValueError("Unsupported Unix HCSR platform: " + platform)


def newest_target_supported(renderer, platform, architecture):
    if renderer not in ("hcsr_newest", "hcsr_newest_dll"):
        return False
    if platform == "windows":
        return architecture == "x86_64"
    if platform == "linuxbsd":
        return architecture in ("x86_64", "arm64")
    return (renderer == "hcsr_newest" and platform in ("linuxbsd", "macos")
            and architecture in ("x86_64", "arm64"))

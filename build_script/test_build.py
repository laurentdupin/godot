import unittest

import build


class NewestRendererPlatformTests(unittest.TestCase):
    def test_supported_build_commands(self):
        targets = [
            ("windows", "x86_64"),
            ("linuxbsd", "x86_64"),
            ("linuxbsd", "arm64"),
            ("macos", "x86_64"),
            ("macos", "arm64"),
        ]
        for platform, architecture in targets:
            for target in build.TARGETS:
                for mono in (False, True):
                    with self.subTest(platform=platform, architecture=architecture, target=target, mono=mono):
                        settings = build.BuildSettings(
                            architecture=architecture, target=target, mono=mono,
                            html_css_renderer="hcsr_newest", suffix="hcsr_newest",
                        )
                        build.validate_settings(settings, platform)
                        command = build.build_command(settings, platform, False, [])
                        self.assertIn(f"platform={platform}", command)
                        self.assertIn(f"arch={architecture}", command)
                        self.assertIn(f"target={target}", command)
                        self.assertIn("module_html_css_renderer=hcsr_newest", command)
                        self.assertIn("extra_suffix=hcsr_newest", command)
                        if platform != "windows":
                            self.assertNotIn("d3d12=yes", command)

    def test_unsupported_targets_still_rejected(self):
        for renderer in ("hcsr_newest", "hcsr_newest_dll"):
            for platform in ("windows", "linuxbsd", "macos", "freebsd"):
                for architecture in build.ARCHITECTURES:
                    supported = (
                        (platform == "windows" and architecture == "x86_64")
                        or (renderer == "hcsr_newest" and platform in ("linuxbsd", "macos")
                            and architecture in ("x86_64", "arm64"))
                    )
                    settings = build.BuildSettings(architecture=architecture, html_css_renderer=renderer)
                    with self.subTest(renderer=renderer, platform=platform, architecture=architecture):
                        if supported:
                            build.validate_settings(settings, platform)
                        else:
                            with self.assertRaises(RuntimeError):
                                build.validate_settings(settings, platform)


if __name__ == "__main__":
    unittest.main()

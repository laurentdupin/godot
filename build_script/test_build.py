import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import build


class NewestRendererPlatformTests(unittest.TestCase):
    def test_unsuffixed_metal_editor_command(self):
        settings = build.BuildSettings(architecture="arm64")
        command = build.build_command(settings, "macos", False, [])
        self.assertIn("module_html_css_renderer=hcsr_newest", command)
        self.assertIn("metal=yes", command)
        self.assertIn("extra_suffix=", command)
        self.assertIn("generate_bundle=yes", command)
        self.assertEqual(build.editor_binary_name(settings, "macos"), "godot.macos.editor.arm64.mono")

    def test_metal_override_and_intel_build(self):
        settings = build.BuildSettings(architecture="arm64")
        command = build.build_command(settings, "macos", False, ["metal=no"])
        self.assertGreater(command.index("metal=no"), command.index("metal=yes"))
        settings.architecture = "x86_64"
        self.assertNotIn("metal=yes", build.build_command(settings, "macos", False, []))

    def test_developer_editor_name(self):
        settings = build.BuildSettings(architecture="arm64", developer_build=True, suffix="test")
        self.assertEqual(build.editor_binary_name(settings, "macos"), "godot.macos.editor.dev.arm64.test.mono")

    def test_renderer_cli_selection(self):
        with patch("sys.argv", ["build.py", "--non-interactive", "--renderer", "hcsr_newest", "--suffix", ""]):
            arguments, extra = build.parse_arguments()
        self.assertEqual(arguments.renderer, "hcsr_newest")
        self.assertEqual(arguments.suffix, "")
        self.assertEqual(extra, [])

    def test_newest_archive_triggers_relink_only_when_newer(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(build, "GODOT_ROOT", Path(directory)):
            settings = build.BuildSettings(architecture="arm64", developer_build=True)
            editor = Path(directory) / "bin" / build.editor_binary_name(settings, "macos")
            archive = (
                Path(directory)
                / "thirdparty/hcsr_newest/build/hcsr-bundle/static/osx-arm64/Release/libhcsr_scene_combined.a"
            )
            editor.parent.mkdir(parents=True)
            archive.parent.mkdir(parents=True)
            editor.touch()
            archive.touch()
            os.utime(archive, ns=(100, 100))
            os.utime(editor, ns=(200, 200))
            build.force_macos_hcsr_relink(settings, "macos")
            self.assertTrue(editor.exists())
            os.utime(archive, ns=(300, 300))
            build.force_macos_hcsr_relink(settings, "macos")
            self.assertFalse(editor.exists())

    def test_managed_glue_does_not_use_stale_bundle(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(build, "GODOT_ROOT", Path(directory)):
            stale = Path(directory) / "bin/godot_macos_editor_mono.app/Contents/MacOS/Godot"
            stale.parent.mkdir(parents=True)
            stale.touch()
            with self.assertRaisesRegex(RuntimeError, "did not produce"):
                build.build_managed_editor_assemblies(build.BuildSettings(architecture="arm64"), "macos")

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
                            architecture=architecture,
                            target=target,
                            mono=mono,
                            html_css_renderer="hcsr_newest",
                            suffix="hcsr_newest",
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
                    supported = (platform == "windows" and architecture == "x86_64") or (
                        (platform == "linuxbsd" or (renderer == "hcsr_newest" and platform == "macos"))
                        and architecture in ("x86_64", "arm64")
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

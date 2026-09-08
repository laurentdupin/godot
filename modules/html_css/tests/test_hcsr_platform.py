"""Exercise actual SCsub platform branches without invoking external builds."""
import importlib.util
import os
from pathlib import Path
import runpy
import sys
import unittest
from unittest.mock import patch, mock_open
from types import SimpleNamespace

MODULE = Path(__file__).resolve().parents[1]
ROOT = MODULE.parent.parent
sys.path.insert(0, str(ROOT))
import SCons.Script

class Environment(dict):
    editor_build = False
    def __init__(self, platform, arch):
        super().__init__(module_html_css_renderer='hcsr_newest', platform=platform, arch=arch,
                         module_html_css_hcsr_auto_build=True, vulkan=True, d3d12=platform=='windows', disable_3d=False)
        self.modules_sources = []
    def Clone(self): return self
    def Append(self, **kwargs):
        for key, values in kwargs.items(): self.setdefault(key, []).extend(values)
    def Prepend(self, **kwargs):
        for key, values in kwargs.items(): self[key] = values + self.get(key, [])
    def File(self, path): return SimpleNamespace(abspath=str(ROOT / path.lstrip('#')))
    Dir = File
    def Depends(self, *args): pass
    def add_source_files(self, target, source): target.append(source)

class PlatformTests(unittest.TestCase):
    def configure(self, platform, arch, host):
        env = Environment(platform, arch)
        with patch('sys.platform', host), patch('os.path.isfile', return_value=True), \
             patch('builtins.open', mock_open(read_data='revision\n')), \
             patch('subprocess.run', return_value=SimpleNamespace(stdout='revision\n', returncode=0)) as build:
            runpy.run_path(str(MODULE/'SCsub'), init_globals={'env':env, 'env_modules':env, 'Import':lambda _:None})
        self.assertEqual(build.call_count, 1, 'A current package must not rebuild')
        return env
    def test_unix_matrix(self):
        for platform, host, prefix in [('linuxbsd','linux','linux-'),('macos','darwin','osx-')]:
            for arch, suffix in [('x86_64','x64'),('arm64','arm64')]:
                with self.subTest(platform=platform, arch=arch):
                    env=self.configure(platform,arch,host)
                    flags=' '.join(env['LINKFLAGS'])
                    self.assertIn(prefix+suffix,flags)
                    self.assertIn('hcsr_scene_initializer.o',flags)
                    self.assertIn('libhcsr_scene_combined.a',flags)
                    self.assertNotIn('.lib',flags)
                    self.assertNotIn('HTML_CSS_HCSR_NEWEST_D3D12',env['CPPDEFINES'])
                    self.assertEqual('HTML_CSS_HCSR_NEWEST_VULKAN' in env['CPPDEFINES'],platform=='linuxbsd')
    def test_windows(self):
        env=self.configure('windows','x86_64','win32')
        self.assertIn('HTML_CSS_HCSR_NEWEST_D3D12',env['CPPDEFINES'])
        self.assertIn('HTML_CSS_HCSR_NEWEST_VULKAN',env['CPPDEFINES'])
        self.assertIn('d3d12.lib',env['LINKFLAGS'])
    def test_cross_os_rejected(self):
        with self.assertRaises(SystemExit): self.configure('macos','arm64','linux')

if __name__=='__main__': unittest.main()

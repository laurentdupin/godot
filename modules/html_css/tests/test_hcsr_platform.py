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
    def __init__(self, platform, arch, renderer="hcsr_newest"):
        super().__init__(module_html_css_renderer=renderer, platform=platform, arch=arch,
                         module_html_css_hcsr_auto_build=True, vulkan=True, d3d12=platform=='windows', metal=platform=='macos' and arch=='arm64', disable_3d=False)
        self.modules_sources = []
    def Clone(self): return self
    def Append(self, **kwargs):
        for key, values in kwargs.items(): self.setdefault(key, []).extend(values)
    def Prepend(self, **kwargs):
        for key, values in kwargs.items(): self[key] = values + self.get(key, [])
    def File(self, path): return SimpleNamespace(abspath=str(ROOT / path.lstrip('#')))
    Dir = File
    def Depends(self, *args): pass
    def CommandNoCache(self, target, source, action): self.setdefault("copied", []).append(target)
    def add_source_files(self, target, source): target.append(source)

class PlatformTests(unittest.TestCase):
    def configure(self, platform, arch, host, renderer="hcsr_newest", metal=None):
        env = Environment(platform, arch, renderer)
        if metal is not None: env["metal"] = metal
        with patch('sys.platform', host), patch('os.path.isfile', return_value=True), \
             patch('builtins.open', mock_open(read_data='revision\n')), \
             patch('subprocess.run', return_value=SimpleNamespace(stdout='revision\n', returncode=0)) as build:
            runpy.run_path(str(MODULE/'SCsub'), init_globals={'env':env, 'env_modules':env, 'Import':lambda _:None, 'Copy':lambda *args:None})
        self.assertEqual(build.call_count, 1, 'A current package must not rebuild')
        return env
    def test_unix_matrix(self):
        for platform, host, prefix in [('linuxbsd','linux','linux-'),('macos','darwin','osx-')]:
            for arch, suffix in [('x86_64','x64'),('arm64','arm64')]:
                with self.subTest(platform=platform, arch=arch):
                    env=self.configure(platform,arch,host,"hcsr_newest_dll" if platform == "linuxbsd" else "hcsr_newest")
                    flags=' '.join(env['LINKFLAGS'])
                    if platform == 'linuxbsd':
                        libraries = [item.abspath for item in env['LIBS']]
                        self.assertEqual(len(libraries), 3)
                        self.assertTrue(all('/dynamic/'+prefix+suffix+'/' in item and item.endswith('.so') for item in libraries))
                        self.assertIn('$$ORIGIN', flags)
                        self.assertNotIn('hcsr_scene_initializer.o', flags)
                        self.assertNotIn('HCSR_SCENE_STATIC', env['CPPDEFINES'])
                        self.assertEqual(len(env['copied']), 3)
                    else:
                        self.assertIn(prefix+suffix,flags)
                        self.assertIn('hcsr_scene_initializer.o',flags)
                        self.assertIn('libhcsr_scene_combined.a',flags)
                    self.assertNotIn('.lib',flags)
                    self.assertNotIn('HTML_CSS_HCSR_NEWEST_D3D12',env['CPPDEFINES'])
                    self.assertEqual('HTML_CSS_HCSR_NEWEST_VULKAN' in env['CPPDEFINES'],platform=='linuxbsd')
                    self.assertEqual('HTML_CSS_HCSR_NEWEST_METAL' in env['CPPDEFINES'],platform=='macos' and arch=='arm64')
                    if platform == 'macos': self.assertIn('QuartzCore', flags)
    def test_linux_static(self):
        env = self.configure('linuxbsd', 'x86_64', 'linux')
        self.assertIn('HCSR_SCENE_STATIC', env['CPPDEFINES'])
        self.assertTrue(env['LIBS'][0].abspath.endswith('libhcsr_scene_combined.a'))
        self.assertTrue(any('hcsr_scene_initializer.o' in flag for flag in env['LINKFLAGS']))
        self.assertNotIn('copied', env)
    def test_windows(self):
        env=self.configure('windows','x86_64','win32')
        self.assertIn('HTML_CSS_HCSR_NEWEST_D3D12',env['CPPDEFINES'])
        self.assertIn('HTML_CSS_HCSR_NEWEST_VULKAN',env['CPPDEFINES'])
        self.assertIn('d3d12.lib',env['LINKFLAGS'])
    def test_macos_without_metal(self):
        env = self.configure('macos', 'arm64', 'darwin', metal=False)
        self.assertNotIn('HTML_CSS_HCSR_NEWEST_METAL', env['CPPDEFINES'])

    def test_legacy_macos_bundle_rebuilds(self):
        env = Environment('macos', 'arm64')
        with patch('sys.platform', 'darwin'), \
             patch('os.path.isfile', side_effect=lambda path: not str(path).endswith('.hcsr_metal_backend_v1')), \
             patch('builtins.open', mock_open(read_data='revision\n')), \
             patch('subprocess.run', return_value=SimpleNamespace(stdout='revision\n', returncode=0)) as build:
            runpy.run_path(str(MODULE/'SCsub'), init_globals={'env':env, 'env_modules':env, 'Import':lambda _:None, 'Copy':lambda *args:None})
        self.assertEqual(build.call_count, 2)
        self.assertIn('build-hcsr-bundle-unix.sh', build.call_args.args[0][1])

    def test_cross_os_rejected(self):
        with self.assertRaises(SystemExit): self.configure('macos','arm64','linux')

if __name__=='__main__': unittest.main()

# Desktop build helper

Run `python3 build_script/build.py` for interactive configuration. The helper
creates its own Python/SCons environment and saves selections in the ignored
`build_script/settings.json`. New configurations use `hcsr_newest`; existing
saved renderer selections are preserved.

To select the newest HCSR architecture and build without a custom suffix:

```sh
python3 build_script/build.py --non-interactive --renderer hcsr_newest --suffix ''
```

This persists both selections. Other settings, including architecture, Mono,
target, and job count, retain their saved values. Arguments after `--` are passed
to SCons; use the helper's `--renderer` and `--suffix` options for those selections
so packaging and managed glue generation use the same configuration.

On macOS ARM64, Metal is enabled. Intel macOS uses Godot's supported drivers and
HCSR's CPU fallback. Mono editor builds first compile the native executable,
generate and build its managed API assemblies, and then package and sign the
`.app`. With Mono enabled and no custom suffix, the outputs are:

- `bin/godot.macos.editor.arm64.mono`
- `bin/godot_macos_editor_mono.app`

Quit and reopen an already-running editor to use the rebuilt executable.
A suffixed build creates a separate app and does not update the unsuffixed app.

Validate the helper with:

```sh
python3 -m unittest discover -s build_script -p 'test_*.py'
```

See [Metal setup and validation](../modules/html_css/README.md) for native build
dependencies, GPU checks, and macOS deployment constraints, or
[Linux HCSR linkage](HCSR_LINUX.md) for static and shared Linux builds.

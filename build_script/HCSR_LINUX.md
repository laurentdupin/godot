# Linux HCSR linkage

Both x86_64 and ARM64 support the same renderer choices as Windows:

- `module_html_css_renderer=hcsr_newest`: statically linked NativeAOT runtime.
- `module_html_css_renderer=hcsr_newest_dll`: shared Linux `.so` libraries.

The build script's `html_css_renderer` setting accepts either option. Shared
builds place `libhcsr_scene.so`, `libhcsr_render_vulkan.so`, and
`libhcsr_native_codecs.so` in `bin`. Keep all three beside the editor or exported
executable. The executable resolves them using `$ORIGIN`, independent of the
working directory. Linux export copies the libraries beside the selected
template into the output, including ZIP exports.

The Unix HCSR producer accepts `--linkage static` (default) or
`--linkage dynamic` (Linux only); products live in separate bundle directories.
Shared builds use the supported NativeAOT shared-library bootstrap and keep its
runtime symbols private to that ELF image. This avoids embedding NativeAOT in
the executable processed by Steam's wrapper. Test the wrapped product's normal
UI startup as well as synthetic/headless smoke tests.

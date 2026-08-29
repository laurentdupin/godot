# HCSR slider text repaint reproduction

This minimal project isolates the issue where an HCSR range thumb moves but a
same-width text value such as `86°` / `87°` is not repainted reliably.

It shows the interactive primary `HTMLView` and a secondary `HTMLViewOutput`
side by side. The latter mirrors the texture-consumer path used by Deep Desktop.

Run it with the custom Godot editor or player:

```text
godot --editor --path .
godot --path .
```

Drag the slider slowly and then quickly. Both HCSR surfaces must update on every
movement. The checkbox switches between Deep Desktop's repeated per-frame text
write and a changed-value-only write, making the two scheduling patterns directly
comparable. The Godot-native diagnostic label remains independent of HCSR damage.

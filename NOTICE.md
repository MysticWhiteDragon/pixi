# Notices

Pixi is free software under the GNU General Public License, version 3 only
(see [LICENSE](LICENSE)). It includes material from the projects below.

## LineXinBar

Pixi's look is taken from [LineXinBar](https://github.com/Petexy/LineXinBar)
by its contributors, licensed GPL-3.0-only. Taken from LineXinBar 0.9.4
(commit `50d1cb501393c729510caa227d327b6c4b03062c`):

| In Pixi | From LineXinBar | Changes |
| --- | --- | --- |
| `assets/glyphs-src/*.svg` | `crates/lxb-desktop/src/glyphs/*.svg` | none |
| `assets/glyphs/*.png` | made from the SVGs above by `tools/make-sdf.py` | signed distance fields, using the same measurement as LineXinBar's `icons::builtin_distance_field` |
| `assets/sounds/*.ogg` | `crates/lxb-desktop/src/sounds/` | none (a subset) |
| `shell/shaders.h` | `crates/lxb-desktop/src/shaders.wgsl` | wallpaper, silk and water currents, glass pane and glyph materials ported from WGSL to GLSL ES 3.00; picture layers, sparkles, overview cards and the backdrop snapshot left out |
| Layout and theme constants in `shell/pixi-shell.c` | `crates/lxb-desktop/src/ui.rs`, `theme.rs` | values copied; the code using them is Pixi's own |

The `pixi_shell_v1` protocol is Pixi's own, modeled on the idea of
LineXinBar's private `lxb_shell_v1`: a privileged interface offered only to
the shell process the compositor started.

Everything else (the compositor, the shell program, the installer and the
tools) was written for Pixi.

## Roboto

`assets/fonts/Roboto-*.ttf` is Roboto by Google, licensed under the Apache
License 2.0 ([assets/fonts/Roboto-LICENSE.txt](assets/fonts/Roboto-LICENSE.txt)),
as distributed with LineXinBar.

## wlroots

The compositor links against [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots)
(MIT) and follows the structure of its `tinywl` example.

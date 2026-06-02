# Bundled retro runtime (aggregated, not linked)

This directory holds an **independent, separately-licensed** program that
PlotJuggler launches as a child process. PlotJuggler links **none** of it.

## Contents (placed at package time; git-ignored)

- `pj-raster-helper` — GPLv2 render helper built from the doomgeneric git
  submodule (`3rdparty/doomgeneric`). It runs as a separate process and renders
  into PlotJuggler through shared-memory IPC.
- `base.wad` — id Software's freely-redistributable **shareware** game data
  (`DOOM1.WAD`), renamed. Redistribution of the unmodified shareware file is
  permitted by its license.

## License artifacts (present in this directory; shipped with the bundle)

- `COPYING` — the full GPLv2 text governing `pj-raster-helper` and the
  doomgeneric engine it links.
- `SOURCE-OFFER.txt` — the written offer of the complete corresponding source
  for the `pj-raster-helper` binary (GPLv2 section 3): `raster_helper/` in this
  repo plus the doomgeneric submodule pinned to a specific upstream commit.
- `SHAREWARE-LICENSE.txt` — id Software's shareware distribution license
  covering `base.wad` (DOOM1.WAD), including id's confirmation that the
  unmodified shareware data is freely redistributable.

The root `CMakeLists.txt` installs these files into `thirdparty/retro/`
alongside the helper whenever it is bundled (`if(TARGET pj-raster-helper)`).
They MUST remain present and legible in the shipped bundle. Only the *trigger*
is hidden — the licenses are not.

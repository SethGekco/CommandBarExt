# CommandBarExt

Adds new named buttons to Yuri's Revenge's tactical bar (the AdvancedCommandBar)
via UImd.ini's `ButtonList=`, plus the matching game commands (hotkey-bindable).

Planned buttons: Aggressive Stance, Evasive Stance, Hold Fire, Hunt, Garrison.
See [DESIGN.md](DESIGN.md) for the full design and phase plan.

## Status: Phase 0 (plumbing probe)

One log-only button ("Hunt", ID 12) + one log-only command ("HuntUnits").
Proves: ButtonList name resolution, bar init/click/tooltip hooks, the third
same-address chain on 0x533066, and loose-uimd.ini override of cncnet.mix.

## Deploy (Phase 0)

1. `CommandBarExt.dll` (CI artifact) -> game dir; add `-i=CommandBarExt.dll`
   to wine-game.sh and ClientDefinitions.ini injection lists.
2. `deploy/uimd.ini` -> game dir (loose file; replaces the cncnet.mix override
   content verbatim, plus the Hunt button).
3. `assets/button12.shp` -> game dir (placeholder art; engine loads bar art
   as `Button%02d.SHP`).

## Build

MSVC v142 x86, DevBuild configuration; CI mirrors BunkerExt's workflow and
runs the hook overlap + bounds checks against the YR Hook Encyclopedia.

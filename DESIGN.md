# CommandBarExt — new tactical-bar buttons + unit commands

Status: PHASE 0 WRITTEN (2026-09-19) — probe committed locally; blocked on GitHub repo creation (permission), then CI + deploy

## Goal

Add new items to the YR tactical bar (bottom command bar) via UImd.ini:

1. **Aggressive Stance** — button for the existing YRAggressiveStance DLL toggle
2. **Evasive Stance** — units avoid all enemies, using enemy weapon ranges as the evasion guide
3. **Hold Fire** — selected units do not fire weapons at all
4. **Hunt** — send selected units on Mission::Hunt
5. **Garrison** — Occupier=yes infantry seek nearest garrisonable building; Bunkerable=yes vehicles seek Tank Bunkers

## How the vanilla bar works (verified 2026-09-18)

- gamemd reads `UIMD.INI` (string @ gamemd, "Failed to load UIMD.INI!"). Sections
  `[AdvancedCommandBar]` and `[MultiplayerAdvancedCommandBar]`, key `ButtonList=`.
- **Rex's ACTIVE UImd.ini lives inside `cncnet.mix`** (517 bytes, extracted — see below).
  Current live list: `ButtonList=Team01,Team02,TypeSelect,Deploy,Guard,PlanningMode` (+`,Beacon` for MP).
  A loose `uimd.ini` in the game dir should override the mix copy — verify in Phase 0.
- Button names are resolved by `AdvancedCommandBarClass::GetButtonIdxByName` (0x6CFD08 region).
  Names → hardcoded IDs. Vanilla owns IDs 0–11 (11 in use + 1 unused); the bar supports
  **25 slots total**, so up to 13 new buttons. Button *positions* are hardcoded; new buttons
  always render after the vanilla ones.
- Button art: `buttonNN.shp` (NN = button ID, so first new button = `button12.shp`) in the
  per-side `sidec0X` mixes — or loose file, verify. Tooltip via `TIP:` CSF label.

### Extracted active UImd.ini (from cncnet.mix)

```ini
[VersionInfo]
Name=Yuri's Revenge (CnCnet)
Version=1.001

[AdvancedCommandBar]
ButtonList=Team01,Team02,TypeSelect,Deploy,Guard,PlanningMode

[MultiplayerAdvancedCommandBar]
ButtonList=Team01,Team02,TypeSelect,Deploy,Guard,PlanningMode,Beacon

[Sidebar]
; (Phobos sidebar keys present — cncnet ships them)

[ToolTips]
ExtendedToolTips=yes
```

## The proven hook pattern — Phobos PR #1993 (open, NOT in release Phobos)

TaranDahl's "Distribution click action mode" PR adds a new bar button with just 5 hooks.
All 5 seats are **free in Rex's stack** (hooks.csv "Also in release? = —"):

| Address | Purpose |
|---|---|
| `0x6CFD08` | GetButtonIdxByName — resolve our names → our IDs, else fall through (return 0) |
| `0x6D0233` | Init — register buttons, IDs start at 12 (OldButtonCount) |
| `0x6D0827` | Update — button clicked: `Execute(ShapeButtonClass::GetButton(id)->IsOn)` |
| `0x6D10DF` | InitButtonIO — mark hold-down/toggle buttons (`ToggleType=1`, `UseFlash`) |
| `0x6D14DD` | InitToolTip — `ShapeButtonClass::SetToolTip(btn, TipName)` |

Reference source cached at scratchpad during research; re-fetch from
github.com/Phobos-developers/Phobos/pull/1993 (`src/Commands/AdvancedCommandBarButtons.*`).
Pattern: an `AdvancedCommandBarButton` base (virtual GetName/GetTipName/CanHoldDown/Execute),
static Array, IDs auto-assigned from 12 upward.

**Collision watch**: if PR #1993 ever merges into the Phobos build we co-load, it takes ID 12
and these 5 seats → re-run overlap check and re-base our IDs at that point.

## Command registration (hotkeys for free)

Each feature is also a `CommandClass` (name shows in the hotkey config dialog, bindable in
KeyboardMD.ini). Registration seat `0x533066` `CommandClassCallback_Register` is already
same-address-chained by release Phobos AND YRAggressiveStance, both live in-game — a third
chain should work but **prove liveness with a log line** (chaining is DISPUTED in memory).
Antares registers its own at 0x533058 (0x7 bytes, ends 0x5F — no overlap with 0x533066).

- The **Aggressive Stance button needs no cross-DLL linkage**: YRAggressiveStance registers
  a command named `"AggressiveStance"` into the shared `CommandClass::Array`; our button's
  Execute just looks it up by name at runtime and calls `Execute()`. Degrades gracefully if
  the stance DLL isn't loaded (log + no-op).

## Sync rules (multiplayer-safe)

Every behavior change goes through the event queue:

- **Hunt**: vanilla MegaMission event with Mission::Hunt per selected unit. No custom event.
- **Garrison**: compute nearest garrisonable target *locally* (deterministic inputs are fine
  because the decision travels in the event), then issue vanilla enter/occupy MegaMission
  events per unit. Occupier=yes → CanBeOccupied buildings; Bunkerable=yes → free Tank
  Bunkers (BunkerExt synergy later).
- **Hold Fire / Evasive Stance**: per-techno synced flags → need custom events, cloning the
  EventExt pattern from YRAggressiveStance (`~/YRAggressiveStance/src/Ext/Event/Body.*`).
  **Event ID registry**: vanilla 0x00–0x2F, CnCNet 0x30–0x3F, Ares 0x60–0x61,
  AggressiveStance 0xFF. Pick from 0xF0–0xFE after auditing co-loaded DLLs (Phobos has its
  own extended events — check its EventExt before choosing).
- Evasive per-frame logic runs in sim (synced) context only; any randomness via
  ScenarioClass::Random (see synced-vs-hashed memory).

## Per-feature difficulty

| Feature | Effort | Notes |
|---|---|---|
| Hunt | trivial | vanilla mission + vanilla event |
| Aggressive Stance button | easy | UI only; command already exists |
| Hold Fire | medium | flag + veto hooks (target scan + fire paths) + custom event + save/load of flag |
| Garrison | medium | target search + vanilla enter events; edge cases (full buildings, pathing) |
| Evasive Stance | hard | continuous threat-range flee logic; whole new stance; do LAST |

Hold Fire veto candidates: target-acquisition (GreatestThreat / SelectAutoTarget path) AND
manual fire path — consult YR-Hook-Encyclopedia before choosing seats (workflow memory).

## Phases

- **Phase 0 (probe)**: loose uimd.ini with one new name in ButtonList + the 5 bar hooks +
  1 log-only button/command. Verifies: loose-file override wins over cncnet.mix, name
  resolves, button renders (what happens with missing button12.shp?), click reaches Execute,
  0x533066 triple-chain liveness. Run hook-overlap CI check first (26-DLL stack!).
- **Phase 1**: Hunt + Aggressive Stance buttons (both thin).
- **Phase 2**: Hold Fire (flag, events, veto hooks, button art).
- **Phase 3**: Garrison (Occupier first, Bunkerable second).
- **Phase 4**: Evasive Stance.

## Open items

- Extract stock full UImd.ini from langmd.mix (Blowfish-encrypted header) if the 517-byte
  cncnet version turns out not to be the whole active file. Probably unnecessary — the
  game ran fine with just it.
- Button art pipeline: 5 SHPs per side? Check whether sidec0X lookup falls back to loose
  files / expandmd99.mix.
- Whether Hold Fire should also block force-fire (decide: yes per wishlist "do not fire").

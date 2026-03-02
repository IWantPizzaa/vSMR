**Target Outcome**

1. Replace the on-radar drawn profile editor with a real movable modeless window (can be moved to another screen).
2. Remove “conditional styling in tag token strings” as the primary workflow.
3. Keep live effect for every edit (no restart, no reload required).
4. Reorganize icon settings into a proper section, not pseudo-submenus.
5. Keep backward compatibility with existing profiles.

**High-Level Architecture**

1. Use a dedicated modeless MFC dialog host window (`CDialogEx`) for editing.
2. Split editor into pages (tabs or left tree + page panel): `Profiles`, `Display`, `Icons`, `Tags`, `Rules`, `Alerts`, `Preview`.
3. Keep rendering logic in radar code, but move editor state/UI logic out of `OnRefresh`.
4. Keep a single shared rules/helper layer in [`SMRRadar_TagShared.hpp`](C:/Users/Mathias/Documents/Coding/Cpp/vSMR/vSMR-1.0.75/vSMR/SMRRadar_TagShared.hpp).

**Data Model Redesign (Important)**

1. Keep tag lines (`L1..L4`) for display content only.
2. Move conditional styling to a separate structured rules block in profile JSON.
3. Add stable schema versioning for migration.
4. Example target shape for profile JSON:

```json
{
  "schema_version": 2,
  "labels": {
    "departure": {
      "definition": ["callsign", "deprwy tobt tsat"],
      "definitionDetailled": ["callsign actype", "deprwy tobt tsat ctot"]
    }
  },
  "rules": {
    "vacdm": [
      {
        "enabled": true,
        "when": { "token": "tsat", "state": "valid" },
        "effects": { "target_color": [0, 170, 0] },
        "priority": 100
      }
    ],
    "runway": [
      {
        "enabled": true,
        "when": { "token": "deprwy", "match": "26R" },
        "effects": { "tag_color": [32, 45, 90] },
        "priority": 50
      }
    ]
  },
  "ui_layout": {
    "profile_editor_window": { "x": 100, "y": 80, "w": 1200, "h": 780, "maximized": false }
  }
}
```

5. Keep old token-style rules readable (`tsat(state_valid=[...])`) during migration, but editor should save only new structured rules.

**Detailed Implementation Plan**

**Phase 1: Stabilize Current Split Before UI Rework**

1. Finalize shared helper ownership so helpers exist in one place only:
   - [`SMRRadar_TagShared.hpp`](C:/Users/Mathias/Documents/Coding/Cpp/vSMR/vSMR-1.0.75/vSMR/SMRRadar_TagShared.hpp)
   - Included by [`SMRRadar.cpp`](C:/Users/Mathias/Documents/Coding/Cpp/vSMR/vSMR-1.0.75/vSMR/SMRRadar.cpp) and [`SMRRadar_TagRendering.cpp`](C:/Users/Mathias/Documents/Coding/Cpp/vSMR/vSMR-1.0.75/vSMR/SMRRadar_TagRendering.cpp)
2. Ensure no duplicated helper definitions remain in `.cpp` files.
3. Keep build green for Win32/Release after each substep.

**Phase 2: Introduce Editor Window Shell (Modeless, Movable)**

1. Create `CProfileEditorDialog` with modeless lifecycle.
2. Add window open/close command from plugin menu or command handler.
3. Persist window geometry in `ui_layout.profile_editor_window`.
4. Remove dependency on fixed radar rectangle positions for editor.
5. Keep old drawn editor temporarily behind feature flag for safe transition.

**Phase 3: Build Page Structure**

1. Add page container (`CTabCtrl` or tree + stack panel):
   - Profiles
   - Display
   - Icons
   - Tags
   - Rules
   - Alerts
   - Preview
2. Move existing profile color controls into this window.
3. Move icon style/size controls into dedicated `Icons` page.
4. Remove the old pseudo “icon menu” path from on-radar UI.

**Phase 4: Tag Layout Editor UX Redesign**

1. Replace raw L1-L4 token strings with visual token chips:
   - Add token
   - Remove token
   - Reorder token
   - Optional style toggles (`bold`, fixed text)
2. Keep line-level editing possible, but token syntax should be abstracted by UI.
3. Disallow conditional color programming in tag-line tokens in UI.
4. Keep a compact “advanced raw text” fallback for power users (optional), but hidden by default.

**Phase 5: Build Dedicated Rules Editor (Replaces Token-Hacked Conditions)**

1. Add `Rules` page with rule list and editor panel.
2. Rule builder fields:
   - Source: `vACDM` / `Runway` / later extensible
   - Condition: token + state/match
   - Effects: `target_color`, `tag_color`, `text_color`, icon overrides
   - Priority and enable/disable
3. Provide predefined state lists (`tobt`, `tsat`, `ttot`, etc.) so users don’t hand-code state names.
4. Add rule conflict handling via priority and visible order.

**Phase 6: Live Update Pipeline (No Restart)**

1. Apply edits to in-memory config object immediately.
2. Notify radar to refresh:
   - Trigger lightweight redraw/update from editor callbacks.
3. Save-to-disk strategy:
   - Debounced autosave (for example 300–500ms after last change)
   - Explicit Save button optional
4. Reload dependent resources live:
   - Fonts when font changes
   - icon scaling/state when icon settings change
   - tag/rule caches invalidated when tag/rule configs change

**Phase 7: Migration Layer**

1. Add `schema_version`.
2. On load:
   - If version < 2, parse old rule tokens from tag definitions into structured `rules`.
   - Preserve original definitions for safety during first run.
3. On save:
   - Write only structured rules (optionally keep old compatibility snapshot).
4. Add one-time migration log output.

**Phase 8: Remove Old Drawn Editor Code**

1. Remove old editor draw/hitbox logic from `OnRefresh` and screen-object click handlers related only to old UI.
2. Keep only necessary radar drawing and interactions.
3. Clean dead fields in `CSMRRadar` tied to old editor rectangles/states.

**Phase 9: Performance/Cache Cleanup**

1. Precompile rule conditions at profile load (instead of repeated parse in refresh paths).
2. Keep per-frame cache for fast evaluation by target.
3. Invalidate compile cache only on relevant edits.
4. Verify no extra per-target string parsing in hot paths unless absolutely needed.

**Phase 10: Testing and Acceptance**

1. Functional:
   - move editor to second monitor
   - change tag line and see immediate radar update
   - create TSAT-valid target color rule and verify live behavior
   - icon settings edits apply live
2. Regression:
   - old profiles still load and render
   - no crash when editor closed/opened repeatedly
   - no stale rules after profile switch
3. Performance:
   - check FPS under event load
   - verify no added stutter from editor open

**Concrete File-Level Worklist**

1. Extend:
   - [`SMRRadar.hpp`](C:/Users/Mathias/Documents/Coding/Cpp/vSMR/vSMR-1.0.75/vSMR/SMRRadar.hpp)
   - [`Config.hpp`](C:/Users/Mathias/Documents/Coding/Cpp/vSMR/vSMR-1.0.75/vSMR/Config.hpp)
   - [`Config.cpp`](C:/Users/Mathias/Documents/Coding/Cpp/vSMR/vSMR-1.0.75/vSMR/Config.cpp)
2. Keep render integration:
   - [`SMRRadar_TagRendering.cpp`](C:/Users/Mathias/Documents/Coding/Cpp/vSMR/vSMR-1.0.75/vSMR/SMRRadar_TagRendering.cpp)
   - [`SMRRadar_TagShared.hpp`](C:/Users/Mathias/Documents/Coding/Cpp/vSMR/vSMR-1.0.75/vSMR/SMRRadar_TagShared.hpp)
3. New UI files (recommended):
   - `ProfileEditorDialog.hpp/.cpp`
   - `ProfileEditor_IconsPage.hpp/.cpp`
   - `ProfileEditor_TagsPage.hpp/.cpp`
   - `ProfileEditor_RulesPage.hpp/.cpp`
4. Resource updates:
   - [`resource.h`](C:/Users/Mathias/Documents/Coding/Cpp/vSMR/vSMR-1.0.75/vSMR/resource.h)
   - [`vSMR.rc`](C:/Users/Mathias/Documents/Coding/Cpp/vSMR/vSMR-1.0.75/vSMR/vSMR.rc)
   - project entries in [`vSMR.vcxproj`](C:/Users/Mathias/Documents/Coding/Cpp/vSMR/vSMR-1.0.75/vSMR/vSMR.vcxproj)

**Delivery Slices TODO**

1. [ ] Slice 1: modeless editor shell + window persistence + open/close wiring.
2. [ ] Slice 2: move existing profile color editor into shell, keep live updates.
3. [ ] Slice 3: icon page redesign and remove old icon pseudo-menu.
4. [ ] Slice 4: structured rules engine + migration + rules UI.
5. [ ] Slice 5: remove old drawn editor code and final cleanup.

# Smart Ground in vSMR

Integrated into `dev` from the `Smart-Ground` branch.
The integration uses the existing vSMR radar, tags, Control Center and plugin
bridges. No separate simulator or JavaScript runtime is required in EuroScope.

## Activation

Smart Ground is disabled at every plugin start. Its data is loaded only after
explicit activation. No Runtime Menu or Control Center switch enables it.
Enter `.smr SG` or `.smr smart ground` in EuroScope to toggle it for the current
runtime session. Commands are case-insensitive. Both aliases also accept `on`,
`off`, or `status` (for example `.smr SG off`). Activation is not saved to the
EuroScope settings, profiles or ASR files; reloading configuration keeps the
current session choice.

While disabled, the Route list item reads `No Data`, route and alert menus do
nothing, and no Smart Ground observation, routing, prediction, overlay or tag
alert runs. Turning it off clears tracked routes, alert suppression, drag previews
and pending edits; reactivation rebuilds routes from current traffic. Existing
RIMCAS, language, holding-point and other vSMR features remain independent.

## Controller interaction

Select a target on the radar or in a EuroScope list. Its remaining LFPG taxi
route appears on the main radar and the AVISO/secondary radar views. Selecting
another target changes the displayed route. Only measured progress removes
travelled segments; extrapolated movement does not erase an untravelled part.

Standard proposals are green. Drag a future route handle near an existing
intersection to assign a legal alternative; only changed segments are blue,
and segments shared with the official route remain green. Editing also works on
the recovery path of an Off Track aircraft. Accepting a legal new route clears
the previous Off Track state without creating a new position report. Right-click
a route handle to undo the previous edit when it is still compatible with the
observed position. Edits preserve the travelled prefix and all applicable hard
taxiway, aircraft and turn restrictions.
During a drag, the marker snaps to the nearest existing intersection within
60 metres. A faint route shows the proposed legal result before release. An
invalid candidate has a warning-coloured marker and no route preview. Moving
outside the source view or changing selection removes the preview. Previewing
does not change the assignment or acknowledge Off Track; release validates the
current track again before applying the route.

`SG COLL <callsign>` identifies the other target in a potential physical conflict.
`OFF TRACK` indicates a persistent difference from the current route. Both use
the existing tag alert band alongside RIMCAS. The Control Center alert switches
`SG COLLISION` and `OFF TRACK` disable their respective tag alerts independently
and use the existing saved settings. Existing RIMCAS warnings retain priority.

Click the `OFF TRACK` text for `Disregard`, `Disregard permanently`, or `Close`.
`Disregard` hides the current deviation episode until the aircraft rejoins its
route; a later deviation can alert again. `Disregard permanently` suppresses
Off Track warnings for that traffic's current session, including later episodes.
Neither action disables warnings for other aircraft. Disconnecting/forgetting
the traffic clears its suppression; a future flight reusing the callsign is new.

EuroScope lists can add the vSMR **Route** tag item and assign the vSMR **Route**
function to its click action. It displays the remaining taxiway sequence, such
as `N B3 D C`, and turns yellow while the aircraft is off track, including when
its alert has been disregarded. The SDK limits a list cell to 15 characters;
longer routes end in `...`. The route menu offers `Edit manually`, `Reset route`,
and `Close`. The manual editor contains the full sequence and accepts taxiway
names separated by spaces. Invalid or impossible sequences leave the current
assignment intact. Reset computes a legal standard continuation to the current
destination from the aircraft's observed position. Popup actions keep the
original aircraft even if the controller changes the selected target.

An off-track target can receive a legal recovery proposal from its observed
directed taxiway back towards its remaining destination and required waypoints.
The proposal does not silently replace the assigned route or grant a clearance.

## Intent and surface reports

The SDK bridge reads each position report once in the plugin callback, then
copies owned position, heading, speed, age and session values into the shared
engine. Additional radar windows and render frames never become independent
position reports. Receipt age uses a monotonic clock; the SDK's
`GetReceivedTime()` is an age, not an absolute timestamp.
Targets already present when vSMR loads are seeded once from their current SDK
report, with its original age. Repainting another window cannot refresh it.

The current LFPG configuration comes from active runway statuses, with the
existing vSID Paris interface as a fallback. Runway assignments use the flight
plan and vSID; an assigned stand comes from the existing Ramp Agent bridge, and
an explicit holding point uses the existing `VSMRHP` remark. Departures do not
require Ramp Agent: a stationary aircraft can use an unambiguous nearby connected
stand, and a target already taxiing can start on its observed legal directed
taxiway after independent positions establish its movement. These remain standard
proposals. The engine retains the inferred origin once the route is established.
Arrivals still require a known destination stand; their current position cannot
identify where they are assigned to park. Missing configuration cannot silently
combine east and west rules. Select the active LFPG runways in EuroScope, or
supply an explicit assigned runway. No stand connector or permission is invented,
and no commands or assignments are sent to other plugins.
Changing or clearing vSMR's existing Holding Point item updates the departure
destination immediately and recalculates from the observed position. A flight
plan amendment received from another controller follows the same path. This
does not reset the radar report's receipt age.

The four bundled LFPG catalogues retain the prototype's reviewed geometry,
east/west flows, main-axis preferences, deicing-pad avoidance, aircraft/span
restrictions and directed turn restrictions. Temporary works restrictions remain
excluded. Dataset provenance and input hashes are in
`vSMR/data/SmartGround/source_manifest.json`; detailed rule sources are in
`smart-ground/docs/OPERATIONS.md` and `AIRCRAFT-RESTRICTIONS.md`.

Position/speed/altitude filters select plausible LFPG surface targets; they are
not an SDK on-ground sensor. Off-track confirmation requires independent reports
over time, with ambiguity at intersections suppressing premature classification.
Stale tracks have bounded prediction and reports older than 20 seconds no longer
display a confident route or alert. A current holding point does not imply
permission for the following crossing. The SDK bridge has no authoritative
colored-axis/deicing-occupancy or crossing-clearance feed; it does not fabricate
those operational states.

Collision prediction uses aircraft length, wingspan, body heading, movement,
oriented body/wing/tail envelopes for A320/A388/B744/B748, swept intersections and
turn coverage. Other aircraft use an oriented conservative footprint. Separate
activation/release margins and fresh-report confirmation reduce flicker. These
are advisory predictions, not an operational safety certification.

## Validation and installation

Build the solution as `Release|Win32` with the existing MSVC toolchain. The normal
distribution contains `vSMR.dll` and `vSMR_Data`; the runtime and
`vSMR_Data/SmartGround` must accompany the loader. This feature does not require
an additional process. Keep personal configuration and `UserData` when installing.

The native regression executable includes exact source-edge/distance parity
vectors exported by the TypeScript planner, route editing, measured clipping,
aircraft/turn restrictions, sparse tracking and collision regressions. The
Control Center tests exercise independent switches and persistence.

For the optional local replay check, first create the anonymous caches using the
existing `smart-ground/scripts/import_euroscope_replays.py`, then run:

```powershell
vSMR/tests/bin/Release/vSMR.Tests.exe --smart-ground-replays .
```

This exercises the native engine using the recorded receipt intervals and the
previously inferred standard references. It verifies ingestion, duplicate
rejection, finite geometry, age and expiry invariants. It writes
`docs/smart_ground_native_validation.json`. The raw logs are read-only and are
not included in the package. The logs contain neither issued taxi clearances nor
labelled collisions, so differences from an inferred standard do not establish
pilot error, and the check cannot measure a false-positive rate. Replay body
orientation is estimated from geometric movement because the archived PBH
true/magnetic reference is unverified.

The original corpus check supplies provisional stand/access references and does
not cover the provider-free SDK path. Reproduce the offline departure regression
separately with `vSMR.Tests.exe --smart-ground-offline-replay .`: it uses the real
`PBN_09_04_26.txt` anonymous cache, empty stand and holding-point context, simulated
active west runways, gate starts and cold starts already taxiing. It also verifies
that arrivals without an assigned destination do not invent one.

Compilation and automated checks do not replace a live EuroScope session. Test
selection from both lists and symbols, dragging and tag placement on the user's
actual radar layouts before operational use.

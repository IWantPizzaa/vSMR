# vSMR Map Renderer IPC

This directory contains the shared protocol for the future x64 MapLibre
renderer process and the x86 EuroScope plugin client.

The protocol is intentionally limited to fixed-width POD structures:

- no C++ object pointers cross process boundaries
- no STL containers appear in mapped memory
- all strings are fixed-size UTF-8 byte arrays
- all published-frame selection fields are aligned for Windows interlocked reads

## Named Objects

Names are built from the prefixes in `MapRendererProtocol.hpp` plus a session
identifier chosen by the plugin supervisor.

- `Local\vSMR.MapRenderer.Command.<session>`
- `Local\vSMR.MapRenderer.Frame.<session>`
- `Local\vSMR.MapRenderer.Camera.<session>`
- `Local\vSMR.MapRenderer.Shutdown.<session>`

Using `Local\` keeps the renderer isolated to the current logon session and
avoids collisions with other users on the same machine.

## Frame Transport

The renderer writes complete BGRA frames into three slots. The plugin reads only
the latest published slot and keeps drawing its last valid local frame when no
new complete frame is available.

Publication order:

1. Pick a slot that is not currently published.
2. Mark the slot as rendering.
3. Write the full BGRA payload.
4. Fill slot metadata.
5. Issue a release barrier.
6. Publish slot index and frame sequence with Windows interlocked operations.

Consumer order:

1. Read published sequence and slot with acquire semantics.
2. Validate slot index, status, dimensions, stride, pixel format, and revision.
3. Copy or draw the complete frame without blocking the renderer.
4. Retain the last accepted frame when validation fails or no newer frame exists.

## Command Transport

The plugin writes only the latest camera, airport, and style state. The renderer
coalesces changes by reading the newest sequence values and ignores obsolete
camera states. Camera and style changes do not require reloading the GeoJSON
source. Airport changes do.

## Current Integration Status

This is phase 1 plumbing only. The current `dev` branch still uses EuroScope map
content refresh and sector-element visibility in `CSMRRadar::OnRefresh`; no
production rendering path consumes these structures yet.

# LFPG routine route quality validation

Generated with `npm run validate:route-quality`. The audit uses the production planner, all four offline catalogues and the declared A320 (35.8m) profile, with default SUP188 works and with all optional works profiles disabled. It verifies 1416 routes, 720,000 exact-edge positions and 400 source-protected physical runway crossings. No routine route uses any of the 67 catalogued treatment-pad edges. All generated traffic is nominal; manual runway crossing authority is preserved.

## Routing model

Hard AIP restrictions, current works, turn prohibitions, stand access phases and ordered nominal corridor gates are applied before route comparison. Routine taxi excludes the explicit, source-mapped treatment-pad lanes; published connectors next to those lanes remain available.

Among feasible routes, the search compares this tuple: foreign/returned apron transit metres; main-axis reentries after apron transit; physical metres plus bounded manoeuvre equivalents; physical metres. An actual change to a different named principal axis adds 120 equivalent metres, including changes across neutral connectors. Total absolute heading variation along the actual source polyline adds 90 equivalent metres per 90° of curvature. Thus a sampled S bend pays for both opposing curves even if no curve vertex is a branching decision. Splitting a straight source segment adds zero curvature; it cannot multiply the cost. A sufficiently shorter legal route can outweigh these finite preferences. These are simulator route-quality preferences, not published ATC minima.

The separate junction counter keeps its existing meaning: choices at branching nodes of at least 30°, measured with 40m tangents along adjoining degree-two curves. Neither that count nor its diagnostic `turnCostMetres` is added again to the search cost. `curvatureDegrees` and `curvatureCostMetres` expose the actual curve contribution. Source-scoped holding-approach profiles match exact physical destination nodes; they are nominal simulator routing preferences, with AIP restrictions and special-area exclusions still applied independently.

The existing no-U-turn option also rejects an immediate deflection of at least 150° between two distinct taxiway edges, using the incident source segments. This is a geometric interpretation of that simulator option, not a new AIP restriction or an aircraft-size limit. Ordinary right-angle junctions and sampled curves remain usable. Parking lead-ins keep their existing pushback abstraction.

The physical distance and simulator interpolation remain the original source-edge lengths. Quality fields are independently reconstructed during route description and checked against the stored result. Synthetic tests cover parallel-axis shortcuts, a justified shorter alternative, densification invariance, and rejection of pad transit during replay/injection.

## Holding access sweep

Eight source origins (B08, M09, B03, F02, A39, B12, J14, I24) are checked against every currently open departure access, excluding PPR accesses unless approved by the production defaults. Each route is checked for endpoint-specific nominal approach gates, source orientation, prohibited turns, pad exclusions, physical length and protected runway crossings.

The final 1000m additionally contains 7494 independently measured taxiway-to-taxiway transitions, at least two segments beyond a parking lead: zero immediate deflections of 150° or more. This is a scoped holding-approach audit, not a claim that every branch in the entire airport has been certified. For the short sweep alone, run `npm run validate:route-quality -- --holdings-only`.

| Works | Configuration | Open source stands | Active accesses | Valid routes |
| --- | --- | ---: | ---: | ---: |
| default | east | 8 | 7 | 56 |
| default | west | 8 | 6 | 48 |
| none | east | 8 | 7 | 56 |
| none | west | 8 | 6 | 48 |

## B12 departures to 26R

The source GMC direction on N remains northbound in west configuration. B12 follows its G3 egress and the legal southbound F corridor toward R/T. T9 is reached directly from R via RT11; T10–T12 use T. There is no requirement to touch T and return to the direct T9 connector. The user's requested continuity is preserved without reversing a source-backed one-way axis.

| Works | Access | Metres | Principal axes | Junction turns | Axis changes |
| --- | --- | ---: | --- | ---: | ---: |
| default | T9 | 4413.4 | F → R | 1 | 1 |
| default | T10 | 4664.1 | F → R → T | 4 | 2 |
| default | T11 | 4776.4 | F → R → T | 2 | 2 |
| default | T12 | 4894.1 | F → R → T | 4 | 2 |
| none | T9 | 4413.4 | F → R | 1 | 1 |
| none | T10 | 4664.1 | F → R → T | 4 | 2 |
| none | T11 | 4776.4 | F → R → T | 2 | 2 |
| none | T12 | 4894.1 | F → R → T | 4 | 2 |

## Automatic traffic

Each row checks 100 aircraft for 600 motion updates (0.25s each); 60,000 exact-edge positions. Every route is replayed through effective restrictions and physical runway-crossing checkpoints are checked using source holding locations. Pending runway clearances also stop future trajectory predictions.

| Works | Configuration | Movement | Aircraft | Generation ms | Junction turns, total | Axis changes, total |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| default | east | departure | 100 | 1026.4 | 290 | 128 |
| default | east | arrival | 100 | 2376.1 | 283 | 127 |
| default | east | transfer | 100 | 1691.4 | 453 | 109 |
| default | west | departure | 100 | 850.7 | 341 | 140 |
| default | west | arrival | 100 | 1602.9 | 286 | 127 |
| default | west | transfer | 100 | 1727.7 | 424 | 100 |
| none | east | departure | 100 | 698.9 | 294 | 128 |
| none | east | arrival | 100 | 2043 | 261 | 126 |
| none | east | transfer | 100 | 1446.3 | 412 | 95 |
| none | west | departure | 100 | 841 | 301 | 131 |
| none | west | arrival | 100 | 1782 | 267 | 120 |
| none | west | transfer | 100 | 1367.4 | 421 | 98 |

## Reproducibility

Seed: 20261112. Node.js: v24.20.0. Elapsed: 22.10s. Timings measure algorithms in Node.js and exclude Canvas rendering.

- `public/data/lfpg.json`: `4597cfcb70d555ca801427a378d7f8fca297e55677d3062faed0c1718ed5b9c5`
- `public/data/lfpg-operations.json`: `b6895a31356631254f3afde5b9e0624f7462203dd328f5d1660467f50784aa83`
- `public/data/lfpg-corridors.json`: `9228e0c4e9293426c2318ea032ce5ac07fc331595201b8c6eeaa6916cdf8def3`
- `public/data/lfpg-aircraft-restrictions.json`: `02e28bb4f889c615533bdec17f87f21b360b13990635f6fb2ef441c29b489eda`

The source snapshot still contains geographic gaps and does not model every current NOTAM or coordinated ATC deviation. The audit demonstrates consistency against the shipped source overlay; it does not certify every real-world taxi clearance.

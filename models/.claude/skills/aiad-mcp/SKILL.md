---
name: aiad-mcp
description: >
  How to operate the Tootega AIAD CAD/CAE app through its `aiad` MCP tools (mcp__aiad__*): read and
  change the open model (parameters and formulas, parts as feature histories, materials, bodies),
  check the result (viewport snapshot, derived physics, engine simulation) and undo. Use whenever the
  user asks to inspect, modify, fix, create or simulate anything in the model, mentions a part,
  dimension, material, section, view, rpm/power, or says "in the app", "in the drawing", "no desenho",
  "no modelo", "no motor".
---

# Operating Tootega AIAD through MCP

The app shows a **document** (the drawing). Everything in the viewport is a part of it, built from
features by the kernel. You change it **only** through the `aiad` MCP tools; each tool runs the app's
Command Bus — the same commands the user's panels run, with validation, **undo** and an audit trail
where your actions appear as `ai`. Never write geometry yourself in files or code: describe it as
features.

Units: **metres** and **degrees** everywhere (a bore of 80 mm is `0.08`). Crank angles of the engine
are in the 720-degree cycle, 0 = firing TDC.

## Workflow

1. **Read before you change.** `aiad_parameters_get` (driving dimensions and formulas),
   `aiad_parts_list` (parts, bodies, materials, masses, errors), `aiad_part_get` (one part's full feature
   history), `aiad_engine_get_derived` (what the simulation uses).
2. **Change with the smallest tool that does it**: a value → `aiad_parameters_set`; one field of one
   feature → `aiad_feature_update`; a part's whole history/material/role → `aiad_part_update`; a new
   part → `aiad_part_create`.
3. **Verify.** `aiad_viewport_snapshot` (look at it), `aiad_engine_get_derived`, and for performance
   `aiad_sim_run` + poll `aiad_sim_job_status` until `state` is `done` (each poll is cheap; wait a
   second or two between polls).
4. **Report** what changed, with numbers. Every change can be undone (`aiad_undo`, or Ctrl+Z by the
   user); `aiad_history` lists the steps.
5. **Saving** (`aiad_doc_save`) overwrites the model file: do it only when the user asks.

Changes are **atomic**: if any affected part cannot be rebuilt, nothing changes and the error names the
part and the feature. Read the error, fix the input, try again; do not retry blindly.

## Tools

| Tool | Use |
|---|---|
| `aiad_parameters_get` | All parameters: definition (number or formula), value, unit, range, meaning |
| `aiad_parameters_set` | `{"values": {"bore": 0.084, "head_r": "R + 0.014 * s"}}` — numbers or formulas; regenerates every dependent part and the physics |
| `aiad_parameter_define` | New parameter (or new definition): `{"name","value","unit","description","min","max"}` |
| `aiad_parts_list` / `aiad_part_get` | Inventory / one part's features, material, role, mass properties |
| `aiad_part_create` | `{"name","body","material","role"?,"sectioned"?,"features":[...]}` |
| `aiad_part_update` | `{"part", ...any of name/body/material/role/sectioned/features}` |
| `aiad_feature_update` | `{"part","index" (from 1),"set":{field: value}}` |
| `aiad_part_delete` | Remove a part (undoable) |
| `aiad_materials_list` / `aiad_material_define` | Material table: density (drives the masses), colour, metallic, roughness, emission |
| `aiad_body_define` | Rigid body and its rest origin (formulas allowed) |
| `aiad_engine_get_derived` | Displacement, chamber volume, crank inertia, rod/piston masses, measured valve lift, magneto timing |
| `aiad_sim_run` / `aiad_sim_job_status` | Measured run of the coupled simulation (Chrono + 0D Otto cycle + magneto): rpm, indicated power, torque, peak pressure, "runs on its own" |
| `aiad_sim_start` / `aiad_sim_stop` / `aiad_sim_status` | Live animation in the viewport |
| `aiad_viewport_snapshot` | PNG of the viewport as the user sees it |
| `aiad_view_set` | `{"view": "iso|front|back|right|left|top", "fit": true, "section": true|false}` |
| `aiad_part_set_visible` / `aiad_selection_set` | Show/hide a part, highlight it |
| `aiad_undo` / `aiad_redo` / `aiad_history` | Undo stack shared with the user |
| `aiad_doc_save` / `aiad_doc_orient` | Save the document; orientation of the whole model |

Parts are referenced by exact name, a unique case-insensitive fragment (`"cam cover"`), or path.

## Feature histories

A part is an ordered list of features. The first creates the solid; the next ones combine with it
through `"mode": "add"` (default), `"cut"` or `"intersect"`. Any number may be a **formula** string over
the parameters (`"bore / 2 + 0.0035"`; functions: min, max, sqrt, abs, sin, cos, tan, asin, acos,
atan2, mod, floor — trigonometry in degrees; `pi`).

| op | fields |
|---|---|
| `extrude` | `plane`, `profile`, `distance`, `start` (offset along the plane normal) |
| `revolve` | `plane`, `profile`, `axis` `[u, v, du, dv]` (default `[0,0,0,1]` = the sketch's v axis), `angle` (360), `start_angle` |
| `cylinder` | `base` `[x,y,z]`, `axis` `[x,y,z]`, `radius`, `height` |
| `box` | `corner`, `size` |
| `sphere` | `center`, `radius` |
| `helix_pipe` | `base`, `axis`, `coil_radius`, `height`, `turns`, `wire_radius` (springs) |
| `pipe` | `path` `[[x,y,z], ...]` (at least 4 points, smooth), `radius` — keep bends wider than the radius |
| `pattern_linear` | `count`, `step` `[x,y,z]` — repeats the previous feature |
| `pattern_polar` | `count`, `axis_point`, `axis`, `step_angle` |
| `rotate` / `translate` | `pivot`, `axis`, `angle` / `offset` — moves the whole part built so far |

**Planes**: `"XY"`, `"YZ"`, `"ZX"`, `"XZ"`, `"YX"`, `"ZY"` — first letter = sketch u axis, second = v axis;
or `{"base": "XZ", "origin": [x,y,z], "offset": d}`; or `{"origin": [..], "x": [..], "y": [..]}`
(normal = x × y). Useful conventions in this model:
- solids of revolution about the **crank axis Z**: plane `"XZ"`, profile as `[radius, z]`, default axis;
- solids of revolution about the **cylinder axis X**: plane `"YX"`, profile as `[radius, x]`, default axis.

**Profiles (sketches)**: a closed vertex list `[[u, v], [u, v, bulge], ...]` — each vertex starts a
segment to the next (the last closes on the first); `bulge = tan(sweep/4)` makes it an arc
(+ = counter-clockwise; 1 = half circle; ±0.4142 = quarter circle). Or one sketch tool:
`{"circle": {"center": [u,v], "radius": r}}`, `{"rect": {"min": [u,v], "max": [u,v]}}`,
`{"rounded_rect": {"min","max","radius"}}`, `{"polygon": {"center","across_flats","sides","rotation"}}`,
`{"sprocket": {"root_radius","tip_radius","teeth"}}`, `{"hull": {"circles": [{"center","radius","from","to"}]}}`
(convex hull, exact arcs and tangents), `{"cam": {"base_radius","lift","open","close"}}` (flat-follower
cam, sin² law, crank angles; follower on -u; extrude only).

Example — a flanged ring about the cylinder axis with four bolt holes:

```json
{"name": "Exhaust flange", "body": "Frame", "material": "Steel 42CrMo4", "features": [
  {"op": "revolve", "plane": "YX", "profile": {"rect": {"min": ["R + 0.004", "deck_x - 0.006"], "max": ["R + 0.020", "deck_x"]}}},
  {"op": "cylinder", "mode": "cut", "base": ["deck_x - 0.010", "R + 0.012", 0], "axis": [1, 0, 0], "radius": 0.0025, "height": 0.02},
  {"op": "pattern_polar", "count": 4, "axis_point": [0, 0, 0], "axis": [1, 0, 0], "step_angle": 90}]}
```

## This model: the single-cylinder engine

- Mechanism frame: crank axis **Z**, cylinder axis **+X** (the model is turned upright for viewing).
  Bodies: `Frame` (fixed), `Crankshaft` (turns about Z), `Rod` (crank pin at its origin, small end on
  +X), `Piston` (origin at the wrist pin), `Camshaft` (origin at the cam centre, half crank speed),
  `IntakeValve`/`ExhaustValve` (origin on the valve seat at the deck), `IntakeSpring`/`ExhaustSpring`
  (origin on the head; scaled by the lift). A part's coordinates are in its body's frame.
- Driving parameters: `bore`, `stroke`, `rod_length`, `compression_ratio`, valve diameters,
  `intake_open/close/lift`, `exhaust_open/close/lift`, `cam_base_radius`, flywheel sizes, `spark_advance`,
  magneto geometry (`magneto_*`), study inputs (`load_coefficient`, `starter_*`). Many others are formulas
  of these (`s` = bore/0.080 scales transverse sizes, `deck_x`, `head_top_x`, `cam_centre_x`, ...).
  Changing a formula's inputs changes every part that uses it — that is intended.
- **The simulation reads the drawing**: masses and inertias come from the parts and their materials per
  body; valve lift is **measured on the parts with role `intake_lobe` / `exhaust_lobe`**; geometry from
  the parameters. Keep those roles (and `spark`, `magnet`, `flywheel`) on the right parts, or the study
  reports what is missing.
- The section view cuts the parts with `"sectioned": true` at y = 0 and shows the back half; parts that
  must stay visible behind the cut (ports, pipes) should run on +y.
- Typical checks after a mechanical change: `aiad_engine_get_derived`, then `aiad_sim_run` with
  `{"seconds": 2.5}` (the starter runs for `starter_duration`; "runs on its own" needs more than that).

## Pitfalls

- Values are metres: `0.08`, not `80`. Ranges are enforced; the error tells the range.
- A profile vertex list must not repeat its first vertex at the end, and must not cross itself.
- Revolve profiles must stay on one side of the axis (they may touch it).
- `pipe` fails when a bend is tighter than the pipe radius: add path points or widen the bends.
- Interference is not checked automatically: after moving or resizing parts, look at a snapshot from
  more than one view (and with `"section": false`) to see collisions.
- Do not delete or rename parts the user did not ask about; prefer editing features.

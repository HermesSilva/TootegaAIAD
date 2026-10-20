---
name: aiad-mcp
description: >
  How to operate the Tootega AIAD CAD/CAE app through its `aiad` MCP tools (mcp__aiad__*): read and
  change the open model (parameters and formulas, parts as feature histories, materials, bodies, joints,
  machine elements), check the result (the simulator's critique, viewport snapshot, simulation) and undo. Use whenever the
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

Units: **metres** and **degrees** everywhere (a bore of 80 mm is `0.08`); joint motors and coordinates in
radians, rad/s, metres.

## Workflow

1. **Read before you change.** `aiad_parameters_get` (driving dimensions and formulas),
   `aiad_parts_list` (parts, bodies, materials, masses, errors), `aiad_part_get` (one part's full feature
   history), `aiad_joints_list` / `aiad_elements_list` (the machine), `aiad_mechanism_check` (what the
   simulator measures on the drawing and what it criticises).
2. **Change with the smallest tool that does it**: a value → `aiad_parameters_set`; one field of one
   feature → `aiad_feature_update`; a part's whole history/material/role → `aiad_part_update`; a new
   part → `aiad_part_create`.
3. **Verify.** `aiad_viewport_snapshot` (look at it), **`aiad_mechanism_check`** (the simulator's critique
   of the drawing: every problem with its fix, and everything it measured), and for behaviour
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
| `aiad_materials_list` / `aiad_material_define` | Material table: density (drives the masses), `shear_modulus` (springs), `remanence` (magnets), `relative_permeability` (cores), colour, metallic, roughness, emission |
| `aiad_body_define` | Rigid body and its rest origin (formulas allowed); `fixed: true` = part of the ground |
| `aiad_instruments_list` / `aiad_instrument_define` / `aiad_instrument_show` / `aiad_instrument_delete` / `aiad_instrument_read` | Measuring instruments with ideal probes (see "Measuring instruments") |
| `aiad_adjust` | `{"joint" or "part", "value"}`: sets an adjuster (a part set by hand, see Machines step 5) inside its range; undoable |
| `aiad_joint_define` / `aiad_joint_delete` / `aiad_joints_list` | Joints between bodies (`revolute`, `prismatic`, `cylindrical`, `spherical`, `planar`, `fixed`; `body1` omitted = ground; `point`, `axis` as formulas; optional `motor` `{"mode": "speed"\|"torque", "value": ..., "free_speed"?, "duration"?}` in rad/s, m/s, N m or N; optional `limits` `[lower, upper]` = stops, e.g. a valve seat `[null, 0]`; optional `start`: where it starts, a value or `{"local_time": period_s, "offset": rad}` = from the computer's date and time, see Machines step 5) |
| `aiad_element_define` / `aiad_element_delete` / `aiad_elements_list` | What else the machine is made of, tied to joints and drawn parts: `gear`, `belt`, `rack`, `contact` (cam / rocker pad on a flat face), `spring`, `torsion` (spiral spring), `touch` (outlines that touch: escapements, ratchets), `load`, `chamber` (gas), `magneto` (see below) |
| `aiad_mechanism_check` | The simulator's critique of ANY drawing: ground, bodies not connected, joints outside their parts, gears that do not mesh, belts that miss their wheels, cams that never reach (or never release) their followers, springs that do not fit, valves without seat/spring/drive, chambers and magnetos that do not match the drawing — each with its fix; plus everything it measured (ratios, lift, stiffness, bore, chamber volume, spark angle) |
| `aiad_mechanism_run` | `{"seconds": s}` (up to 5): runs the machine and returns every joint (position, range, mean speed), chambers, magnetos, how far each body moved |
| `aiad_sim_run` / `aiad_sim_job_status` | Measured run in the background (up to 30 s): joint speeds, chambers (displacement, compression ratio, peak pressure, combustions, indicated power), magnetos, and `runs_on_its_own` (does it keep going after its timed motors stop) |
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

## Machines: any machine, from the drawing

The simulator is generic: it knows no machine. A machine is, all in the document:

- **bodies** — rigid groups of parts; mass, centre of mass and inertia come from their parts and materials;
- **joints** — how bodies move (with motors and stops);
- **elements** — what else it is made of, tied to joints and to drawn parts. Everything physical is
  measured on the parts: teeth and pitch radii, cam profiles, coil wire and turns, bore, chamber volume,
  valve sizes, magnet and core geometry.

Build one like this:

1. Make the base part of the ground: `aiad_body_define {"body": "Frame", "origin": [0,0,0], "fixed": true}`.
2. Put each moving part in a body of its own (`aiad_part_create` with `body`), one body per rigid group.
   Draw it **assembled** (where it is at rest); a body's origin is where its parts' frame sits.
3. Join the bodies where they meet. A shaft in a bearing:
   `aiad_joint_define {"name": "Crank bearing", "type": "revolute", "body0": "Crankshaft", "body1": "Frame",
   "point": [0, 0, 0], "axis": [0, 0, 1]}`. A pin in a hole puts the point on the pin's axis. A ball end is
   `spherical`; a slide is `prismatic`, with `limits` for its stops.
4. Add the elements (`aiad_element_define {"name", "type", "definition": {...}}`):
   - `gear` `{joint0, joint1, part0, part1, internal?}`: two drawn gears (sketch tool `gear`, or `sprocket`)
     on parallel shafts held by the same body. Ratio from the teeth, direction from the mesh; an idler
     needs its own body and joint.
   - `belt` `{joint0, joint1, part0, part1, belt}`: a drawn belt or chain around two drawn wheels (same
     direction).
   - `rack` `{joint0 (pinion), joint1 (prismatic rack), part0, part1?}`.
   - `contact` `{part, body, point, normal, radius}`: the profile of `part` pushes the flat face of `body`.
     The profile is an extruded sketch: a cam lobe (tool `cam`), a rocker pad (a `hull`). The face is given
     by its centre `point` and its `normal` (pointing toward the profile) as drawn, and its `radius`.
     Unilateral: the follower can lift off.
   - `spring` `{part, body0, point0, body1, point1, free_length}`: a drawn coil (`helix_pipe`), in a body of
     its own, between two bodies. Its material needs `shear_modulus`. The view stretches it.
   - `torsion` `{joint, part, free?, damping?, holder?}` (holder: the part holding the strip near its outer end,
     a regulator's index pins: see Adjusters): a drawn spiral spring (hairspring, mainspring) acting on the joint
     it winds around: sketch tool `spiral`, or any imported spiral solid. The strip (length, thickness, height) is
     measured on the part's section; its material needs `elastic_modulus`. `free` is the joint angle where it is
     relaxed (a mainspring wound n turns: n·2π, its sign the way it drives).
   - `touch` `{part0, part1, at?, stiffness?}`: two parts of different bodies that touch by their outlines in the
     plane the bodies turn in (escape wheel teeth and pallet stones, impulse pin and fork, guard pin and safety
     roller, ratchet and click, a fork and solid bankings). The outlines are cut from the solids (drawn or imported)
     where their heights overlap, or at `at` (a height along the axis) when a part has details at other heights.
   - `load` `{joint, constant?, linear?, quadratic?}`: resisting torque or force (friction, brake, propeller).
   - `chamber` `{piston, cylinder, head, intake: [{joint, part}], exhaust: [...], ignition?, crank?}`:
     the gas between a piston (its prismatic joint), the bore (the part whose inside it is) and the head.
     The valves need a seat (a joint stop at the drawn closed position), something that opens them (a
     contact) and a spring.
   - `magneto` `{rotor, magnet, shoes, core, coil?, points_open}`: magnet and pole shoes on the rotor's body,
     the U core (two legs and a yoke) on the stator. The magnet's material needs `remanence`. The spark
     fires the chamber that names it in `ignition`.
5. Drive the input: a joint `motor` (`speed`, or `torque` with `free_speed` and `duration` for a starter).
   **Start position, and starting at the computer's date and time.** A joint may start away from its drawn
   position with `start` in `aiad_joint_define`: a number or formula (rad or m), or
   `{"local_time": period, "offset": rad}` (revolute or cylindrical) to take it from the computer's local date
   and time **when the simulation starts** (`aiad_sim_start` and every run read the clock then):
   coordinate = offset + 2π · (local seconds since 1970-01-01 00:00 mod period) / period. The body turns there
   with what is fixed to it; gears and belts take the positions as they start, so setting one wheel drags no
   train (the escape wheel keeps its phase with the pallets). Typical uses:
   - a watch or clock in sync with the computer: the joint carrying each hand gets `{"local_time": 60}`
     (seconds), `3600` (minutes: the centre wheel / cannon pinion), `43200` (hours: the hour wheel); a 24 h
     hand `86400`; a day-of-week disc `604800` (1970-01-01 was a Thursday: set `offset` to match);
   - `offset` = the joint angle where the hand points at 12 when the drawn hands do not (measure it on the
     drawing); the period's sign must match the way the train turns that joint (run 1 s and read the sign of
     its speed: negative → use a negative period).
   Check: `aiad_mechanism_check` lists each joint that starts at the local time ("starts at the local time,
   one turn per ... s"); `aiad_sim_status` right after `aiad_sim_start` shows the coordinates.
   **Adjusters: parts set by hand.** A joint with a range (`limits` on both sides) that nothing in the machine
   drives (no motor, gear, belt, spring, contact or touch on it) is recognised as an **adjuster**: set by hand,
   as in real life, and held where it is set while the machine runs. Examples: a watch's regulator (index), an
   idle screw, a setting lever, a crown pulled out. To make one, put the part in a body of its own (moving), join
   it where it pivots or slides, and give the joint its range:
   `aiad_joint_define {"name": "Regulator", "type": "revolute", "body0": "Regulator index", "point": [...],
   "axis": [0,0,1], "limits": [-0.15, 0.15]}`. `aiad_mechanism_check` then reports "Adjuster ...: set by hand".
   Set it with `aiad_adjust {"joint": "Regulator", "value": -0.05}` (rad or m, inside the range; undoable; the
   joint's start). In the app, selecting one of its parts shows its control on the part (a lever on an arc for a
   joint that turns, a slider for one that slides) and in Properties ("Set by hand"); dragging it moves the part
   at once and commits on release.
   What an adjuster changes must be measured from where it is: a spiral spring (`torsion`) may name a `holder`
   (the regulator's index pins on its outer coil): only the strip up to it works, so turning the index changes
   the active length and the rate (ETA 6497: ±0.15 rad ≈ ±2.3 min/day).
   **A part's features never make it movable**: they are its geometry (import, sketch, extrude, translate,
   rotate). A `translate`/`rotate` feature only redraws it elsewhere, still fixed. Being set by hand comes from
   its **body** (its own, not fixed) and its **joint** (pivot or slide, with `limits`). When the user asks to
   make a part adjustable, define those; do not edit its features.
   **ETA 6497 regulator** (models `Clock*.usda`): the index is "303 Two–piece regulator with pointer" [1], [3],
   [4] (the dash is the character U+0096 from the STEP import; copy the name from `aiad_parts_list`); [4] holds
   the two pins on the hairspring's outer coil; [2] and [5] are the stud carrier (it stays fixed). The ready
   script is `models/Clock_regulator.aiad.json` (`aiad-model apply <doc> models/Clock_regulator.aiad.json`):
   body "Regulator index" with [1], [3], [4]; joint "Regulator", revolute about the balance axis, limits
   [-0.15, 0.15]; the hairspring drawn up to the stud (13.18 turns, 33.36 µm, 18,000 vph at 0) with `holder`
   [4]. Then `aiad_adjust {"joint": "Regulator", "value": ...}`: + slows, − speeds up. Not adjusters in that
   movement: the screws (5125, 5121...) only hold bridges.
6. Run `aiad_mechanism_check`, apply its fixes, and check again until it finds nothing. Then run it:
   `aiad_mechanism_run {"seconds": 1}`, or `aiad_sim_run` for a measured run.

## Measuring instruments

Instruments observe the machine; they are not part of it. Their **probes are ideal**: they read the solver's
state at every step and change nothing (no mass, no force, no step). Events (crossings, turning points) are
timed between steps by interpolation. An instrument is saved with the document, shown or hidden, and undoable.

- `aiad_instrument_define {"name", "type", "definition": {"probes": [{"joint": "..."} | {"element": "..."},
  "quantity"?], "unit"?, "digits"?, "gate"?, "window"?, "visible"?, ...}}` plugs one in (or replaces it).
  - Quantities: joint `position`, `speed`, `acceleration`; chamber `pressure`, `temperature`, `volume`;
    magneto `voltage`; touch, contact, spring `force`; torsion `torque`. Each type has a default.
  - `digits`: decimals (0-9). `gate`: averaging time in s (0 = live). `window`: the chart's span in s.
- Types (`aiad_instruments_list` gives the catalogue and their units):
  - `tachometer` (rpm...): the speed of a joint;
  - `oscillation`: a watchmaker's timegrapher. It gives the beat rate (vph), frequency, period, beat error,
    amplitude, centre, and rate in s/day against `reference` (vph);
  - `manometer`, `thermometer` (a chamber), `protractor` (angle; `wrap` for 0-360°), `dial` (a slide's
    displacement), `gauge` (force or torque), `voltmeter` (a magneto's spark), `accelerometer`;
  - `timer`: `mode` interval, frequency, count or stopwatch, `edge` rise, fall or both, `threshold` in SI
    (default: the middle of the swing over the window);
  - `meter`: any quantity; `chart`: up to four probes over time.
- `aiad_instrument_show {"name", "visible"}`, `aiad_instrument_delete {"name"}`.
- `aiad_instrument_read {"name"?}` gives the readings of the live run. Start it with
  `aiad_sim_start {"time_scale": 1}`, wait, then read. It returns the main value, secondary values
  (min/max/mean, beat error...) and a status.
- In the app:
  - each visible instrument is a panel with a seven-segment display, secondary readouts, HOLD, RESET, unit,
    decimals, gate and chart window, and its chart;
  - its probe is marked in the view with its reading;
  - the user plugs one with a right click on a part, in the view or in the tree: the menu lists the joints that move the part and the elements that use it, each with the instruments that can read it (a seconds hand: its arbor, "Second wheel arbor" → tachometer); View > Instruments shows and hides them.
- Examples:
  - ETA 6497: `{"name": "Timegrapher", "type": "oscillation", "definition": {"probes": [{"joint":
    "Balance staff"}], "reference": 18000}}` reads 17,998 vph, beat error 0.3 ms, amplitude ~140°;
  - a beat counter: `timer` on "Pallet staff", `edge` both → 200 ms per beat;
  - an engine: `manometer` on the chamber, `tachometer` on "Main bearing", `dial` on a valve guide.

**When the simulator refuses a drawing, the drawing is what to fix**, never the application. Its findings
say what and how. If something truly cannot be expressed with these tools, tell the user. Do not ask to
change the app's code.

## This model: the single-cylinder engine

- Mechanism frame: crank axis **Z**, cylinder axis **+X** (the model is turned upright for viewing). It is
  drawn assembled at the **firing TDC**: crank pin on +X, piston at the top.
- Bodies:
  - `Frame` (fixed);
  - `Crankshaft` (with flywheel and magnet);
  - `Rod` (origin on the crank pin);
  - `Piston` (origin at the wrist pin);
  - `Camshaft`;
  - `IntakeValve`/`ExhaustValve` (origin on the seat; in the OHC engine they include the bucket tappets);
  - `IntakeSpring`/`ExhaustSpring` (spring bodies).
  The pushrod engine adds `IdlerGear`, tappets, pushrods and rockers. A part's coordinates are in its
  body's frame.
- Joints:
  - main bearing (with the starter: a timed torque motor with a DC curve);
  - crank pin and wrist pin;
  - piston bore;
  - camshaft bearing;
  - valve guides (stop `[null, 0]` = the seat).
- Elements:
  - the timing drive (OHC: a `belt`; pushrod: two `gear`s through the idler);
  - cam `contact`s (pushrod: also the rocker pads on the valve tips);
  - valve `spring`s;
  - crank friction and propeller `load`;
  - the `Magneto`;
  - the `Cylinder` chamber.
- Driving parameters:
  - `bore`, `stroke`, `rod_length`, `compression_ratio` and the valve diameters;
  - `intake_open/close/lift` and `exhaust_open/close/lift`: they shape the drawn lobes, and the simulation
    reads the lobes;
  - `cam_base_radius`, `valve_lash`, `spring_preload`;
  - the flywheel sizes, `spark_advance` (where the magnet sits on the rotor) and `points_open_deg`;
  - the magneto geometry (`magneto_*`), `load_coefficient` and `starter_*`.
  Many others are formulas of these (`s` = bore/0.080 scales transverse sizes, `deck_x`, `head_top_x`,
  `cam_centre_x`, ...). Changing a formula's inputs changes every part that uses it — that is intended.
- The section view cuts the parts with `"sectioned": true` at y = 0 and shows the back half. Parts that must
  stay visible behind the cut (ports, pipes) should run on +y.
- Typical checks after a mechanical change: `aiad_mechanism_check`, then `aiad_sim_run` with
  `{"seconds": 2.5}`. The starter runs for `starter_duration`; "runs on its own" needs more than that.

## Pitfalls

- Values are metres: `0.08`, not `80`. Ranges are enforced; the error tells the range.
- A profile vertex list must not repeat its first vertex at the end, and must not cross itself.
- Revolve profiles must stay on one side of the axis (they may touch it).
- `pipe` fails when a bend is tighter than the pipe radius: add path points or widen the bends.
- Interference is not checked automatically: after moving or resizing parts, look at a snapshot from
  more than one view (and with `"section": false`) to see collisions.
- Do not delete or rename parts the user did not ask about; prefer editing features.

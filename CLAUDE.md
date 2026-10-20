# CLAUDE.md — Tootega AIAD project directives

> Read by Claude Code (and any agent) working in this repository. These directives were decided
> by the project owner; do not change them without asking. Detailed rationale lives in
> `Docs/01-levantamento.md`, `Docs/02-arquitetura.md` and `Docs/03-plano-execucao.md` (plan v0.4).

---

## 1. Product

| Field | Value |
|-------|-------|
| Name | **Tootega AIAD** — Artificial Intelligence-Aided Design |
| What | 3D CAD/CAE tool where the user **and an AI** operate the same exact (B-rep NURBS) model |
| Maintainer | Tootega Pesquisa e Inovação · GitHub: **HermesSilva** |
| License | **Open source.** Follow whatever the dependencies require; current recommendation: **Apache-2.0** |
| **Target milestone** | **A four-stroke internal combustion engine, designed in the tool and running in its simulator** (set by the owner on 2026-10-18). Work is prioritized by what this milestone needs. |

## 2. Hard rules (non-negotiable)

1. **Windows only** — x64, Windows 10/11. No Linux, no macOS, **no WSL**, no multi-platform code paths.
2. **Graphics = DirectX 12.** No OpenGL, no Vulkan in the product.
3. **Compute = CUDA** (NVIDIA). GPU and D3D12 share the same adapter (matched by LUID).
4. **Static linking for everything we build or vendor.** One self-contained `aiad.exe` (plus
   solver executables). The only allowed dynamic dependencies are:
   - OS components (D3D12, DXGI, Win32);
   - the NVIDIA driver (`nvcuda.dll`);
   - the runtime shader compiler (`dxcompiler.dll` / `dxil.dll`), because shaders are generated at run time;
   - MS-MPI runtime, only inside solver executables;
   - cuSPARSE / cuBLAS (`cusparse64_*.dll`, `cublas*64_*.dll`), used by MFEM — NVIDIA ships no
     static versions of them on Windows (the CUDA runtime itself is linked static: `cudart_static`);
   - the MSVC C/C++ runtime (`/MD`, VC++ Redistributable) — deliberate, to avoid CRT mismatches
     with the DLLs above.
   Consequence: OpenUSD is built **static from source** (no Python), and USD plugins (HdAurora,
   hdUsdBrep, omniSolid, aiad schema) are linked **into the executable** with `/WHOLEARCHIVE`.
5. **English only** in code, comments, commit messages and the **software UI**. No i18n layer.
   (Planning documents under `Docs/` are currently in Portuguese.)
6. **Toolchain:** Visual Studio 18 (2026) Community, **MSVC toolset v145**, C++20, CUDA 13.x. The application
   builds with **native Visual Studio projects**: one versioned `.vcxproj` per project next to its code, the
   solution `TootegaAIAD.slnx` at the repository root (owner, 2026-10-19: a checkout must build from Visual
   Studio as is; no CMake for the app). Third-party libraries keep their own build systems (CMake + Ninja,
   VS 18's CMake 4.3+) through the `deps` scripts; every one is built with v145.
7. **OpenUSD is the source of truth.** The document *is* a `UsdStage` (geometry, materials,
   simulation studies). No parallel internal model.
8. **One path to mutate the model: the Command Bus.** UI, AI and scripts run the same commands,
   with the same undo, validation and audit trail.
9. **The AI is just another operator.** Everything it does is visible, permission-gated and undoable.
10. **Do not reimplement the agent.** The agent loop lives in the provider (initially Claude Code
    CLI, `stream-json`). The app renders events and implements the client side of permissions,
    questions and plans (lessons from the Cockpit project).

## 3. Stack (decided)

| Area | Choice |
|------|--------|
| Geometry kernel | NVIDIA **USD BRep** (SMLib), `omniSolid` schema (`BrepArray`), `hdUsdBrep` Hydra plugin |
| Scene / file | **OpenUSD 25.11** (commit `363a7c8d`), static |
| Viewport | **Hydra** + **fork of Autodesk Aurora** (`aurora-4AIAD`): path tracing (DXR) + raster CAD mode to be added (edges, ID buffer, selection, CUDA result fields) |
| Materials | **MaterialX 1.39.5** (Autodesk fork tag used by Aurora, with Slang generator) — **one MaterialX** for USD and Aurora; OpenPBR + Standard Surface |
| UI | **Dear ImGui** (docking), backends Win32 + DX12; ImGuizmo, ImPlot |
| Meshing | **Netgen** 6.2.2604 (LGPL-2.1, static, **C++ API**, not the nglib C wrapper) from the `.brep` text written by `OCCT_BREP_EXPORT`, healed with OCCT `ShapeFix_Shape` |
| FEA | **MFEM** + CUDA, structural / thermal / modal, in process |
| Multibody | **Project Chrono** (BSD-3), static, MSVC v145 — mechanism dynamics for the engine milestone; joints/masses live in the stage as **`UsdPhysics`** |
| Engine cycle | Own 0D thermodynamic model in `aiad_sim` (Otto cycle: valves, Wiebe combustion, heat loss) |
| Ignition | Magneto: **2D magnetostatics with MFEM** (permanent magnet) → Φ(θ) table → lumped primary/secondary circuit → spark |
| Electromagnetics | **Palace** via fork `palace-4AIAD` (Windows conversion), separate executable — CPU build compiles and runs (2026-10-19); not on the engine milestone path |
| Compressible flow | **HONEE** (CEED) via fork `honee-4AIAD`, on `libceed-4AIAD` + `petsc-4AIAD` |
| Precision | FP64 when the GPU has full FP64; **FP32 automatically** on GPUs limited in FP64 (GeForce). MFEM precision is compile-time → two backend builds |
| AI | Own engine with swappable provider (`IAgentProvider` + `EngineCaps`); initial provider **Claude Code CLI** |
| MCP | Standalone `aiad-mcp.exe` (stdio, C++) ↔ loopback agent bridge `aiad-bridge/1` in the app (lessons from DASE50); tool catalog generated from the Command Bus registry |

## 4. Architecture rules

- **Everything shown in the view comes from a drawing, never from code** (owner, 2026-10-19). Every
  object in the viewport is a part of the document (`UsdStage`, B-rep with its feature history) created
  and edited through Command Bus commands (by the user, the AI or a command script). Application code
  holds only generic CAD operations (sketch, extrude, revolve, boolean, fillet, pattern...), never the
  geometry of a specific object: no hard-coded dimensions, no procedural part generators. A model
  (e.g. the engine) is a saved document (`models/*.usda`), not a C++ function. The procedural
  `aiad_model::BuildEngine` violates this and is being replaced (plan §6, 2026-10-19).

- **What is simulated is what is drawn, and the other way round.** The design parameters generate the
  parts; every physical input of a simulation (masses, inertias, centres of mass, valve lift, chamber
  volume, magneto geometry...) is derived from those parts (`src/aiad_model`). No hand-entered
  physics that the drawing does not show; no drawing detail that the simulation contradicts.
- **The simulator is generic** (owner, 2026-10-19): "o desenho tem de permitir qualquer máquina... o simulador
  deve rodar qualquer uma" — e.g. draw a turbine and it works, no code change, like CATIA. Mechanisms (bodies,
  UsdPhysics joints, couplings, cam pairs, springs, motors), domain elements and studies (flow, structure, EM) are
  document entities; solvers are built from them. Never add machine-specific simulation code
  (plan G-A..G-D done 2026-10-19: joints, elements, `aiad_sim/machine/Machine`, `Docs/02-arquitetura.md` §9.1).
- **The simulator criticises the drawing** (owner, 2026-10-19): finding and pointing out failures is its job.
  The study (`src/aiad_app/MechanismStudy.cpp`) measures every input on the drawn parts and checks that they
  fit (joints on their parts, gears that mesh, cams that reach and release their followers, springs on their
  seats, valves with seat, spring and drive, chambers and magnetos that match the drawing) and refuses to run
  otherwise, listing every problem with a concrete fix. The critique reaches the agent (`aiad_mechanism_check`,
  and the refusal of `aiad_sim_run`/`aiad_sim_start`) and the CLI (`aiad.exe --check <doc>`).
  A drawing that would not work must never "run".
- Layers only point downward; no cycles (project references in the `.vcxproj`). See `Docs/diagrams/dependencias.svg`.
- Facades over third parties: only `aiad_kernel` includes USD BRep; only `aiad_render`/`aiad_gpu`/
  `aiad_ui` include D3D12; only `aiad_sim`/`aiad_gpu` include CUDA; only `aiad_sim` includes MFEM/Netgen;
  only `aiad_ui` includes ImGui.
- `aiad_ai` knows nothing about geometry: it reaches the model only through MCP → Bridge → Command Bus.
- All `UsdStage` mutation happens on the **main thread**; other threads post to the main-thread queue.
  The UI must never wait (owner, 2026-10-19): the main thread does only short steps. The document caches what
  is derived from each part's solid (display mesh, mass, bounds, cuts) by shape revision; the view rewrites
  changed parts in slices (`DocumentScene::Update`, ~10 ms per frame); the machine study is rebuilt after the
  view caught up; agent requests wait (on the bridge thread) until then; pure computation of a command goes
  to `CommandOutput::background` and runs on the bridge's thread. SMLib is not thread-safe: kernel calls stay
  on the main thread.
  The live simulation runs on its own thread (\Session::Work\, batches under \Session::Lock\; readers are
  handed the lock between batches); the magneto field table solves its angles on every core (one MFEM
  problem per thread: MFEM keeps assembly scratch state in the mesh). A mechanism step itself is sequential.
- Long operations (simulation) are **async commands** (job id, progress, cancel).
- Stream parsing is version-tolerant: unknown events are ignored, never fatal.

## 5. Forks (GitHub HermesSilva, suffix `-4AIAD`)

`aurora-4AIAD`, `palace-4AIAD`, `libceed-4AIAD`, `petsc-4AIAD`, `honee-4AIAD` (HONEE imported from
GitLab). In every fork:
- `main` mirrors upstream and is **never modified**; changes go to `main-4AIAD` (default branch);
- `README.md` explains what changes and why (English); the original is kept as `README.upstream.md`;
- keep the diff small and isolated, mark changes with `4AIAD:` comments, rebase onto upstream periodically.

## 6. Dependencies and builds (`deps/`)

- Third-party sources in `deps/_src/`, build trees in `deps/_build/`, installs in `deps/static/<config>/`,
  forks in `deps/forks/`, local patches in `deps/patches/`. See `deps/README.md`.
- Pin versions; record the commit/tag of everything built. Upgrades are separate, deliberate changes.
- Never leave a temporary change in an upstream clone: build, store the result, keep the change as a
  patch file, restore the clone.

### Known build-environment quirks (this machine)

- The agent's shell sets `NoDefaultCurrentDirectoryInExePath=1`; `cmd` then cannot run sibling `.bat`
  files by bare name. Build `.bat` scripts must `set NoDefaultCurrentDirectoryInExePath=` first.
- `vswhere` also lists **SQL Server Management Studio 21**; always pin VS 18 explicitly.
- VS 2022 BuildTools (v143) is also installed; check that builds really use **v145 / MSVC 14.51**.
- Packman (USD BRep build) rejects `(` in paths → Windows SDK via junction in `deps/_toolchain/winsdk/`.
- Build scripts need the `vcvars64.bat` environment of VS 18; NASM portable is in `deps/_toolchain/`.
- CMake 4.x: set `CMAKE_POLICY_VERSION_MINIMUM=3.5` for older third-party projects.

### Consumer rules learned in the spikes

- **SMLib / USD BRep headers:** compile the including translation units as **C++17** (the headers
  are not C++20-clean: ambiguous rewritten `operator==`), with `SM_STATIC_LINK`,
  `SM_GRAPHICS_CALLBACKS=1`, `UNICODE`, `_UNICODE`. Keep them behind `aiad_kernel` so the rest of
  the code stays C++20.
- **Netgen:** define `NETGEN_STATIC` (and `OCCGEOMETRY`); link `zlibstatic.lib`, not `zlib.lib`
  (a foreign `zlib.dll` on `PATH` gets picked up otherwise). Use the C++ API (`OCCGeometry`,
  `Mesh`, `GenerateMesh`): `Ng_DeleteMesh` corrupts the heap. Netgen does not report a failed
  face and then crashes in the volume mesher → mesh to `MESHCONST_OPTSURFACE`, check the surface
  is watertight, then mesh the volume (retry with another `maxh` on failure).
- **`.brep` from `OCCT_BREP_EXPORT`:** has no p-curves and writes inner shells (cavities) with the
  outer orientation → run `ShapeFix_Shape` before meshing, otherwise cavities get filled.
- **MFEM + CUDA:** replace `CUDA::cudart` by `CUDA::cudart_static`; cuSPARSE/cuBLAS stay DLLs
  (allowed exception, rule 4).
- **Eigen / Chrono:** Chrono exports `/arch:AVX2` and extends Eigen through
  `EIGEN_MATRIXBASE_PLUGIN`/`EIGEN_SPARSEMATRIX_PLUGIN`. **Never include Eigen directly**: use
  `src/aiad_sim/common/EigenConfig.h`, and compile with `/arch:AVX2` (project baseline). Otherwise the
  linker merges different versions of the Eigen templates (ODR) and the program crashes.
- **Project Chrono 10 at run time:** `ChTimestepperEulerImplicitLinearized` asks the solver for a full analysis
  every step, so `ChSolverSparseLU` redoes COLAMD every step even with the pattern locked. `SolverLU`
  (`Multibody.cpp`) keeps the ordering, refactors with Eigen's pivots (KLU-style, repivots when a pivot falls
  below 0.1 of its column, and every solution must have a normwise backward error <= 1e-14, else Eigen
  repivots and the step is solved again; the componentwise error is meaningless here) and writes the matrix into its slots (Chrono's `SetElement` searches each row);
  joints are `MateJoint` (`ChLinkMateGeneric`, leaner Jacobian; `ChLinkLock*` is ~2x costlier). LDLT,
  AMD ordering and reusing a stale LU with iterative refinement fail or lose on this regularized,
  ill-conditioned KKT system (measured 2026-10-19). Profile with a stack sampler before optimizing.
- **Project Chrono 10:** build with `CMAKE_DISABLE_FIND_PACKAGE_Thrust=ON` (the multicore collision
  does not compile with the CUDA 13 CCCL 3 Thrust) and without OpenMP (`vcomp140.dll`, rule 4).
- **libCEED:** built with `cl` only; C99 VLAs replaced by `CEED_VLA` (`_alloca` on MSVC).
- **Palace superbuild** (`deps/_src/build_palace.bat cpu`): MS-MPI x64 runtime is `deps/_toolchain/msmpi-10.1.3/Bin64`
  (`Bin` is x86); flang must use the `/MD` Fortran runtime; MFEM must open meshes in binary mode.
- **Aurora fork (`aurora-4AIAD`) fixes to keep:** type-checked primvar reads in `HdAuroraMesh`;
  `addInstances` comparison (hidden meshes on moving bodies crashed); `PTRenderBuffer::data()` must not
  return the padded GPU row pitch (host copy is tight). Readers use width x pixel size, never `GetStride()`.
  Motion vectors of moving instances (previous-frame transform per instance in `gTransformMatrixBuffer`):
  without them the denoiser smears every part that moves in a running simulation.
- **SMLib mass properties change the brep:** after `SmApiBrepComputeMassProperties`, every tessellation of that
  solid (and of its copies) comes out coarse or broken (a hairspring's section lost 7 points in 8, display meshes
  with -485 % volume). `kernel::ComputeMass` therefore integrates a copy. Keep it that way.
- **Imported (STEP) solids lack UV trim curves:** SMLib rebuilds them on every exact mass integration of a copy
  (~10 s for a toothed wheel). Building them on the solid (`GetOrCreateUVTrimCurve`) does not survive save/load and
  broke a mainspring's section; SMLib's analytic recognition (NURBS -> plane/cylinder) took minutes and 48 GB.
  What works: mass and bounds saved with the part (`aiad:derived`, fingerprint = hash of the B-rep vertices), and
  no mass for fixed bodies.
- **SMLib booleans:** solids touching exactly along faces can make `SmApiBooleanUnion` read out of bounds (access
  violation): `kernel::Boolean` catches it (SEH) and fails with the cause; features finer than ~0.15 mm fail too
  (SMLib's model size/tolerance setting does not help).
- **Document files:** `<name>.usda` (design, text) + `<name>.geom.usdc` (BrepArray prims, binary) by a payload on
  the root prim (OpenUSD interface + payload); `Document::Open` flattens them into one layer for editing.
- **Scales:** a watch's parts are ~1e-11 kg m^2. No absolute floors near that in the mechanism (inertia floor,
  effective mass, coupling shafts are relative); automatic penalty stiffness for stops and touches comes from the
  masses (`kAutoContactFrequency`), never a fixed N/m. Touches cut outlines from any solid (`Document::SectionOutline`,
  fine tessellation), so imported parts work without sketches.
- **Documents and the kernel:** models live in `models/*.usda` (built by `models/*.aiad.json` command
  scripts with `aiad-model build`); geometry only through `src/aiad_kernel` (the one SMLib user, C++17).
  Static OpenUSD reads `PXR_PLUGINPATH_NAME` at process start: tests call `PlugRegistry::RegisterPlugins`;
  `OMNISOLID_PLUGIN_PATH` must be set with `_putenv_s` (CRT copy). SMLib's tessellation carries no
  normals (the kernel computes them, crease angle); SMLib booleans are unreliable when cutting solids of
  revolution, so the section view is display-only (`kernel::SectionMesh`). Inactive prims expose no
  attributes: read before deactivating.
- **Never toggle visibility on animated content** with HdAurora: each toggle removes/re-creates the
  instance (~110 ms stall). Switch by transform (e.g. scale) instead.
- **Dear ImGui pinned to v1.92.9-docking** (ImPlot v1.0 draws no items with 1.93); keep
  `AntiAliasedLinesUseTex = false`; no ImPlot inside `ImGui::BeginTable` (use `BeginSubplots`).

- **AI layer = Cockpit parity** (owner, 2026-10-18): the chat/hub must have almost every feature of the
  Cockpit (`D:\Tootega\Source\Cockpit\Cockpit`, README feature grid); map in `Docs/02-arquitetura.md` §10.3.
  Every model mutation is a Command Bus command (`src/aiad_cmd`); engine commands in
  `src/aiad_app/EngineCommands.cpp`; the agent reaches them only via `aiad-mcp.exe` → bridge.
- **stream-json control answers:** `allow` needs `updatedInput` (the full input); AskUserQuestion answers go
  in `updatedInput.answers` keyed by question text; "always" sends the request's `permission_suggestions`
  as `updatedPermissions`; `--append-system-prompt` is not additive (one file with everything).

### Build and verification loop (application)

- Fresh clone: `git clone --recurse-submodules`, then `pwsh -File deps\fetch-prebuilt.ps1` (release assets listed in
  `deps/prebuilt.json`; maintainer publishes with `deps\pack-prebuilt.ps1 -Publish`). Then open `TootegaAIAD.slnx`
  (folders `Tests`, `ThirdParty/{Aurora,libCEED,Palace}`; startup `aiad`; Release and Debug, both with the release CRT
  `/MD` because the prebuilt dependencies are release builds). Shared settings: `Directory.Build.props` (paths:
  `AiadRoot`, `AiadDeps`, `AiadCuda`, `AiadPython`) and `src/Aiad.props`; outputs in `_build/<Config>/{bin,lib,obj}`.
  Aurora (`Aurora4AIAD.props`, `Aurora4AIAD.Runtime.targets`) and libCEED have their `.vcxproj` in their forks; Palace is
  `deps/vs/palace.vcxproj` (NMake, runs the superbuild script). Adding a source file = adding it to its `.vcxproj`.
- **Static OpenUSD in a VS project:** keep `/Zc:inline` off (`RemoveUnreferencedCodeData=false`) and incremental
  linking off: either drops or pads the `.pxrctor` entries that run the static registrations (plugins then fail
  with "missing TfType registration").
- `pwsh -File build-dist.ps1` (repo root): timed steps, stops at the first error, checks that `TootegaAIAD.slnx` loads
  in the IDE (`devenv`), builds the solution with MSBuild (`aiad.exe`,
  `aiad-mcp.exe` and the unit tests (`command_bus_test`, `agent_session_test`, `mcp_bridge_test`,
  `claude_cli_test` — live CLI handshake, no tokens), runs them, assembles `Dist/` (D18 check via
  `dumpbin`), then `aiad.exe --snapshot` (engine runs, 60 animated frames at an unaligned width with
  hide/show/highlight/section toggles, frame-phase timings, `Dist/snapshot*.png`) and
  `aiad.exe --ui-test` (real window: MCP → bridge → Command Bus checks, simulation job, chat fixture,
  window capture `Dist/ui_test.png`). `-SkipSnapshot` skips the two app tests.
- Crashes write `Dist/aiad_crash.dmp`; read with `cdb -z <dmp> -y Dist` (Release builds carry a PDB).
- Never wait blindly on long builds: background run + check the log, with a time limit.

## 7. Engineering principles (from DASE50)

- Every line must pay rent; prefer deletion to decoration.
- Tests answer "what failure would this catch?"; no coverage theater, no tests of impossible worlds.
- Fail fast with precise, actionable errors. Make state explicit.
- Measure before optimizing. Security is an invariant: loopback-only bridge with token, never log
  credentials, never bypass permission prompts.
- Report results honestly: if a build or test fails, say so with the output.

## 8. Workflow for agents

- Follow the execution plan (`Docs/03-plano-execucao.md`): spikes S1–S7, then phases F1–F10.
  Record each spike's outcome in §6 of that document.
- Shell: PowerShell 7 is the default; Git Bash only for POSIX needs; no WSL.
- Ask before outward-facing actions (creating repositories, pushing to new remotes, publishing).
- **Operating a model: MCP for everything** (owner, 2026-10-20). When working on the open drawing, read,
  measure, change and verify it only through the `aiad` MCP tools. Never read the model's files (`.usda`,
  `.usdc`, STEP sources) or run scripts/tools beside MCP to find out what the drawing contains. If MCP cannot
  answer something, say so (it is a gap in the tools), do not work around it.

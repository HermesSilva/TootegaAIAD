# deps — dependências de terceiros

## Como obter

| Caminho | Comando | O que traz |
|---------|---------|-----------|
| **Pré-compilado** (recomendado) | `pwsh -File deps\fetch-prebuilt.ps1` | os pacotes de [`prebuilt.json`](prebuilt.json) (OpenUSD estático, USD BRep, MFEM, Chrono, externals do Aurora) do release do GitHub, verificados por SHA-256, com os caminhos CMake corrigidos para a pasta do clone |
| **Submódulos** | `git submodule update --init --recursive` | forks `-4AIAD`, Dear ImGui, ImPlot, Eigen |
| **Do código-fonte** | scripts `deps\_src\build_*.bat` e `deps\_externals\aurora\*.bat` | cada script acha `deps\` pela própria pasta; fontes em `deps\_src\<nome>` no commit/tag das tabelas abaixo; patches em `deps\patches` |

Ordem do build a partir do código-fonte: externals do Aurora (`install_externals_release.bat`, traz
MaterialX/Slang/DXC) → `build_static_stack.bat` (oneTBB, OpenSubdiv, OpenUSD) → USD BRep
(`repo build`, depois `build_usdbrep_static.bat`) → `build_mfem_cuda.bat double` → `build_chrono_static.bat`;
opcionais: `build_netgen_static.bat`, `build_openblas_static.bat` + `build_palace.bat cpu`. Os scripts
que chamam Python usam `%AIAD_PYTHON%` (padrão `python`, 3.10+). Para publicar pacotes novos:
`pwsh -File deps\pack-prebuilt.ps1 -Publish`.
## usd-brep (NVIDIA USD BRep / SMLib) — compilado com MSVC v145

| Item | Valor |
|------|-------|
| Upstream | https://github.com/NVIDIA-Omniverse/usd-brep |
| Versão | `1.0.0`, commit `c9c5979f04cea8c38213004d4f04df0dcfb40d05` (2026-10-09) |
| Compilador | VS 18 Community, toolset **v145**, MSVC **14.51.36231**, Windows SDK 10.0.26100.0 |
| OpenUSD | `usd.py312.windows-x86_64.v142.stock` **0.25.11** (Packman; compilado pelo upstream com v142, ABI-compatível com v145; inclui imaging/Hd e MaterialX) |
| Binários | `deps/usd-brep/1.0.0-v145/{release,debug}/` (layout do `repo package`: `include/`, `lib/`, `extraLibs/`, `plugin/`, `python/`, `PACKAGE-LICENSES/`) |
| Fonte | `deps/_src/usd-brep/` (clone raso, fixtures LFS **não** baixadas) |

### Patch temporário (`deps/patches/usd-brep-msvc-v145.patch`)

O upstream fixa VS 2019 (v142). O patch altera só `repo.toml` e `premake5-public.lua`:

1. `msbuild.vs_version = "vs18"` + `msbuild.msbuild_version = "18"`: o `repo_build` já conhece VS 18
   (gera com a action `vs2022` do premake), e o filtro de versão impede que o vswhere escolha o
   **SQL Server Management Studio 21** ou o VS 2022 BuildTools desta máquina.
2. `msbuild.winsdk_path` → junction `deps/_toolchain/winsdk/10.0.26100.0`, porque o Packman recusa
   o `(` de `C:\Program Files (x86)\Windows Kits\...`. O patch grava o caminho absoluto desta máquina
   (`D:/Tootega/Source/TootegaAIAD/deps/...`): num clone em outra pasta, ajuste essa linha antes de aplicá-lo.
3. `toolset "msc-v145"` no premake: sem isso a action `vs2022` grava `v143`, e o MSBuild do VS 18
   compila com o v143 do VS 2022 BuildTools (MSVC 14.44).

O token `abi = "v142"` **não** muda: ele só escolhe o pacote do OpenUSD no Packman.

### Reproduzir

```powershell
# PowerShell 7 — o .bat precisa ser chamado por caminho absoluto
$r = 'D:\Tootega\Source\TootegaAIAD\deps\_src\usd-brep'
git -C $r apply ..\..\patches\usd-brep-msvc-v145.patch      # se o clone for novo
New-Item -ItemType Junction -Path 'D:\Tootega\Source\TootegaAIAD\deps\_toolchain\winsdk\10.0.26100.0' `
         -Target 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0'
& "$r\prebuild.bat"            # baixa deps (Packman) e gera os projetos
& "$r\repo.bat" build -r       # release  (~3 min)
& "$r\repo.bat" build -d       # debug
& "$r\repo.bat" package -c release
& "$r\repo.bat" package -c debug
```

Efeitos colaterais do Packman: variável de usuário `PM_PACKAGES_ROOT=D:\packman-repo` e cache
nessa pasta.

### Verificação feita

- `sm_api_boolean_bounds_example.exe`: placa 12 × 12 × 1,5 com 16 furos de raio 0,75 →
  volume 173,588 (analítico: 173,59). OK.
- `tests\HYDRA_USD_BREP_test` (plugin `hdUsdBrep`): 6/6 PASS (animação/visibilidade, opacidade,
  GeomSubsets, instancing, dois corpos com materiais, normais/seleção).

## openusd — OpenUSD 25.11 (o mesmo usado pelo USD BRep)

| Item | Valor |
|------|-------|
| Origem | pacote Packman `usd.py312.windows-x86_64.v142.stock` `0.25.11-gl.18041+v25.11.363a7c8d` |
| Local | `deps/openusd/0.25.11-py312-v142/{release,debug}/` (`include/`, `lib/`, `bin/`, `plugin/`, `cmake/`, `python/`) |
| Build | `msvc_toolset-142`, `imaging: true`, Alembic; `usd_hd`, `usd_usdImaging`, `usd_hdMtlx`, `usd_usdMtlx`, `usd_hdSt`/`usd_hgiGL` (não usados pelo AIAD) |
| MaterialX | **1.39.3**, geradores GLSL/MDL/MSL/OSL — **sem Slang** (ver risco R15) |
| CMake | `find_package(pxr CONFIG)` apontando para `cmake/` |

O pacote do USD BRep traz só o runtime do OpenUSD (`extraLibs/`); para **compilar** o AIAD contra
o mesmo USD são necessários estes headers e import libs.

### Em runtime

- `PATH`: `lib/` e `extraLibs/` do pacote.
- `PXR_PLUGINPATH_NAME`: recursos do schema `omniSolid` e `lib/plugins/hdUsdBrep` (devem ser
  registrados **antes** do OpenUSD carregar os tipos).
- Microsoft Visual C++ Redistributable (v14).

## forks — repositórios -4AIAD (GitHub HermesSilva)

Clones de trabalho em `deps/forks/` (rasos, `--depth 1`), cada um com `origin` = fork e
`upstream` = projeto original. Branch padrão `main-4AIAD`; `main` espelha o upstream sem
alterações. Em cada fork, o `README.md` explica o que muda e por quê (em inglês), e o README
original fica intacto em `README.upstream.md`.

| Fork | Upstream | Licença |
|------|----------|---------|
| [aurora-4AIAD](https://github.com/HermesSilva/aurora-4AIAD) | [Autodesk/Aurora](https://github.com/Autodesk/Aurora) | Apache-2.0 |
| [palace-4AIAD](https://github.com/HermesSilva/palace-4AIAD) | [awslabs/palace](https://github.com/awslabs/palace) | Apache-2.0 |
| [libceed-4AIAD](https://github.com/HermesSilva/libceed-4AIAD) | [CEED/libCEED](https://github.com/CEED/libCEED) | BSD-2-Clause |
| [petsc-4AIAD](https://github.com/HermesSilva/petsc-4AIAD) | [petsc/petsc](https://github.com/petsc/petsc) (espelho de gitlab.com/petsc/petsc) | BSD-2-Clause |
| [honee-4AIAD](https://github.com/HermesSilva/honee-4AIAD) | [gitlab.com/phypid/honee](https://gitlab.com/phypid/honee) (importado; GitHub não faz fork de GitLab) | Apache-2.0 / BSD-2-Clause |

`aurora-4AIAD`, `libceed-4AIAD` e `palace-4AIAD` são submódulos deste repositório (branch
`main-4AIAD`) e aparecem na solução do Visual Studio na pasta **ThirdParty**. Estado (2026-10-19):

| Fork | Alterações | Verificado |
|------|------------|------------|
| aurora-4AIAD | build `AIAD_INTEGRATION` (estático, sem OpenGL), API do USD 25.11, correções de crash em `HdAuroraMesh` e `PTRenderBuffer` | compilado com o app; `--snapshot`/`--ui-test` |
| libceed-4AIAD | build CMake com `cl` (C17), VLAs trocados por `CEED_VLA`, backends CPU `ref/blocked/opt/avx` | `ceed.lib` pelo `build_libceed.bat` e `libceed.lib` pelo `libceed.vcxproj` da solução da solução |
| palace-4AIAD | superbuild com MSVC v145 + MS-MPI + flang (ARPACK), correções de Windows (ver README do fork) | exemplo `spheres` reproduz a referência de regressão do upstream |
| petsc-4AIAD, honee-4AIAD | nenhuma (fora do caminho do marco) | — |

## static — pilha estática (D18)

| Componente | Versão | Local | Script |
|------------|--------|-------|--------|
| oneTBB | 2021.13.0 (o 2023.1 quebra o `workTBB` do USD 25.11) | `deps/static/release` | `deps/_src/build_static_stack.bat tbb` |
| OpenSubdiv | 3.6.1 (CPU) | `deps/static/release` | `… osd` |
| OpenUSD | 25.11 (`363a7c8d`), estático, sem Python/GL/Vulkan, MaterialX ligado | `deps/static/release` | `… usd` |
| MaterialX | 1.39.5 (fork Autodesk, com gerador Slang), estático | `deps/_externals/aurora/MaterialX` | externals do Aurora |
| USD BRep | 1.0.0, estático (`SM_STATIC_LINK`) | `deps/usd-brep/1.0.0-v145-static/release` | `deps/_src/build_usdbrep_static.bat` + `deps/patches/usd-brep-static-v145.patch` + `usd-brep-static-postgen.py` |
| Aurora / HdAurora | fork `aurora-4AIAD` (`AIAD_INTEGRATION`) | compilado na solução do AIAD pelos `.vcxproj` do fork (`Libraries/*/*.vcxproj`) | — |
| MFEM | 4.10 + CUDA 13.3, fp64 e fp32 | `deps/mfem/4.10-cuda13.3-v145-{double,single}` | `deps/_src/build_mfem_cuda.bat` + `install_mfem.bat` + `deps/patches/mfem-4.10-msvc-cuda.patch` |
| Netgen | 6.2.2604 (`ngcore` + `nglib`, sem GUI/Python), consumidores definem `NETGEN_STATIC` | `deps/netgen/6.2.2604-static` | `deps/_src/build_netgen_static.bat` + `deps/patches/netgen-6.2.2604-static.patch` |
| OCCT | 7.8.1, estático, gerado pelo superbuild do Netgen | `deps/_build/netgen-static/dependencies/occ` (`inc`, `win64/vc14/lib`) | idem |
| Project Chrono | 10.0.0, só `Chrono_core`, estático, sem OpenMP/Thrust | `deps/chrono/10.0.0-static` | `deps/_src/build_chrono_static.bat` |
| Eigen | 3.4.1 (só headers; usar via `src/aiad_sim/common/EigenConfig.h`) | `deps/_src/eigen` | — |
| Dear ImGui | v1.92.9-docking (fonte, compilado com o app; o ImPlot 1.0 não desenha itens com o 1.93) | `deps/_src/imgui` | — |
| ImPlot | v1.0 (fonte, compilado com o app) | `deps/_src/implot` | — |
| MS-MPI | SDK 10.1.3 + runtime x64 10.1.1, portáteis (extraídos, sem instalar) — só executáveis de solver; `Bin` tem o runtime x86, `Bin64` o x64 | `deps/_toolchain/msmpi-10.1.3` | — |

Regras práticas do link estático:
- linkar com `/WHOLEARCHIVE` **só** os alvos `pxr`, as libs do USD BRep e o `hdAurora`
  (são as que têm objetos de registro); nunca as libs do sistema;
- consumidores do USD estático compilam com `PXR_STATIC=1`; do USD BRep com `SM_STATIC_LINK`;
  do Aurora com `AURORA_STATIC` (nos `.vcxproj`);
- consumidores dos headers do USD BRep: **C++17** (não são C++20-clean), `SM_STATIC_LINK`,
  `SM_GRAPHICS_CALLBACKS=1`, `UNICODE`, `_UNICODE`;
- Netgen: `NETGEN_STATIC` + `OCCGEOMETRY`; trocar o `zlib.lib` exportado pelo alvo `nglib` por
  `zlibstatic.lib`; usar a API C++ (não o wrapper C `nglib`, cujo `Ng_DeleteMesh` corrompe o heap);
- MFEM com CUDA: trocar `CUDA::cudart` por `CUDA::cudart_static`; **cuSPARSE/cuBLAS ficam DLL**
  (a NVIDIA não distribui versão estática no Windows — exceção da D18), então a pasta `bin` da
  CUDA precisa estar no `PATH` (ou as DLLs ao lado do executável);
- layout em runtime ao lado do executável: `usd/` (plugInfo do USD, `hdUsdBrep`,
  `omniSolid/resources`, `hdAurora`) e `MaterialX/libraries`.

## nlohmann/json — 3.12.0 (cabeçalho único)

| Item | Valor |
|------|-------|
| Origem | `nlohmann/json` v3.12.0 (cópia do `single_include` trazido pelo superbuild do Palace) |
| Local | `deps/_src/nlohmann_json/single_include/nlohmann/{json.hpp,json_fwd.hpp}` + `LICENSE.MIT` |
| Uso | só via `src/aiad_core/Json.h` (`aiad::Json`, `ParseJson` sem exceção) |

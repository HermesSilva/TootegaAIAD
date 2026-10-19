# 03 — Plano de execução

> Tootega AIAD · plano v0.4 · 2026-10-18 · **somente Windows x64 · DirectX 12 · CUDA**
> Base: [01-levantamento.md](01-levantamento.md) · [02-arquitetura.md](02-arquitetura.md)

---

## 0. Marco M1 — motor 4 tempos desenhado e rodando no simulador (prioridade)

Definido pelo usuário em 2026-10-18: **um motor a combustão de 4 tempos, desenhado na ferramenta e
rodando no seu simulador.** O trabalho passa a ser priorizado pelo que este marco exige.

**Escopo decidido (2026-10-18):**

| Item | Decisão |
|------|---------|
| Motor | **monocilíndrico**, comando no cabeçote (OHC), 2 válvulas, ignição por **magneto** (ímã no volante + bobina) |
| "Rodando" | **mecanismo + ciclo termodinâmico 0D**: as peças montadas com juntas se movem; a pressão do ciclo Otto (admissão, compressão, combustão, escape) empurra o pistão e o motor gira sozinho; rpm, torque, potência e diagrama p-V; animação no viewport |
| Multicorpos | **Project Chrono** (BSD-3), estático, MSVC v145 — juntas, inércia, came-seguidor |
| Juntas no documento | **`UsdPhysics`** (juntas de revolução/prismáticas, massas) — o `UsdStage` continua sendo a fonte da verdade; o Chrono é montado a partir dele |
| Ignição | **magnetostática 2D com MFEM** (ímã permanente no volante, núcleo e bobina): tabela de fluxo enlaçado Φ(θ) → tensão induzida `−N·dΦ/dt` → circuito primário/secundário (0D) → faísca quando a tensão do secundário passa da ruptura da vela → início da combustão (Wiebe) |
| Fora do marco | tensões (MFEM estrutural), CFD da combustão (HONEE), Palace — **S6 pausado** (libCEED com `cl` e scripts preservados) |

**Caminho crítico:**

| Passo | Entrega | Critério de aceite |
|-------|---------|--------------------|
| **M1.1** | Chrono estático com MSVC v145 (spike S8) | biela-manivela no Chrono: posição do pistão = solução analítica (erro < 1e-6) |
| **M1.2** | Modelo 0D do ciclo Otto (`aiad_sim`) | ciclo ideal reproduz η = 1 − r^(1−γ); ciclo real com válvulas, Wiebe e perda de calor dá p-V plausível |
| **M1.3** | Magneto 2D com MFEM (spike S9) | Φ(θ) de um ímã girando diante de um núcleo em U; tensão de pico do secundário na ordem de 10–20 kV na rotação de partida |
| **M1.4** | Acoplamento (sem UI) | Chrono + 0D + magneto: motor dá partida (motor de arranque por 1 s) e se mantém girando estável; rpm, torque e potência em CSV |
| **M1.5** | F1 enxuta: app DX12 + ImGui + viewport Hydra | abre o `.usda` do motor e anima o mecanismo a partir do resultado da simulação |
| **M1.6** | Desenho do motor na ferramenta | peças paramétricas (bloco/cilindro, pistão, pino, biela, virabrequim, volante com ímã, comando com cames, válvulas, molas, bobina, vela) criadas por comandos do Command Bus (F2 mínima), montadas com juntas `UsdPhysics` |
| **M1.7** | Simulador na UI | botão "rodar": animação ao vivo, gráficos de rpm/torque/p-V (ImPlot), faísca visível |

**Correção de rumo (dono, 2026-10-19): todo objeto do viewport vem de desenho, nunca de código.** O
gerador procedural `aiad_model::BuildEngine` (malhas com medidas fixas no C++) foi um erro e é
substituído pelo M1.6 abaixo, nesta ordem:

| Passo | Entrega | Aceite |
|-------|---------|--------|
| **M1.6a** | `aiad_kernel`: fachada do SMLib (esboço em plano com linhas e arcos, extrusão, revolução, primitivas, hélice/tubo, booleanas, corte por plano, propriedades de massa, tesselação, gravar/ler `BrepArray` no stage) | teste com volumes e massas analíticos; B-rep gravado no stage e lido de volta igual |
| **M1.6b** | `aiad_doc`: documento = `UsdStage`; parâmetros do documento com expressões; cada peça é um prim com material, corpo, papel e **histórico de features** (JSON) + o `BrepArray` resultante; regeneração; comandos no bus (parâmetros, criar/editar/apagar peça e feature, material, salvar/abrir) | mudar um parâmetro regenera só as peças dependentes; desfazer volta o documento exato |
| **M1.6c** | O motor como **documento** `models/Engine_SingleCylinder.usda`, construído por um roteiro de comandos (dado, não código) e salvo | todas as peças do motor atual existem como features editáveis; nenhuma geometria no C++ |
| **M1.6d** | App sobre o documento: cena, corte (booleana real), seleção, física derivada das peças do documento; `BuildEngine`/`MeshBuilder` removidos | motor dá partida e roda; snapshot e ui-test verdes; a IA altera qualquer peça pelo MCP |

Fases F3/F4 (IA operando via MCP) entram depois do M1.6, para que a IA possa desenhar o motor pelos
mesmos comandos.

---

## 1. Visão das fases

| Fase | Entrega | Depende de | Gate (critério de aceite) |
|------|---------|-----------|---------------------------|
| **F0** | Spikes de viabilidade S1–S7 | — | S1–S4 verdes liberam a F1; S5 libera a F7; S6 a F8; S7 a F9 |
| **F1** | Esqueleto: janela Win32 + DX12, ImGui docking, viewport `HdAiad` (fork do Aurora, modo raster), abrir/salvar `.usda` | S1–S4 | abre um `.usda` de exemplo do USD BRep e orbita a ≥ 60 fps na RTX 2060 SUPER; picking de face pelo ID buffer |
| **F2** | Kernel + documento + Command Bus | F1 | primitivas, booleanas, fillet, chamfer, shell, transform como features; regen; undo/redo; propriedades; issues |
| **F3** | Agent Bridge + `aiad-mcp.exe` | F2 | Claude Code **no terminal** constrói um flange via MCP, confere por snapshot, e o usuário desfaz tudo com Ctrl+Z |
| **F4** | Motor de IA + painel de chat | F3 | o fluxo de [fluxo-ia.svg](diagrams/fluxo-ia.svg) ponta a ponta **dentro do app**: streaming, permissão, mutação, snapshot, uso/custo |
| **F5** | Sketch 2D + paramétrico + STEP | F2 | sketch com restrições → extrude/revolve/sweep; IDs persistentes v2 sobrevivem a edições a montante; import/export STEP |
| **F6** | Materiais MaterialX | F1, F2 | OpenPBR com texturas no viewport; editor; biblioteca de engenharia com aparência + físicas |
| **F7** | Simulação estrutural / térmica / modal (MFEM + CUDA) | F5, F6, S5 | viga engastada: deslocamento e tensão com erro < 2% da solução analítica; campo exibido via interop sem cópia; a IA ajusta uma espessura até a tensão ficar abaixo do limite |
| **F8** | Eletromagnetismo (fork `palace-4AIAD`) | F7, S6 | capacitor de placas paralelas (C analítico, erro < 2%); cavidade retangular (1º modo, erro < 1%) |
| **F9** | Escoamento compressível (fork `honee-4AIAD`) | F7, S7 | tubo de choque de Sod (perfil vs. solução exata); bocal convergente-divergente (Mach na garganta ≈ 1) |
| **F10** | Produto | todas | `TootegaEngineProvider`; instalador (MSIX ou WiX) com runtime CUDA redistribuível; assinatura de código; Aurora (DXR) opcional; documentação bilíngue |

F3 vem **antes** de F4 de propósito: com o MCP pronto, o Claude Code no terminal já opera o app,
o que valida as ferramentas antes de existir o chat embutido. Foi assim que o DASE amadureceu.

---

## 2. F0 — Spikes de viabilidade (detalhe)

Cada spike é um diretório descartável em `spikes/Sx-*`, com um README do resultado: **verde**
(segue), **amarelo** (segue com contingência) ou **vermelho** (aciona o plano B).

| Spike | Pergunta | Como | Situação / plano B |
|-------|----------|------|--------------------|
| **S1** | O USD BRep compila com **MSVC v145** e o app consegue linkar o mesmo OpenUSD? | patch temporário (`deps/patches/usd-brep-msvc-v145.patch`); `prebuild.bat` + `repo.bat build`; binários em `deps/usd-brep/` | **verde** (ver §6); falta só o teste de link a partir do CMake do AIAD (F1) |
| **S2** | O OpenUSD do Packman traz UsdImaging, Hd, usdMtlx, hdMtlx? Existe adapter Hydra para `BrepArray`? | inspecionar libs/plugInfo | **verde:** OpenUSD **25.11** com imaging, `hdMtlx`/`usdMtlx` e MaterialX **1.39.3** (sem Slang → R15); o USD BRep traz **`hdUsdBrep`**, 6/6 testes PASS |
| **S3** | O **fork do Aurora** compila com v145 contra o OpenUSD 25.11 e desenha um `BrepArray` via `hdUsdBrep`? Um passe raster (arestas + ID buffer) cabe no device dele? | build do Aurora; usdview/app mínimo com `HdAurora`; protótipo do passe raster | ajustar a versão de USD do Aurora no fork; em último caso, delegate raster próprio e Aurora só para o modo Render |
| **S4** | **MFEM + CUDA + MSVC v145**, em fp64 **e** fp32? Interop D3D12 ↔ CUDA? | `MFEM_USE_CUDA` (CUDA 13.3); ex1/ex2 com `-d cuda` nas duas precisões; campo escrito pela CUDA lido num shader | fixar CUDA 12.6; alternar o par de versões |
| **S5** | **Netgen** gera tetraedros a partir do `.brep` exportado pelo USD BRep, preservando a numeração de faces? | `nglib` (estático, com OCCT) em 5 peças reais; conferir mapeamento de faces; exportar para MFEM | caminho STL estanque; healing OCC explícito |
| **S6** | **`palace-4AIAD`**: Palace + dependências compilam com MSVC + CUDA + MS-MPI? | superbuild com cuDSS/SuperLU_DIST, hypre, libCEED; ARPACK-NG (`ifx`) ou SLEPc; rodar um exemplo oficial | desligar dependências opcionais uma a uma; corrigir no fork |
| **S7** | **`honee-4AIAD`**: libCEED + PETSc + HONEE compilam com CUDA no Windows? | `libceed-4AIAD` (clang-cl onde houver VLA), `petsc-4AIAD`, `navierstokes.exe` num caso de exemplo | corrigir no fork; PETSc sem CUDA como passo intermediário |

---

## 3. Entregas por fase (resumo técnico)

### F1 — Esqueleto
- CMake + Ninja + presets; vcpkg (manifest) para libs leves; `deps/` para USD BRep/OpenUSD.
- `aiad_core` (log, settings, tabela de strings, fila da main thread), `aiad_gpu` (adaptador, device, filas).
- `aiad.exe`: janela Win32, swapchain DX12, ImGui docking (Win32 + DX12).
- `HdAiad` v0 (fork do Aurora): modo raster com malha (via `hdUsdBrep`), arestas, PBR simples, ID buffer, câmera orbital; modo Render (path tracing DXR) herdado do upstream.
- Abrir/salvar `.usda`; Outliner; painel de Log.
- Regras de camada no CMake (falha de build se um alvo incluir o que não pode).

### F2 — Kernel, documento e comandos
- `aiad_kernel`: fachada + isolamento (SEH + timeout) + tesselação.
- `aiad_schema`: `AiadFeature`; `aiad_doc`: camadas, regen incremental, seleção, validação.
- `aiad_cmd`: registro, JSON Schema, transação, undo/redo, auditoria.
- Painéis Features, Propriedades, Issues; ImGuizmo para transformações.

### F3 — Bridge + MCP
- `aiad_bridge` (`aiad-bridge/1`, discovery, token, Origin, fila).
- `aiad-mcp.exe` (JSON-RPC stdio, catálogo dinâmico, imagens, re-descoberta).
- Ferramentas: documento, leitura, features, sólidos, lote, view, histórico.
- Teste de contrato: catálogo do bridge = `tools/list` do MCP (comparação automática).

### F4 — Motor de IA + chat (paridade com o Cockpit)
Meta do dono (2026-10-18): **quase todas as funcionalidades do Cockpit**, ver
[02-arquitetura.md §10.3](02-arquitetura.md). Em etapas, cada uma utilizável:
- **F4a — núcleo:** `aiad_ai` (`IAgentProvider`, `ClaudeCliProvider` com CreateProcessW + Job
  Object, parser tolerante com fixtures reais do CLI); chat com streaming, thinking, timeline de
  cards (miniatura do snapshot), Markdown; permissão, modos, plano editável, perguntas compostas,
  interromper; sessões (lista, retomar, renomear, apagar, rewind da conversa e do modelo).
- **F4b — consumo:** contexto, cache + keep-alive, custo estimado, limites OAuth `/usage`,
  atribuição, tokens globais, gasto 7 dias, tempos, atividade da sessão, compactação visível.
- **F4c — extensibilidade:** slash commands, plugins, skills, painel MCP, descoberta de modelos,
  esforço, motor Tootega, prompt de sistema/quiet, piso de esforço.
- **F4d — entrada e conforto:** imagens/arrastar, @-mention de peças, seleção do viewport, busca
  Ctrl+F, exportar, verbosidade, marcadores, ditado por voz, corretor PT/EN, Remote Control.
- Prompt de sistema do domínio CAD/CAE; UI inteira em inglês.

### F5 — Sketch e paramétrico
- Sketcher 2D (linhas, arcos, círculos, splines) com restrições; decidir solver (R7).
- Extrude/revolve/sweep/loft a partir de sketch; IDs persistentes v2; import/export STEP via OCCT.

### F6 — Materiais
- Pipeline completo MaterialX → Slang → DXIL com cache; texturas; IBL (domeLight).
- Editor de materiais; `AiadPhysicsMaterialAPI`; biblioteca `materials/`.
- Ferramentas `aiad_material_*`.

### F7 — Simulação estrutural/térmica/modal
- `aiad_schema`: `AiadStudy`, `AiadBoundaryCondition`; painel Simulação; escolha de faces pelo
  picking.
- `NetgenMesher` (`nglib`); `MfemBackend` com CUDA em fp64/fp32 (D15); jobs assíncronos; resultados via interop.
- Ferramentas `aiad_sim_*`; casos canônicos em `tests/sim/`.

### F8 — Eletromagnetismo
- `PalaceBackend` sobre o fork `palace-4AIAD`; domínio de ar via kernel;
  portas e condições de contorno EM; pós-processamento (parâmetros S, frequências, Q, C/L).

### F9 — Escoamento compressível
- `HoneeBackend` sobre o fork `honee-4AIAD`; domínio de fluido via
  kernel; condições de entrada/saída/parede; campos de pressão, Mach e velocidade.

### F10 — Produto
- `TootegaEngineProvider`; instalador; assinatura; verificação de driver/GPU no primeiro uso;
  Aurora opcional; manual do usuário e da API MCP (no formato do `MCP_API_SPEC.md` do DASE).

---

## 4. Práticas transversais

- **Testes:** GoogleTest; os testes de GPU são marcados e rodam num runner self-hosted (esta
  máquina), porque runners hospedados não têm GPU NVIDIA.
- **Fixtures reais:** stream-json gravado do Claude Code CLI; `.usda` com `BrepArray`; casos de
  simulação com solução analítica.
- **Sem métrica de fachada** (princípios do DASE): teste só o que pode acontecer em produção;
  código inalcançável é removido, não "coberto".
- **Segurança:** bridge só em loopback, com token; nunca logar credenciais; permissões da IA sempre
  passam pelo usuário (ou pela allowlist explícita dele).
- **Versões fixadas:** OpenUSD, USD BRep, MaterialX, MFEM, libCEED, CUDA e MSVC fixados em
  `deps/versions.json`; upgrade é um PR dedicado.

---

## 5. Decisões

### 5.1 Resolvidas pelo usuário (2026-10-18)

| # | Decisão |
|---|---------|
| O2 | Hydra: **fork do Autodesk Aurora** como base do delegate (D4) |
| O3 | Palace e HONEE: **fork e conversão para Windows** (D10) |
| O5 | **FP32 automático** quando a GPU for limitada em FP64 (D15) |
| O6 | Mesher: **Netgen** (D16) |
| O7 | Projeto **open source**; a licença segue o que as dependências exigirem (recomendação: Apache-2.0) (D9) |
| — | USD BRep compilado com **v145**, binários guardados (D17) |
| O1 | **Só inglês**: código, comentários, commits e UI do software (sem i18n) |
| O8 | Forks no GitHub pessoal **[HermesSilva](https://github.com/HermesSilva)** com sufixo **`-4AIAD`**: [aurora-4AIAD](https://github.com/HermesSilva/aurora-4AIAD), [palace-4AIAD](https://github.com/HermesSilva/palace-4AIAD), [libceed-4AIAD](https://github.com/HermesSilva/libceed-4AIAD), [petsc-4AIAD](https://github.com/HermesSilva/petsc-4AIAD), [honee-4AIAD](https://github.com/HermesSilva/honee-4AIAD); branch padrão `main-4AIAD`, `main` espelha o upstream |

### 5.2 Retiradas

- **O4** ("Compressão" = escoamento compressível?): retirada pelo usuário em 2026-10-18. O plano segue com o HONEE para **escoamento compressível**.

---

## 6. Registro de execução

### 2026-10-18 — S1/S2: USD BRep compilado com MSVC v145

- Fonte: `NVIDIA-Omniverse/usd-brep` `1.0.0`, commit `c9c5979f` (clone raso, sem fixtures LFS),
  em `deps/_src/usd-brep/`.
- Patch temporário `deps/patches/usd-brep-msvc-v145.patch` (detalhes em `deps/README.md`):
  `vs_version = "vs18"`, `msbuild_version = "18"`, `winsdk_path` por junction e
  `toolset "msc-v145"` no premake.
- Três obstáculos encontrados e contornados: o vswhere escolhia o **SQL Server Management Studio
  21**; o Packman recusa `(` no caminho do Windows SDK; e a action `vs2022` do premake gravava
  `v143`, que o MSBuild do VS 18 resolvia com o MSVC 14.44 do VS 2022 BuildTools.
- Resultado: **release (163 s) e debug (60 s) compilados com v145 / MSVC 14.51.36231**, com
  `/W4 /WX` e zero erros. Pacotes em `deps/usd-brep/1.0.0-v145/{release,debug}` (+ `.7z` em
  `archives/`). OpenUSD 25.11 (headers e libs) copiado para `deps/openusd/0.25.11-py312-v142/`.
- Verificação: exemplo de booleanas (volume 173,588 ≈ analítico 173,59) e teste oficial do
  `hdUsdBrep` (6/6 PASS).

### 2026-10-18 — S3 (Aurora) e mudança para linkagem estática (D18)

- **Fork `aurora-4AIAD` — primeiras alterações** (branch `main-4AIAD`, ainda não publicadas):
  `installExternals.py` ganhou `--usd-root` (usa um OpenUSD do host e pula o fork de USD da
  Autodesk e o OpenSubdiv), `--materialx-static` e `--no-oiio`; suporte a **VS 2026 / v145**
  (gerador "Visual Studio 18 2026", Boost `vc145`/`msvc-14.5`); correção do pin do `pystring` no
  OpenImageIO 3.1.13 (a `master` andou); opção CMake `ENABLE_IMAGE_PROCESSING_RESOLVER`.
- Achados: o Aurora **já tem** gerador MaterialX → **Slang** e denoiser **NRD** (eram itens do
  plano do fork); o núcleo DirectX não precisa de USD (só o `HdAurora`); o `HdAurora` foi feito
  para o usdview em OpenGL (linka GL/GLEW/`hdx`), então a saída D3D12 é trabalho do fork.
- Externals do Aurora compiladas com v145 em `deps/_externals/aurora/` (zlib, jpeg, png, tiff,
  Boost, oneTBB, OpenEXR, MaterialX 1.39.5 estático com Slang, Slang, DXC, NRD/NRI, GLEW, GLFW,
  GTest…). O OpenImageIO foi dispensado (só serve ao resolver de imagens do usdview e aos testes).
- O OpenUSD do Packman não traz configs CMake das dependências (OpenSubdiv, TBB) e não serve para
  linkagem estática. Com a **D18 (tudo estático)**, a pilha passa a ser compilada do fonte em
  `deps/static/release/`: **oneTBB 2023.1.0** e **OpenSubdiv 3.6.1** estáticos (prontos, sem DLLs);
  **OpenUSD 25.11** estático (em compilação). Script: `deps/_src/build_static_stack.bat`.
- Achado do ambiente: o shell do agente define `NoDefaultCurrentDirectoryInExePath=1`, o que faz o
  `cmd` não achar `.bat` vizinhos (era a causa dos erros "não é reconhecido"); os scripts de build
  zeram a variável.

### 2026-10-18 — S2 (estático) e S3a: verdes

- **OpenUSD 25.11 estático** em `deps/static/release/` (60 libs, nenhuma DLL), compilado com
  oneTBB **2021.13.0** (o 2023.1 quebra o `workTBB` do USD 25.11 porque `TBB_INTERFACE_VERSION_MAJOR`
  deixa de chegar por inclusão transitiva) e OpenSubdiv 3.6.1, com o MaterialX 1.39.5 + Slang
  (estático) das externals do Aurora — **um MaterialX só**.
- **Spike `spikes/S2-static-usd`**: executável único (17 MB) com todas as libs USD via
  `/WHOLEARCHIVE` → 38 plugins registrados sem DLLs, schemas, adapter UsdImaging de `Mesh`,
  leitor `.mtlx`: **ALL PASS**. Lições: aplicar `/WHOLEARCHIVE` só aos alvos `pxr` (não às libs
  do sistema); os `plugInfo.json` do modo estático têm `"LibraryPath": ""` e ficam em `usd/` ao
  lado do executável (é o layout do produto).
- **USD BRep estático** (patch `deps/patches/usd-brep-static-v145.patch` + pós-geração
  `deps/patches/usd-brep-static-postgen.py`): macro `SM_STATIC_LINK` redefinindo as macros de
  export **depois** dos blocos originais (envolver os blocos apagava typedefs e
  `SMU_THREAD_LOCAL`), `PXR_STATIC=1`, `StaticLib` no premake e `<Lib>` sem dependências (senão o
  `lib.exe` embute TBB/USD nas `.lib`). Artefatos em `deps/usd-brep/1.0.0-v145-static/release/`.
- **Spike `spikes/S3a-hydra-brep-static`**: USD + USD BRep + `hdUsdBrep` estáticos num executável
  de 23 MB; abre `sphere_nurbs.usda` e o Hydra (só scene indices) entrega a malha tesselada pelo
  SMLib (114 pontos) **e as arestas B-rep como `basisCurves`**: **ALL PASS**. Lições: o schema
  `omniSolid` precisa estar em uma pasta `omniSolid/resources` (o USD BRep procura esse trecho de
  caminho); o resolvedor de procedurals (`HdGpGenerativeProceduralResolvingSceneIndex`) **não**
  entra sozinho — o OpenUSD só o inclui com `HDGP_INCLUDE_DEFAULT_RESOLVER`; o AIAD o anexa
  explicitamente na sua cadeia Hydra, antes de `SetStage`.

### 2026-10-18 — S3b: verde (com pendências conhecidas)

- **Fork `aurora-4AIAD`**, commit `e7edb54` (publicado em `main-4AIAD`): opção CMake
  `AIAD_INTEGRATION` → `Aurora` e `HdAurora` **estáticos**, `HdAurora` **sem OpenGL/GLEW/hdx**
  (`HDAURORA_NO_GL`), registro do plugin por `TF_REGISTRY_FUNCTION`
  (`HdAuroraStaticRegistration.cpp`, substitui o `DllMain`), API do OpenUSD 25.11
  (`IsSupported(HdRendererCreateArgs const&)`, `HdMeshComputationResult`), `add_subdirectory`.
- **Spike `spikes/S3b-aurora-render-static`**: USD + USD BRep + `hdUsdBrep` + Aurora + `HdAurora`
  estáticos (executável de 24 MB); sem `Hgi`, sem GL; tarefa Hydra mínima própria (o `hdx` não
  existe sem backend de GPU); AOV de cor lido por `Map()` e gravado em PNG. **Path tracing DXR do
  `BrepArray` convergiu em 28 quadros / 2,2 s na RTX 2060 SUPER.**
- Achados: o Aurora devolve a cor em **half-float RGBA** com linhas alinhadas (pitch 6400 B para
  800 px; usar `HdAuroraRenderBuffer::GetStride()`); o Aurora **já gera shaders MaterialX via
  Slang** e **já tem o denoiser NRD**.
- Pendências para o fork (F1): normais *face-varying* do `hdUsdBrep` não tratadas pelo `HdAurora`
  ("normal array length does not match" → sombreamento facetado); tesselação padrão grossa (114
  pontos na esfera) — ajustar a tolerância; modo raster, arestas, ID buffer.
- DLLs que ainda acompanham o build do Aurora (exceções a resolver na D18): `slang*.dll`
  (pré-compilado), `dxcompiler.dll`/`dxil.dll` (permitidos), `NRD.dll`/`NRI.dll`, `amd_ags_x64.dll`
  e cópias desnecessárias de Boost/OpenEXR/TBB/GLEW (o deploy do Aurora copia todas as externals).

### 2026-10-18 — S4: verde

- **MFEM 4.10** (último release, set/2026) com **CUDA 13.3 + MSVC v145** (`sm_75`), estático, nas
  duas precisões: `deps/mfem/4.10-cuda13.3-v145-double/` e `…-single/` (`MFEM_USE_SINGLE`).
  O CUDA 13.3 aceita o MSVC 19.51 como compilador hospedeiro (risco R16 resolvido para este par).
- Foram necessárias 5 correções para MSVC + CUDA, guardadas em
  `deps/patches/mfem-4.10-msvc-cuda.patch` (o clone é restaurado após cada build):
  `/wd4819` só para C++ (e via `-Xcompiler` para o nvcc); `_USE_MATH_DEFINES`; `__lzclz` (não é
  intrínseco do MSVC) → `_BitScanReverse`; `Epsilon` `const` → `constexpr` (uso em device);
  `asm("trap;")` → `__trap()`. Além disso o CCCL do CUDA 13 exige `-Xcompiler=/Zc:preprocessor`.
- `ex1` (Laplace, ordem 4, assembly parcial): fp64 CPU 5,15 s → **CUDA 0,94 s**; fp32 CPU 4,20 s →
  **CUDA 0,65 s**; mesma convergência (400 iterações). `ex2` (elasticidade, 15.795 incógnitas)
  convergiu (o exemplo serial não tem opção de dispositivo).
- **Spike `spikes/S4b-d3d12-cuda-interop`**: D3D12 no adaptador com o **mesmo LUID** da CUDA;
  buffer D3D12 compartilhado importado na CUDA (`cudaImportExternalMemory`), kernel escreve 1M
  floats, fence D3D12 compartilhada sinalizada pela CUDA e esperada pela fila D3D12 na GPU;
  **0 divergências**. `cudaDevAttrSingleToDoublePrecisionPerfRatio` = 32 na RTX 2060 SUPER → a
  regra D15 escolhe fp32. Executável depende só de `d3d12.dll`, `dxgi.dll` e do CRT (CUDA
  estática; `nvcuda.dll` é carregado pelo runtime, como previsto na D18).

### 2026-10-18 — S5: verde (Netgen estático, API C++)

- **Netgen 6.2.2604** estático (`ngcore` + `nglib`, sem GUI/Python) com **OCCT 7.8.1** estático
  do superbuild do Netgen: `deps/netgen/6.2.2604-static/`, script `deps/_src/build_netgen_static.bat`,
  patch `deps/patches/netgen-6.2.2604-static.patch` (`NETGEN_STATIC` zera `dllexport/dllimport`).
- **Spike `spikes/S5-netgen-mesh`** (`run.ps1`): USD BRep → `OCCT_BREP_EXPORT` (`.brep` texto) →
  `BRepTools::Read` + `ShapeFix_Shape` → `netgen::OCCGeometry` → tetraedros → `mfem::Mesh` em
  memória (atributo de contorno = face B-rep) → volume comparado com `SmApiBrepComputeVolume`.
  Resultado com `maxh = 0,1·∛V` (erro de volume relativo):

  | Peça | Faces B-rep → atributos | Tetraedros | Erro de volume |
  |------|------------------------|-----------:|---------------:|
  | `Cube` | 6 → 6 | 4.888 | 1e-15 (exato) |
  | `cubeWithVoid` (cavidade esférica) | 7 → 7 | 4.749 | 3,2e-3 |
  | `Cone_SmConnect_Brep` | 2 → 2 | 6.103 | 3,8e-3 |
  | `mouse` | 4 → 4 | 2.719 | 3,5e-3 |

  O erro nas faces curvas é erro de corda dos tetraedros lineares e converge com o refino
  (`maxh` 0,05·∛V: 8e-4 / 1,2e-3 / 8e-4); elementos de 2ª ordem ficam para a F7. Os arquivos de
  `coverage_limits/` são superfícies abertas (volume 0) e não se aplicam à malha volumétrica.
- Achados (viraram regras em `CLAUDE.md` e no `deps/README.md`):
  - o `.brep` exportado não tem p-curves (`Curve2ds 0`) e grava a **casca interna com a orientação
    da externa** → sem `ShapeFix_Shape` o Netgen preenche a cavidade (volume 1 em vez de 0,966);
  - `Ng_DeleteMesh` do wrapper C `nglib` corrompe o heap → usar a **API C++** do Netgen;
  - o Netgen **não propaga a falha de uma face** (só imprime aviso) e depois cai (access
    violation) na malha volumétrica: malhar até `MESHCONST_OPTSURFACE`, conferir estanqueidade
    (toda aresta em exatamente 2 triângulos) e só então o volume; se falhar, nova tentativa com
    outro `maxh` (aconteceu no cone com 0,1·∛V; a 2ª tentativa passou);
  - headers do SMLib exigem C++17, `SM_STATIC_LINK`, `SM_GRAPHICS_CALLBACKS=1`, `UNICODE`;
  - o `nglib` exporta `zlib.lib` (import) → trocar por `zlibstatic.lib` (um `zlib.dll` alheio no
    `PATH` era carregado);
  - **cuSPARSE/cuBLAS não têm versão estática no Windows** → nova exceção da D18 (o runtime CUDA
    continua estático via `cudart_static`).

### 2026-10-18 — Marco M1 (motor 4 tempos): M1.1–M1.4 verdes

- **M1.1 — Project Chrono 10.0.0** estático, MSVC v145 (`deps/chrono/10.0.0-static`, script
  `deps/_src/build_chrono_static.bat`), sem módulos, sem OpenMP (D18) e **sem Thrust** (a colisão
  multicore não compila com o Thrust do CCCL 3 do CUDA 13). Spike `spikes/S8-chrono-crank`:
  biela-manivela a 3000 rpm, posição do pistão vs. solução fechada, **erro máx. 1,9e-7 m**.
- **M1.2 — ciclo 0D** (`src/aiad_sim/engine/EngineCylinder`): zona única, sistema aberto, válvulas
  como orifício compressível, Wiebe em **graus de virabrequim**, Woschni, γ(T) = 1,392 − 8,13e-5·T
  com u(T) em forma fechada. Ciclo Otto ideal: η = 0,55340 vs. 0,55332 (erro 1,5e-4), balanço de
  energia fecha em 1e-14. Ciclo real (302 cc, 3000 rpm, WOT): IMEP 12,4 bar, pico 74 bar,
  ηv 0,95, ηi 0,40, 9,4 kW indicados.
- **M1.3 — magneto** (`src/aiad_sim/magneto`): magnetostática 2D em MFEM (A_z, ímã de ferrite no
  aro de volante de alumínio, sapatas de aço, núcleo em U, bobina no jugo); material avaliado por
  ponto de quadratura no rotor girado (malha fixa, refino graduado: 62 mil DOFs, 0,8 s/ângulo,
  Cholesky do Eigen); Φ(θ) e L₁(θ) em ±45°; circuito de platinado com condensador e capacitância do
  secundário. Verificações: |Φ| < Br·A, simetria espelhada 3e-4, L > 0; faísca a partir de baixa
  rotação contra 12 bar. Tabela com cache por parâmetros (`CachedMagnetoTable`).
- **M1.4 — motor acoplado** (`src/aiad_sim/engine/EngineSimulation`): Chrono (virabrequim livre,
  biela, pistão) + ciclo 0D + magneto + atrito (FMEP de Heywood) + carga ∝ ω² + motor de partida
  (curva CC). **Dá partida e se mantém a ~3050 rpm** (variação < 0,1%), 25 combustões/s (uma a cada
  2 voltas), 8,1 kW no freio; **sem faísca o motor para** quando o motor de partida desliga.
- Achados (regras no `CLAUDE.md`):
  - o Chrono exporta `/arch:AVX2` e estende o Eigen com *plugins*: misturar unidades com e sem isso
    quebra a ODR e derruba o programa → Eigen só via `src/aiad_sim/common/EigenConfig.h`;
  - duração da combustão fixa em segundos causava *kickback* na partida (queima toda antes do PMS a
    ~400 rpm); em graus de virabrequim (como nos modelos 0D usuais) a partida é correta;
  - inércia do volante 0,012 kg·m² não vence a compressão na partida; 0,03 kg·m² (faixa usual de
    monocilíndricos de ~300 cc) com motor de partida de 50 N·m resolve;
  - NdFeB de 1,2 T dava 59 kV no secundário; ferrite (0,4 T), como nos magnetos de volante, e a
    capacitância do secundário no modelo dão valores realistas.
- Próximo: M1.5 — app DX12 + ImGui + viewport Hydra abrindo o `.usda` do motor e animando o
  mecanismo com o resultado da simulação.

### 2026-10-18 — M1.5: app com o motor desenhado = motor simulado

- **Fonte única de verdade** (`src/aiad_model`, regra no `CLAUDE.md`): `EngineDesign` → peças (malhas)
  → física. Massas, centros de massa e inércias vêm das malhas (integrais de poliedro de
  Mirtich/Eberly × densidade do material): virabrequim + volantes + rotor 0,0342 kg·m², biela
  0,348 kg (CdM a 25,3 mm do moente), pistão + anéis + pino 0,415 kg. O levantamento das válvulas é
  **medido no came desenhado** (tucho de face plana, 1 valor por grau). O magneto é desenhado com a
  mesma `MagnetoGeometry` do cálculo MFEM. O motor desenhado dá partida e gira a ~2950 rpm.
- Cena (`EngineScene`): peças por corpo rígido, materiais PBR, peças fixas em corte (y = 0) com
  faces de seção, ambiente HDR de estúdio procedural, fundo em gradiente, denoiser NRD do Aurora
  na animação.
- App (`aiad.exe`): layout acoplado (árvore de especificação, viewport, propriedades/motor,
  simulação, gráficos), seleção por clique (raio × triângulos) e pela árvore com destaque, vistas
  padrão, enquadrar, navegação do CATIA, bússola, gráficos ImPlot (p×θ com PMS/PMI/IVO/IVC/EVO/
  EVC/faísca, levantamento medido, p-V log-log, rpm/torque). `aiad.exe --run` dá partida ao abrir;
  `--snapshot` verifica (inclusive 60 quadros animados numa largura não alinhada).
- `build-dist.ps1` (raiz): build sequencial com tempo por etapa, deploy em `Dist/` com checagem da
  D18 por `dumpbin`, teste de snapshot com limite de tempo.
- Correções no fork `aurora-4AIAD`: leitura de pontos/normais com checagem de tipo
  (`HdAuroraMesh`); `PTRenderBuffer::data()` sobrescrevia o passo de linha com o passo preenchido
  da GPU (queda ao arrastar janelas e viewport vazio em larguras não múltiplas de 32 px).
- Dear ImGui fixado em **v1.92.9-docking**: o ImPlot v1.0 não desenha itens com o ImGui 1.93.
- Pendências conhecidas: peças ainda são malhas procedurais (B-rep via Command Bus = M1.6);
  edição de parâmetros na UI com regeneração; corte também das peças móveis; arestas/silhuetas.
- Correção posterior no `HdAuroraMesh::Sync` (fork): a comparação `addInstances` estava invertida —
  malha oculta num corpo em movimento (peça desligada na árvore, faísca) tomava o ramo "só
  transformação" e endereçava um recurso nulo. O teste do snapshot oculta/mostra/destaca/corta
  durante a animação; **sem a correção ele cai, com ela passa** (verificado).
- Medição do quadro animado (1200×675): simulação 0,75 ms, pose 0,08 ms, câmera 0,03 ms,
  leitura 8,5 ms, render ~17 ms com **picos**: a faísca ligava/desligava por visibilidade, e o
  Aurora removia e recriava a instância a cada troca (~110 ms; 1,5 s na primeira). Agora a faísca
  existe sempre e liga por escala (só transformação): render ~7 ms + leitura ~9 ms ≈ 60 fps, sem
  picos após o primeiro quadro. Regra: **nunca alternar visibilidade em conteúdo animado**.

### 2026-10-18 — IA: F2 mínima + F3 + F4 (paridade com o Cockpit)

Pedido do dono: a IA deve ter quase todas as funcionalidades do Cockpit (mapa em
[02-arquitetura.md §10.3](02-arquitetura.md)). Implementado, de baixo para cima:

- **`aiad_core`**: JSON (nlohmann 3.12.0 em `deps/_src/nlohmann_json`), UTF-8/UTF-16, Base64, token
  aleatório (BCrypt), caminhos (`%APPDATA%\Tootega\AIAD`), fila da main thread.
- **`aiad_cmd` — Command Bus (F2 mínima)**: registro com JSON Schema + flags (readOnly/destructive/
  idempotent → anotações MCP), validação por schema (tipos, união de tipos, faixas, enum, chaves
  desconhecidas), transação com undo/redo, `UndoTo(profundidade)` (rewind do prompt), trilha de
  auditoria (quem: user/ai). `aiad_undo`, `aiad_redo`, `aiad_history` são comandos do próprio bus.
- **Comandos do motor** (`aiad_app/EngineCommands`): `aiad_engine_get_design`,
  `aiad_engine_set_parameters` (21 parâmetros com unidade e faixa + checagens cruzadas: biela ≥ 1,4 ×
  curso, válvulas cabem no diâmetro, abertura ≥ 120°...; regenera desenho + física + simulação + cena
  num passo desfazível), `aiad_engine_get_derived`, `aiad_parts_list`, `aiad_part_set_visible`
  (desfazível), `aiad_selection_set`, `aiad_view_set`, `aiad_viewport_snapshot` (PNG),
  `aiad_sim_start/stop/status`, `aiad_sim_run` + `aiad_sim_job_status` (job assíncrono). A UI usa os
  mesmos comandos: painel Engine com edição paramétrica (Enter aplica), visibilidade da árvore,
  Start/Stop, **Ctrl+Z / Ctrl+Y**, painel **History** (mudanças e auditoria, IA em laranja).
- **`aiad_bridge` (F3)**: `aiad-bridge/1` em `127.0.0.1` (porta efêmera), token por instância,
  recusa `Origin` e `Host` não-loopback, arquivo de descoberta `%APPDATA%\Tootega\AIAD\bridge\
  aiad-<pid>.json`; as chamadas executam na main thread.
- **`aiad-mcp.exe` (F3)**: servidor MCP stdio estático; `tools/list` = catálogo do bus (gerado, nunca
  escrito duas vezes); imagens viram conteúdo `image`; sem app aberto lista o último catálogo e
  responde com erro acionável; re-descoberta + 1 retry.
- **`aiad_ai` (F4)**: `ClaudeProcess` (CreateProcessW + pipes + Job Object, mesmos argumentos do
  Cockpit: `-p --output-format/--input-format stream-json --include-partial-messages
  --permission-prompt-tool stdio --verbose --permission-mode ...`, `--mcp-config` com o
  `aiad-mcp.exe` preso ao bridge, `--allowedTools` com os comandos readOnly, prompt de sistema por
  arquivo); `AgentSession` (parser tolerante → timeline com streaming, thinking, cards de
  ferramenta, imagens; permissão/pergunta/plano com as respostas que o CLI valida; compactação;
  avisos do motor por forma; custo cumulativo entre processos; contexto, pico, cache, resets;
  limites do `rate_limit_event`; `get_context_usage`); `Transcripts` (contextos salvos em
  `~/.claude/projects/<cwd codificado>`, título ai-title/primeiro prompt, renomear, apagar, rewind
  atômico); `Account` (OAuth `/usage`, `/v1/models`, preços da página pública com cache de 1 dia,
  versão mais nova do CLI no npm).
- **UI** (`AiPanels`, layout do hub do Cockpit): **AI Hub** (cabeçalho com versões e Update, botões
  Usage / Sign in-out / Plugins / MCP / Skills / Credentials vault / Settings, janela de contexto,
  duração, vida do cache + keep-alive, tokens, cache com economia estimada, custo, atividade da
  sessão, modelo / esforço / permissão, Agents & workflows, contextos salvos agrupados por data com
  renomear/apagar/remote control) e **AI Chat** (timeline com Markdown, tabelas e blocos de código,
  thinking recolhível, cards com miniatura do snapshot, busca Ctrl+F, verbosidade, exportar
  HTML/MD, autosave em `<workspace>/.aiad/TimeLine`, rewind da conversa **e do modelo**, composer
  com Enter/Shift+Enter, menus `/` e `@peça`, colar imagem, anexar imagem do viewport, chip da
  seleção; modais de permissão com caracteres invisíveis expostos, perguntas em abas com Other e
  nota, plano com Edit/Preview e "Keep planning"). Fontes Segoe UI/Cascadia Mono (acentos).
- **Testes** (no `build-dist.ps1`): `command_bus_test`, `agent_session_test` (fixtures de
  stream-json: streaming sem duplicar, protocolo de controle, evento desconhecido, custo entre
  turnos, contextos salvos, preços, /usage), `mcp_bridge_test` (aiad-mcp.exe ↔ bridge ↔ bus,
  token, descoberta), `claude_cli_test` (handshake real com o CLI instalado, sem gastar tokens) e
  `aiad.exe --ui-test` (app real: MCP → bridge → bus, bore 84 mm → 332,5 cm³, recusas por faixa e
  por geometria, snapshot, job de simulação "roda sozinho", undo/redo/histórico, chat com fixture,
  captura da janela em `Dist/ui_test.png`).
- Pendências desta etapa: ditado por voz, corretor ortográfico, contador global de tokens por dia,
  gasto de 7 dias por modelo, motor Tootega (`agent.exe`) — ver §10.3.

### 2026-10-19 — M1.6a–d: o motor é um desenho (nada de geometria no código)

Correção pedida pelo dono: todo objeto do viewport vem de desenho, nunca de código. Feito:

- **`aiad_kernel`** (único módulo com SMLib, C++17): esboço em plano (linhas e arcos por *bulge*),
  extrusão, revolução, perfil suave (spline fechada), caixa/cilindro/esfera/toro **como esboços**
  (a primitiva `SmApiCreateBox` do SMLib falha em dimensões de milímetros), tubo helicoidal, tubo por
  caminho (com tentativas de parametrização), booleanas, propriedades de massa, tesselação com
  **normais por ângulo de vinco** (o SMLib não entrega normais), `BrepArray` ↔ stage, e corte de
  malha com tampa (vista de seção). `kernel_test`: volumes e inércias analíticos, normais para fora,
  áreas exatas das tampas (inclusive coroa com furo), ida e volta pelo USD.
- **`aiad_doc`**: o documento é o `UsdStage` — parâmetros com **fórmulas** (avaliador próprio, ciclos e
  nomes desconhecidos recusados, faixas), materiais (densidade = massas), corpos com posição de
  repouso (fórmulas), peças = **histórico de features em JSON** + `BrepArray` exato; regeneração só
  das peças dependentes, **atômica**; ferramentas de esboço genéricas (círculo, retângulo,
  retângulo arredondado, polígono, roda dentada, envoltória de círculos com arcos exatos, came de
  seguidor plano). Comandos no bus: `aiad_parameters_get/set`, `aiad_parameter_define`,
  `aiad_materials_list`, `aiad_material_define`, `aiad_parts_list`, `aiad_part_get/create/update/delete`,
  `aiad_feature_update`, `aiad_body_define`, `aiad_doc_orient`, `aiad_doc_save` (todos desfazíveis).
  `document_test` cobre fórmulas, dependências, atomicidade, desfazer, salvar/reabrir.
- **O motor = `models/Engine_SingleCylinder.usda`**, construído por **`models/Engine_SingleCylinder.aiad.json`**
  (roteiro de comandos: 34 parâmetros de projeto + derivados, 11 materiais, 9 corpos, **63 peças**)
  com `aiad-model build`. 185 comandos em 1,2 s. Massas do desenho: virabrequim+volante+rotor
  0,0342 kg·m² (igual ao gerador antigo), biela 0,336 kg, pistão 0,416 kg; levantamento medido nos
  ressaltos desenhados 8,00/8,00 mm. O motor dá partida e gira a 2945 rpm (antes 2944).
- **App**: abre o documento; `EngineStudy` deriva toda a entrada da simulação do desenho (parâmetros
  por nome, massas por corpo, came pelo papel `intake_lobe`/`exhaust_lobe`); `DocumentScene` mostra
  as peças a partir do B-rep (camada de sessão: malhas de exibição, poses, seleção, corte — nada disso
  vai para o arquivo). Painel Engine edita parâmetros/fórmulas; Properties mostra e edita o histórico
  de features da peça. `aiad_model` (`BuildEngine`/`MeshBuilder`) e `EngineScene` **removidos**.
- Achados: (1) o OpenUSD estático lê `PXR_PLUGINPATH_NAME` na partida do processo — testes registram a
  pasta com `PlugRegistry::RegisterPlugins`; (2) as booleanas do SMLib falham ao cortar sólidos de
  revolução quando a costura fica do lado mantido → a vista de seção é **de exibição** (malha cortada
  e tampada), como no CATIA, e não altera o B-rep; (3) prims desativados não expõem atributos — ler
  antes de desativar (agora o documento mantém os sólidos em memória).
- Pendências: arestas/silhuetas no viewport; tubos ocos (escape, admissão); correia × escape (o dono
  apontou que a correia atravessa o escapamento — agora se corrige pelo desenho); tabela do magneto
  recalculada quando a geometria do magneto muda; botão "otimizar ponto de abertura" (hoje parâmetro).
- **Skill do agente** (`models/.claude/skills/aiad-mcp/SKILL.md`): como operar o app pelo MCP — fluxo
  (ler, mudar com a menor ferramenta, verificar por snapshot/física/simulação, desfazer), as 27
  ferramentas, o formato das features (planos, esboços com *bulge*, ferramentas de esboço, fórmulas), o
  motor (corpos, referenciais, papéis que a simulação lê) e armadilhas. Fica na pasta do modelo, que é a
  pasta de trabalho do agente: o Claude Code o carrega como skill de projeto (conferido com
  `get_context_usage`, sem gastar tokens). O prompt de sistema do app aponta para ele.

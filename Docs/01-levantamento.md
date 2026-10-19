# 01 — Levantamento

> Tootega AIAD (Artificial Intelligence-Aided Design) · plano v0.4 · 2026-10-18
> Estado: **pasta vazia**. Este documento registra o que foi apurado antes de escrever código.

---

## 1. Objetivo do produto

Uma ferramenta de modelagem 3D **sólida e exata** (B-rep NURBS), nativa em C++ **para Windows**, cujo modelo
é **OpenUSD de ponta a ponta**, em que o usuário modela **com as mãos e com uma IA ao mesmo
tempo**, sobre o mesmo documento, com o mesmo undo e as mesmas regras.

| Pilar | Escolha do usuário | Papel |
|-------|--------------------|-------|
| Plataforma | **somente Windows** (x64) | um ambiente só: sem Linux, sem WSL, sem macOS |
| Gráficos | **DirectX 12** | viewport, UI e render de materiais |
| Computação | **CUDA** | simulação, malha e pós-processamento na GPU |
| Kernel geométrico | **USD BRep (SMLib)** — NVIDIA | sólidos exatos, booleanas, fillets, I/O OpenUSD |
| UI | **Dear ImGui** (backends Win32 + DX12) | shell de painéis (docking) |
| Viewport 3D | **Hydra** (UsdImaging) + **fork do Autodesk Aurora** (D3D12/DXR) | render do stage USD (ver §3.2: não existe Storm em DirectX) |
| Formato / modelo | **OpenUSD** | fonte da verdade, arquivo, interoperabilidade |
| Materiais / texturas avançadas | **MaterialX** | aparência física (OpenPBR) via `UsdShade`; shader via Slang → DXIL |
| Simulação estrutural / térmica | **MFEM** (LLNL) + CUDA | elementos finitos em processo |
| Eletromagnetismo | **Palace** (AWS) | solver EM 3D sobre MFEM (Linux; ver §3.6) |
| Fluidos / escoamento compressível | **CEED** (DOE/ECP) → **libCEED** / **HONEE** | Navier-Stokes compressível (Linux; ver §3.7) |
| IA | **motor próprio, provedor trocável** — inicial **Claude Code CLI** | agente que opera a ferramenta |
| Referências internas | **Cockpit** (UI da IA) · **DASE50** (design de MER com MCP) | padrões já validados pela Tootega |

---

## 2. Projetos de referência (Tootega)

### 2.1 Cockpit — `D:\Tootega\Source\Cockpit\Cockpit`

Extensão VS Code (TypeScript + React) que é **GUI do Claude Code CLI**. O que importa para o AIAD:

| Lição do Cockpit | Onde está | Como o AIAD herda |
|------------------|-----------|-------------------|
| **Princípio fundador:** a UI só apresenta e controla; toda a orquestração (loop do agente, ferramentas, MCP, permissões, compactação) mora no CLI | `CLAUDE.md` §1 | O Motor de IA do AIAD **não reimplementa agente**: faz spawn do provedor e renderiza eventos |
| Canal = `claude -p --output-format stream-json --input-format stream-json --verbose` | `src/cli/CliProcessManager.ts` | `ClaudeCliProvider` em C++ (CreateProcess + pipes) com os mesmos argumentos |
| Contrato de eventos tolerante (`system/init`, `assistant`, `user`, `result`, `stream_event`, `control_request`, `rate_limit_event`; desconhecido é ignorado) | `shared/events.ts` | Port para C++ sobre `nlohmann::json`, com o mesmo princípio de tolerância |
| **Dois motores, um protocolo** (`claude` e `tootega`/`agent.exe`), diferenças concentradas em `engineCaps` | `src/cli/Engine.ts` | `IAgentProvider` + `EngineCaps` — é exatamente o "provedor trocável" pedido |
| Permissões interativas via `control_request can_use_tool` → modal | `PermissionModal.tsx` | `PermissionBroker` + modal ImGui |
| Sessões resumíveis (`--resume`), uso/cache/custo/contexto, limites de assinatura | `session/*`, `stats/*` | `SessionStore`, `UsageAggregator`, painel Uso/Contexto |
| `--mcp-config`, `--append-system-prompt`, `--disallowedTools` | `CliOptions` | o app injeta o `aiad-mcp` e o prompt de sistema do domínio CAD |
| i18n obrigatório pt-BR + inglês internacional | `CLAUDE.md` §5 | **não** se aplica: o AIAD é só em inglês (código, comentários e UI) |

### 2.2 DASE50 — `D:\Tootega\Source\DASE50\DASE50`

Designer visual de ORM/MER em VS Code, **operável por IA via MCP**. O que importa:

| Lição do DASE | Onde está | Como o AIAD herda |
|---------------|-----------|-------------------|
| **Arquitetura v2 invertida:** o app **não hospeda MCP**; expõe um *agent bridge* loopback (`POST /bridge {method,args} → {ok, result\|error}`, `dase-bridge/1`) e um servidor MCP **standalone stdio** traduz MCP → bridge | `MCP/docs/MCP_INTEGRATION.md`, `DASE/src/AgentIntegration/Bridge/XAgentBridgeServer.ts` | `aiad_bridge` (no app) + `aiad-mcp.exe` (proxy stdio) |
| Discovery file por janela (`url`, `protocol`, `workspacePath`, `pid`), escolha pela janela viva cujo workspace casa com o `cwd`, re-descoberta + 1 retry | `MCP/src/server/Discovery.ts`, `BridgeClient.ts` | mesmo algoritmo, arquivo em `%APPDATA%\Tootega\AIAD\` |
| Segurança: bind só em `127.0.0.1`, Origin allowlist (anti DNS-rebind), whitelist de comandos | `XAgentBridgeServer.ts` | idem, + token por instância |
| Superfície de ferramentas: leitura / escrita / gatilhos, com anotações `destructiveHint`, `idempotentHint`, `openWorldHint` | `MCP_API_SPEC.md` | idem; anotações vêm das flags do comando |
| Endereçamento por documento (`document` opcional, abre se preciso) e **identidade por ID** com fallback por nome + erro listando candidatos | `MCP_API_SPEC.md` §2 | prims endereçados por **SdfPath** (único por construção) e por nome com desambiguação |
| `XAgentBridge` = adaptador único entre IA e o modelo; mutações sempre via ele, com refresh + dirty + save | `AgentBridge.ts` | vira o **Command Bus**, que no AIAD é usado também pela UI |
| Organização "MCP-native": a IA externa lê contexto JSON, calcula e aplica um plano; há *revert* | `dase_get/apply/revert_organization` | padrão para operações em lote da IA (ex.: "aplicar plano de features") |
| Saídas em Markdown legível por LLM, erros como texto, nunca stack | `MCP_API_SPEC.md` §2 | idem, com JSON estruturado opcional |

**Dívida do DASE que o AIAD evita:** as ferramentas foram escritas duas vezes (lista do bridge
**e** registro MCP em `XDaseMcpTools.ts`/`XDaseMcpWriteTools.ts`). No AIAD o catálogo nasce
**uma vez** no Command Bus (nome + JSON Schema + flags) e o `tools/list` do MCP é gerado dele.

---

## 3. Tecnologias externas (apuradas em 2026-10-18)

### 3.1 USD BRep / SMLib (NVIDIA)

- Repositório **`NVIDIA-Omniverse/usd-brep`**, licença **Apache-2.0**, aberto em **outubro/2026**.
  SMLib veio da Solid Modeling Solutions (adquirida pela NVIDIA em 2022); já é usado no ClassCAD.
- "Biblioteca standalone C++ e Python para modelagem sólida B-rep baseada em NURBS".
  Não exige Omniverse.
- Operações citadas: booleanas (exatas e em malha), fillet/chamfer (inclusive raio variável),
  shell, sweep, interseções superfície-superfície, propriedades de massa, **healing**, tesselação.
- APIs C++ visíveis na documentação: `SM_API`, `SM_API_USD`, `SmApiTessellate`,
  `SmApiUsdTessellateFile`, `SmApiHealBrep`, `SMU_BrepConvert`, `UsdBrep`, `UsdBrepData`,
  importação/exportação via **OCCT** (`OCCT_BREP_IMPORT/EXPORT`: STEP/IGES/JT).
- Representação no USD: schema **`omniSolid`** com o prim **`BrepArray`** em `.usda`.
  O plugin do schema **precisa ser registrado antes** do OpenUSD carregar os tipos
  (`PXR_PLUGINPATH_NAME` → `resources` do omniSolid).
- Build: ferramenta `repo` da NVIDIA + **premake** + **Packman** (que baixa OpenUSD, Python 3.12,
  pybind11). Windows exige **VS2019/2022 com toolset v142** (`msbuild.vs_version` fixado em vs2019).
  Git LFS para fixtures. Linux x86_64/aarch64; macOS só Apple Silicon.
- Avisos do próprio projeto: **"conhecido por crashar dentro do kernel para algumas entradas"**;
  o projeto **não aceita contribuições** (issues sim).
- **Não encontrei** menção a adapter de Hydra/UsdImaging para `BrepArray` → o AIAD deve
  tesselar para `UsdGeomMesh` (decisão D4); confirmar no spike S2.

### 3.2 OpenUSD + Hydra — e o problema do DirectX

- Hydra separa **cena** (UsdImaging / scene index) de **renderizador** (*render delegate*).
- O renderizador de referência, **Storm**, fala com a GPU pelo **Hgi**, que tem backends **OpenGL,
  Metal e Vulkan** (este experimental desde o OpenUSD 24.08). No Windows o padrão é **HgiGL**.
  **Não existe HgiDX12** no OpenUSD; as referências a DirectX 12 encontradas são de terceiros
  (por exemplo, o **Autodesk Aurora**, render delegate de path tracing com **DXR**, Apache-2.0).
- Opções para ter DirectX "de verdade":

  | Opção | Esforço | Comentário |
  |-------|---------|------------|
  | **A. Render delegate próprio em D3D12** (Hydra 2.0) | médio | **recomendada**: controle total (arestas CAD, ID buffer de picking, campos de simulação vindos da CUDA sem cópia), e o Hydra continua sendo o contrato com o USD |
  | B. Storm em HgiGL + interop `WGL_NV_DX_interop2` | baixo | mantém OpenGL dentro do processo, o que contraria a decisão de um ambiente só; serve apenas como contingência |
  | C. Escrever um **HgiDX12** para o Storm rodar em D3D12 | alto | shaders do Storm são GLSL (precisaria de GLSL → SPIR-V → HLSL); frágil a cada upgrade do OpenUSD |
  | + Aurora (DXR) como segundo delegate | baixo/médio | modo fotorrealista opcional; a RTX 2060 SUPER desta máquina suporta DXR |
  | **D. Fork do Aurora com modo raster CAD** | médio | **decidido (v0.4):** reaproveita o D3D12, o Hydra e o MaterialX do Aurora; o fork acrescenta arestas, ID buffer, seleção, campos CUDA e denoiser |

- **Autodesk Aurora** (Apache-2.0): path tracer em tempo real por ray tracing de hardware; DXR no
  Windows (ou Hgi Ray Tracing); delegate `HdAurora`; materiais MaterialX **Standard Surface**;
  instancing; API própria utilizável fora do Hydra. Limitações declaradas: *work in progress*,
  só triângulos, sem denoiser (NVIDIA Real-Time Denoisers planejado), Windows 11 oficial (Windows
  10 "pode funcionar"), CMake ≥ 3.29.3 e Python ≥ 3.11 para o build.
- **`hdUsdBrep`** (do próprio USD BRep): adapter UsdImaging para `BrepArray` + *generative
  procedural* de tesselação + scene index de contornos com `loadWithRenderer: ""`, ou seja,
  funciona com **qualquer** render delegate, inclusive o fork do Aurora.

- Licença do OpenUSD: Tomorrow Open Source Technology License (Apache-2.0 modificada).

### 3.3 Dear ImGui + DirectX 12

- ImGui tem backends oficiais **`imgui_impl_win32`** + **`imgui_impl_dx12`**. O viewport desenha a
  textura do render target do delegate com `ImGui::Image`, no mesmo device D3D12.
- Complementos: **ImGuizmo** (gizmos de transformação), **ImPlot** (gráficos de uso e resultados),
  **imgui_md + md4c** (Markdown no chat). Todos MIT.

### 3.4 MaterialX

- **ASWF, Apache-2.0**; release atual **1.39.4** (atualização do TAC de fev/2026). Suporta
  **OpenPBR Surface** (OpenPBR 1.1 desde MaterialX 1.39.2).
- Integração com OpenUSD: plugin **`usdMtlx`** (lê `.mtlx` como `UsdShade`) e **`hdMtlx`** no
  Hydra. O Storm monta um documento MaterialX a partir da rede de material do Hydra e gera GLSL
  com um gerador de shader específico. Isso vale quando USD e Hydra foram compilados com MaterialX
  (é o padrão do build do OpenUSD).
- OpenUSD 25.05 é o primeiro release que usa MaterialX 1.39 por padrão; o 25.02 usava 1.38.10.
- **Sem Storm, o GLSL não serve.** O MaterialX ganhou um **gerador de shader Slang** (anunciado
  pela ASWF/Khronos). O Slang (NVIDIA → Khronos) compila para HLSL/DXIL, então o caminho para o
  delegate D3D12 é `hdMtlx` (rede do material) → **MaterialX Slang** → **Slang → DXIL**. O alvo
  D3D12 desse gerador precisa ser validado em spike (risco R15).
- Consequência para o AIAD: o OpenUSD que vier do Packman (USD BRep) **precisa** trazer
  `usdMtlx`/`hdMtlx` (risco R2).

### 3.5 MFEM (Modular Finite Element Methods)

- **LLNL, BSD-3**. Release oficial mais recente no site: **4.9** (dez/2025); a Wikipedia já cita
  **4.10** (set/2026), a confirmar no GitHub.
- Build com **CMake** ou makefile. **Serial sem dependências obrigatórias**; paralelo com
  MPI + hypre (+ METIS). Opcionais: PETSc, SUNDIALS, SuiteSparse, libCEED, CUDA/HIP, OCCA, RAJA,
  OpenMP.
- Malhas: tri/quad/tet/hex, superfície, periódicas; espaços **NURBS (isogeométricos)** em 2D/3D.
  Visualização nativa via GLVis; saída para ParaView/VisIt por *data collections*.
- **Não gera malha a partir de CAD.** É preciso um **mesher tetraédrico** entre o B-rep e o MFEM
  (decisão em aberto: fTetWild MPL-2.0, Netgen LGPL-2.1, Gmsh GPL só como processo).
- **Windows/CUDA:** builds **shared com MSVC via CMake** são suportados desde a 4.5; CUDA via
  CMake (`MFEM_USE_CUDA`, `CUDA_ARCH`). A combinação MSVC + nvcc não está documentada como
  testada, e o pareamento de versões CUDA × MSVC importa (risco R16, spike S4).
- Inclui **miniapps eletromagnéticos** (Volta: eletrostática; Tesla: magnetostática; Maxwell:
  transiente; Joule: aquecimento indutivo) e o exemplo **ex18** (Euler compressível em DG). São
  pontos de partida para EM e fluidos **em processo**, caso Palace/HONEE não compilem no Windows.
- Encaixe no AIAD: **em processo**, Windows nativo, **CUDA** (assembly parcial na GPU),
  começando por elasticidade linear, térmico estacionário/transiente e modal. Sem MPI no início
  (uma GPU); **MS-MPI** só se houver multi-GPU.
- Oportunidade de pesquisa: análise **isogeométrica** direto dos patches NURBS do SMLib, sem
  malha tetra.

### 3.6 Palace (eletromagnetismo)

- **AWS Labs, Apache-2.0.** Solver de elementos finitos EM 3D construído sobre **MFEM** e
  **libCEED**, com MPI, BLAS/LAPACK e GPU (CUDA/HIP, MAGMA).
- Problemas: **autovalores** (com perdas e impedâncias concentradas), **driven** no domínio da
  frequência, **transiente** (explícito ou implícito), **eletrostático/magnetostático** (extração de
  matrizes de capacitância e indutância) e modos de guia de onda 2D.
- Build: CMake ≥ 3.27, que baixa a maior parte das dependências; também via Spack. CI em Linux e
  macOS; **Windows não é citado**.
- Com a decisão "somente Windows", restam dois caminhos (spike **S6**):
  1. **compilar o Palace nativo** com MSVC + CUDA (+ MS-MPI), como `.exe` chamado pelo app com o
     caso gerado (malha + configuração);
  2. se o port não for viável, **portar as formulações** para o `aiad_sim` sobre MFEM + CUDA. A
     licença Apache-2.0 permite reaproveitar código com atribuição, e os miniapps EM do MFEM servem
     de base.
- Formato e execução exatos (configuração, formatos de malha, pós-processamento): confirmar na
  documentação "Running Palace" / "Configuration File".

### 3.7 CEED → HONEE (fluidos / escoamento compressível)

- **CEED** (Center for Efficient Exascale Discretizations) é um centro de co-design do DOE/ECP, e
  **não** uma biblioteca única. O pacote do CEED inclui libCEED, MFEM, Nek5000/NekRS, Laghos,
  Remhos, MAGMA e outros.
- Para **escoamento compressível**, o componente adequado é o **HONEE** (High-Order Navier-stokes
  Equation Evaluator): biblioteca de mecânica dos fluidos sobre **libCEED + PETSc**, com
  elementos finitos estabilizados (**SUPG**), elementos de alta ordem *matrix-free*, GPU
  (CUDA/ROCm/Intel), integração implícita e explícita, executável `navierstokes` com opções em YAML.
  Exige o branch principal do libCEED.
- **libCEED: BSD-2-Clause.** A licença do HONEE ainda precisa ser confirmada.
- Alternativas no ecossistema CEED: **NekRS** (incompressível/baixo Mach, espectral, só hexaedros)
  e **Laghos** (hidrodinâmica Lagrangiana compressível sobre MFEM; miniapp de pesquisa).
- Com a decisão "somente Windows" (spike **S7**): o PETSc tem instalação documentada para Windows
  (MSVC via ambiente Cygwin/MSYS2), mas PETSc + CUDA + libCEED + HONEE no Windows não tem relato
  de uso. Caminhos possíveis:
  1. compilar **libCEED + PETSc + HONEE nativos** (MSVC + CUDA) e chamar como `.exe`;
  2. se não for viável, usar **libCEED em processo** com as *QFunctions* de Navier-Stokes do
     HONEE, montadas sobre MFEM (que já integra libCEED) + CUDA, com integração no tempo do MFEM.
     O ex18 (Euler DG) serve de base.
- O domínio de fluido é obtido com booleana no kernel (caixa/duto − peça).
- Interpretei "Compressão" como **escoamento compressível**. Se a intenção for outra (por exemplo,
  compressão estrutural), isso fica no MFEM.

### 3.8 Claude Code CLI

- Instalado nesta máquina em `C:\Users\Hermes\.local\bin\claude.exe` (instalador nativo).
- Modo headless bidirecional: `-p --output-format stream-json --input-format stream-json --verbose`,
  `--resume`, `--mcp-config`, `--append-system-prompt`, `--disallowedTools`,
  permissões via `control_request`.

---

## 4. Ambiente desta máquina

| Item | Encontrado | Observação |
|------|-----------|------------|
| SO | Windows 10 Pro 19045 | **único alvo** (Windows 10/11 x64) |
| GPU | **NVIDIA GeForce RTX 2060 SUPER** — Turing, compute **7.5**, 8 GB, driver 610.74 | suporta D3D12, DXR e CUDA; FP64 lento em GeForce (risco R19) |
| CUDA Toolkit | **13.3** e 12.6 (`CUDA_PATH` → 13.3) | `nvcc` presente; validar pareamento com MSVC (R16) |
| Windows SDK | 10.0.26100 | D3D12; Agility SDK entra por NuGet/vcpkg |
| Visual Studio | 2022 BuildTools (**v143**), VS 18 Community (**v145**) | **v142 ausente** — necessário para o build oficial do USD BRep (risco R1) |
| CMake | via Python Scripts | recomendável CMake oficial ≥ 3.28 + Ninja |
| Python | 3.10.11 | USD BRep/Packman usam **3.12** |
| Git | sim | Git LFS a confirmar |
| Claude Code CLI | sim | provedor inicial |
| WSL | Ubuntu | presente, mas **não faz parte do produto** (decisão: um ambiente só) |

Compatibilidade binária MSVC: libs compiladas com v142 **podem** ser linkadas por v143/v145
(ABI estável desde 2015, com o linker ≥ ao toolset mais novo). O problema é **compilar** o USD BRep
pelo `repo`, que pede v142 — instalar o componente "MSVC v142" no VS 2022 resolve.

---

## 5. Riscos identificados

| # | Risco | Prob. | Impacto | Mitigação |
|---|-------|-------|---------|-----------|
| R1 | Toolchain/ABI: USD BRep fixa v142 e traz o **próprio OpenUSD 25.11** via Packman; o app precisa linkar **a mesma** build do USD | Alta | Bloqueante | **decidido:** compilar o USD BRep com v145 (patch temporário) e usar o OpenUSD do Packman como o OpenUSD do app; ver S1 |
| R2 | O OpenUSD do Packman pode **não trazer UsdImaging/Hd** e/ou **MaterialX** (`usdMtlx`, `hdMtlx`) | Média | Bloqueante | Spike S2; plano B: compilar OpenUSD da mesma versão com imaging + MaterialX (sem precisar de Storm/GL) e recompilar USD BRep contra ele |
| R3 | ~~Sem adapter Hydra para `BrepArray`~~ | — | — | **resolvido:** o USD BRep traz `hdUsdBrep` (adapter + procedural de tesselação + scene index), independente de renderizador |
| R4 | Crashes do kernel em entradas ruins (documentado) | Média | Alto | fachada `aiad_kernel` com SEH + timeout, validação antes da operação, autosave; depois, worker de kernel fora do processo |
| R5 | Projeto não aceita contribuições | Certa | Médio | patches locais isolados em `deps/patches`, upgrades controlados |
| R6 | Mudanças no protocolo stream-json do CLI | Média | Médio | parser tolerante + fixtures reais versionadas (lição do Cockpit) |
| R7 | Sketch 2D com restrições exige solver; os conhecidos são LGPL (PlaneGCS/FreeCAD) ou GPL (SolveSpace) | Alta | Médio | F5: avaliar licença; alternativa: solver próprio mínimo (distância, ângulo, coincidência, tangência) |
| R8 | Concorrência IA × usuário no mesmo stage | Média | Médio | toda mutação na main thread via Command Bus; bloqueio visível "IA operando" |
| R9 | Custo/latência de cada chamada MCP (round trip processo → HTTP → main thread) | Baixa | Baixo | ferramentas em lote (aplicar plano de features) como o `apply_organization` do DASE |
| R10 | **Mesher**: decidido o **Netgen** (LGPL-2.1). Riscos: `dllimport` do `nglib` no Windows, healing OCC manual, falhas de malha de superfície em STEP reais | Média | Alto | `nglib` estático; `.brep` do `OCCT_BREP_EXPORT` (sem acoplar ABI de OCCT); healing explícito; caminho STL como alternativa; spike S5 |
| R11 | **Palace e HONEE não são suportados no Windows**: a decisão é **fork e conversão** (`palace-4AIAD`, `honee-4AIAD`, `libceed-4AIAD`, `petsc-4AIAD`). Pontos duros: autovalores (SLEPc/PETSc ou ARPACK-NG com Fortran), configure do PETSc (shell POSIX no build), VLAs C99 no libCEED (MSVC não tem), LIBXSMM sem Windows | Certa | Alto | superbuild CMake por fork; cuDSS como solver direto; Intel `ifx` para Fortran; clang-cl para C99; LIBXSMM desligado; rebase periódico do upstream |
| R12 | **Persistência de cargas/apoios**: condições de contorno presas a faces somem quando a topologia muda após o regen (*topological naming*) | Alta | Alto | IDs persistentes de face (D5) + revalidação do estudo depois de cada regen, com issue explícita quando uma referência quebra |
| R13 | Maturidade/estabilidade do HONEE (exige o branch principal do libCEED) | Média | Médio | fixar commits; fixtures de casos canônicos; caminho em processo (libCEED + MFEM) como alternativa |
| R14 | Simulações longas competindo com a UI e com a IA | Média | Médio | comandos `async` → jobs com id, progresso e cancelamento; a IA consulta o status em vez de bloquear |
| R15 | **MaterialX → D3D12**: o gerador Slang é recente; o alvo HLSL/DXIL precisa ser provado | Média | Médio | spike S3; contingência: gerar HLSL a partir de um subconjunto OpenPBR mantido pelo projeto |
| R16 | **CUDA × MSVC**: cada versão do CUDA aceita só certas versões do MSVC (v145 pode ainda não ser suportado); MFEM/libCEED com nvcc + MSVC são pouco exercitados | Média | Alto | spike S4: fixar o par CUDA 13.x + v143; CI com esse par |
| R17 | **Fork do Aurora**: o upstream é *work in progress*, só oficial em Windows 11, sem denoiser, só triângulos e sem modo raster; o fork precisa acrescentar arestas, ID buffer, seleção e campos | Alta | Alto | escopo mínimo do modo raster na F1; denoiser e OpenPBR depois; manter o diff do fork pequeno e isolado para facilitar rebase |
| R18 | Interop D3D12 ↔ CUDA (memória externa, fences) exige o mesmo adaptador e sincronização correta | Média | Médio | `aiad_gpu` escolhe o adaptador por LUID e centraliza a interop; testes de stress |
| R19 | **FP64 em GeForce** é 1/32 a 1/64 do FP32 | Alta | Médio | **decidido (D15):** FP32 automático quando a placa for limitada. A precisão do MFEM é de compilação, e a 4.9 não aceita fp32 com CEED, SUNDIALS, SuperLU, SLEPc e GSLIB, então são duas DLLs de backend; Palace/HONEE começam em FP64 |

---

## 6. Fontes

- USD BRep — repositório: https://github.com/NVIDIA-Omniverse/usd-brep
- USD BRep — API Python (lista também a API C++): https://nvidia-omniverse.github.io/usd-brep/latest/docs/python.html
- DEVELOP3D — lançamento do kernel USD BRep: https://develop3d.com/cad/nvidia-launches-usd-brep-geometry-kernel-for-solid-modelling/
- 3D Printing Industry — análise do kernel aberto: https://3dprintingindustry.com/news/nvidia-opens-cad-modelling-kernel-what-could-it-mean-for-ai-assisted-design-and-3d-printing-255720/
- OpenUSD — `UsdImagingGLEngine`: https://openusd.org/23.05/api/class_usd_imaging_g_l_engine.html
- AOUSD fórum — Hydra com múltiplos viewports: https://forum.aousd.org/t/hydra-multiple-viewports/1685
- OpenUSD — MaterialX no Hydra e no USD: https://openusd.org/release/api/_page__material_x__in__hydra__u_s_d.html
- ASWF TAC — atualização do MaterialX 2026: https://tac.aswf.io/meetings/2026-02-04/MaterialX_TAC_Update_2026_Final.pdf
- AOUSD — OpenUSD v25.02: https://aousd.org/blog/announcing-openusd-v25-02-key-features-and-improvements/
- MFEM — repositório: https://github.com/mfem/mfem · site: https://mfem.org · Wikipedia: https://en.wikipedia.org/wiki/MFEM
- Palace — repositório: https://github.com/awslabs/palace · documentação: https://awslabs.github.io/palace/stable/
- CEED — relatório MS13: https://ceed.exascaleproject.org/docs/ceed-ms13-report.pdf
- libCEED (BSD-2): https://awesome.ecosyste.ms/projects/github.com%2Fceed%2Flibceed
- HONEE — README: https://fluid.colorado.edu/source/xref/honee/README.md
- Malha tetra a partir de CAD (Gmsh/OCC): https://cad.pyvista.org/examples/05_workflows/cad_to_fea_tet_mesh.html
- fTetWild: https://github.com/wildmeshing/fTetWild
- Netgen: https://github.com/NGSolve/netgen · formatos de malha do MFEM: https://mfem.org/mesh-formats/
- Netgen/nglib no Windows (problemas de link): https://forum.ngsolve.org/t/compiling-and-linking-to-nglib/2119 · STEP com nglib: https://forum.ngsolve.org/t/step-file-meshing-with-nglib/1174
- Palace — instalação e opções de build: https://awslabs.github.io/palace/stable/install/
- MFEM precisão simples (`real_t`, PR #3922): https://github.com/mfem/mfem/pull/3922
- OpenUSD — HdStorm (backends Hgi: GL, Metal, Vulkan): https://openusd.org/release/api/hd_storm_page_front.html
- CG Channel — OpenUSD passa a suportar Vulkan (24.08): https://www.cgchannel.com/2024/08/openusd-now-officially-supports-vulkan/
- Autodesk hydra-viewport-toolbox (HgiGL é o padrão no Windows): https://github.com/Autodesk/hydra-viewport-toolbox/pull/212
- Autodesk Aurora (Hydra + DXR): https://github.com/Autodesk/Aurora
- MaterialX ganha gerador Slang (ASWF): https://www.aswf.io/blog/materialx-adds-support-for-slang-shader-generation/
- Khronos — MaterialX + Slang: https://www.khronos.org/news/permalink/materialx-adds-support-for-slang-shader-generation
- MFEM — Building (CUDA via CMake): https://mfem.org/building
- libCEED — Getting Started: https://libceed.readthedocs.io/en/latest/gettingstarted/
- Referências internas: `Cockpit/CLAUDE.md`, `Cockpit/shared/events.ts`, `Cockpit/src/cli/Engine.ts`,
  `Cockpit/src/cli/CliProcessManager.ts`, `DASE50/MCP/docs/MCP_INTEGRATION.md`,
  `DASE50/MCP/docs/MCP_API_SPEC.md`, `DASE50/MCP/src/server/*`,
  `DASE50/DASE/src/AgentIntegration/Bridge/XAgentBridgeServer.ts`

# Tootega AIAD — Artificial Intelligence-Aided Design

Ferramenta de CAD/CAE 3D para **Windows** em que o usuário **e uma IA** operam o mesmo modelo exato:
geometria B-rep NURBS (USD BRep / SMLib) num documento **OpenUSD**, materiais **MaterialX**, viewport
**Hydra** com path tracing **DirectX 12 (DXR)** e simulação em **CUDA** (MFEM, Project Chrono).
Toda alteração do modelo passa pelo mesmo **Command Bus**, com undo, validação e trilha de auditoria,
seja ela feita pela interface, por script ou pela IA (via MCP).

![Tootega AIAD: motor monocilíndrico em corte, chat da IA e AI Hub](Docs/images/aiad.png)

## Fase do projeto

**Marco atual: motor de combustão de quatro tempos desenhado na ferramenta e funcionando no seu
simulador.** As etapas M1.1 a M1.6d estão concluídas (ver [Docs/03-plano-execucao.md](Docs/03-plano-execucao.md)
§0 e §6). Os spikes de infraestrutura S1 a S8 estão encerrados (o S6, eletromagnetismo com Palace,
compila e roda; está fora do caminho do marco).

| Pronto | Em aberto |
|--------|-----------|
| Documento OpenUSD paramétrico: 34 parâmetros de projeto + derivados (fórmulas), 11 materiais, 9 corpos rígidos, 63 peças B-rep com histórico de features | arestas/silhuetas no viewport |
| Simulação lida do desenho: massas e inércias das peças × densidade, levante de válvula medido no came desenhado | tubos ocos (escapamento/admissão) |
| Mecanismo (Chrono) + ciclo Otto 0D (válvulas, combustão de Wiebe, perda de calor) + magneto 2D (MFEM) → faísca | correia cruzando o escapamento |
| Viewport Hydra/Aurora com path tracing, seleção, destaque, vista em corte | tabela do magneto não é recalculada quando a geometria do magneto muda |
| Camada de IA no nível do Cockpit: hub, sessões em abas, permissões, perguntas, planos, uso/custo | estimativa de custo ao vivo durante o turno |
| Servidor MCP (`aiad-mcp.exe`) com o catálogo gerado do Command Bus + skill para o agente | |

## O que ele faz e como

### Aplicação (`aiad.exe`)

Abre `models\Engine_SingleCylinder.usda` (ao lado do executável) e monta o motor a partir do desenho.

| Janela | O que faz |
|--------|-----------|
| **Viewport** | path tracing do modelo; **Espaço + arrastar com o botão esquerdo** (ou Alt + esquerdo, ou meio + direito) gira; botão do meio desloca; roda aproxima; clique seleciona a peça |
| **Specification Tree** / **Properties** | árvore de corpos e peças; material, massa, volume, features e parâmetros da peça selecionada |
| **Engine** | parâmetros do projeto (diâmetro, curso, comando de válvulas…) e grandezas derivadas do desenho |
| **Simulation** / **Plots** | roda o motor (mecanismo + ciclo + ignição) e mostra rotação, pressão no cilindro por ângulo e levante de válvula medido nos cames desenhados |
| **History** | histórico do Command Bus; **Ctrl+Z / Ctrl+Y** desfazem e refazem qualquer comando, inclusive os da IA |
| **AI Hub** | sessões do Claude Code, uso e limites, servidores MCP, skills, plugins, cofre de credenciais, configurações; **+ New session** abre uma aba de chat |
| **Chat** (uma aba por sessão) | prompt com fila (**Send** enquanto o turno anterior roda, **Stop** ao lado), Markdown, permissões (**Ctrl+Enter** permite, **Esc** nega), perguntas, planos, estatísticas ao vivo |

Opções de linha de comando:

```text
aiad.exe                      interface normal
aiad.exe --run                abre e já põe o motor para rodar
aiad.exe --snapshot <png>     teste sem janela: monta, anima 60 quadros, alterna corte/destaque, grava PNGs
aiad.exe --ui-test <png>      teste com janela real: MCP -> bridge -> Command Bus, simulação, chat, captura
```

### IA e MCP

- O agente (Claude Code CLI, `stream-json`) roda com a **pasta do modelo aberto** como diretório de
  trabalho, e lá encontra a skill [`models/.claude/skills/aiad-mcp/SKILL.md`](models/.claude/skills/aiad-mcp/SKILL.md).
- `aiad-mcp.exe` é um servidor MCP stdio que conversa com a aplicação aberta pelo bridge loopback
  `aiad-bridge/1` (token por instância). As ferramentas são os comandos do Command Bus:
  parâmetros, materiais, peças e features, corpos, orientação, visibilidade, seleção, câmera,
  snapshot do viewport, simulação (job assíncrono), salvar, undo/redo.
- A IA é só mais um operador: tudo o que ela faz aparece no histórico, passa por permissão e pode
  ser desfeito.

### Modelo por desenho (`aiad-model.exe`)

Tudo o que aparece no viewport é peça do documento, criada por comandos; nada de geometria no código.
O motor de exemplo é um script de comandos:

```powershell
Dist\aiad-model.exe build models\Engine_SingleCylinder.aiad.json models\Engine_SingleCylinder.usda
Dist\aiad-model.exe info  models\Engine_SingleCylinder.usda     # peças, massas por corpo, erros de regeneração
```

## Requisitos

| Item | Versão |
|------|--------|
| Sistema | Windows 10/11 x64 |
| GPU | NVIDIA com DXR (RTX), driver recente; CUDA e D3D12 no mesmo adaptador |
| Visual Studio | **2026 (18) Community**, carga "Desenvolvimento para desktop com C++", toolset **v145** (MSVC 14.51), Windows SDK 10.0.26100, CMake 4.3 incluído |
| CUDA Toolkit | **13.3** no caminho padrão (`C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3`) |
| Ferramentas | Git, PowerShell 7, 7-Zip (opcional; sem ele o `tar` do Windows extrai os pacotes) |
| IA (opcional) | [Claude Code CLI](https://claude.com/claude-code) no `PATH`, com login feito |

## Compilar e executar

```powershell
git clone --recurse-submodules https://github.com/HermesSilva/TootegaAIAD.git
cd TootegaAIAD
pwsh -File deps\fetch-prebuilt.ps1     # dependências pré-compiladas (~0,5 GB), verificadas por SHA-256
pwsh -File build-dist.ps1              # compila, roda os testes, monta Dist\ e roda os testes da aplicação
Dist\aiad.exe
```

O `build-dist.ps1` executa nove passos cronometrados e para no primeiro erro: configura (preset
`vs2026`), compila `aiad.exe`, `aiad-mcp.exe`, `aiad-model.exe` e os testes, roda os testes unitários
(`kernel`, `document`, `command_bus`, `agent_session`, `mcp_bridge`, `claude_cli`; o último é pulado
sem a CLI), monta `Dist\` (verificando com `dumpbin` que só as DLLs permitidas são importadas), e roda
`--snapshot` e `--ui-test`. `-SkipSnapshot` pula os dois últimos. `Dist\` é autocontido e pode ser
copiado para outra máquina com GPU NVIDIA.

### Visual Studio 2026

```powershell
cd src
cmake --preset vs2026                  # gera src\_build\TootegaAIAD.slnx (um .vcxproj por alvo)
start _build\TootegaAIAD.slnx
```

A solução organiza os projetos do AIAD na raiz, os testes em **Tests** e os forks de dependências
alterados para o AIAD em **ThirdParty**: `Aurora` (compilado junto com o app), `libCEED` e `Palace`
(fora do build padrão; compile o projeto pelo menu). Para depurar, use `aiad` como projeto de
inicialização; a pasta de execução é `src\_build\aurora\bin\Release`.

## Dependências

Política: tudo o que é nosso ou de terceiros é **linkado estático** num único `aiad.exe`. As únicas
DLLs permitidas são as do sistema (D3D12, DXGI, Win32), o driver NVIDIA, o compilador de shaders
(`dxcompiler`/`dxil`, Slang), cuSPARSE/cuBLAS (a NVIDIA não os distribui estáticos), o runtime do
MSVC (`/MD`) e, só nos solvers, o MS-MPI.

| Dependência | Versão | Licença | Como vem |
|-------------|--------|---------|----------|
| OpenUSD (+ oneTBB 2021.13, OpenSubdiv 3.6.1) | 25.11 (`363a7c8d`), estático, sem Python | TOST 1.0 / Apache-2.0 | pré-compilado |
| NVIDIA USD BRep (SMLib, `omniSolid`, `hdUsdBrep`) | 1.0.0 (`c9c5979f`), estático | Apache-2.0 | pré-compilado |
| Autodesk Aurora + HdAurora | fork [aurora-4AIAD](https://github.com/HermesSilva/aurora-4AIAD) | Apache-2.0 | submódulo, compilado com o app |
| Externals do Aurora (MaterialX 1.39.5 com Slang, Slang, DXC, NRD/NRI, Boost…) | ver `deps/_externals/aurora/*.version.txt` | várias | pré-compilado |
| MFEM + CUDA | 4.10, fp64 | BSD-3 | pré-compilado |
| Project Chrono (`Chrono_core`) | 10.0.0, estático | BSD-3 | pré-compilado |
| Eigen | 3.4.1 | MPL-2.0 | submódulo |
| Dear ImGui (docking) / ImPlot | v1.92.9-docking / v1.0 | MIT | submódulos |
| nlohmann/json | 3.12.0 | MIT | no repositório (`deps/_src/nlohmann_json`) |
| libCEED | fork [libceed-4AIAD](https://github.com/HermesSilva/libceed-4AIAD) | BSD-2 | submódulo (ThirdParty) |
| Palace | fork [palace-4AIAD](https://github.com/HermesSilva/palace-4AIAD) | Apache-2.0 | submódulo (ThirdParty; precisa de MS-MPI SDK e LLVM `flang`) |

Os pacotes pré-compilados ficam no release [`deps-2026.10.19`](https://github.com/HermesSilva/TootegaAIAD/releases),
listados com SHA-256 em [`deps/prebuilt.json`](deps/prebuilt.json). O `fetch-prebuilt.ps1` também
corrige os caminhos absolutos gravados nos arquivos CMake dos pacotes para a pasta do clone.

Para **recompilar as dependências a partir do código-fonte**, use os scripts de
[`deps/_src`](deps/_src) e [`deps/_externals/aurora`](deps/_externals/aurora) com os patches de
[`deps/patches`](deps/patches); ordem, versões e armadilhas estão em [deps/README.md](deps/README.md).
O mantenedor publica os pacotes novos com `pwsh -File deps\pack-prebuilt.ps1 -Publish`.

## Estrutura

| Pasta | Conteúdo |
|-------|----------|
| `src/aiad_core` | JSON, texto, caminhos, fila da thread principal |
| `src/aiad_cmd` | Command Bus: registro, validação por JSON Schema, undo/redo, auditoria |
| `src/aiad_bridge`, `src/aiad_mcp` | bridge loopback `aiad-bridge/1` e servidor MCP `aiad-mcp.exe` |
| `src/aiad_ai` | processo do Claude Code CLI, sessão `stream-json`, transcrições, conta e uso |
| `src/aiad_kernel` | fachada do USD BRep/SMLib (único módulo que o inclui; C++17) |
| `src/aiad_doc` | documento OpenUSD paramétrico, expressões, features, comandos de documento, `aiad-model` |
| `src/aiad_sim` | ciclo Otto 0D, magneto (MFEM), mecanismo (Chrono) |
| `src/aiad_render` | viewport Hydra + HdAurora |
| `src/aiad_app` | aplicação ImGui: cena, estudo do motor, comandos do motor, painéis de IA |
| `models/` | documentos (`*.usda`) e scripts de comandos (`*.aiad.json`); skill do agente |
| `deps/` | scripts, patches, manifesto dos pré-compilados e submódulos |
| `spikes/` | spikes S2–S8 (código de validação de cada tecnologia) |
| `Docs/` | levantamento, arquitetura, plano de execução, diagramas |

## Documentos

| Documento | Conteúdo |
|-----------|----------|
| [Docs/01-levantamento.md](Docs/01-levantamento.md) | referências internas (Cockpit, DASE50), tecnologias, ambiente, riscos |
| [Docs/02-arquitetura.md](Docs/02-arquitetura.md) | princípios, decisões, módulos, contratos (Command Bus, IA, Bridge, GPU, simulação, render) |
| [Docs/03-plano-execucao.md](Docs/03-plano-execucao.md) | fases, spikes, critérios de aceite, registro de resultados |
| [CLAUDE.md](CLAUDE.md) | regras do projeto para agentes (e para pessoas) |

| Diagrama | O que mostra |
|----------|--------------|
| [arquitetura-blocos.svg](Docs/diagrams/arquitetura-blocos.svg) | blocos do sistema e protocolos entre processos |
| [dependencias.svg](Docs/diagrams/dependencias.svg) | alvos CMake, dependências de build e runtime, terceiros e licenças |
| [fluxo-ia.svg](Docs/diagrams/fluxo-ia.svg) | sequência de um pedido à IA, do prompt ao snapshot de verificação |

## Licença

Código aberto. A licença do projeto segue o que as dependências exigem; a recomendação atual é
**Apache-2.0** (decisão pendente do mantenedor). Mantenedor: Tootega Pesquisa e Inovação
([HermesSilva](https://github.com/HermesSilva)).

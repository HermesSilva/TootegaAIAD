# CLAUDE.md

Diretivas para trabalhar nos modelos do Tootega AIAD nesta pasta.

## 1. Alterações no modelo somente via MCP

- **Nunca** editar diretamente os arquivos do modelo (`*.aiad.json`, `*.usda` ou qualquer outro arquivo gerado pelo aplicativo), nem com Edit/Write, nem com scripts, nem com comandos de shell.
- Toda alteração deve ser feita pelas ferramentas MCP `aiad` (`mcp__aiad__*`), que passam pelo Command Bus do aplicativo e mantêm undo e trilha de auditoria.
- Ler antes de alterar (`aiad_engine_get_design`, `aiad_engine_get_derived`, `aiad_parts_list`, `aiad_parameters_get`) e verificar depois (`aiad_viewport_snapshot`, `aiad_sim_run` + `aiad_sim_job_status`).
- Salvar apenas via `aiad_doc_save`.

## 2. Limitações do MCP devem ser reportadas

- Se uma alteração **não puder** ser feita pelas ferramentas MCP, **não** contornar a limitação editando os arquivos.
- Em vez disso, parar e reportar ao usuário, para correção no aplicativo:
  - o que se tentou alterar;
  - qual ferramenta/comando faltou ou falhou (com a mensagem de erro, se houver);
  - o que o aplicativo precisaria expor para tornar a alteração possível.

## 3. Salvar ao final de cada conjunto de alterações

- Ao concluir um conjunto de alterações pedido pelo usuário, e depois de verificá-lo (snapshot/simulação), **sempre** salvar o modelo com `aiad_doc_save`, sem esperar pedido explícito.
- Passar sempre o `path` explícito do arquivo do modelo em edição, para não gravar em outro arquivo por engano (o MCP não informa qual arquivo o aplicativo tem aberto).
- Informar ao usuário o caminho em que o modelo foi salvo.
- Se a verificação falhar ou o conjunto ficar incompleto, não salvar: reportar o problema primeiro.

## 4. Escopo de busca no disco

- **Nunca** pesquisar, listar ou ler arquivos em pastas fora da pasta do modelo em edição no momento.
- Exceção: somente quando o usuário pedir explicitamente e informar a pasta.

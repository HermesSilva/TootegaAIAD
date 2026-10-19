"""Generate Docs/diagrams/fluxo-ia.svg (sequence diagram of one AI request)."""
import sys
from xml.sax.saxutils import escape

OUT = sys.argv[1]

LANES = [
    ("Usuário", "designer", "#ECEFF1", "#455A64"),
    ("Painel Chat", "aiad_ui", "#E3F2FD", "#1565C0"),
    ("Motor de IA", "aiad_ai", "#F3E5F5", "#8E24AA"),
    ("Claude Code CLI", "processo externo", "#EDE7F6", "#5E35B1"),
    ("aiad-mcp.exe", "processo externo", "#E0F2F1", "#00796B"),
    ("Agent Bridge", "aiad_bridge", "#E0F2F1", "#00796B"),
    ("Command Bus", "aiad_cmd", "#FFF3E0", "#EF6C00"),
    ("Documento + Kernel", "aiad_doc · SMLib", "#E8F5E9", "#2E7D32"),
    ("Render", "Hydra · D3D12", "#FCE4EC", "#C2185B"),
]
X0, DX = 110, 172
W = X0 * 2 + DX * (len(LANES) - 1)
def lx(i): return X0 + DX * i

# (phase title, [ (from, to, label, kind) ]) ; kind: call | ret | self | proc
PHASES = [
    ("1 · Pedido", [
        (0, 1, "“flange Ø120 com 6 furos M8 no círculo de 90”", "call"),
        (1, 2, "Send(prompt + contexto: seleção, unidades, doc ativo)", "call"),
        (2, 3, "stdin: {type:\"user\", message} — stream-json", "proc"),
        (3, 2, "stdout: stream_event / assistant (texto, thinking, tool_use)", "procret"),
        (2, 1, "timeline em streaming (token a token)", "ret"),
    ]),
    ("2 · Leitura de contexto", [
        (3, 4, "MCP tools/call aiad_get_scene", "mcp"),
        (4, 5, "POST /bridge {method:\"GetScene\"}", "mcp"),
        (5, 6, "enfileira na main thread (future + timeout)", "call"),
        (6, 7, "comando readOnly: lê o stage composto", "call"),
        (7, 6, "árvore, features, unidades, issues", "ret"),
        (6, 3, "resultado (texto/JSON) volta pela mesma rota", "ret"),
    ]),
    ("3 · Permissão (humano no circuito)", [
        (3, 2, "control_request can_use_tool (aiad_feature_create)", "procret"),
        (2, 1, "PermissionBroker → modal com nome + parâmetros", "call"),
        (1, 0, "Permitir · Negar · Sempre permitir", "call"),
        (0, 1, "Permitir", "ret"),
        (1, 2, "decisão", "ret"),
        (2, 3, "stdin: control_response {behavior:\"allow\"}", "proc"),
    ]),
    ("4 · Mutação", [
        (3, 4, "tools/call aiad_feature_create {revolve, hole_pattern…}", "mcp"),
        (4, 5, "POST /bridge {method, args}", "mcp"),
        (5, 6, "Execute(cmd, args) na main thread", "call"),
        (6, 7, "transação + regen do histórico (SMLib: booleana, padrão)", "call"),
        (7, 8, "UsdNotice: prims mudaram", "call"),
        (8, 8, "tesselar → UsdGeomMesh → redraw (D3D12)", "self"),
        (6, 3, "ok + ids criados + issues de validação + entrada no undo", "ret"),
    ]),
    ("5 · Verificação visual", [
        (3, 4, "tools/call aiad_viewport_snapshot {view:\"iso\"}", "mcp"),
        (4, 5, "POST /bridge {method:\"Snapshot\"}", "mcp"),
        (5, 6, "Execute(\"view.snapshot\") na main thread", "call"),
        (6, 8, "comando de view → render target offscreen → PNG", "call"),
        (8, 3, "imagem PNG (content image) — a IA confere o resultado", "ret"),
    ]),
    ("6 · Fechamento", [
        (3, 2, "result {usage, total_cost_usd, num_turns}", "procret"),
        (2, 1, "resumo + contexto / cache / custo atualizados", "ret"),
        (1, 0, "modelo pronto · Ctrl+Z desfaz cada passo da IA", "ret"),
    ]),
]

STEP, HEAD_Y, HEAD_H = 40, 64, 56
y = HEAD_Y + HEAD_H + 30
rows = []          # (y, msg)
bands = []         # (y0, y1, title)
for title, msgs in PHASES:
    y0 = y
    y += 44
    for m in msgs:
        rows.append((y, m))
        y += STEP + (8 if m[3] == "self" else 0)
    bands.append((y0, y - STEP + 18, title))
    y += 16
H = y + 70

STYLE = {
    "call":    ("#37474F", "", "ar"),
    "ret":     ("#78909C", "6 4", "ag"),
    "proc":    ("#6A1B9A", "", "ap"),
    "procret": ("#6A1B9A", "6 4", "ap"),
    "mcp":     ("#00796B", "", "at"),
    "self":    ("#C2185B", "", "ak"),
}

o = []
o.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
         'font-family="Segoe UI, Helvetica, Arial, sans-serif">')
o.append('  <title>Tootega AIAD — Sequência de um pedido à IA</title>')
o.append('  <defs>')
for mid, col in (("ar", "#37474F"), ("ag", "#78909C"), ("ap", "#6A1B9A"), ("at", "#00796B"), ("ak", "#C2185B")):
    o.append(f'    <marker id="{mid}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" '
             f'orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="{col}"/></marker>')
o.append('''    <style>
      .h  { font-size: 24px; font-weight: 700; fill: #263238; }
      .sh { font-size: 13px; fill: #546E7A; }
      .hn { font-size: 13.5px; font-weight: 700; fill: #263238; }
      .hs { font-size: 11px; fill: #546E7A; }
      .pt { font-size: 12.5px; font-weight: 700; fill: #455A64; }
      .ml { font-size: 12px; fill: #263238; paint-order: stroke; stroke: #FFFFFF; stroke-width: 4px; }
      .cs { font-size: 11px; fill: #546E7A; }
    </style>
  </defs>''')
o.append(f'  <rect x="0" y="0" width="{W}" height="{H}" fill="#FAFBFC"/>')
o.append('  <text x="20" y="38" class="h">Tootega AIAD — Sequência de um pedido à IA</text>')
o.append(f'  <text x="{W-20}" y="38" class="sh" text-anchor="end">'
         'exemplo: “flange Ø120 com 6 furos” · provedor inicial = Claude Code CLI</text>')

for i, (y0, y1, title) in enumerate(bands):
    fill = "#FFFFFF" if i % 2 == 0 else "#F1F4F6"
    o.append(f'  <rect x="14" y="{y0}" width="{W-28}" height="{y1-y0}" rx="8" fill="{fill}" stroke="#E0E6EA"/>')
    o.append(f'  <text x="26" y="{y0+19}" class="pt">{escape(title)}</text>')

bottom = H - 60
for i, (name, sub, fill, stroke) in enumerate(LANES):
    x = lx(i)
    o.append(f'  <line x1="{x}" y1="{HEAD_Y+HEAD_H}" x2="{x}" y2="{bottom}" stroke="{stroke}" '
             'stroke-width="1.2" stroke-dasharray="3 4" opacity="0.7"/>')
    o.append(f'  <rect x="{x-78}" y="{HEAD_Y}" width="156" height="{HEAD_H}" rx="8" fill="{fill}" '
             f'stroke="{stroke}" stroke-width="1.6"/>')
    o.append(f'  <text x="{x}" y="{HEAD_Y+24}" class="hn" text-anchor="middle">{escape(name)}</text>')
    o.append(f'  <text x="{x}" y="{HEAD_Y+42}" class="hs" text-anchor="middle">{escape(sub)}</text>')

for y, (a, b, label, kind) in rows:
    col, dash, mk = STYLE[kind]
    da = f' stroke-dasharray="{dash}"' if dash else ""
    if kind == "self":
        x = lx(a)
        o.append(f'  <path d="M{x},{y-8} L{x+44},{y-8} L{x+44},{y+14} L{x+4},{y+14}" fill="none" '
                 f'stroke="{col}" stroke-width="1.7" marker-end="url(#{mk})"/>')
        o.append(f'  <text x="{x-8}" y="{y+6}" class="ml" text-anchor="end">{escape(label)}</text>')
        continue
    x1, x2 = lx(a), lx(b)
    sgn = 1 if x2 > x1 else -1
    o.append(f'  <line x1="{x1}" y1="{y}" x2="{x2 - sgn*3}" y2="{y}" stroke="{col}" stroke-width="1.7"{da} '
             f'marker-end="url(#{mk})"/>')
    cx = (x1 + x2) / 2
    o.append(f'  <text x="{cx:.0f}" y="{y-6}" class="ml" text-anchor="middle">{escape(label)}</text>')

ly = H - 34
leg = [("#37474F", "", "ar", "chamada em processo"), ("#78909C", "6 4", "ag", "retorno"),
       ("#6A1B9A", "", "ap", "stdin/stdout do provedor (stream-json)"),
       ("#00796B", "", "at", "MCP stdio / HTTP aiad-bridge/1")]
lx0 = 20
for col, dash, mk, txt in leg:
    da = f' stroke-dasharray="{dash}"' if dash else ""
    o.append(f'  <line x1="{lx0}" y1="{ly}" x2="{lx0+60}" y2="{ly}" stroke="{col}" stroke-width="1.8"{da} '
             f'marker-end="url(#{mk})"/>')
    o.append(f'  <text x="{lx0+70}" y="{ly+4}" class="sh">{escape(txt)}</text>')
    lx0 += 90 + len(txt) * 7
o.append(f'  <text x="{W-20}" y="{H-14}" class="cs" text-anchor="end">'
         'Tootega Pesquisa e Inovação · plano v0.4 · 2026-10-18</text>')
o.append('</svg>')

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(o) + "\n")
print(OUT, W, H)

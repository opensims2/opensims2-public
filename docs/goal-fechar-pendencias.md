# Goal — Fechar as pendências do port (item 13 e a fila restante)

- **Status:** **aberto em 18/10/2026** — continua o `goal-port-fidelity.md`. **Fechados:**
  1–4, 9, 11, **5, 8, 10, 12, 13 (TTAB), 14 (formatos do AL), o RayTrace 0x78** e
  **todos os tipos do Set to Next (13, 19/20 e os do AL)**. **Aberto:** o **7**
  (performance, parcial: o `tickOverlayEvents` e a carga), as três âncoras do **soak
  round 20** (o gambá do AL gastando o orçamento, o carro da empregada no "Snap Into" e a
  contagem de atributos que o save traz menor — ver `soak-report.md`) **e o
  interpretador de Lua** (a raiz do pool de NPCs do AL, `data-findings.md` §21 e
  `lua-scripts.md`: sem ele nenhum NPC é "do tipo" e os carros de serviço não têm
  motorista); o 6 fica parcial e o
  6-resto com o mantenedor do Mac. Roda em **loop autônomo**, sem pedir licença entre
  passos, até a fila zerar ou só restarem itens bloqueados com critério escrito.
- **Mandato:** Artur — "Faça um goal para resolver próximos passos e pendências"
  (18/10/2026).
- **Branch:** `windows-x64-fidelity` — os 21 commits do nosso trabalho rebased sobre o
  `origin/main` novo (**v0.5.0**, `244cc68`); o antigo `windows-x64-ghidra-pc` ficou em
  `backup/ghidra-pc-v040`. Um item = um commit, linha
  única: código em inglês, docs em pt-BR **sem acentos** na mensagem; área como prefixo.
- **Regras herdadas (valem para todo passo):**
  - Toda mudança aponta a âncora/medida que a justifica; o que não estiver medido fica
    marcado **"inferido"** no código/doc.
  - `tools\build_windows.cmd` (configure + build + ctest) **39/39 verdes a cada item**.
  - Nunca tocar nos untracked pré-existentes: `app_00.png`, `frame*.png`, `frame_vm.txt`,
    `save-test/`, `vc140.pdb`.
  - Reverter **toda** instrumentação TEMP antes de commitar (hoje: o dump de TTABs em
    `cli/s2pkg/main.cpp`, env `S2PKG_TTAB_RAW` — uncommitted).

## Objetivo

Tirar a fila do port do ponto morto, começando pelo maior ganho funcional já medido — os
**1.177 TTABs** do `objects.package` do Apartment Life que hoje são recusados e deixam os
objetos sem interações — e fechar (ou bloquear com critério) o resto: pacote vazio do soak,
vizinhanças/lotes do AL, performance, painel do Build, `RayTrace` e as primitivas
`0x7D/0x7E`.

## Ponto de partida medido (18/10/2026)

### Item 13 — TTAB

- Censo no `objects.package` do AL (decode-all): **1.177** TTABs recusados —
  **0x4F 149, 0x50 106, 0x51 21, 0x53 7, 0x54 440, 0x55 454** (no log do decode-all são
  1.176 linhas; o dump TEMP gravou 1.177 arquivos). O gate do port hoje é `0x3F..0x4E`
  (`InteractionTable::parse`, `src/formats/simantics.cpp`).
- **0x4F / 0x50 / 0x51 (276 arquivos): layout idêntico ao 0x4E** — parse byte-exato em
  **276/276**: contagens, entradas e nome final fecham sem sobra. Pronto para portar.
- **0x53 / 0x54 / 0x55 (901 arquivos): formato novo, estrutura medida:**
  - Cabeçalho igual ao antigo (nome 64 B, typeCode, versão, `n` u16 em +76).
  - Entrada = **54 bytes fixos** + cauda. Campos fixos medidos por assinatura estrita
    (`join = -1` em +24, `ui = 0` em +28, `mem = 0.5f` em +34, `obj = 0` em +38,
    `model = 1` em +42) em milhares de entradas:
    action u16 +0 · guard u16 +2 · flags u16 +4 · flags2 u16 +6 · stringIndex u32 +8 ·
    attCode u32 +12 · attValue f32 +16 · autonomy u32 +20 · joinIndex i32 +24 ·
    uiDisplayType u16 +28 · facialAnimation u32 +30 · memoryMultiplier f32 +34 ·
    objectType u32 +38 · modelTableId u32 +42 · **X** u16 +46 (e 0 em +48) · **Y** u16 +50
    (e 0 em +52).
  - Cauda = **X × (4 + 6·Y)** bytes: X grupos, cada um com **header de 4 bytes + Y anúncios
    de 6 bytes** (o trio `min, delta, personality` do formato antigo). Evidência (tamanho
    da entrada − 54): (5,10)→320, (5,14)→440, (5,7)→230, (5,16)→500, (5,6)→200,
    (5,1)→50, (1,16)→100, (1,8)→52 — os casos com **X≠5** derrubam o modelo anterior
    `4X + 30Y` (que só coincide quando X=5); no E2F o trio `(50,10,0)` aparece a cada
    **64 = 4 + 6×10** bytes, no fim de cada grupo.
  - **Status do parse em nível de arquivo:** fecha exatamente (n entradas + nome final) em
    **255/440** dos 0x54 e **300/454** dos 0x55 — **555 arquivos**; em nível de entrada, o
    modelo `4X+30Y` já fechava **4.217/4.934** e o refinamento `X×(4+6Y)` corrige os casos
    X=1 (100/94/88/52).
  - **Anomalias abertas:** (a) entradas lidas como **(X=0, Y=8)** com cauda de **320 bytes**
    (o modelo daria 0) — 79 ocorrências de um mesmo tamanho, hipótese principal: entrada
    anterior não detectada pela assinatura estrita ou contadores lidos fora de lugar;
    (b) os **7 arquivos 0x53** não fecham (um "untitled tree table" tem quatro u32 = 16 na
    região fixa e não bate na assinatura); (c) semântica do header de 4 bytes do grupo
    (índice? contagem?) e mapeamento para os `motives` do port ainda **inferidos**.
  - Ferramentas: dumps em `%TEMP%\opencode\i6e\ttab\` (**1.177 `.bin`**) e analisadores
    `ttab_*.py` na mesma pasta; o `ttab_model2.py` (validação do modelo) saiu **sem
    saída** — primeiro passo é rodá-lo com log em arquivo e traceback.
- **Fio do Ghidra (PC):** o leitor de TTAB no `Sims2EP9RPC.exe` ainda não foi localizado
  (`ScanConsts` poluído por strings/GUIDs; `FUN_0082fa60` não inspecionada a fundo). Próxima
  tentativa: filtrar escalares por **bitLength == 32** e varrer o type id **0x54544142** +
  as versões `0x4F..0x55`; se o modelo de dados fechar antes, o Ghidra vira só confirmação.

### Outras pendências

- **5 (Buy §1.4, "6 de 53"):** **FECHADO 21/10** — o census provou que os mods desta máquina
  (TS2 Extender, Lighting Mod UC, `userStartup.cheat`, Starship UI — o `README_MODS.txt` do
  backup `_MOD_BACKUP_20261016`) **não** acrescentam objetos de catálogo: o
  `ObjectScripts.package` do Extender tem 0 OBJD, os `Overrides` do mod de luz só repetem
  GUIDs já contados e o tema de UI não tem objetos. Os 6 a mais da captura eram conteúdo do
  `Downloads` do usuário, que esta máquina não tem mais (o backup guardou só os arquivos
  substituídos). O port conta a instalação: 47 cadeiras, 1666 compráveis.
- **6 (resto):** pessoa 164, ActionSkipped "evento 4", testes `0x214`/`0x2e8`, NGBH u32
  `+0x34`/`+0x38`, registros `+0x60c` e a troca de fila pela vtable `0x1FC` — todos pedem RE
  do binário **Mac PPC**, que fica **com o mantenedor do Mac** (decidido pelo Artur em
  21/10: "mac é com o mantenedor"). Não bloqueia mais nada aqui.
- **8:** **FECHADO** (rodada 19 do soak, `fdf29d3`): o `N002_Lot32` (stub DBPF de 96 bytes,
  sem recursos) deixou de ser erro de carga — o `Lot::load` lança `EmptyLotPackage`
  (`formats/lot.hpp`) e o `tools/soak.sh` conta `empty|autonomy` / `empty|lot-save` à
  parte; medido em `docs/soak-report.md` (rodada 19) e coberto pelo `testEmptyLotPackage`.
- **7:** **índice por tile FECHADO em 22/10** (`ff0fabb`: `N001_Lot11 --ticks 3600` de
  6,71 s para **4,25–4,68 s**, saída idêntica; `OS2_CHECK_TILE_INDEX=1` sem divergências no
  ctest e no `autonomy`) e o **`std::function` do Set to Next medido e descartado** (21/10:
  22,2/22,5 s contra 22,3/21,3 s, sem ganho). **`notifyRoomChanges` FECHADO em 23/10**: o
  `VM` guarda só os objetos com a sala escrita desde o último tick (`noteRoomChanged`, os
  12 escritores + `Context::set` + `createObject`/`restoreState`), com o
  `OS2_CHECK_ROOM_DIRTY=1` sem divergências no ctest (39/39) e no `N001_Lot11 --ticks
  29600` com a instalação, saída idêntica (era 0,36 s de uma corrida de 43.200 ticks).
  Fica só o `tickOverlayEvents` (0,58 s no lote pequeno, ~0,4 s com a instalação, 0,5%: o
  índice exigiria marcar os seis sítios de add/erase e a passagem mexe nas poses) e a
  carga (texturas dos Sims 45 ms cada, `Catalog::load` relendo 47 recursos).
- **10:** **FECHADO 21/10** pelo método do item 4 (as capturas não estão nesta máquina): a
  auditoria do painel contra a tabela do `buy-build-mac.md` §7.8 e contra o censo novo do
  catálogo de construção achou três lacunas, todas fechadas (a jardinagem A800 inteira, as
  colunas BA20 e os pincéis B751–B753).
- **Lacunas da VM medidas no AL (21/10, pelo censo do `bhav-sweep`)** — as falhas do port
  que o jogo não tem, para atacar depois do 0x7E: o **Run Tree by Name modo 3** (648 usos no
  AL — o modo 2 empurra a árvore na pilha e o doc diz que o 3 também empurra, mas o port só
  faz 0/1/2), os **tipos de busca do Set to Next sem semântica no doc** (13, 19 e 20 —
  **medido em 22/10**: o AL usa **39 instruções** dos tipos que faltavam de fato (o 38:
  36 e o 39: 3); o 31 (cônjuge do temp 0) e o 34 (o grupo de multi-tile vizinho) já
  estavam implementados, então a leitura anterior de 52 estava errada), e o
  **Want Satisfy** (295) não é lacuna: o port joga o **mesmo** erro do jogo (0x1C, "Object
  referred to must be a person.") — é artefato do sweep. O "stack object does not exist"
  (1.345) é erro do próprio jogo (o sweep roda cada árvore sozinha, fora de contexto). O
  **"Invalid global sim call" (3.972) NÃO era artefato**: era o port recusando as chamadas
  54–84 que o AL usa (abaixo), e num lote de verdade isso reiniciava o Main de todo Sim. **Fechados no
  mesmo dia**: os tipos de espécie (40-45, as pessoas, e 46-51, os vizinhos, pelo dado de
  pessoa 186), o tipo 33 (Hauntable) e o **índice do texto do Add/Change the Action String**
  (o u8 em 4 na versão de nó 0 e o u16 em 14 na versão de nó 1 - o port lia sempre o byte 4,
  e no BV as árvores que montam uma action string não achavam a string), e o **modo 3 do Run
  Tree by Name** (o RE do exe do PC: o GUID nos temps 0/1 → o objeto → a árvore dele, rodada
  na hora como o modo 0; o erro 0x2C para um modo inválido e o 0x7A para um GUID que
  nenhum objeto tem). O sweep do AL passa de 12.894.204 instruções, 18.622 true e 14.129
  falhas para **13.124.187, 19.070 true e 13.228 falhas**; as buscas de tipo inválido caem de
  **888 para 70**, as action strings sem índice, de **364 para 47**, e o modo 3 tira **257
  falhas**. **Fechados em 22/10**: os tipos **38** e **39** — o 38 percorre a lista de
  definições do app (o elemento é a definição OBJD; os campos 75/76, offsets 0xD6/0xD8, são
  a categoria/subcategoria do seletor do AL: 117 rádios, 121 badges/tokens, 153 acessórios;
  o GUID vai para os temps 2/3 e o índice+1 no alvo) e o 39 é o próximo vizinho com
  relacionamento vitalício ≥ 0 com o vizinho do temp 0 (pulando o próprio e os sem objeto
  no lote; erro 0x77 sem vizinho e 0x68 sem tabela). Medição: o sweep do AL vai a
  **13.170.043 instruções, 19.087 true e 13.200 falhas** (as 70 buscas de tipo inválido
  somem; +38 "no stack object" são árvores que agora acham um GUID e rodam numa árvore sem
  o objeto no lote sintético do sweep) e o jogo base fica igual (7.234.312 / 11.107 /
  2.461). **Fechados em 22/10**: os tipos **19** e **20** — o próximo/anterior do **grupo
  multi-tile** do objeto no temp 0: a âncora é `GetObjectByID(temp 0)` (nula: alvo 0 e
  falso; o 19 também recusa a âncora morrendo), a chave é o grupo (`+0xe4`, negativa para
  o objeto fora de grupo) e a lista é a do módulo de objetos (`+0x14c`), andada na ordem
  da lista, pulando os objetos que morreram; o 20 tem o off-by-one do jogo (começa em
  `i-2`, pulando o anterior). Os únicos usuários no base e no AL são as árvores
  `0x7FFD4F2D:2006`/`:2007` do grupo da **lareira**, e o handler que apaga uma escada
  modular (`FUN_009234f0`) usa a mesma lista para achar as outras peças — só uma lista por
  **grupo** explica isso (por nível ou por sala levaria as outras escadas junto); o RE
  está no `re-pc-ghidra.md` §(e) e no `primitives-mac.md` §0x1F. Medição: o sweep do AL
  fica **idêntico** (**14.025.434 / 19.927 true / 10.335 falhas**, 46.880 árvores; era o número
  daquele dia) porque os dois tipos não executam no sweep. O **13** (a pessoa mais próxima, 1 uso, "Reaction -
  Banter") **fechou em 22/10**: a âncora é pessoa (erros 0x17/0x1C), as candidatas são
  as pessoas da mais perto para a mais longe com o filtro comum, o alvo é o cursor, e a
  distância no **temp 4** e a direção 0–7 no **temp 5** (`re-pc-ghidra.md` §(e) e
  `primitives-mac.md` §0x1F). Com isso o Set to Next está completo nos tipos 0–51.
- **Achado novo em 22/10, fora do sweep (num lote de verdade)**: as chamadas **54–84**
  da Generic Sims Call (0x01). O AL usa até **0x54** (~232 instruções acima de 53) e o
  port recusava tudo acima de 53; o `Function - Main - Set Skin` do Sim chama a **60**,
  e com o erro 0x2A o Main de todo Sim reiniciava (e cada reinício reabria o look_at, que
  estourava o limite por turno): no `N001_Lot11` com a instalação inteira, **5.914
  resets** do Mortimer e os outros dois travados no "Motive Failure - Pass Out - Loop".
  Agora 54–84 são **registradas em `VM::engineRequests` e devolvem verdadeiro** (o mesmo
  tratamento das chamadas de motor do base; o limite do 0x2A sobe para 0x54). Medido no
  mesmo lote: **15 resets** do Mortimer (9/62 dos outros, por outras falhas do AL), Main
  em "Routing - Goto Head of Bed" e buscas em **17/94/25** (antes 58/109/38, travados).
  O `primitives-mac.md` §Generic Sims Call tem o censo e o A/B (devolver falso é muito
  pior). No `bhav-sweep` do AL as **3.982** falhas "Invalid global sim call" somem e as
  contagens vão de 14.025.434 / 19.927 true / 10.335 falhas para **14.093.824 / 23.864
  true / 6.385 falhas** (as árvores que paravam na chamada agora seguem e terminam true).
- **Ferramenta de teste (22/10)**: o `autonomy` indexava só a pasta do EP dado, não a
  instalação (o app indexa a instalação inteira). Com o AL o lote rodava **sem as
  animações do base e dos outros EPs**: o laço do "Pass Out" girava até o orçamento de
  instruções, o Main dos Sims reiniciava **116 vezes** em 29.600 ticks e **o ônibus
  escolar não ia embora** (27 peças às 09:27); agora indexa os mesmos roots do app:
  **91 resets**, 1 animação faltando, os Sims em "Routing - Goto Head of Bed"
  (**17/94/25 buscas** contra 185/253/229) e **o ônibus vai embora**. Detalhe em
  `performance.md`.
- **Filtro comum do Set to Next** (`data 56 & 5` e `107`, com as flags 0x01/0x04): entrou
  no `eligible` em 22/10, gateado pelo layout de BV (o sweep do AL fica igual: no lote
  sintético os dois dados estão zerados; o jogo base não passa pelo filtro por
  construção).
- **O "attribute 8 of 8" do controlador do assistente social (22/10, não é bug do
  port)**: o OBJD `0x7F029114/0x41A7` diz `numAttributes = 10` e o objeto **nasce com
  10**, mas o **save do lote** traz 8 e o `cTSObject::Read` adota a contagem salva (o port
  faz `assign`); a árvore do AL "Function - Update" #88 lê o atributo 8 e erra 0x06 como o
  jogo erraria com esse save. É estado do lote, não do motor — não mexer.
- **12:** primitivas `0x7D`/`0x7E` — **fechado em 22/10**: o RE do PC está no
  `re-pc-ghidra.md` §(g); sem interpretador de Lua o port devolve **false** (ver o item 9).
- **14:** **FECHADO** (`data-findings.md` §20): `NGBH 0xCB`, `FAMI 0x55`, `SDSC 0x36`,
  `NID` 0x0A (+20 bytes) e o *lot info* 11 de apartamento entraram em
  `neighborhood.cpp`/`.hpp` e nos writers; `hood-sweep` do E001 com **234 Sims, 21
  famílias, 56 lotes, 1.051 SREL, 0 erros** (antes 0/0/0/313) e o `test_neighborhood`
  cobrindo a ida e volta.
- **RayTrace (0x78):** **FECHADO em 20/10** — o 0x78 resolve o ponto pelo slot do mesmo
  modo que o Look At (censo de 35 leituras tipadas no AL), com o `testRayTraceSlots` (o
  slot, o caso sem slot e o 0x1B) e o sweep do AL medido; o detalhe está na entrada 8 da
  fila abaixo.

## Fila (ordem de ataque)

Cada item só fecha com **build + ctest 39/39**, evidência medida no log e **1 commit**.

1. **13a — gate 0x4F..0x51** (o ganho barato): ✅ **FECHADO 18/10** — o gate
   aceita `0x3F..0x51` com o layout 0x4E; `testInteractionTableVersions` cobre
   0x4E..0x51 (campos, anúncios, nome) e recusa 0x52/0x53/0x55; TEMP revertido.
   Medição: decode-all do AL com **901** falhas (só 0x53 7 / 0x54 440 / 0x55 454)
   e **1.208** ok; build+ctest 39/39; `data-findings.md` §17.
2. **13b — fechar o modelo 0x53+**: **FECHADO 19/10** (o 0x53 fechou em 20/10, ver a §19) — o RE
   do leitor de TTAB no `Sims2EP9RPC.exe` (`FUN_0087c720` @ `0x87c720`, no projeto Ghidra
   `C:\Users\Artur\sims2-re\pcb`) deu o layout: 0x44–0x52 = 7 tabelas, **0x53 = 11
   tabelas**, **≥ 0x54 = 46 bytes fixos + dois arrays de grupos de anúncios** (o primeiro,
   `[u32 grupos]` e por grupo `[u32 m]` + `m` anúncios de 6 bytes; o segundo, por grupo,
   `[u32 X]` + `X−1` itens de 4 bytes de conteúdo desconhecido + `[u32 m]` + `m` anúncios).
   Com esse layout o leitor fecha **878 dos 894 arquivos 0x54+** (o modelo anterior fechava
   555 dos 901). Restam 12 com o `X` do segundo array ambíguo (lido como u32 dá
   `0x00050000`; os bytes sugerem um par menor que 4 bytes) — os 7 arquivos 0x53 fecharam
   em 20/10 (item 1 da fila de 20/10).
3. **13c — leitor novo no port:** **FECHADO 19/10** para 0x54/0x55 e **20/10** para 0x53 —
   `InteractionTable::parse` ganhou o caminho novo (46 bytes fixos + os dois arrays, cada
   grupo virando uma tabela de motivos; no 0x53, o layout do 0x4E com **11** tabelas). O
   teste `testInteractionTableVersions` cobre 0x4E..0x51, 0x53 (11 tabelas) e 0x54/0x55, e
   recusa 0x52 e acima de 0x55. Medido no `objects.package` do Apartment Life: **2.097 ok
   / 12 falhas** (antes 1.208 ok / 901 falhas; 2.090/19 antes do 0x53) — as 12 são o `X`
   ambíguo (9 + 2) e 1 "693 trailing bytes" (`0x7FB36E92`, grupo `0x80`). `data-findings.md`
   §18 e §19 registram o RE, os layouts e as medições.
4. **8 - N002_Lot32 / pacote vazio:** ✅ **FECHADO 20/10** — `Lot::load` distingue o pacote
   vazio (o stub de 96 bytes sem recursos, `EmptyLotPackage`) do erro "no elevation array", o
   `s2pkg` responde "empty lot package" (sem "error:") e o `soak.sh` conta os lotes vazios à
   parte (`empty|autonomy|...`, `empty|lot-save|...`); rodada do lote afetado medida em
   `docs/soak-report.md` (round 19); teste `testEmptyLotPackage` (o stub de 96 B escrito pelo
   `DbpfWriter`); build+ctest 39/39.
5. **14 — vizinhanças/lotes do AL:** ✅ **FECHADO 20/10** — NGBH 0x6F/0x70/0xBE/0xC2/0xCB,
   FAMI 0x4E..0x55 (gates 0x4F/0x50/0x51/0x55), SDSC 0x20..0x36 (com o 0x35 do template
   E002) e a cauda BV, NID 3..0x0A e lot info 18/base 11 (cabeçalho de apartamento, cauda
   em `rest`); teste `test_neighborhood` por formato (ida e volta), `hood-sweep` do E001 em
   **234 Sims / 21 famílias / 56 lotes / 0 erros** (antes 0/0/0/313) e `lot-run` do
   `E001_Lot127` por 60 ticks; as versões foram medidas nos 20 pacotes do UC
   (`data-findings.md` §20).
6. **7 — performance:** um sub-item por vez (índice por tile, notificações por tick, Set to
   Next, texturas), medidor antes/depois no log; 1 commit por sub-item. **Primeiro sub-item
   FEITO 21/10** — o `VM::tick` não reconsulta mais o mapa das threads: ele guarda os
   **iteradores** (os nós de um `std::map` são estáveis) num vetor que é membro da VM, e o
   `deleteGroup` passa a **matar** a thread do objeto removido e a anotá-la (ID + serial)
   em `VM::deadThreads` para o tick a apagar no fim, em vez de apagar durante (o
   comportamento é o de antes: as threads criadas no tick só recebem a vez no tick
   seguinte, e a thread de um objeto removido não roda). Medido no Windows (`autonomy
   N001_Lot11 --ticks 43200`, `S2PKG_NO_LIGHT=1`): **22,3 s / 21,3 s → 19,5 s / 18,2 s**
   com a saída **idêntica** (as 516 linhas); build + ctest 39/39. A mudança também
   conserta um **comportamento indefinido**: o `deleteGroup` apagava a thread do objeto
   removido *enquanto ela rodava* (o `Thread&` do `VM::run` ficava pendurado); agora a
   thread é **morta** (o status `Killed`, a regra do jogo) e apagada no fim do tick. Por
   isso o `bhav-sweep` do AL muda em **3 das 46.880 árvores** (18.625 → 18.622 true,
   14.126 → 14.129 falhas, e 4 "Slot number out of range" a mais: as árvores cuja thread
   foi morta no lugar de continuar com a memória liberada). **Segundo sub-item FEITO
   22/10 — o índice por tile.** `objectsOnTile` varria os ~1.300 objetos a cada chamada
   (35.799 chamadas em 3.600 ticks do `N001_Lot11` com o AL: 47,1 M verificações). O
   `VM` agora mantém `tiles_` (`TilePos -> IDs`), construído na primeira consulta e
   mantido pelo `VM::setPosition` — todas as escritas de `Object::position` do VM e do
   app passam por ele — e pelo `removeObject`; a consulta devolve os IDs em ordem de ID,
   e `OS2_CHECK_TILE_INDEX=1` compara cada uma com a varredura completa (ctest 39/39 e
   `autonomy`: zero divergências). Medido no `autonomy N001_Lot11 --ticks 3600` com o
   AL ativo: **6,71 s → 4,25-4,68 s** (~35% menos), saída idêntica; o `bhav-sweep` do
   AL fica igual (14.025.434 / 19.927 / 10.335). O `objectsAround` fica na varredura:
   com o índice nele o raio de 20+ tiles das buscas custava 3x mais (19,4 s). Ficam:
   `notifyRoomChanges`/`tickOverlayEvents` por tick e as texturas. O sub-item do
   "Set to Next sem `std::function`" foi **medido sem ganho** (22,2/22,5 s contra
   22,3/21,3 s) e descartado — o registro está em `performance.md`.
7. **10 — painel do Build:** **FEITO 21/10 pelo método do item 4** (as capturas 30–34 não
   existem nesta máquina): o painel foi auditado contra a tabela do `buy-build-mac.md` §7.8 e
   contra o censo novo do catálogo de construção (`s2pkg catalog --installation --build`), e
   a auditoria achou **três lacunas**, corrigidas: a categoria **A800 jardinagem** inteira
   (não havia `case 8` — as árvores, os arbustos e as flores, o build type 4 com os bits
   0x01/0x02/0x04), as **colunas BA20** (o subsort 2 do painel de diversos, o build type 1
   com o bit 0x200) e os **pincéis B751–B753** (o subsort 5 do painel de terreno, as
   ferramentas de jardim, o build type 4 com o bit 0x10). O `B620` (a ferramenta de
   corrimão) já estava no clique do subsort, e o resto da tabela bate com o port.
8. **RayTrace 0x78:** ✅ **FECHADO 20/10** — o 0x78 resolve o ponto pelo slot do mesmo
   jeito que o Look At (byte 4 = 1/2/3 → os SLOT de tipo 4/3/0, byte 5 = o índice dentro
   deles, pasto a contagem erro 0x1B; o alvo nos bytes 11/12) e, sem kind, pelo tile do
   objeto; antes o port usava o tile dos dois, o que não batia com o jogo. Teste novo
   `testRayTraceSlots` (o slot, o caso sem slot e o 0x1B) e sweep do AL medido (4 erros
   0x1B a mais, as árvores globais que pedem um slot que o objeto não tem — o que o jogo
   também dá); build+ctest 39/39.
9. **12 - 0x7D/0x7E: FECHADO 22/10 — o RE do PC está completo; a resposta é false desde 22/10**
   — no exe do PC (o build do AL) as duas primitivas estão na **classe base** (mesma tabela de
   despacho do RayTrace 0x78 e do Change Outfit 0x79), e não só no routing aggregate como no
   Mac: o gate de tipo de objeto é "qualquer objeto". Medidos: os handlers (`FUN_008b9bd0` Follow
   Sim e `FUN_008b9e00` Run Script), o **layout dos operandos** (o bloco de 16 B deslocado em
   -4: o id do STR# em o[0..1], o id do script em o[2..3], o byte de flags em o[4], os três
   pares (tipo u8, valor u16) em o[6]/o[7..8], o[9]/o[10..11] e o[12]/o[13..14]) e o **byte de
   flags** (o bit 0 pede também o `.objLua`, o bit 1 é o escopo **privado** (`vfunc 0x108` do
   objeto), o bit 2 o **semi-global** (`vfunc 0x114`) e o bit 3 só o log dos parâmetros; sem
   eles o escopo é o grupo global `0x7FD46CD0` — a leitura de 21/10, "só o bit 0 vale e o
   ramo Private é código morto", estava errada: o `MOV AX, word ptr [ESI+8]` recarrega o byte
   sem a máscara do `AND AL,1`). **O pedido é o mesmo do `Run Tree by Name`**: o id
   empacotado `(STR# << 16) | id` e a busca
   `(**(code **)(*gerenciador + 0xC))(&pedido[5], 0x89B7DC8, &achado, byte[0x25], 0)` — o `0x1C`
   da classe base (`FUN_008d3510`) pede do mesmo jeito, com o STR# 0x12F fixo. **A peça que
   faltava (22/10)**: o STR# do id (304 no AL) é a tabela **"Lua Scripts"** (7.233 entradas; o
   0x12F é "Call Named Tree prim string set") e o id do script é 1-based nela — o AL
   bate (a global 0x0137 "NPC - Get Outfit GUID for Type in T[0,1]" é uma única
   instrução, o `0x7E` com o id 49, e a entrada 49 da tabela é o mesmo nome). **Logo o
   `0x7E` roda um script Lua** (do `.objLua` do objeto); a árvore de descrição com o
   mesmo nome é o próprio chamador (a 0x0137 chama o próprio nome), então trocá-la
   pelo script seria recursão. **O port não tem interpretador de Lua, mas a recusa
   (erro 0x05) derrubava os lotes do AL**: os controladores do AL (LTA Superpowers,
   Climate, Business - Customer Loyalty, Fish Pond Effects, Community Lot -
   Populator...) e a Main dos Sims resetavam a cada poucos ticks — no `N001_Lot11`
   com os dados do AL os 4 Sims ficaram com **0 buscas e 0 escolhas em 3.600 ticks**,
   e **3.208 das 13.200 falhas** do sweep do AL eram esse Run Script. Desde 22/10 o
   `0x7E` (e o `0x7D`) devolve **false** — o mesmo que o handler do jogo quando o
   `run` do script/tree não acha o que rodar — e o lote volta a rodar (buscas,
   escolhas, motivos andando), com os recursos de Lua inertes. Sweep do AL:
   13.170.043 / 19.087 / 13.200 → **14.025.434 / 19.927 / 10.335**. O `0x7D` tem
   **1 uso** no AL (a 0x1001 "Function - Main" do grupo 0x7F649DA1) e o sentido do
   array de objetos continua sem medida; false também.
   `docs/re-pc-ghidra.md` §(g) e `docs/data-findings.md` §21 têm o registro completo.
10. **Set to Next 38/39: FECHADO 22/10** — o 38 é a lista de definições do app filtrada
    pelos campos 75/76 (a categoria/subcategoria do seletor do AL; o GUID vai para os temps
    2/3 e o índice+1 no alvo) e o 39 é o próximo vizinho com relacionamento vitalício ≥ 0
    com o vizinho do temp 0 (erro 0x77 sem vizinho, 0x68 sem tabela). As 70 buscas de tipo
    inválido do sweep do AL somem: 13.124.187 / 19.070 / 13.228 → **13.170.043 / 19.087 /
    13.200**, o jogo base igual; build+ctest 39/39; `data-findings.md` §22 e
    `primitives-mac.md` §0x1F registram o assembly, o censo dos 10.952 OBJDs e a
    conferência da árvore `0x7F067AA0:2003` (117/4 → GUID `0x1071F92F`). O **filtro comum**
    (`data 56 & 5` e `107`, flags 0x01/0x04) entrou no `eligible` no mesmo dia, gateado pelo
    layout de BV; o sweep do AL fica igual (no lote sintético os dados estão zerados).
11. **6-resto:** só com o binário **Mac PPC**, que fica com o mantenedor do Mac (decidido em
    21/10); o item 5 fechou por censo. Ficam por último, sem travar a fila.

## Critérios de sucesso

- [ ] Item 13 completo: 1.177 TTABs do AL lidos (0x4F..0x55) e interações visíveis no app.
- [ ] Build + ctest 39/39 ao fim de **cada** item (evidência no log).
- [x] Item 8 fechado com o soak-report citando o caso; itens 7 e 10 com medição antes/depois.
- [x] Item 14: `E001_Lot127` do AL carrega (ou o residual está documentado com bytes).
- [ ] RayTrace e 12 fechados com sweep/teste; 5 e 6-resto explicitamente bloqueados.
  (RayTrace **fechado em 20/10**; o 12 ainda espera o RE do exe do PC.)
- [ ] Nenhum TEMP no diff final; nenhum untracked pré-existente tocado.

## Riscos

- O modelo 0x53+ pode ter mais de uma variante (0x53 diferente de 0x54/0x55) → aceitar só o
  que fechar e manter o resto recusado com erro claro (nada de parse "otimista").
- O Ghidra pode continuar sem achar o leitor; o modelo de dados é a fonte primária — o exe
  entra como confirmação, não como bloqueio.
- Itens de Mac PPC e a pergunta do item 5 não têm prazo; ficam registrados como bloqueados.
- A máquina é compartilhada com o Artur: Ghidra pesado só solo.

## Log

- (18/10) Goal aberto a partir do estado do item 13 (1.177 TTABs; modelo 0x53+ medido até
  `X×(4+6Y)`, 555 arquivos fechando) e da fila restante do `goal-port-fidelity.md`. Primeiro
  passo: reverter o TEMP e fechar 13a.
- (18/10) **13a FECHADO** — `InteractionTable::parse` aceita 0x4F..0x51 com o layout 0x4E
  (276 recursos medidos); `testInteractionTableVersions` (0x4E..0x51 ok; 0x52/0x53/0x55
  recusados); TEMP revertido; decode-all do AL: **901 falhas** (0x53 7 / 0x54 440 / 0x55
  454) e 1.208 ok; build+ctest **39/39**; `data-findings.md` §17 registra a medição.
  Próximo: **13b** (fechar o modelo 0x53+ — anomalias (0,8) e os 7 arquivos 0x53).
- (18/10) **Reconciliação com o upstream v0.5.0** — o `main` do `opensims2` andou 48 commits
  desde `fc6694f` (v0.4.0) e já traz o **nosso** port Windows (o `52af245`, PR #1 aplicado
  pelo mantenedor, que fechou o PR sem merge). Os 21 commits nossos que ficaram de fora
  (catálogo da instalação, Buy mode medido, câmera, renderer, BHAV `0x8009`, TTAB
  `0x4F..0x51`, Look At, conjuntos de variáveis, reset do od 7 e os docs dos goals) foram
  rebased sobre `244cc68` no branch `windows-x64-fidelity`; um único conflito, na linha dos
  Erros do `inventario-binario.md`, resolvido com o texto novo do upstream + a correção do
  reset do object data 7. Diff final contra o upstream: **33 arquivos, +1.893/−135**.
  O que o upstream fechou **para nós** (núcleo compartilhado, o port herda): a rodada n46
  ("VM, resto") levou os registros da pessoa (`cTSPerson +0x60c`), o 109/164, o
  `UpdatePersonDataFields` (37/38/40/55), o `cTSGlobals`/`cTSDataTables` e as sobras da fila
  (evento 4 do `ActionSkipped`, `0x214`/`0x2e8`, a troca da fila cheia `0x1FC`) — ou seja,
  o **item 6 inteiro** da fila do `goal-port-fidelity.md`; e a n47 ("Salvar, resto") mediu o
  `NGBH` (`+0x34`/`+0x38` = tiles, `+0xFD` um bool sem uso, o dono `0x22` só de leitura 1–15)
  e a transação do persist system. O que **continua nosso** (o upstream não tocou):
  item 5 (bloqueado), 7, 8, 10, 12 (`0x7D`/`0x7E`), 13b, 14 e RayTrace — o item 14 segue
  exclusivo nosso: o upstream ainda só lê NGBH `0x6F`, SDSC `0x20` e FAMI `0x4E`, e o AL
  usa `0xCB`/`0x36`/`0x55`. A fila do upstream agora é 48 (colocação), 49 (rotas),
  50 (água/objetos/cinema), 51 (CAS) e 52 (bairro) —reas de 48/49 tocam colocação e rotas,
  onde o nosso item 7 (performance) mede.
- (19/10) **13b/13c FECHADOS para 0x54/0x55** — RE do leitor de TTAB no `Sims2EP9RPC.exe`
  (Ghidra, projeto `C:\Users\Artur\sims2-re\pcb`; o type id `0x54544142` aparece 15 vezes no
  binário e o leitor é `FUN_0087c720` @ `0x87c720`, na vtable da classe em `0x1222cfc`):
  a entrada 0x54+ tem **46 bytes fixos** (as flags do 0x4E, sem as contagens de motivo) e
  depois **dois arrays** de grupos de anúncios — o primeiro `[u32 grupos]` e, por grupo,
  `[u32 m]` + `m` anúncios de 6 bytes; o segundo, por grupo, `[u32 X]` + `X−1` itens de
  4 bytes (conteúdo desconhecido) + `[u32 m]` + `m` anúncios. Os dois arrays usam
  funções diferentes no binário (`FUN_00879690` e `FUN_00879800`), o que explica os
  formatos diferentes dos itens. O RE também disse que **0x53 tem 11 tabelas** (e não 7).
  Medido: o leitor fecha **878 dos 894 arquivos 0x54+**; o `decode-all` do
  `objects.package` do Apartment Life passa de **1.208 ok / 901 falhas** para
  **2.090 ok / 19 falhas** nas TTABs (as 19: 7 de 0x53, que segue recusado, e 12 com o
  `X` do segundo array ambíguo). `simantics.cpp` tem o caminho novo, o teste
  `testInteractionTableVersions` cobre 0x54/0x55, build+ctest **39/39**, e a
  `data-findings.md` §18 registra o RE, o layout e os bytes do resíduo.
- (19/10) **Rebase sobre o upstream v0.6.0** (`ff4f71a`) — o mantenedor andou **53 commits**
  desde o v0.5.0 e **implementou o port Windows em paralelo**: o renderer com GUOB/bounds/
  planos de luz (`34db5be`), o refactor da câmera (`setKeyMove`/`setKeyZoom`/`setKeyRotate`),
  o host que constrói no macOS também (`ce98217`, com `OS2_WINDOWS_CHECK`) e as releases
  `opensims2-windows-x64.zip` (o `main` agora anuncia macOS **e** Windows). Por isso os
  **dois commits nossos** de câmera e renderer foram **omitidos** no rebase (o upstream já os
  tem, com a mesma medição) e o branch ficou com 21 commits. O que continua só nosso: o
  catálogo da instalação (448 → 1.666), o Buy mode medido, o BHAV `0x8009`, o Look At com
  slots, os conjuntos de variáveis, o reset do od 7 e **as TTABs 0x4F..0x55** (o upstream
  ainda recusa tudo acima de 0x51). Build+ctest **39/39** local e no CI do Windows; o
  `decode-all` do AL mantém **2.090 ok / 19 falhas** nas TTABs. PR #2 (`windows-x64-fidelity`
  → `main`) mergeável, com o CI verde.
- (20/10) **0x53 FECHADO** (item 1 da fila de 20/10) — o 0x53 é o **layout do 0x4E com 11
  tabelas de motivos**: `action` u16, `guard` u16, **11 contagens u32** e depois os mesmos
  campos do 0x4E, com as 11 tabelas de anúncios (6 B cada) no fim. Os 7 recursos do AL
  (todos "untitled tree table", conteúdo idêntico, 2.387 B) fecham exatamente no nome
  final: entradas de 570 B (contagens `[16,0,0,0,0,0,0,16,16,16,16]` → 80 anúncios) e de
  1.146 B (11×16 = 176 anúncios na terceira); os anúncios são zero neste recurso, por isso
  a cauda parecia padding. `InteractionTable::parse` passou a ler o 0x53 no caminho do
  0x4E (11 contagens) e a recusar 0x52 e > 0x55 (nada acima de 0x55 aparece no conteúdo);
  `testInteractionTableVersions` cobre 0x4E..0x51 e 0x53 e recusa 0x52/0x56; build+ctest
  **39/39**; `decode-all` do AL: **2.097 ok / 12 falhas** (antes 2.090/19). As 12 restantes
  são o `X` do segundo array do 0x54/0x55 (9 "bad TTAB item count" + 2 "bad advertisement
  count") e 1 "693 trailing bytes" (`0x7FB36E92`, grupo `0x80`). `data-findings.md` §19
  registra o layout e a medição. Próximo: item 2 da fila (0x53 fechado; segue **14 —
  vizinhanças/lotes do AL**).
- (20/10) **14 FECHADO** — as versões das vizinhanças do AL entraram: **FAMI** 0x4E..0x55
  (0x4F põe o bairro depois do álbum, 0x50 o último lote jogado, 0x51 o business funds e
  0x55 o lote de férias; o 0x50 do B001 tem o último lote e não o business), **SDSC**
  0x20..0x36 com a tabela de *person data* (170/179/195/199/200/201/204/228/**230** no
  0x35 do E002/231) e a cauda do BV (u64 de colecionáveis), **NGBH** 0x6F/0x70/0xBE/0xC2/
  0xCB (EP ready marker, versão por slot, flags2, número de inventário e o u16 do Seasons),
  **NID** 3..0x0A (os 20 bytes do E001) e **lot info** 18/base 11 (cabeçalho de
  apartamento; a cauda de sublots fica em `rest`). Medido: `hood-sweep` do E001 passa de
  **0 Sims / 0 famílias / 0 lotes / 313 erros** para **234 Sims / 21 famílias / 56 lotes /
  0 erros**; N001–N003, F001, G001 e os 13 templates decodificam com 0 failed;
  `lot-run` do `E001_Lot127` por 60 ticks carrega 485 objetos (483 no VM; prédio de
  apartamentos sem família) e só esbarra nos 0x7E/Set to Next 38 do item 12; build+ctest
  **39/39**; `test_neighborhood` cobre as versões novas; `data-findings.md` §20 registra
  os layouts. Próximo: item 3 da fila (**12 — 0x7D/0x7E**).
- (20/10) **RE do item 12 (0x7D/0x7E) no exe do PC** — as strings do binário
  (" Follow Sim in ", "defined in objLua file", "Semi-Global", "Global", "Primitive not supported
  for this type of object.") levam aos handlers `FUN_008b9bd0` (Follow Sim) e `FUN_008b9e00`
  (Run Script); a tabela de despacho por opcode (thunks de 35 B a partir de `0x8ba900`, tabela
  em `0x8bab34+`) mostra as duas **na classe base** (mesma tabela do RayTrace 0x78 e do Change
  Outfit 0x79) — no Mac base elas não existem, e no BV são do routing aggregate; o gate de tipo
  de objeto é "qualquer objeto". Medidos: o layout dos parâmetros (id u16 em op0-1, valor u16
  em op2-3, byte de flags em op4 com os bits Global/Semi-Global/Private e mais dois de cache,
  e três pares (tipo u8, valor u16) em op6/op7-8, op9/op10-11 e op12/op13-14), o id empacotado
  do pedido `(op0-1 << 16) | op2-3` e o fallback do 0x7E para um arquivo `.objLua` (o registro
  de scripts Lua, `FUN_007fd6c0`). Censo do AL: **2.876** instruções 0x7E (em 567 grupos, id
  sempre 304) e 21 de 0x7D. **Bloqueio**: o id 304 (`0x130`) não é instância de árvore em
  nenhum escopo (no grupo do chamador não existe a 0x130; a única é a global "Family -
  Relationships - Reset") e nenhum op2-3 bate com uma instância do grupo — falta resolver a
  que o pedido é resolvido (RE de `FUN_00407f8a` e do registro de tipos). Registro completo em
  `docs/re-pc-ghidra.md` §(d) e nota no `primitives-mac.md` §2; o port continua recusando as
  duas (erro 0x05) e as 2.876 chamadas do AL continuam falhando.
- (20/10) **RayTrace 0x78 FECHADO** — o 0x78 resolvia o ponto pelo tile do objeto (uma
  aproximação); agora usa a mesma convenção de slot do Look At: byte 4 = 1/2/3 → os SLOT de
  tipo 4/3/0 (targeting/routing/containment) e byte 5 = o índice dentro deles, pasto a
  contagem erro **0x1B** (o mesmo do Look At), e o alvo pelos bytes 11/12; sem kind, o meio
  da caixa (no port, o tile do objeto). Medido no AL (`bhav-sweep`): antes 12.894.214
  instruções, 18.625 true e 14.126 falhas; depois 12.894.204, 18.622 true e 14.129 falhas —
  as 4 a mais são **"Slot number out of range"** (0x1B), as árvores globais que pedem um
  slot que o objeto não tem, que é o que o jogo dá. O censo do AL: 47 instruções 0x78 (o
  kind da origem em 3/0/1/255 e o do alvo em 3/2/1/0) e 1.052 SLOTs com no máximo 1
  entrada de cada tipo (2 de contenção), o que confirma o 0x1B nos índices altos. Teste novo
  `testRayTraceSlots` (o slot, o caso sem slot e o 0x1B); build+ctest **39/39**. No jogo base
  o mesmo desenho (22.861 árvores): antes 7.234.322 instruções, 11.110 true e 2.458 falhas;
  depois 7.234.312, 11.107 true e 2.461 falhas, com os mesmos 4 "Slot number out of
  range" a mais (0x1B).
- (20/10) **Item 8 FECHADO** — o `N002_Lot32` (stub DBPF de 96 bytes, sem recursos) deixava de
  ser erro de carga: `Lot::load` agora lança `EmptyLotPackage` (novo tipo em `formats/lot.hpp`)
  quando o pacote não tem nenhum recurso, o `s2pkg` responde "empty lot package: the lot
  package has no resources (the empty lot stub)" (sem "error:", sai com 0) e o `tools/soak.sh`
  conta os lotes vazios à parte (`empty|autonomy|...` e `empty|lot-save|...`, nova categoria
  "empty" no relatório). Medido: rodada do lote afetado (`soak.sh --one N002_Lot32`,
  `SOAK_TICKS=1800`, `SOAK_NO_APP=1`) com as duas linhas "empty" e nenhum "error";
  `docs/soak-report.md` round 19; teste novo `testEmptyLotPackage` (o stub de 96 B escrito
  pelo `DbpfWriter`); build+ctest **39/39**.

# Modos compra e construção: o que o binário de Mac faz

As fontes e convenções são as de `docs/shaders-mac.md`. Os endereços são do BV 1.10 ppc
(`/mac/sims2_bv.ppc`, nomes do symxfer conferidos no código). "base" é o
Mac 1.0 (`sims2_base_1.0.ppc`, sem símbolos: só foi possível conferir strings e
constantes). "dados" são os arquivos do jogo instalado (`data/game`, base
2004). "inferido" é dedução nossa e "só BV" marca o que não existe no jogo base.

**Campos do OBJD.** O código lê os campos com `cITSObjectDefinition::GetField(i)`
(vtable `+0x10`). Aqui *i* é o **índice do rótulo** do editor (STR#
`0x7FE59FD0`/0xCC em `objects.package`; o campo *i* fica no offset
`0x40 + 2i`, como em `src/formats/simantics.hpp`). Ex.: `GetField(0x27)` =
rótulo 39 "room sort flags" = offset `0x8E`. Os object data (dados de
instância) são lidos por `cITSObject::GetObjectData(i)` (vtable `+0x34`), com os
rótulos do STR# 0x8D (ex.: 42 "placement flags", 4 "allowed height flags",
41 "current value").

O modo compra já está no app (`src/app/buy_mode.cpp`); a construção (§7)
ainda não.

---

## 1. Catálogo (o que entra no modo compra)

### 1.1 Produtos (`cTSCatalog`, `cTSProductObject`)

- `cTSCatalog::LoadObjects` (BV `0xb312ec`) percorre **todos** os seletores de
  objeto carregados e chama `AddObjectProduct` (BV `0xb30f20`) para cada um.
  Um produto não é criado quando o seletor é de pessoa
  (`GetIsPerson`, vtable `0x98`) ou é uma parte não-líder de multi-tile
  (`GetIsMultiTileSubObject`, `0x94`). A lista é ordenada por
  **preço crescente** (`CatalogSortPred`, BV `0xb2a2d0`, compara
  `Price()`; `std::sort`, que não é estável, então a ordem entre preços iguais não está definida).
- `cTSProductObject::SetSelector` (BV `0xe501f8`) preenche o produto a
  partir do OBJD e das strings:
  - nome e descrição = `GetCatalogName` / `GetCatalogDescription` do
    seletor (abaixo); preço = **campo 18 "price"**;
  - `CatalogIDString` (`+8`) = nome do objeto (`GetObjectName`);
  - `NicenessMultiplier` = **campo 45** (float);
  - dica de construção: `BuildTipStringIndex` (BV `0x51ddf8`) só vale para
    objetos de construção (campo 69 "build mode type" = 1 ou 8 e campo 74
    "build Mode Subsort"), uma string do STR# `0x91`/`200` no grupo
    `0x7F606C7A`. Objetos de compra não têm dica.
- Getters do produto (vtable do `cTSProduct`): `0x5c` Price (`+0x44`), `0x60`
  NicenessMultiplier, `0x68` ShowInCatalog (`+0x50`), `0x78` Deprecated
  (`+0xa8`), `0x7c` IsPlaceholder, `0x80` IsCustom (`+0xaa`).

### 1.2 Strings do catálogo (binário + dados)

`cTSObjectSelector::GetCatalogResource` (BV `0x51e914`) carrega um
`cTSCatalogResource` com o **grupo do objeto** e a **instância = campo 41
"catalog strings id"**. Nos dados ele é o CTSS (`0x43545353`) do mesmo grupo.
Dos 485 OBJDs com room sort ≠ 0, 481 têm esse CTSS (4 não). Entrada 0 = nome,
1 = descrição (por idioma; o jogo usa o idioma atual). Ex.: grupo
`0x7FBBE64F`, `0x41A7`: "Gagmia Simore "RefuseNik" Trash Compactor", §375.

### 1.3 Quem aparece num botão (`cTSWinProcCatalog`)

`LoadObjectsByFunction(função, subsort)` (BV `0xa32bec`), `LoadObjectsByRoom(sala,
função)` (BV `0xa33038`) e `LoadObjectsByCommunity` (BV `0xa32e58`) percorrem os
produtos e aceitam um objeto quando **todas** as condições abaixo valem:

```
lote residencial: campo 39 (room sort) ≠ 0              ← isto é o que torna o objeto "comprável"
lote comunitário: campo 100 (community sort) ≠ 0 (ou room sort ≠ 0 em modo atacado, só BV)
por função: campo 40 (function sort) & função ≠ 0
            e, se subsort ≠ 0: campo 94 (function sub-sort) & subsort ≠ 0
por sala:   campo 39 & sala ≠ 0 e, se função ≠ 0: campo 40 & função ≠ 0
ValidForCatalog (BV 0xa2e818):
   ObjectUnlocked: campo 16 "Item Is Unlockable" = 0, ou o GUID está numa das
                   duas listas de desbloqueados (recompensas)
   e (só BV) campo 105 & (1 << código do EP atual) = 0
AddProduct (BV 0xa30b28): ShowInCatalog, não Deprecated; conteúdo
                   personalizado (IsCustom) só se o painel permitir
```

Bits (rótulos STR# 0xCD/0xCE; conferidos com os botões abaixo):

| bit | room sort (39) | function sort (40) |
|---|---|---|
| 0x001 | cozinha | Conforto |
| 0x002 | quarto | Superfícies |
| 0x004 | banheiro | Utensílios (cozinha) |
| 0x008 | sala de estar ("family room") | Eletrônicos |
| 0x010 | exterior | Hidráulica |
| 0x020 | sala de jantar | Decoração |
| 0x040 | diversos | Diversos |
| 0x080 | estudo | Iluminação |
| 0x100 | crianças | Hobbies |
| 0x200 | — | Recompensas de aspiração |
| 0x400 | — | Recompensas de carreira |

O base tem 485 objetos com room sort ≠ 0 nos dados. As funções 0x400 e 0x800
aparecem em 9 e 10 OBJDs.

### 1.4 Botões do painel (UI script `0x49060004`, `cTSWinProcBuyPanel`)

O layout está no script (`s2pkg ui-dump … 49060004`). O código
(`onBtnClickedID`, BV `0x22c60c`; `SetFunctionSort` `0x228e70`,
`SetFunctionSubSort` `0x2283e4`, `SetRoomSort` `0x229c80`, `SetRoomSubSort`
`0x229818`) mapeia os IDs assim:

- **Abas**: `0x2C584E6C` = por função (mostra `0xA000`), `0x4C578F23` = por
  sala (`0xC000`), `0x2CACE0CD` = coleções (só BV), `0x0D87DCF9` =
  comunitário (`0xF000`). Ferramentas: `0xCC607031` mão, `0xCC607032`
  conta-gotas, `0xCC607030` design, `0xCC607033`/`34` desfazer/refazer,
  `0xCC607035` dia/noite, `0xCC607036` vista de cima.
- **Função** `0xA100…0xA900`: cada botão abre o painel `0xB<k>00` da função
  bit `1 << (k−1)`: B1 Conforto, B2 Superfícies, B3 Utensílios, B4
  Eletrônicos, B5 Hidráulica, B6 Decoração, B7 Diversos, B8 Iluminação, B9
  Hobbies (`SetFunctionSort` mostra `0xB100` para 1, `0xB200` para 2, …).
  O botão A da função é o do caption (A100 Comfort, A200 Surfaces, A300
  Appliances, A400 Electronics, A500 Plumbing, A600 Decorative, A700 Misc,
  A800 Lighting, A900 Hobbies).
- **Subsort** dentro de `0xB<k>00`: `…10` voltar; **`…70` = Tudo (subsort
  0, sem filtro)**; `…60` = Diversos (0x80); os outros pelo binário:

| painel | 20 | 30 | 40 | 50 | 80 | 90 | A0 |
|---|---|---|---|---|---|---|---|
| B1 Conforto | 1 cadeiras de jantar | 2 poltronas | 4 sofás | 8 camas | 0x10 reclináveis | | |
| B2 Superfícies | 1 bancadas | 2 mesas | 4 mesas de canto | 8 escrivaninhas | 0x10 mesinhas | | |
| B3 Utensílios | 1 fogões | 2 geladeiras | 4 pequenos | 8 grandes | | | |
| B4 Eletrônicos | 1 entretenimento | 2 TV/PC | 4 áudio | 8 pequenos | | | |
| B5 Hidráulica | 1 vasos | 2 chuveiros/banheiras | 4 pias | 8 hidromassagem | | | |
| B6 Decoração | 1 pinturas | 2 esculturas | **0x20** cortinas | 8 plantas | 0x10 espelhos | **4** tapetes (oculto) | |
| B7 Diversos | — | — | **8** festa | **2** guarda-roupas | (B760 = **0x10** crianças) | 0x20 | 0x40 |
| B8 Iluminação | 1 mesa | 2 chão | **8** teto | **4** parede | 0x10 externas | | |
| B9 Hobbies | 1 criatividade | — | **2** conhecimento | **4** exercício | 8 recreação (inferido pelos dados) | | |

  Em negrito estão as exceções ao padrão 20→1, 30→2, 40→4, 50→8, 80→0x10. No B7, o
  `…60` é crianças (0x10) e o "Diversos" é `B770` (0x80); o "Tudo" é `B780`.
  O histograma do campo 94 nos dados confere com esses bits, e as contagens
  por bit particionam exatamente os 75 do Conforto (17+12+20+8+11+7). Os 17
  do `B120` (cadeiras de jantar) eram só o base: o app agora indexa a
  instalação inteira (`ResourceIndex::addRoot` + `installationResRoots`, no
  CLI `s2pkg catalog --installation`; o `TSData/Res` do base primeiro, os
  outros packs por caminho), e com as expansões o `B120` tem **47** (os 1666
  compráveis da instalação, Conforto 201 = 47+31+45+42+13+23). Conferido: a
  lista do `B120` não muda invertendo a prioridade dos packs, e nenhum
  candidato de `campo 94 & 1` é escondido pelos filtros (recompensa/Sim;
  nenhum item só-comunitário). **Resolvido 21/10**: os 6 que faltam para os 53
  da captura do jogo em 1280x720 (barra: 304 px de 619, 26 por página) não vêm
  dos mods desta máquina — o `README_MODS.txt` do backup `_MOD_BACKUP_20261016`
  lista os que estavam instalados (TS2 Extender, Lighting Mod UC,
  `userStartup.cheat`, Starship UI) e nenhum acrescenta objetos de catálogo: o
  `ObjectScripts.package` do Extender tem 0 OBJD, os `Overrides` do mod de luz
  só repetem GUIDs já contados (128AC80D, B28AC9D7, 128AD07E, 128AC991) e o
  tema de UI não tem objetos. Eram conteúdo do `Downloads` do usuário, que esta
  máquina não tem mais (o backup guardou só os arquivos substituídos). O port
  conta a instalação: 47 cadeiras, 1666 compráveis.
- **Sala** `0xC100…0xC900`: C100 sala de estar = 8, C200 jantar = 0x20,
  C300 exterior = 0x10, C400 diversos = 0x40 (oculto), C500 estudo = 0x80,
  C600 quarto = 2, C700 cozinha = 1, C800 crianças = 0x100, C900 banheiro
  = 4. Depois de escolher a sala, `0xD100…0xD900` filtram por função
  (D100 = 1 … D900 = 0x100, na ordem dos bits) e `0xDA00` volta. Os
  cabeçalhos `0xE100…0xE900` são as imagens da sala escolhida. Se a
  combinação sala+função não tiver itens (`AnyObjectsByRoom`), o subsort volta a −1.
- Sem item escolhido, o painel volta à ferramenta mão (`SelectTool
  0x492DEF1C`).

### 1.5 Miniaturas e preço na grade (`cTSWinProcCatalogThumbnail`)

- `SetProduct` (BV `0xdf3cac`): o texto do item é o **preço formatado** (com o
  cheat `showCatalogID` vira `"%s (%s)"` = CatalogIDString + preço). Há um
  ícone extra para conteúdo personalizado (`IsCustom`).
- `UpdateAffordability` (BV `0xdf3264`): se o preço cabe nos fundos da
  família, o fundo do botão segue os estados normal/realçado (faixa de 4
  quadros; `SetHilited` usa o quadro 3, normal o 1). Se não cabe, fica no quadro
  0 (**inferido**: o "apagado").
- A imagem vem de `cTSUIThumbnailService::GetProductThumbnail` (BV
  `0x2a9e88`). Para produto de objeto ela chama `GetObjectThumbnail(seletor)`, que
  **renderiza o modelo do objeto** com a fábrica de objetos (chave
  `0xAC2950C1`), com o rig de luz `UIObjectThumbnails` (`Lights/LightRig.txt` →
  recurso `Thumbnails`), e guarda o resultado como imagem `0x856DDBAC` no cache
  `Thumbnails/ObjectThumbnails.package` do usuário. Na próxima vez lê do cache. A
  falha mostra o texto "No Pic". (As miniaturas da fila de ações usam o
  mesmo serviço; ver `docs/ui-live-mac.md`.)
- A descrição (`cTSWinProcCatalog::ShowDescription`, BV `0xa2ef74`) usa os
  UI scripts "catalog description" (`0x49063003`…); as estrelas de motivo
  vêm de `GetCatalogRating` (BV `0x51e348`). Os índices 0–5 são os campos 82–87
  (fome, conforto, higiene, bexiga, energia, diversão), 8 é o campo 88
  (ambiente), 9–0xE e 0xF/0x16 são os bits 1…0x20 e 0x40/0x80 do campo 89 (perícias), e
  0x10–0x15 são os bits 1…0x20 do campo 17 (catalog use flags). Os índices 6/7 (campos
  103/104) são só BV.

**Para nós**: renderizar a miniatura com o nosso renderer (uma câmera fixa
de 3/4 e uma luz) e guardar em cache. Não é preciso ler o pacote do usuário.

### 1.6 A colocação do painel na tela (medido no jogo, 800x600 e 1280x720)

Coordenadas de cliente, medidas nas capturas do jogo em janela (o mesmo
binário; o jogo estava preso no preset `$Low` do `Graphics Rules.sgr`,
máx 800x600 — destravado para o 1280x720). Com o catálogo cheio de itens:

- À esquerda nada anda com a tela: o puck em x 0 (a arte da pílula em 516 e
  396, 3 px acima dos 519/399 que o script alcança — igual em Buy, Build e
  Live) e a janela do catálogo (`0x1000`/`0x2000`) com o cap esquerdo em
  x 537 nas duas resoluções — 5 px à direita do slot do script; o endcap da
  barra (`0x4CC48AA4`, 23 px) fica em 532..555 por cima da emenda (o cinza
  dele cobre 532..536, o cap em 537) e a coluna 1 em 589 = 537 + 52 (o canto
  (52, 19) da própria janela, sem inset).
- Com o catálogo fechado (nenhuma categoria escolhida) a barra fecha no fim
  da área do sort: o endcap no 503 do script (588 no cliente, 23 px) com o
  repeat (`0x4CC48AA6`) esticado do 265 até lá — o gradiente da arte corre
  até o cap (medido em 800x600); aberto, o endcap volta ao próprio 447
  (532 no cliente) e o repeat fica no 265..447 do script.
- A grade também não anda: coluna 1 em x 589 e topo da linha 1 em
  `altura − 129` (471 em 800x600, 591 em 1280x720); placas 44x44 de passo
  48, duas linhas (a expandida `0x2000`, quatro).
- Quem estica é só o catálogo: o cap final (20 px, `0x1003`/`0x2003`) fica
  com a borda direita visível em `largura − 30`, o miolo (`…02`) se estica
  até ele e o bloco de células (colunas × 48 − 4) cabe até lá — 3 colunas
  em 800x600 e 13 em 1280x720. O `▶` e o `⌃` vão por cima do cap (o glifo
  do `▶` em `largura − 61`, o `⌃` 3 px à direita) e o `◀` some na página 0.
- A barra de rolagem (`0x1008`, 12 px de altura, cantos de raio 4 — a
  coluna extrema abrange 8 das 12 linhas) vai da coluna 1 (x 589) até
  `largura − 72` (o navy fecha em 1207, o papel em 1208): 619 px = colunas
  × 48 − 5; o trilho (135,163,152) sob 1 px de navy no topo e na base, com
  o fill alcançando as pontas, e o knob — 2 px de navy em todas as bordas,
  1 px de (157,171,241) por dentro nas pontas, (174,189,255) no miolo —
  preenche a fração da página sobre o bloco: 304 px de 619 numa lista de
  26 de 53.

**Para nós**: o app segue isso (puck em x 0 com a pílula 3 px acima do pé
do script, a janela 5 px à direita do slot — cap em 537 —, o grid no canto
(52, 19) da própria janela, o endcap em 532 com o repeat esticado até o fim
da barra quando o catálogo está fechado, as colunas dinâmicas até o cap) e
bate a captura do jogo em 1280x720 (cap 1244..1250, `▶`
(1219,615)-(1231,662), `⌃` (1222,584)-(1230,596), o trilho de 589 ao navy
em 1207 com o papel em 1208); ficam de nota de polimento as bevels do knob
(±1 px) e o raio das placas (8 no app; as artes do jogo divergem ±1 px nas
colunas extremas).

---

## 2. Preço, fundos, valor e depreciação

- **Comprar** (`cTSUserToolUndoableObject::TryCreateImpl`, BV `0x35f178`):
  custo = campo 18. Recompensas de aspiração e de carreira custam 0. O
  objeto é criado na hora por `cEdithObjectModule::MakeNewOutOfWorldObject`
  (vtable `0x40` do módulo, BV `0x533fb8`; ver `docs/primitives-mac.md` §4c):
  roda o init (`OnInit`), `JustBorn` e `Reset`, e grava **od 41 = preço**.
  Fica fora do mundo, "segurado" pelo cursor.
- **Cobrar** (`cTSUserToolBuyBuild::SubmitUndoable`, BV `0x9b3b94`): ao
  confirmar uma operação, se a ferramenta não está em "sem custo"
  (`NoCharge`, vtable `200`, cheat/tutorial), **fundos += −TotalCost()**
  (`cTSUserToolUndoable::TotalCost`, BV `0x9b5568`, soma o custo próprio e o das
  pré/pós-operações). `CanAfford(c)` (BV `0x9b38fc`) = c ≤ 0 ou c ≤ fundos
  da família.
- **Lote sem família não cobra** (rodada 17): o `Init` de toda operação
  desfazível (BV `0x9b47a4`) e o da ferramenta (`cTSUserToolBuyBuild`, BV
  `0x9b36e0`, nomeado errado como destrutor) põem "sem custo" (+0x30 / +0x49)
  quando `cTSGameStateController::CurrentFamily()` (vtable `0x38`) é nulo, a
  não ser que a ponte da UI tenha um Sim selecionado
  (`cTSUIBridge::GetSelectedPerson`, vtable `0x24`; aqui: global 3 com um
  Sim no lote, mapeamento inferido). Com isso `TotalCost` dá 0 (nada é
  cobrado nem devolvido, vender também não dá dinheiro) e o
  `cTSUserToolObject::TryDrop` (BV `0x2111b4`) larga o objeto sem olhar os
  fundos. Os fundos do simulador (`cTSSimulator::GetFunds`, BV `0x498a24`:
  +0x98 + +0x9c) ficam como estão (§0 num lote novo). No opensims2:
  `vm::noCharge`, `vm::canAfford`, `vm::chargeFunds` (`placement.hpp`).
- **Vender** (o handler de remoção do undoable, BV `0x35f030`, nomeado errado
  `cTSUserToolFloorLevel::CalculateCost`): um objeto que já existia no lote
  (tipo 3 = movido) passa a custar **−GetCurrentValue()**, ou seja, o reembolso é o
  valor atual (0 para recompensas). Um objeto comprado nesta sessão (tipo 1)
  é destruído na hora com custo 0, e o preço volta porque a operação de compra sai
  da pilha. **Não há fator de revenda para objetos.** `cTSCatalog::RefundRate`
  = **0,8** (BV `0xb2a66c`) é para construção.
- **Valor atual** (`cTSObject::GetCurrentValue`, BV `0xafe9b8`) = object data
  41 "current value", **metade se quebrado** (`IsBroken`). Em
  `GetRawValue` (BV `0xafea2c`), as flags de venda do od 106 são só BV.
- **Depreciação diária** (`cTSObject::DayPassed`, BV `0xb07a4c`, chamado por
  `cEdithObjectModule::DayChanged`, BV `0x525a60`, para todos os objetos):
  ```
  se campo 37 "self depreciating" = 0 (e uma condição global da sim, vtable 0x54, for falsa):
      d = campo 35 "initial depreciation" se ≠ 0, senão campo 36 "daily depreciation"
      valor = valor − d
      se valor < campo 38 "depreciation limit": valor = campo 38
  ```
  Isto foi conferido no assembly (BV `0xb07c6c…0xb07d04`): o binário usa o
  campo 35 todo dia quando ele é ≠ 0. Nos dados, os 377 objetos compráveis com
  os dois campos têm inicial ≈ 15% do preço e diária = 10%, e o limite ≈ 40%. Ao carregar
  (`cTSObject::PostLoad`, BV `0xb0b3dc`), um valor abaixo do limite é
  corrigido para o **preço** (campo 18). Os multi-tile usam o OBJD do líder.
  O valor inicial é gravado na criação: `MakeNewOutOfWorldObject` põe od 41
  = campo 18 (preço). A depreciação diária começa na primeira meia-noite.
- Campo 34 "sale price" vale 35 em todos os objetos dos dados. Ele não é lido
  no caminho de compra/venda acima (aberto).

---

## 3. Colocação

### 3.1 Fluxo da ferramenta (`cTSUserToolObject`)

```
clicar no catálogo → TryCreate (BV 0x20ebf0): cria o undoable e o objeto (tipo 1)
mover o mouse     → OnMove/TryPlace (BV 0x213118) a cada mudança de célula
soltar o botão    → OnRelease (BV 0x208668) → TryDrop (BV 0x2111b4)
                     → SubmitUndoableForPlace (BV 0x20ccbc) → cobra (§2)
                     → se campo 96 "Keep Buying" ≠ 0 (ShouldKeepBuying, BV 0x203c44): cria outro igual
pegar no lote     → TryGrab (BV 0x20fe08): o undoable passa a ser tipo 3 (movido)
Delete/arrastar à lixeira → DeleteCurrentObject (BV 0x2083c8) / TryDelete (BV 0x211e40)
```

**No app** (rodada 21, `app/buy_mode`): `mouseDown` segura o objeto no tile sob o
mouse, `mouseDrag` o gira para o lado do mouse (a partir de meio tile do centro, em
passos de 2 = 90°; inferido), `mouseUp` coloca (`placeAt`); Esc (`cancel`) solta. A
pegada são os nós de contorno do jogo, `objectPlacementIconGood`/`objectPlacementIconBad`
(CRES em `Sims3D/Objects05.package`, nomes do binário perto de `CreateTileOutlines`):
moldura do tile com um chevron na borda +Y do modelo (a frente dos objetos), girada
pela direção. O raio do mouse anda até o primeiro ponto no piso do nível (a fundação
deixa o mesmo nível a 0,75 sobre ela e a 3,0 fora dela). "Keep Buying": a cópia nova
já fica no mesmo tile, com a mesma direção.

**A mão** (rodada 21, r21-cursor): pegar um objeto do lote já mostra o contorno
no tile onde ele está (antes só aparecia depois que o mouse andava, e nada aparecia
enquanto o botão que o pegou seguia apertado: playtest do tutorial, passo 20).
Com o botão ainda apertado, arrastar leva o objeto junto e soltar o põe no tile
do mouse; apertar e soltar sem sair do tile o deixa na mão (inferido: `TryGrab`
e depois `OnRelease` → `TryDrop`). Cursores: camera-ui-mac §2.7.

**TryPlace(loc)**, em ordem:
1. Ajusta a posição (vtable `0x168`; meio tile), escolhe o nível e alinha à
   calçada/estrada se o objeto pede (`OrientPerpendicularToRoad`,
   `ShouldObjectAlignWithSidewalk`).
2. `ShouldAutoRotate` → `AutoRotateObjectToMatchWalls` (BV `0x209e14`).
3. Prepara pisos (`TryPrepareFloors`), tenta colocar no telhado ou em
   moirão de cerca (ganchos vtable `0x11c`/`0x120`).
4. **Slot**: `FindFirstAvailableSlot` (BV `0x202e6c`) procura, no tile sob o
   cursor, um objeto com slot livre que aceite o objeto segurado. Se o achar e
   `TSObjectCanPlaceInSlot` passar (`FUN_009eba64`), coloca no slot
   (`TryPlaceInSlot`) e lembra o recipiente e o slot, para alternar entre os slots
   no próximo clique.
5. Senão, no chão: roda a árvore **"user placement"** (`RunUserPlacement`,
   BV `0x20dec4` → `OnUserPlacement` = **entry point 11**, se o bit 9 do od
   40 "flag field 2" estiver ligado), testa a posição
   (`FUN_009eb860`, abaixo) e, se passar, chama `TrySetPosition` e roda a árvore de novo.
   Se não passar, o objeto sai do mundo e o cursor mostra o fantasma na superfície
   (`SurfacePositionForObject`) com contorno vermelho.

**Teste de posição** (`FUN_009eb860`, BV `0x9eb860`). Com o cheat
`moveObjects` ligado (vtable `0xa4` do serviço da sim; a string existe no
base), tudo passa. Sem o cheat:
`TSObjectIsInLockedArea` → `TSObjectCanSetPosition` (BV `0x8ae264`) →
`TSObjectCanSetPositionAndOrientation` (BV `0x8ae104`). Os multi-tile vão para
`TSMTObjectCanSetPosition` (BV `0x8add68`), que testa cada parte deslocada e exige
que as partes não fiquem metade dentro e metade fora de uma coluna.
Para os outros objetos, `TSObjectCanSetPositionImpl` (BV `0x8adb34`) faz
`TestPlaceInWorld` (BV `0x8ad754`):

```
TestPlacementCommon(obj, loc, dir)                (sempre)
TestPlacementWithRespectToWalls(obj, loc, dir)    (se o tipo do objeto ≠ 2 pessoa)
TestPlacementWithRespectToObjects(obj, loc, dir)  (footprints e allow-intersection)
od 4 "allowed height flags" & máscara de altura do local → senão erro 0x19 (lista de lugares)
```

Sem lista de erros, o primeiro teste que falha encerra a função. Com lista
(no clique), todos os erros são juntados e mostrados.

### 3.2 `TestPlacementCommon` (BV `0x8aad90`), com p = od 42 "placement flags", p2 = od 82 "Placement Flags 2"

Os bits de p são os rótulos do STR# 0xCA ("Allowed Height Flags", na verdade
as flags de colocação; o mesmo enum da FreeSO `VMPlacementFlags`):
1 chão, 2 terreno, 4 sob a água, 8 preso à orientação do slot, 0x40 tile
travado, 0x80 solto do chão, 0x100 rampa, 0x200 no ar, 0x400 parede, 0x800
piscina, 0x1000 teto, 0x2000 moirão de cerca, 0x4000 superfície d'água,
0x8000 telhado.

| ordem | teste | erro (código → texto do UIText STR# 0x89) |
|---|---|---|
| 1 | local dentro do lote (multi-tile: cada retângulo da pegada) | 2 "Location out of bounds" |
| 2 | nível entre o mínimo e o máximo do lote | 3 "Level out of bounds" |
| 3 | pessoa (tipo 2): precisa de piso ou p&0x200; outros: piso sob a pegada, salvo p&0x200 ou p2&4 | 0x8E (lista de lugares) |
| 4 | parcialmente na água sem p&4 nem p&0x4000 | 0x90 (lista) |
| 5 | piscina sem p&0x800 | 0x8F (lista) |
| 6 | **terreno não plano** sem p&0x100 (multi-tile: todo canto da pegada) | 0x12 "Can't place on slope" |
| 7 | p&1 = 0 e o tile tem piso → não; p&2 = 0 e é terreno → não | 0x8C/0x8D (lista) |
| 8 | od 30 "room placement": 1 = só fora (erro se dentro), 2 = só dentro | 0x0A "Must be placed outside" / 0x0B "…inside" |
| 9 | p&0x2000 exige moirão de cerca | 0x79 |
| 10 | `FUN_008a69e8` (altura livre) e `TSObjectCeilingTooLow` / teto acima ocupado (salvo p2&0x20) | 0x2F "Ceiling too low" / 0x30 |
| 11 | pegada cruza poste de toldo (só BV) | 0x0C |
| 12 | escada ou elevador sobre deck | 0x99 |
| 13 | p2&2 / p2&0x10 / p2&8: precisa de/proíbe zona, lagoa (só BV) | 0x3C/0x3F/0x3E |

As praias (p2&0x40/0x80/0x100, códigos 0x9E–0xA0, 0xA5) são só BV.

**Texto do erro** (`cTSSimSystem::PlacementErrorString`, BV `0x58a6d4`): o
código vira um índice **1-based** da string em `UIText.package` STR# **0x89**
(ex.: código 2 → 1 → "Location out of bounds", 0x12 → 0x11 → "Can't place on slope",
0x19/0x8C…0x91 → 0x5B → "Must place in one of these places:" mais a lista das
superfícies permitidas, montada a partir de od 4/od 42 por `PlacementSurfaceTypeString`).
Para o resto da tabela, ver a função. Um erro do tipo 1 é texto do próprio
objeto (`cTSObject::PlacementErrorString`, BV `0xafac28`: STR# do grupo do objeto,
índice = código, chave `0x98xxxx`). O tipo 2 é a lista de superfícies.
Os 98 textos de erro existem nos dados base (entradas 0–97). As vazias
correspondem a códigos de EP.

### 3.3 Paredes (`TestPlacementWithRespectToWalls`, BV `0x8a8550`)

- **Flags de parede** (`FUN_008a7a60`, BV `0x8a7a60`, com f =
  `GetWallPlacementFlags`, os rótulos do STR# 0xE5): para as 6 direções
  (frente, direita, trás, esquerda, diagonal frente-direita, diagonal
  frente-esquerda) o código olha se há parede no lado do tile:
  - bits "exige parede" `1, 2, 4, 8, 0x10, 0x40`: sem parede ali → erro 7
    "Must be against wall" (ou 0x3B se o lado tem algo que não é parede
    utilizável). Com f&0x1000 (exige cerca), o erro é 0x88 "Must be placed on a fence";
  - bits "não permite" `0x100, 0x200, 0x400, 0x800` (frente…esquerda) e,
    nas diagonais, "permite" `0x20, 0x80` (o teste é invertido): parede ali →
    erro 8 "Can't intersect wall";
  - uma parede que vai do chão ao teto exige a altura livre do teste 10;
  - cercas com altura ≤ 0,35 (BV `0x274b824`) não contam como parede, e
    estufas exigem objeto de estufa (só BV).
- **Atravessar paredes** (`IntersectsWalls`, BV `0x8a7f1c`): salvo p&0x400 ou
  p&0x2000, nenhum retângulo da pegada pode cruzar um segmento de parede do
  nível → erro 8. O mesmo teste vale para arcos de cerca.
- Portas e janelas (tipo 8/9) usam o próprio teste de portal (`IsWallPortal`).

### 3.4 Objetos (`TestPlacementWithRespectToObjects`, BV `0x8ace04`)

Para cada objeto que a pegada do objeto segurado toca
(`TSObjectWorldSpaceFootprint`, BV `0x8aa40c`, que gira a pegada do OBJD pela
direção), `TestPlacementWithRespectToObject` (BV `0x8ac158`) testa:
- se as pegadas se cruzam (a do objeto parado e a do segurado);
- as flags do od 8 (STR# 0x8E: 0x2 "allow object intersection", 0x4
  "…object and person", 0x10 "…person") e o od 40 bit 0x4000 ("Low Small
  Object");
- as "exclusive placement flags" (od 63, STR# 0xFB: chão, parede, "nenhum
  outro do mesmo tipo", teto). Assim um quadro de parede e uma mesa podem dividir o tile;
- se tudo passou, a árvore **"allow intersection" (entry point 5)** dos dois
  objetos tem a última palavra (parâmetro 0 = o outro objeto).

A falha dá o erro 0x0C "Can't intersect other objects".

### 3.5 Rotação

- A direção é um valor de **0 a 7 (passos de 45°)**. `TryTurn(n)` (BV
  `0x204158`) soma n à direção, faz o resultado dar a volta em 0..7 e chama
  `TryOrient`, que testa `TSObjectCanOrient` (BV `0x8ae2fc`, o mesmo teste
  completo com a nova direção). Objetos que não aceitam diagonal só usam
  valores pares (`SwapDiagonalObject` troca o objeto pelo "diagonal
  selector" do OBJD, campos 21–24; o 1.0 base também tem, §7.6.1).
- O auto-giro só acontece com `ShouldAutoRotate`, encostando o objeto na parede.

### 3.6 Níveis

A posição tem nível (`cLocation +8`). O teste 2 limita o nível ao lote, e
objetos no andar de cima precisam de piso (teste 3). Tetos e paredes vêm do
nível. "Must be on first level" é o código 0x22 (`TSObjectCanDropImpl`).

**No app** (rodada 8): as teclas são **Page Up / Page Down** ("Subir um Andar
(Page Up)", "Descer um Andar (Page Down)": STR# `0x8A` do `UIText`, e as
dicas dos botões `0x2001`/`0x2000` do puck `0x4905F85B`, `btnclicksnd`
`ui_*` do script). No modo compra mudam o andar onde os objetos entram
(`BuyMode::changeLevel`, entre o primeiro e o último nível do lote) e a
câmera segue; no modo vivo só a câmera troca de andar, até o Sim selecionado
mudar de andar. Os dois botões do puck fazem o mesmo.

---

## 4. O que roda na VM

| momento | código | entry point (STR# 0xF5) |
|---|---|---|
| comprar (criar) | `MakeNewOutOfWorldObject` (BV `0x533fb8`) | 0 init (`OnInit`), depois `JustBorn` e `Reset`; od 41 = preço |
| clicar para colocar | `OnUserPlacement` (BV `0xafed44`) | **11 "user placement"** |
| mudar de posição | `SetPosition` → árvore de colocação | 9 "placement" |
| tirar do mundo | `RemoveFromWorld` | 10 "pickup" |
| pegar com a mão | `OnUserPickup` (BV `0xaf4bcc`) | **12 "user pickup"** |
| vender/apagar | `OnUserDelete` (BV `0xaf4c44`) | **50 "user delete"** |
| destruir | `KillObject` (BV `0x35cb04`) → gerente de objetos (vtable `0x44`) | 49 "delete", 3 "cleanup" |
| testar slot | recipiente | 52 "prevent place in slot" |
| todo dia | `DayPassed` | — (depreciação, §2) |

Multi-tile: `OnUserPickup`/`OnUserDelete` só rodam no objeto se ele não for
multi-tile (o grupo trata). Entry points são os índices do STR# 0xF5 que o
motor passa a `RunEntryPoint` (vtable `0x3b4`); a tabela é a mesma da TTAB/OBJf
(init 0, main 1, …).

---

## 5. Desfazer (`cTSUserToolUndoable*`, `cTSUserToolUndoManager`)

- Cada operação do jogador é um **undoable** com um custo e uma lista de
  pré e pós-operações (piso aplainado, terreno, paredes de sauna…). O
  undoable de objeto guarda o objeto (`+0x6c`), o tipo de mudança (`+100`: **1
  criado, 2 apagado, 3 movido**) e a posição original (`RecordPlacement`).
- **Desfazer** (`UndoSelf` → `ToggleChange`, BV `0x35eb6c`): o criado sai do
  mundo e vira "apagado". O apagado volta à posição/slot original e vira
  "criado". O movido troca a posição atual pela gravada.
- Um objeto **apagado não é destruído na hora**: ele sai do mundo
  (`RemoveImpl` = `RemoveFromWorld` + `OnUserDelete`). Ele só morre quando o
  histórico é zerado (`ResetSelf`, BV `0x35f4dc`, tipo 2 → `KillObject`), ao
  entrar no modo Vivo (rodada n20, §7.24: não ao trocar de Compra para
  Construção). Por isso é possível desfazer uma venda.
- Comprar e apagar um objeto criado na mesma sessão o destrói na hora
  (custo 0).
- Recompensas: confirmar marca a recompensa de carreira como usada ou cobra
  os pontos de aspiração, e desfazer devolve (`nTSRewards::*`).
- Atalhos: Ctrl-Z / Ctrl-Y (tooltips do script). `UndoLastCommand`, BV
  `0x30251c`.

---

## 6. Sons

`OnRelease` toca um som de UI ao soltar um objeto
(`RZ::WCM()->vtable 0x28`, ID em `PTR_DAT_02788d3c/44/50`): um para porta/janela
(tipo 8/9), um para plantas (`IsObjectOfFoliageType`) e um para o resto. Girar
(`TryTurn`) toca outro e "sem dinheiro" (`TryDrop`) outro. Os IDs são
ponteiros para globais. **Aberto**: a que sons eles apontam. No app (inferido pelos nomes `ui_*`
do executável, todos existem no `Sfx`): porta/janela `ui_build_door_plop`,
planta (função decoração com subsort plantas) `ui_build_garden_plop`, o resto
`ui_object_plop`; girar `ui_object_rotate`; sem dinheiro ou posição
recusada `ui_error`; pegar com a mão `ui_object_pickup`; vender
`ui_object_sell`; desfazer/refazer `ui_undo`/`ui_redo`; expandir/recolher o
catálogo `ui_panel_expand`/`ui_panel_collapse`.

---

## 7. Construção

Base comum: cada ferramenta é um `cTSUserToolBuyBuild` com um ou mais
undoables (§5), e o custo é cobrado no `SubmitUndoable` (§2: fundos −=
`TotalCost`, que soma o custo próprio e o das pré/pós-operações). Os nomes do
`baserom.sym` (o outro build ppc com símbolos, `references/sims2_mac_decomp`)
cobrem muitas funções que o symxfer não nomeou ou nomeou errado no BV; aqui
cada endereço BV vem com o nome do `baserom.sym` quando ele foi casado pelo
tamanho e pela ordem das funções no arquivo (ex.: o BV chama de
`Sims1AbsoluteDirectionToSims2Angle` o que é `PartitionStyleIsFenceArch`, e
de `cTSUserToolAutoRoof::RemoveAllRoofs` o que é `UserMayPlacePartitionCommon`).

Para saber se algo existe no base, `tools/re/constscan.py` conta onde o
código monta uma constante de 32 bits (ID de mensagem, CLSID) num binário ppc:
0 no base 1.0 e ≥ 1 no BV = só BV.

### 7.1 Catálogos de construção (dados)

Os produtos de construção não vêm de OBJDs, e sim de scripts de texto em
`TSData/Res/Catalog/Scripts/` (`walls.txt`, `fences.txt`, `fencearches.txt`,
`floorPatterns.txt`, `wallPatterns.txt`, `deckfoundations.txt`,
`modularstairs.txt`, `roofs.txt`, `roofPatterns.txt`, `terrainPaints.txt`,
`terrainSculptBrushes.txt`, `userToolProxies.txt`, `fireplaces.txt`). Cada
arquivo explica a própria sintaxe nos comentários. Eles são lidos por
`cTSCatalog::LoadWalls/LoadFences/...` (BV `0xb2fb9c`, `0xb325c0`...) com um
`cTSProduct*Parser`, mais os produtos empacotados em
`Catalog/Patterns/catpatterns.bundle.package`: XMLs `cGZPropertySetString` dos
tipos `0x2CB230B8` (cerca, 32 no base), `0xCCA8E925` (padrões de parede, 330,
e de piso, 146), `0xACA8EA06` (padrões de telhado, 8) e `0x4DCADB7E`
(pinturas de terreno, 14). Chaves dos XMLs: `cost`, `name`, `description`,
`showincatalog`, `deprecated`, `subsort`, `surfacetype`, `routingcost`,
`material`/`filename` (piso/parede), `rail`/`diagrail`/`post`/`height`/
`maywalkover` (cerca), `guid`.

**Preço** = chave `cost` (empacotados) ou, nos scripts de texto, a entrada
`3·i` do STR# indicado por `catalogTextResource build N` em
`TSData/Res/Text/Build.package` (grupo `0xFFFFFFFF`, instância N): para o
índice de catálogo *i*, a string `3i` é o preço ("70 -- Price - DO NOT
TRANSLATE"), `3i+1` o nome e `3i+2` a descrição. Preços do base:

| produto | STR# | preço |
|---|---|---|
| ferramenta de parede / de cômodo (`walls.txt`, índices 0 e 1) | 0x81 | **70** por trecho de parede |
| telhados (tipos) | 0x8C | 0; águas-furtadas 560/840/1120 (estreita/média/larga) |
| padrões de telhado | 0x8D | 0 |
| fundação / fundação diagonal | 0x8A | 4 por tile |
| deck com treliça / diagonal | 0x8A | 3 por tile |
| deck com colunas / diagonal | 0x8A | 2 por tile |
| piscina | 0x8A | 75 por tile |
| escadas | 0x8B | 40–85 (Wooden 40, Nouveau 70, Country 50, Chic 60, Steadfast 45, Holy Smoke 80, WroughtWright 85, Arboreal 40); corrimão 3 |
| terreno: subir/descer/nivelar/suavizar | 0x80 | 1 |
| água (lago) | 0x85 | 1 |
| lareiras | 0x90 | 600–3000 |
| pisos (`cost` do XML) | — | 2–21 por tile |
| revestimentos de parede (`cost`) | — | 2–15 por face de trecho |
| cercas (`cost`) | — | 5–250 por trecho |
| pinturas de terreno (`cost`) | — | 0 |

Os IDs de estilo de partição (`wall N`, `fence N`, `fenceArch N`) dividem o
mesmo espaço (< 255). No base: 1 parede normal (a única no catálogo), 3
parede de sótão (`floorToRoof`, apagada pela ferramenta de telhado), 23
parede de fundação, 16/24/4 saias de deck, 29 parede de piscina; 2 a cerca de
estacas escondida; o resto das cercas vem do pacote. As propriedades
(`mayChangeSurface`, `mayAttachObjects`, `mayCutAway`, `requiresFlatBottom`,
`requiresFlatTop`, `submersible`, `mayPlaceDiagonally`, `verticalSpan`,
`blocksPlacement`, `bearsLoad`, `blocksLight`, `deleteTool`,
`requiresSupportUnderneath`) viram os `nTSWorld::PartitionStyle*` (BV
`0x8b566c`…`0x8b5c48`) que as regras abaixo consultam.

**Meia-parede, estufa, parede de palco, espaçamento de postes** (chaves
`ishalfwall`, `isgreenhouse`, `stageWall`, `posttilespacing`) são **só BV**:
as strings não existem no binário base e nenhum XML do base usa essas chaves.
No base, toda cerca tem poste em cada vértice.

### 7.2 Ferramenta de parede (`cTSUserToolWall`, BV `0x1e0bbc`…)

**Modos** (`+0x70`, o atual; `+0x74`, o padrão da ferramenta):

| modo | como se chega | `UserAddPartitions` tipo |
|---|---|---|
| 0 parede em linha | produto "Wall Tool" (variante 0) | 0 |
| 1 demolir | Ctrl (estado 2) ou Ctrl+Shift (estado 4) | — (`UserRemovePartitions`) |
| 2 cômodo retangular | produto "Room Tool" (variante 1), ou Shift (estado 3) com a ferramenta de parede | 1 |
| 3 cômodo diagonal | variante 2 (só BV) | 2 |
| 4 cômodo de tamanho fixo girável | variante 3 (só BV; teclas de tamanho e giro) | 3 |

`StateChange` (`0x1e0e40`) recebe o estado dos modificadores (1 nenhum, 2
Ctrl, 3 Shift, 4 Ctrl+Shift, 5/6 só BV) e põe o modo; o estado 1 volta ao
padrão. As teclas `0x714798EE`, `0x314798AC`, `0x51462721`, `0x9146274C` do
`KeyCommand` (`0x1e0bbc`) não existem no base (constscan = 0) e não têm atalho
nos arquivos de teclado do base. Cada `LoadWalls` cria 4 produtos por parede
(variante 0–3, vtable `0x150` do produto = o modo), mas o texto do base só tem
as variantes 0 e 1 (nome/preço/miniatura).

**Arrastar** (`OnClick` `0x1e25b4` → `OnDrag` `0x1e1c88` → `OnRelease`
`0x1e1fd0`): o clique guarda o ponto inicial (`+0x7c`, vértice) e o nível; a
cada mudança de vértice, `ChangeWallsAndPlaceCursorAtEnd` (`0x1e1828`) desfaz
a prévia (vtable `0x20` dos três undoables) e refaz tudo do início até o
cursor com `UserAddPartitions` (BV `0x9aa90c`) ou `UserRemovePartitions`
(`0x9ac6b8`), mostra o custo (`ShowCostPreview` = custo da parede + da
terraplanagem) e marca "sem dinheiro" se `CanAfford` falhar (a soltura então
mostra o erro e descarta). Soltar: se houve ≥ 1 mudança, o undoable de
terraplanagem vira **pré-operação** e o de toldos (só BV) pós-operação, e a
operação é submetida (cobrança); toca o efeito `build_mode_tool_poof`.

Os três undoables da ferramenta: `+0x60` parede (`cTSUserToolUndoableWall`,
CLSID `0xEC167825`), `+0x64` elevação do piso/terreno
(`cTSUserToolUndoableFloorElevation`, `0x6C167887`), `+0x68` toldos
(`0x303AB8DF`, só BV).

**Geometria do traço** (`UserAddPartitions`, `0x9aa90c`):

```
tipo 0 (linha):
   dir = round((atan2(dy, dx) + π) / (π/4)) mod 8           # 8 direções, inclui diagonais
   passo[dir] = (−1,0) (−1,−1) (0,−1) (1,−1) (1,0) (1,1) (0,1) (−1,1)   # tabela BV 0x2738240
   n = max(|dx|, |dy|) (arredondados)
   PlaceWallLine(estilo, dir, n, início)
tipo 1 (retângulo): se dx = 0 ou dy = 0 vira uma linha; senão tenta os dois
   "L" (pelo canto (x1,y0) e pelo canto (x0,y1)) e só aceita se os dois passam;
   depois aplaina o chão de dentro (FUN_009a7dd8, §7.24)
tipo 2 (diagonal, só BV): se |dx| = |dy| vira o tipo 0; senão um losango
tipo 3 (só BV): octógono de lado ≥ 3 (AddOctagonToWallList)
```

O cursor só pousa em vértices (cantos de tile). Um traço diagonal anda em
(±1, ±1) por trecho: o trecho é a diagonal de um tile.

**`PlaceWallLine`** (`0x9a9fec`): para cada um dos *n* trechos monta o
`cWallLocator` (dois vértices + nível) e chama **`TryPlacePartition`**
(`0x9a9774`): primeiro `TryAdjustFloorElevationForWall` (`0x9a7f48`: nivela o
terreno sob o trecho, nos dois tiles de cada lado, pelo undoable de elevação),
depois **`UserMayPlacePartition`** (`0x9a95cc`) e, se passou,
`cTSUserToolUndoableWall::TryCreateAndInsertWall` (BV `0x30d75c`, nomeado
errado `RemoveFromLotObjectsThatShouldBeRemoved`). Uma falha que não seja o
código `0x41` interrompe a linha. Para cercas, depois da linha, um poste é
criado em cada vértice do traço que ainda não tem (`+ início e fim`; no BV a
cada `PostTileSpacing`).

**`UserMayPlacePartition`** (`0x9a95cc`), em ordem:

1. `UserMayPlacePartitionCommon` (`0x9a7384`), com o código de erro (texto em
   `cTSSimSystem::PlacementErrorString`, BV `0x58a6d4`, que mapeia o código
   para o índice 1-based do STR# 0x89 do `UIText`; ver §3.2):
   - o tile de um lado fora do mundo (parede) ou os dois (cerca) → `0x40`
     (texto padrão "Location out of bounds");
   - cruza outra partição no mesmo tile (BV `0x9a6b20`, nomeado errado
     `TSObjectUserCanPickup`; inferido: `PartitionBlockedByPartition`/
     `WallCrossesAnotherInSameTile`);
   - trecho diagonal e o estilo não aceita diagonal (`mayPlaceDiagonally`)
     → `0x4C` "Cannot build diagonally.";
   - sem o cheat de construir em qualquer lugar (sim system vtable `0xd4`):
     um lado (parede) ou os dois (cerca) fora da área editável do lote ou
     travados (`WorldDB` vtable `0xc4`) → `0x45`;
   - `requiresFlatBottom` e as pontas em elevações diferentes → `0x49`
     ("Can't place on slope");
   - `requiresSupportUnderneath` e sem apoio embaixo → `0x48` ("Needs
     something to support it");
   - os dois lados reprovados por `FUN_001648ac` (um teste de tile também
     usado pelos pisos) → `0x46` (texto vazio no base);
   - os dois lados na água e o estilo não é `submersible` → `0x47`.
2. Arco de cerca → `UserMayPlaceFenceArch` (`0x9a92fc`). Estilo 300 (parede
   invisível, BV) → sempre passa.
3. `MayPlaceWallWithRespectToObjects` dos dois lados (`0x9a86a0` +
   `0x9a8bd0`): para cada objeto no tile do lado (salvo os de od 0x22 ≠ 0):
   um **Sim** (tipo 2) → `0x4B` "Wall too close to Sim"; objeto que proíbe
   parede naquele lado (`NumWallOrFenceProhibitedFlags`, vtable `0x270`) ou
   cuja pegada é cruzada pelo trecho → `0x0C` "Can't intersect other objects".
4. Parede ou meia-parede: `UserMayPlaceWall` (`0x9a71f8`): os tiles dos dois
   lados precisam passar no teste de lote do `WorldDB` (vtable `0x108`), senão
   `0x45`. Cerca: `UserMayPlaceFence` (`0x9a6dd4`): inclinação
   |Δz| / comprimento horizontal > √3 (60°) → `0x31` "Terrain too steep";
   cerca em nível < 0 → `0x1C`; no térreo, um teste das salas dos dois lados
   (`0x0C`; não lido em detalhe); estufa (BV) → `0x9D`.

**Custo de colocar** (`TryCreateAndInsertWall`): preço do produto do estilo
(`cTSCatalog` vtable `0x4c` → `Price`, vtable `0x5c`) **por trecho**; o
undoable grava a mudança (`cWallChange`, vetor `+0x68`) e soma o preço ao
seu custo. Parede = 70; cerca = `cost` do XML.

**Apagar** (`UserRemovePartitions` `0x9ac6b8` → `EraseWallLine` `0x9ac0e8`
→ `TryErasePartition` `0x9abec8`): mesma linha de 8 direções. Para cada
trecho:
- `UserMayEraseWallOrFence` (`0x9abd20`): não há parede/arco ali → `0x42`
  (ignorado em silêncio: a linha continua); objeto **em uso** encostado na
  parede, em qualquer lado (od `0x3E` > 0 ou `IsOccupied`, também nas partes
  de um multi-tile) → `0x1B` "Wall next to object in use"; Sim no tile →
  `0x4B`;
- `TryRemoveWallOrFence` (BV `0x30d41c`, nomeado errado
  `cTSUserToolTerrainSculpt::SmoothenBrushFn`): se o produto do estilo não é
  "deleteTool levelRoom", **vende os objetos presos à parede** dos dois lados
  (`EraseWallAttachedObjects`, BV `0x30c514`: objeto que se encosta naquele
  trecho, vtable `0x26c`; cada um vira um undoable de objeto do tipo
  apagado, como em §2/§5, com o reembolso do valor atual) e, para cerca,
  apaga os postes das duas pontas que ficaram com menos de 2 cercas; tira a
  partição e grava a mudança com
  **reembolso = int(preço × `RefundRate` 0,8)** (`FullRefundMode` dá o preço
  inteiro, mas só as paredes de sauna do BV o ligam). Parede: 56 por trecho.
- Paredes de palco e invisíveis (estilos 301/300, BV) têm caminhos próprios.

Rodada 9 (lido): `UserMayEraseWallOrFence` testa os dois lados com
`FUN_009ab944` (BV): para cada objeto do tile do lado (salvo od `0x22` ≠ 0),
um Sim → `0x4B`; um objeto **preso àquele trecho** (vtable `0x26c` com o
locator; para multi-tile, qualquer parte presa conta pelo grupo) e em uso
(`FUN_009ab5f8`) → `0x1B`. Ou seja, o `0x1B` só vale para objetos presos à
parede, não para qualquer vizinho. `EraseWallAttachedObjects` (BV `0x30c514`,
nomeado errado `cTSUserToolMoveLot::DoMouseMove`) percorre os objetos do tile
do lado (`WorldDB` vtable `0xe4`/`ObjectOnTileAtIndex`) e apaga
(`cTSUserToolUndoableWall::EraseObject`) os que se prendem ao locator
(vtable `0x26c`), salvo os de categoria od `0x3B` = `0x7D` com vtable `0x348`
≠ 0. Aqui (inferido, o corpo do `0x26c` não foi lido): preso = porta/janela
com o trecho na frente ou atrás, ou objeto cujas flags de parede (od 13)
exigem parede do lado do trecho; em uso = od `0x3E` > 0 ou um Sim num slot
dele.

**Terraplanagem sob a parede** (`TryAdjustFloorElevationForWall`,
`0x9a7f48` → `FUN_009a79d0` por vértice): com `requiresFlatBottom`, cada
vértice do trecho (os dois da parede reta; a caixa do tile para a diagonal)
vai para uma altura dada (`FUN_009a5a68`); com `requiresFlatTop`, o vértice do
andar de cima também. Aqui a altura é a do início do traço e um vértice preso
por piso ou por outra parede não se move (inferido; rodada n20: o nivelador
e a altura do início lidos, §7.24).

O preço cheio só volta pelo **desfazer** (a mudança gravada sai com o custo).

**Desfazer** (`ToggleChange`, BV `0x30c134`): percorre as mudanças de trás
para frente e troca o estado (`+100`: 1 = aplicado, 2 = desfeito); paredes
voltam com os padrões das duas faces gravados na mudança.

### 7.3 Salas (`cRoomManager`) e o que é refeito

**Grafos e definições de sala (rodada 9, conferido no código).** O
`cPartitionManager` guarda **12 grafos de parede** (`cWallGraph`, um grafo
planar quad-edge: arestas com `Orig/Dest/LNext/Sym`, faces = salas) em
`+0x4c..+0x78`. `AddWallToGraph` (BV `0xc2e7cc`) põe cada partição nos grafos
conforme as propriedades do **estilo** (o `cTSProductPartition` lido de
`walls.txt`/`fences.txt`/`fencearches.txt`: `blocksPlacement`, `bearsLoad`,
`blocksLight`, `verticalSpan`, `notRendered`); `cPartitionManager::Save` dá
a instância do WGRA (`0x0A284D0B`) de cada um:

| grafo (`WallTopology(tipo)`) | entra | WGRA |
|---|---|---|
| `+0x4c` (0) | renderizada, `verticalSpan floorToFloor` | `0x16` |
| `+0x50` (1) | renderizada, `floorToFloorBasedCeiling` (decks) | `0x08` |
| `+0x54` (2) | renderizada, `floorToRoof` (sótão, estilo 3) | `0x17` |
| `+0x58` (6) | arcos de cerca (`PartitionType` 2) | `0x13` |
| **`+0x5c` (3)** | **`blocksLight`** | **`0x07`** |
| **`+0x60` (4)** | **`bearsLoad`** | **`0x06`** |
| **`+0x64` (5)** | **`blocksPlacement`** | **`0x05`** |
| `+0x68` (7) | tudo menos arcos e estilos 300/301 | `0x18` |
| `+0x6c`/`+0x78` (8/11) | meias-paredes (só BV) | `0x19`/`0x1f` |
| `+0x70` (9) | não renderizada de tipo 4 (parede invisível de luz) | `0x1a` |
| `+0x74` (10) | parede de palco (estilo `0x12d`, só BV) | `0x1c` |

Só três grafos têm gerenciador de sala (`InitWallGraphs`, BV `0xc2f644`):
`+0x5c` ← `tRoomDefinition` **0**, `+0x60` ← **1**, `+0x64` ← **2**. E
`cRoomManager::Save` grava cada um num 3ARY (`0x2A51171B`): definição 0 →
**0x0B**, 1 → **0x0A**, 2 → **0x09**. Ou seja:

- **3ARY 0x09 (definição 2) = salas de colocação**: as paredes que
  `blocksPlacement`. Mensagem `0x2B4D18F6`, escutada pelo
  `cEdithObjectModule` (sala dos objetos na VM). É o mapa da VM.
- **3ARY 0x0A (definição 1) = salas de carga** (`bearsLoad`).
- **3ARY 0x0B (definição 0) = salas de luz** (`blocksLight`). Mensagem
  `0x2B4D18FB`: paredes, pisos, cercas e light maps (`docs/lighting-mac.md`:
  "fora" = sala 0 **deste** mapa).

Pelos scripts de catálogo: parede comum: as três (padrões `true`); sótão
(3): sem `bearsLoad`; fundação (23): as três; deck (16) e piscina (29): sem
`blocksLight`; saia de deck (4): sem `blocksLight` nem `blocksPlacement`;
**cercas: só `blocksPlacement`** (padrões do `fences.txt`: `bearsLoad` e
`blocksLight` falsos); arcos de cerca: nenhuma. Portanto **as cercas dividem
as salas do 3ARY 0x09 e não as de 0x0A/0x0B**. Conferido no `N001_Lot11`:
em 0x09 o quintal dentro da cerca é a sala 17 (e as varandas cercadas, 20 e
22), a faixa entre a cerca e a borda do lote é a sala 0; em 0x0A e 0x0B o
quintal inteiro é 0 e só a casa tem salas. É assim que "as salas de pátio
viram a sala 0": não viram; elas só existem no mapa de colocação, e luz e
"fora" usam o 0x0B. (Corrige o que estava aqui: "cercas não mudam salas"
vale para 0x0A/0x0B; o aviso 1/2 do `UpdateRooms` é para o render.)

**Cada sala é um ID `short` por quarto de tile** (4 triângulos por tile; o
`cFloorTile` = x, y, nível + máscara de quartos 1, 2, 4, 8; uma parede
diagonal separa as metades 3/12 ou 6/9). `RoomIDInFloorTile` (BV
`0xc283fc`) devolve o ID do primeiro quarto da máscara (0xFFFF fora);
`RoomContaining` (vtable `0x38`, `0xc284b8`); "dentro" (`InsideRoomIDSet`,
vtable `0x44`, `0xc28ce0`) = todos os IDs ≠ 0 e ≠ −1 do mapa; a sala 0 é a
de fora. Slots do `cIRoomManager` (vtable `0x281ba30`): `0x24` `AddSeed`
(`0xc28ffc`), `0x28` `RefreshMap` (`0xc2a36c`), `0x2c`
`FlushRoomChangedNotifications`, `0x30` `SetRoomIDInFloorTile`
(`0xc29618`, também mantém a lista de quartos por sala), `0x3c`
`FloorTilesInRoom`, `0x40` `FloorTileInRoom`, `0x48`
`InsideRoomIDSetForLevel`, `0x4c` `RandomFloorTileInRoom`.
**Rodada 15:** o `FloorTileInRoom` (`0xc28ef4`; o do
`cPartitionManager`, BV `0xc314f8`, escolhe o gerente pela definição de
sala 0/1/2 em `+0x40`) devolve o **elemento 0** da lista da sala no mapa
`+0x34` (sala sem lista: um tile inválido). O `BuildRoomTileListMap` (BV
`0xc29c74`) monta as listas andando os tiles do `WorldDB` nível a nível, x
por fora e y por dentro (o iterador `FUN_0015b9c8`, dos limites do lote), e
põe o tile (com a máscara dos quartos daquela sala) na lista de cada sala
que um quarto dele tem; o `SetRoomIDInFloorTile` depois acrescenta no fim.
Para um lote carregado, o tile devolvido é o primeiro da sala nessa ordem
(é o que `vm/lot.cpp setLotRooms` usa para o valor 4, piscina).

**Quem decide os IDs é o grafo, não o flood fill.**
`cWallGraph::SetRoomsPostAttach` (BV `0xc1f1bc`, depois de cada aresta
nova):

1. Anda o ciclo da face à esquerda da aresta nova (`LNext`). Se volta pela
   própria aresta oposta (`Sym`), a aresta **não fecha** nenhum ciclo: as duas
   faces ficam com a sala que contém o meio da aresta (`RoomContaining`) e
   nada muda no mapa.
2. Se fecha, a face se divide: `CreateRoom` (`0xc1f070`) cria uma sala com o
   **menor ID livre** (a lista de salas é ordenada; o primeiro buraco, não o
   maior + 1). A sala nova fica com o ciclo à esquerda da aresta, **salvo**
   quando a sala velha é a 0 (fora) e o ciclo da esquerda não cerca a
   aresta (`FUN_00c1c480`: teste de paridade de raio contra as arestas do
   ciclo): aí a nova vai para o ciclo da direita. Ou seja: cercar um pedaço
   de fora sempre cria a sala do lado de dentro, e o resto continua 0;
   dividir uma sala ≠ 0 dá o ID novo ao lado esquerdo da primeira aresta
   (não ao menor pedaço).
3. Para cada aresta do ciclo da sala nova: a face vira a sala nova e
   `AddSeedToRoomManager` (`0xc1a454`) chama `AddSeed(locator do lado,
   ID)`; depois `RefreshMap` e `AttachWallsToRoom` (`FUN_00c1b254`: toda
   seção cujo lado virado para a sala tem esse ID ganha a face).

`cWallGraph::SetRoomsPreDetach` (`0xc1de18`, antes de tirar uma aresta) é o
inverso: se as duas faces têm salas diferentes, uma morre: sobrevive a da
direita, **mas a sala 0 sempre sobrevive** (tirar uma parede entre uma sala
e o lado de fora devolve a sala inteira à 0). Cada aresta do ciclo da que
morre recebe a sobrevivente e semeia o ID dela; `RefreshMap`; os quartos que
ainda têm o ID morto (bolsões que a inundação não alcançou) recebem a sala
do lado da parede mais próxima (`NearestWall` + sala virada para o tile); a
sala sai da lista.

**`AddSeed`/`RefreshMap`.** `AddSeed(locator, ID)` guarda num mapa
`cFloorTile → {ID, já coberto}` (`+0x1c`) o quarto (parede reta) ou a
metade (diagonal) do lado do locator; se um quarto vizinho já tem semente,
a nova não entra (ou a metade substitui os quartos dela) e as vizinhas
ficam marcadas "já coberto". `RefreshMap`: (1) lê o ID velho no tile da
primeira semente; (2) pinta **todas** as sementes com o ID delas
(`SetRoomIDInFloorTile`); (3) para cada semente não marcada,
`FloodRoomFromSeed(velho, novo, tile)` (`FUN_00c289e8`) parte dos **dois
quartos vizinhos dentro do tile** da semente (não do outro lado da aresta,
onde está a parede) e `FloodRoomRecurse` (`FUN_00c2887c`) segue: um quarto
com o ID velho vira o novo e espalha para os 3 adjacentes (`FUN_0015ddc0`:
os 2 do mesmo tile e o do outro lado da aresta do tile) que estejam no lote
(a única condição é `cWorldDB::IsOnLot(x, y)`, conferido no assembly).
**A inundação não testa paredes**: como só o ciclo da sala nova é semeado e
toda aresta dele tem o quarto de dentro já pintado, a inundação nunca chega
a um quarto de onde atravessaria uma parede. (4) limpa as sementes e marca a sala para o
`FlushRoomChangedNotifications` (`0xc28638`), que manda `0x2B4D18FB`
(definição 0), `0xAC2A4F4E` (1) ou `0x2B4D18F6` (2) por sala mudada.

`cTSUserToolUndoableWall::UpdateRooms` (BV `0x30bec8`, chamado no
`CommitSelf`) pede o recálculo ao gerenciador de partições (vtable `0x98`)
e avisa o mundo (vtable `0x54` do undoable) com **1** se alguma mudança foi
parede ou meia-parede e com **2** se só houve cercas; parede de piscina
também atualiza o scenegraph.

Quem escuta a mensagem de sala 0 (`FindConst`): `nTSSG::cWallManager`
(render groups por sala), `cFenceManager`, `cFloorManager`, `cObjectModule`,
o gerenciador de light maps (`FUN_006f3d30`, perto de
`cLightMapManager`) e outros dois do scenegraph. A de definição 2 vai para o
`cEdithObjectModule` (sala dos objetos para a VM). O roteamento escuta as
mensagens de mudança do mundo no `cTSRouteManager::DoMessage` (BV
`0x4f8038`; IDs `0x0B4CF313`, `0x2B4CF319`, `0x2B4CF327`, `0x2B4CF334`,
`0x0B537176`, `0x2B4D1900`…) e refaz os retângulos de parede
(`AddWallRectangle`/`RemoveWallRectangles`, `docs/routing-mac.md` §2).

Para implementar: depois de cada operação de parede, refazer os **três**
mapas, cada um só com as partições do seu grafo (WGRA 5/6/7 ↔ 3ARY
0x09/0x0A/0x0B): cercas dividem só o 0x09. Fechar um ciclo cria o menor ID
livre do lado de dentro (ou à esquerda); abrir junta na da direita ou na 0.
Um flood fill por componentes com as linhas do grafo como barreiras dá as
mesmas regiões; só a escolha de IDs difere (ver acima). Depois as paredes
do roteamento e os light maps das salas afetadas.

### 7.4 Pisos (`cTSUserToolFloorPattern`, BV vtable `0x27b1648`)

- **Modos** (`+0x84`/`+0x88`, por `StateChange` `0x2ffe3c`): 0 pintar
  retângulo, 1 apagar retângulo (Ctrl), 2 encher a sala (Shift), 3 apagar a
  sala (Ctrl+Shift). Apagar usa o "sem piso" do `WorldDB` (vtable `0x98`).
- **Atualização** (BV `0x300b2c`): com início (`+0x64`) e cursor (`+0x74`)
  diferentes, cada tile do retângulo recebe `UserSetFloorPattern` (BV
  `0x9a4e80`) com a máscara 0xF (tile inteiro); num clique sem arrastar, o
  tile do cursor com a máscara de quartos que veio do ponto do mouse
  (`+0x80`). Encher a sala = `UserSetFloorPatternRoom` (BV `0x9a4f90`).
- **Máscara do clique** (`FUN_003000cc`, lido na rodada 9): se o tile tem
  parede diagonal (`WorldDB` vtable `0xbc`/`0xc0`), a metade do lado do
  mouse; senão o tile inteiro (0xF), salvo com o modo de quarto de tile (só
  BV); no caminho de cima, os quartos da mesma sala do quarto clicado.
- **Girar** o padrão: tecla `0xACB3A629` (`,`/`.`) cicla o modo de rotação do
  `cTSMainToolbar` 1 → 2 → 4 → 8. A tecla `0xB1265EF3` (alterna o modo de
  quarto de tile no sim system, vtable `0xcc`/`0xdc`) é **só BV**. O
  `ChangeTile` compara o padrão (`WorldDB` vtable `0x9c`) **e** a rotação
  (`0xa0`) de cada quarto com os da ferramenta (toolbar vtable `0x78`). Nos
  lotes a rotação fica no **3ARY `0x20`** (`cWorldDB +0x94`, um `u16` por
  tile, um nibble por quarto; nenhum lote do jogo o tem); o 3ARY `0x15`,
  que o app lia como a rotação, é a camada de visibilidade do piso (rodada
  n48, §7.26: só 0 e 1 nos lotes).
- **Ao soltar** (BV `0x301324`): se pintou (modo 0 ou 2), cria um undoable de
  elevação como pré-operação e **aplaina o terreno** sob o retângulo (a altura
  do tile inicial, `LevelRect`) ou sob a sala; submete e toca o som.
- **Regras** (`UserMayPlaceFloorPattern`, BV `0x9a46cc`): área travada/fora do
  lote → `0x51`; água → `0x57`; praia (BV) → `0xA4`; terreno não plano no
  térreo (salvo `UnevenTilesFloorable` do `WorldDB`) → `0x53`; tile não
  "floorable" (no andar de cima, sem parede/apoio) → `0x52`; objeto no tile
  sem a flag od `0x2A` bit 1 → `0x55`.
- **`IsFloorable`** (BV `0x16a990`): acima do térreo o tile tem que ser plano
  e editável; passa se `HasSupport(tile)`; senão, se um dos 4 vizinhos está
  `SupportedUnderneath` (o quarto virado para o tile); e, se algum vizinho tem
  piso (`WorldDB` vtable `0xb4`), se algum tile a até 2 (|dx| + |dy| < 4,
  fora os cantos ±2/±2) está apoiado, ou um do 3×3 passa em `FUN_001654b4`.
  Aqui `HasSupport`/`SupportedUnderneath` = piso no tile de baixo ou parede
  que carrega peso numa aresta dele (inferido).
- **Custo** (`cTSUserToolUndoableFloorPattern::ChangeTile`, BV `0x3ee7e0`):

```
FloorPatternCost(p, reembolso) = floor(preço(p) × (reembolso ? 0,8 : 1))   (BV 0x9a3e50; sem produto = 0)
para cada quarto da máscara cujo padrão ou rotação muda:
   pintar:            custo += 0,25 × FloorPatternCost(novo, não)    (o piso velho não é reembolsado)
   apagar/restaurar:  custo −= 0,25 × FloorPatternCost(velho, sim)
```
  O custo é acumulado em float e convertido no `Cost`.

### 7.5 Revestimento de parede (`cTSUserToolWallpaper`, BV `0x27ec9c`…)

- **Modos** (`+0x54`): 0 uma face, 1 tirar de uma face (Ctrl), 2 todas as
  faces ligadas (Shift), 3 tirar de todas (Ctrl+Shift). `Update` (`0x27f3a4`)
  roda a cada movimento: desfaz a prévia, aplica na face sob o mouse e mostra
  o custo; soltar submete (se tem dinheiro).
- **Uma face** = um `cWallLocator` (trecho + lado). `UserSetWallPattern` (BV
  `0xa5406c`): só para estilos de parede com `mayChangeSurface`; não faz nada
  se a face já tem o padrão.
- **Shift (flood)**, `UserSetWallPatternFlood` (BV `0xa5428c`): parte da face
  clicada e anda pelo anel de faces da sala (camada de paredes do `WorldDB`,
  vtable `0xa0` com 7 = lado A, 10 = lado B; próximo `+0x3c`, anterior
  `+0x40`) nos dois sentidos, pintando cada face **enquanto ela tiver o mesmo
  padrão que a face clicada tinha**; para no primeiro diferente.
- **Tirar** = pôr o padrão padrão do estilo (`defaultPattern`, "blank" na
  parede normal; `FUN_00a53f20`).
- **Custo** (`ChangeWallPattern`, BV `0x394914`; `WallPatternCost` BV
  `0x3941e0`): por face, `+ preço(novo)`; quando o novo é o padrão em branco,
  `− int(preço(velho) × 0,8)`. Pintar por cima não reembolsa o antigo.

- **Geometria** (rodada 15, binário, `nTSSG::cWallGeometryNode`): a
  espessura padrão é **0,15** tile (construtor BV `0x74ab40`, campo `+0x1c4`
  do `SetThickness`; quem chama o setter não foi achado, só a vtable), a
  altura das paredes baixadas **0,2** (`+0x1c8`, `SetWallDownHeight`) e o
  deslocamento `−0,01` (`+0x1cc`). No papel de parede
  (`cWallPiece::ComputeSegmentVertices`, BV `0x744520`) **V = altura / 3**
  (uma textura cobre os 3 de altura; mais 0,05 num tipo de trecho) e **U vai
  de 0 a 1 em cada trecho de um tile** (0,5 no meio): uma textura por tile.
  O código usava a posição no mundo; desde a rodada n19 a parede inteira
  (chanfros, topo, tampas, encontros, corte por pontas) segue o binário:
  `docs/walls-floors-mac.md`.

### 7.6 Portas, janelas e arcos

São **objetos** (OBJD tipo 8 porta, 9 janela; arcos são portas sem folha),
postos pela ferramenta de objeto (§3), no catálogo de construção pelo campo
69 "build mode type" e 74 "build Mode Subsort" (`LoadBuildObjectsByFunction`,
BV `0xa32a24`: tipo igual e `subsort & campo 74 ≠ 0`). Regras: as de §3.3
com os bits "exige parede" das wall placement flags (sem parede → `7`
"Must be against wall") e a permissão de cruzar parede (p&0x400), mais o
teste de portal da cerca para portões (`MayPlaceFencePortalObject`, BV
`0x8a88a4`: o trecho tem que ter cerca; não pode cruzar arco → `0x85`).

**Recorte na parede**: o objeto tem uma máscara de 8 bits de lados com recorte
(`cTSObject +0x124`; bits 1/2/4/8 = os 4 lados, 0x10–0x80 = as diagonais;
contagem = vtable `0x27c`, BV `0xaf8124`; o i-ésimo trecho relativo = vtable
`0x29c`, BV `0xaf815c`, meio tile para o lado do bit). Para cada parte do
objeto e cada trecho, `nTSSG::cObject::ComputeWallCutoutLocators` (BV
`0x6612d8`) monta o nome do material da máscara com
`WallCutoutMaterialName` (BV `0x658428`):

```
<base>_<dx>_<dy>_<nível>_<dir>_wallMask
   base = STR# 0x85 (nomes de modelo) do objeto, entrada (tag 0xCC + 1)
   dx = "%dE"/"%dW"/"0", dy = "%dN"/"%dS"/"0" (deslocamento do trecho em relação ao objeto)
   dir ∈ S W N E NW SE SW NE   (o lado/diagonal do trecho)
```
Ex. nos dados: `windowdesigner_1e_0_1_n_wallmask_txmt`,
`doorclub2diagonal_1e_1n_0_nw_wallmask_txmt`.

**Correção (rodada 12):** a `base` **não** é o "objectCutout" do CRES (só as
5 pias de bancada têm essa propriedade, para o recorte do tampo).
`ComputeWallCutoutLocators` (BV `0x6612d8`) monta um texto de recurso
(`0xda590`: STR# `0x85`, índice = `GetFieldByTag(0xCC)` do objeto + 1,
língua 1) e usa esse texto; se for vazio, não há recorte. Nos dados o STR#
0x85 de portas e janelas é `["", "<nome do modelo sem _cres>"]` (ex.
`windowQuaint`, `windowQuaintDiagonal`), então a base é a entrada 1 (tag
0xCC = 0, inferido: o índice do modelo atual). Com essa base, **os 100
grupos de OBJD "Window…"/"Door…" do `objects.package` têm os TXMTs
`…_wallmask`** (453 nomes de máscara nos dados; 91 conferidos pelo nome
exato, os outros 9 também existem com o prefixo de tamanho na varredura).
O pré-teste `nTSWorld::ObjectHasWallCutouts` (BV `0x16a064`): a máscara de
lados (vtable `0x27c`) do objeto ou, se for multi-tile (vtable `0x19c`), de
alguma parte, > 0. Para cada parte e cada lado da máscara: dx/dy = posição
da parte menos a do objeto líder, nível = nível da parte + 1, e o nome vai
para `cWallManager::SetCutoutMaterial`. No app (rodada 12,
`formats/lot_model`): a base é o nome do modelo (o CRES que o lote guarda);
para cada trecho reto que a porta/janela atravessa, dx = tiles ao longo do +x
do objeto, nível = andares acima do objeto, e a textura do TXMT (alfa 0 =
buraco; as duas faces usam a mesma) vai na face com u ao longo do **−x** do
objeto (u = 0 na borda do tile do lado +x) e v de 3 tiles acima do piso até
ele, teste de alfa no shader. **Correção (rodada 16):** o u corria ao longo
do +x; as metades "0" e "1e" do arco de 2 tiles (a "1e" um tile para +x) só
se encontram na borda comum com u ao longo do −x: com +x os arcos da frente
de `N001_Lot82` abriam nas bordas de fora e deixavam uma tira de parede no
meio, que parecia duas folhas abertas (inferido pelas texturas: o buraco da
"0" vai de u = 0 a 0,85, o da "1e" de 0,15 a 1, o topo do arco mais alto no
lado comum; teste `testTwoTileArchMask`, `docs/screenshots/
r16-portas-arco-antes-depois.jpg`). A diagonal segue a mesma convenção
(inferido; nenhum lote do jogo base tem porta diagonal de 2 tiles para
conferir). Diagonais (rodada 13): os modelos "…Diagonal" ficam
em pé alinhados aos eixos (rotação de 0/90/180/270°) e a parede deles corre
ao longo de +x + o norte local (+x girado 90°), o que os 24 trechos diagonais
de `N002_Lot3` confirmam; a máscara do trecho é `<base>_<k>e_<k>n_<nível>_
<nw|se>_wallmask` (k = o trecho ao longo, a partir do do objeto: os de 2 tiles
têm `0_0` e `1e_1n`; o sinal do norte também é tentado ao contrário, inferido),
u ao longo da parede (√2 por trecho, inferido como o reto) e v igual.
`N002_Lot3`: 142 → 166 faces com máscara; `docs/screenshots/r13-wallmask-diagonal.jpg`
(as janelas da baía de `N001_Lot77`). Objetos sem máscara continuam com o
recorte retangular. **Rodada n36** (`walls-floors-mac.md` §6.2–6.3): cada
face acha o seu TXMT pelo `WallCutoutMaterialName` e o amostra pelo seu UV1
(com o `tsMirrorWallMask`), o topo leva a máscara da face A, e sem máscara
não há recorte nenhum.
`SetWallCutout` passa o nome
ao `cWallManager::SetCutoutMaterial` (BV `0x6c5d6c`), que troca o material
daquela face pelo TXMT da máscara (alfa = buraco). Ao apagar uma parede, os
objetos presos a ela são vendidos (§7.2), portas e janelas inclusive.

Portas viram portais do roteamento (`cTSRouteManager::AddDoorPortal`, BV
`0x4f1ea8`) e da luz (portais de `docs/lighting-mac.md` §5.4); não dividem
salas (só partições dividem).

#### 7.6.1 Porta segurada, auto-giro e paredes cortadas (issue 002, binário: Ghidra MCP, BV)

**Auto-giro** (`cTSUserToolObject::TryPlace`, BV `0x213118`, passo 2): se
`ShouldAutoRotate` (vtable `0x1f4`) e o od 59 (`0x3b`) do objeto ≠ `0x7D`
(sauna, só EP), roda `AutoRotateObjectToMatchWalls` (BV `0x209e14`) a cada
`TryPlace`, ou seja, a cada movimento do mouse para outro tile:
- `cTSObject::ShouldAutoRotate` (BV `0xafec08`): as wall placement flags
  (vtable `0x25c` = od 13) `& 0x5F` ≠ 0 (exige parede na frente, direita,
  trás, esquerda ou nas diagonais). `cEdithMTObject::ShouldAutoRotate` (BV
  `0xaf062c`): o líder ou qualquer parte.
- `AutoRotateObjectToMatchWalls`: no local da ferramenta (`this+0x7c`), testa
  a direção atual e depois +2, +4, +6 (90° cada, volta em 0..7) com
  `FUN_009eb96c` (`TSObjectIsInLockedArea` e
  `TSObjectCanSetPositionAndOrientation`; sem procurar slot). A primeira que
  passa vira a direção do objeto (vtable `0x2b4`). Nenhuma passando: troca
  pelo objeto do "diagonal selector" (`SwapDiagonalObject`) e tenta mais 4;
  ainda nada, troca de volta e a direção fica a mesma. **O 1.0 base tem o
  mesmo** (`/mac/sims2_base_1.0.ppc`, sem nomes, achado pelo vtable de
  `cTSUserToolObject` em `0x15e3288`: `TryPlace` `0xd8da48` → auto-giro
  `0xd904a4`, troca `0xd9068c`, os mesmos campos `+0xc8`–`+0xca` que o BV
  guarda em `+0x104`–`+0x106`).
- **O par grade/diagonal** (`TryGrab`, BV `0x20fe08`;
  `SetGridAlignedProductID` `0x2038b8`): ao pegar um objeto a ferramenta
  guarda o GUID do produto (`+0xac`) e o do par (`+0xb0`) e cria o par num
  segundo undoable (`+0x60`, ou `+0x5c` se o pego é o diagonal), escondido.
  `ObjectIsDiagonal` (BV `0x202bbc`): a propriedade `0xCE` do OBJD
  (`DiagonalSelector`, `0x202b04`) é 0; o par de um diagonal é a `0xCF`
  (`GridAlignedSelector`, `0x202b60`). Nos dados são os campos 21–22 e 23–24
  do OBJD (offset `0x6A`/`0x6E`, `DiagonalSelectorGUID`/`GridAlignedSelectorGUID`
  da OpenTS2): no `objects.package` base, 121 OBJDs apontam para o par — portas de 1
  e 2 tiles, arcos, janelas e cortinas ("Door - 1 Tile - Value" ↔ "Door - 1
  Tile - Diagonal - Value", "Window - Privacy" ↔ "Window - Privacy -
  Diagonal", "Curtains - Value" ↔ "Curtains - Value - Diagonal"). Os
  diagonais não estão no catálogo: só se chega a eles pela troca.
  `SwapDiagonalObject` (BV `0x203df4`) só troca se os dois existem
  (`+0x105`/`+0x106`), alterna `+0x104` e esconde o que sai (vtable `0x2ac`).
- **Paredes diagonais na colocação** (`FUN_008a7a60`, as 6 direções; a
  parede de cada lado é o wall locator entre dois cantos do tile,
  `FUN_0016530c`): os bits `0x10`/`0x40` exigem a diagonal que cruza o tile,
  e uma diagonal ali dá erro 8 salvo `0x20`/`0x80` (teste invertido).
  `PortalDiagonalWallPlacement` (BV `0xe1edd8`): portal com flags `& 0x50`. As
  janelas diagonais salvas têm flags `0xC0`, as portas diagonais `0xFC0`; as
  comuns, `0x4`/`0x1`.
- Girar com `,`/`.` (`TryTurn` → `TryOrient`) não passa por aqui; o próximo
  movimento do mouse sim.

**O mouse acha o chão, não a parede.** O vtable `0x80` da ferramenta
(`cTSUserToolGroundBased::OnAnyMouseEvent`, BV `0x9b6e08`) é, para a
ferramenta de objeto, `cTSUserToolObject::GroundAtGameWindowPoint` (BV
`0x2055d0`, sem nome no Ghidra; o vtable de `cTSUserToolObject` começa em
`0x27ab328`): o chão do andar (`cTSUserToolBase::GroundAtGameWindowPoint`),
e para objetos de piscina (od 42 & `0x800`) e de telhado um ajuste. Não usa
`GroundOrWallIntersectionAtGameWindowPoint` (BV `0x1f9c90`: o mais perto
entre o chão e `NearestWallIntersection`, só nas outras ferramentas).
`SnapToValidLocation` (vtable `0x168`, BV `0x203ec8`) põe no centro do tile.
Então, com a parede cortada, o tile sob o mouse é o de dentro ou o de fora
da parede, e é o auto-giro que encosta a porta nela (H3 da issue: o
comportamento do app já era o do jogo).

**O que some com a parede cortada** (`nTSSG::cWallManager::
HideShowAttachedObjects`, BV `0x6c2ee4`): para cada objeto preso à parede
(`cTSObject` vtable `0x26c`, BV `0xaf8458`: a parede está entre as que as
wall placement flags pedem, vtable `0x274`/`0x284`), esconde (nó de cena,
vtable `0x180`) quando:
- `cTSObject::HideWhenWallDown` (BV `0xaff050`): tipo do OBJD 9 (janela) **ou**
  object data 8 ("flags") com o bit `0x400` (o `HideForCutaway` da FreeSO; as
  próprias árvores do objeto o ligam: portas `0x2490`, janelas `0x0404`,
  portões, quadros, arandelas e lustres têm o bit nos lotes do jogo);
- e as wall placement flags não têm `0x1000` (em cerca; vtable `0x268`);
- e **não** é (modo de jogo 4 = Construção, `cTSMainToolbar +0x64`,
  `ePlayMode`; `SelectBuildModeTool` só age no modo 4) **e**
  `IsWallPortal` (tipo 8 ou 9). **No modo Construção portas e janelas ficam
  à mostra na parede cortada**; nos modos Vivo e Compra somem (H4/H5).
- Na mão: `cTSUserToolUndoableObject::SuppressHideWhenWallDownFlag` (base
  `0x1335da0`; no BV inlinado) desliga o bit enquanto o objeto é segurado e
  `RestoreHideWhenWallDownFlag` (BV `0x35ceb0`, chamado por `CommitSelf` e
  `ResetSelf`) o religa (`od 8 |= 0x400` se `+0x8c`).
- A parede **não sobe** sob a porta segurada: nada no caminho da ferramenta
  de objeto (`OnAnyMouseEvent`, `TryPlace`) mexe no corte; o
  `cWallCutawayManager::AddTargetLocation` (BV `0x6c22f4`) é dos Sims.

**No app** (rodada da issue 002): `vm::shouldAutoRotate`/`vm::autoRotate`
(`vm/placement.cpp`), chamados pelo `BuildMode::holdObjectAt`/`placeObjectAt`
e pelo `BuyMode::holdAt` quando o mouse anda, aperta ou arrasta (não nas
teclas de giro); `--door <obj>@x,y` sem direção (app e `s2pkg lot-save`)
gira como um clique. O gancho de visibilidade (`app/lot_vm_setup.mm`) segue
`HideWhenWallDown` + as exceções acima, no lugar da lista de prefixos de nome
("door", "window", "curtain"...). Inferidos: qual parede o objeto ocupa é a
geometria de `onCutWall` (`app/lot_view.mm`: parede cortada a até 0,6 tile, de
frente para o objeto), não os wall locators.

**Troca diagonal no app** (rodada i002-rest): `vm::diagonalTwin`/
`createDiagonalTwin` acham e criam o par (o OBJD do produto: o mestre do
multi-tile, senão o do objeto); `vm::autoRotate(vm, id, twin, ...)` faz as 4
direções com cada um e diz qual segurar. O `BuildMode` e o `BuyMode` criam o
par ao escolher no catálogo e trocam o objeto segurado (o que sai sai do
mundo); o comprado com o par usa o nome e o preço do produto do catálogo e o
OBJD/modelo do par (`catalogTwin`, inferido: o binário compra o produto do
undoable). `s2pkg lot-save --door` também troca. O `checkPlacement` agora lê
os lados diagonais: em nossas direções a diagonal de `0x40` corre ao longo de
direção + 1 e a de `0x10` ao longo de direção + 7 (conferido nas janelas
diagonais salvas de `N002_Lot3`: cada uma cabe onde está, as outras direções
não); um portal com `0x50` não leva o erro "parede diagonal no tile"
(inferido: o `IntersectsWalls` do jogo testa a pegada contra os segmentos).
Não feito: o par de um objeto já colocado e pego com a mão (o `TryGrab` do
jogo cria um para ele também; trocar o objeto exigiria vender um e comprar
outro), copiar as escolhas de design para o par (`+0x108`, vtable `0x478`).
Teste `tests/door_cut_wall.sh` (porta e janela comuns numa parede diagonal
viram as diagonais, no `s2pkg` e no app; as janelas de `N002_Lot3` cabem);
`docs/screenshots/i002-porta-parede-diagonal.jpg` (duas portas e uma janela
numa parede diagonal, Construção).
Teste `tests/door_cut_wall.sh`; `docs/screenshots/
i002-porta-parede-cortada-antes-depois.jpg` (antes: vermelho, "Deve estar
contra a parede"; depois: porta fantasma verde virada para a parede,
colocada, e com as paredes erguidas) e `i002-porta-paredes-erguidas.jpg`.

**De onde vem o bit `0x400`** (rodada i002-rest, Ghidra MCP + dados): só das
árvores do objeto. No binário, fora o `HideWhenWallDown` que o lê, o
`cTSUserToolUndoableObject::SuppressHideWhenWallDownFlag` (BV `0x35d564`, sem
nome no Ghidra) guarda o bit em `+0x8c` (`(od 8 >> 10) & 1`) e o apaga, e o
`RestoreHideWhenWallDownFlag` o devolve: só esconde/repõe o que já havia.
Nos BHAVs do jogo base, quem liga o bit (Expression "set flag" 11 no od 8 do
próprio objeto): `PaintingGlobals` "Sub - Init for wall hanging" e "Function -
User Place" (`0x7F446D21`), as luminárias de parede/teto (`0x7F60C397` "Init -
Wall"/"Init - Ceiling", `0x7F18E0F0`, `0x7F48CA31`), as janelas (`0x7F3B9D51`
"SG: Init", "SG: Init Diagonal"...), lareira, etc. Quem o **apaga**: o
`MirrorGlobals` "Function - Init" (`0x7F84A9F4` árvore `0x2000`, nó 7: "clear
flag" 11 no od 8) e os espelhos de chão (`0x7F5B99A2`, `0x7FC83200`). Então os
espelhos de parede, o telefone de parede e o detector de fumaça (od 8 sem
`0x400` nos lotes) **ficam à mostra** na parede cortada no jogo, e o app agora
também (saiu a regra inferida "base do modelo acima de 0,5"; antes/depois em
`docs/screenshots/i002-espelho-parede-cortada-antes-depois.jpg`, `N001_Lot82`,
os dois espelhos Marroquinos sobre as pias e a banheira). Quadros e arandelas
continuam sumindo (têm o bit).

### 7.7 Desfazer na construção

Igual a §5: cada operação é um undoable com pré/pós-operações; `CommitIt`
(BV `0x9b4ff0`) roda pré, a própria, pós absolutas e pós, e grava o custo
total. Pisos e revestimentos guardam por tile/face o padrão (e a rotação)
antigo e trocam com o mundo (`SwapStateWithWorld`/`SwapOneTile`, BV
`0x3ee340`/`0x3ee268`); a terraplanagem guarda as alturas dos vértices.
Ctrl-Z/Ctrl-Y são os IDs `0x0CA297F4`/`0x6CA297FC` do arquivo de teclado
(`ui.package`, tipo `0xA2E3D533`); os botões `0xA1`/`0xA2` do painel chamam
o gerente de desfazer (vtable `0x20`/`0x14` e `0x24`/`0x18`).

### 7.8 Painel de construção (UI script `0x49060003`, `cTSWinProcBuildPanel`)

`onBtnClickedID` (BV `0x287898`): ferramentas `0xA5` mão (ferramenta 7),
`0xA4` conta-gotas (`0x2D`), `0xAB` design (`0x2F`), `0xAA` dia/noite,
`0xA3` vista de cima, `0xA1`/`0xA2` desfazer/refazer, `0xA6` volta ao painel
de categorias (`0xA000`), `0xA7` coleções (só BV). Categorias
`0xA100`…`0xAA00` → `SetFunctionSort(1…10)` (A100 parede, A200 fundações e
varandas, A300 portas e janelas, A400 pisos, A500 revestimentos, A600
escadas, A700 terreno, A800 jardinagem, A900 telhados, AA00 diversos). Cada
uma abre o painel `0xB<k>00`, cujo `…10` é "voltar". Subsorts
(`SetFunctionSubSort(código)`, BV `0x283b54`):

| painel | botões → código |
|---|---|
| B300 portas/janelas | B320 portas, B330 janelas de 2 andares `0x13`, B340 de 1 andar `0x14`, B350 arcos `0x16`, B360 tudo `0x17` |
| B400 pisos | B420 pedra `0x25`, B430 tijolo `0x26`, B440 madeira `0x27`, B450 carpete `0x28`, B460 ladrilho `0x2A`, B470 linóleo `0x29`, B480 moldado `0x2B`, B490 outros `0x2C`, B4A0 tudo `3` |
| B500 revestimentos | B520 tijolo `0x1C`, B530 alvenaria `0x1D`, B540 madeira (siding) `0x1E`, B550 moldado `0x1F`, B560 pintura `0x20`, B570 papel `0x21`, B580 ladrilho `0x22`, B590 painéis `0x23`, B5A0 tudo `1` |
| B600 escadas | B630 escadas `0xE`, B640 de ligação (modulares) `0x10`, B620 ferramenta de corrimão (ferramenta `0xF`) |
| B700 terreno | B720 elevação `0xB`, B730 cobertura `9`, B740 água `10`; B751–B753 pincel P/M/G |
| B800 jardim | B820 árvores `0x18`, B830 arbustos `0x19`, B840 flores `0x1A`, B850 tudo `0x1B` |
| B900 telhados | B920 tipos `6`, B930 padrões `0xD`, B940 águas-furtadas `0xC`, B950 automáticos `0x30` |
| BA00 diversos | BA20 colunas `0x12`, BA30 lareiras `2`, BA40 piscinas `0x2E`, BA50 cercas `0x11`, BA60 portões `0x15` |

Pisos e revestimentos filtram pela chave `subsort` do XML (`"stone"`,
`"brick"`, `"wood"`, `"carpet"`, `"tile"`, `"lino"`, `"poured"`, `"other"`
nos pisos; `"brick"`, `"masonry"`, `"siding"`, `"poured"`, `"paint"`,
`"wallpaper"`, `"tile"`, `"paneling"` nas paredes; `"all"` = todos;
`cTSWinProcCatalog::LoadFloorPatterns/LoadWallPatterns`, BV
`0xa320bc`/`0xa32238`). O painel de telhados automáticos é o `0xAC` (C100 sem beiral, C200
triangular curto, C500/C600 mistos, C300 remover, C700 laje, C400 preservar).

**Auditoria do port (21/10).** Sem as capturas (não estão nesta máquina), o
painel foi conferido contra esta tabela e contra o censo do catálogo de
construção (`s2pkg catalog <Res> --installation --build`, novo: os objetos por
campo 69 "build mode type" e campo 74 "build Mode Subsort"). O censo: tipo 1
→ subsorts 0x08 (24), 0x20 escadas (13), 0x40 (5), 0x100 (7), 0x200 colunas
(10), 0x400 garagens (7), 0x800 elevadores (4), 0x1000 cornijas (58); tipo 4
→ 0x01 árvores (35), 0x02 arbustos (13), 0x04 flores (27), 0x10 as ferramentas
de jardim (11); tipo 8 → 0x01 portas (57), 0x02 janelas de 2 andares (3),
0x04 janelas (67), 0x08 portões (15), 0x10 arcos (21), 0x100 (4). As três
lacunas que a tabela denuncia e o port tinha: **a categoria A800 jardinagem
inteira** (não havia `case 8`), **as colunas BA20** (o subsort 2 do painel de
diversos) e **os pincéis B751–B753** (o subsort 5 do painel de terreno, as
ferramentas de jardim). As três foram implementadas; o `B620` (a ferramenta de
corrimão) já estava no clique do subsort, e o resto da tabela (portas, pisos,
revestimentos, escadas, terreno, telhados, lareiras, piscinas, cercas e portões)
bate com o port.

### 7.9 O resto, resumido

- **Escadas** (`cTSUserToolModularStairs`): `modularstairs.txt` define cada
  escada por cinco objetos (patamar, marcador, degrau, degrau de cima, de
  baixo); a ferramenta monta degraus como objetos. Textos de erro próprios
  (STR# 0x89 #58–#71: patamar, degraus, "No ground at top/bottom"). Preço por
  escada (tabela).
- **Telhados** (`cTSUserToolRoof`, `cTSUserToolAutoRoof`): de graça (0),
  salvo águas-furtadas; auto-telhado por sala (`CreateAutoRoofs`,
  `AppendRoomTilesIfValid`); paredes de sótão (estilo 3) acompanham.
- **Fundações, decks e piscinas** (`cTSUserToolDeckFoundation`,
  `deckfoundations.txt`): cada tipo diz o estilo das paredes laterais
  (fundação 23, deck 16/4, piscina 29) e o piso; preço por tile (tabela).
  Os textos "Foundations only allowed on terrain" (código `0x1A`) e "Pools
  only allowed on terrain" (`0x8A`) existem; os testes não foram lidos.
- **Terreno** (`cTSUserToolTerrainSculpt`): só terreno exposto do térreo
  (senão `0x8B` "Only exposed terrain can be edited"); o pincel é aplicado a
  cada **100 ms** com o botão apertado (`ApplyTerrainSculptBrush`, BV
  `0x32a214`; largura 6, força 0,0002/0,00002 do `terrainSculptBrushes.txt`);
  altura arredondada ao passo da ferramenta. Custo (BV `0x4003c8`):
  `round(Σ|Δaltura| × custo por unidade)`, cobrado **na hora** a cada
  aplicação (não no soltar) e recusado se faltar dinheiro.
  `FloorElevationUnitChangeCost` devolve 0. Pintura do terreno
  (`cTSUserToolTerrainPaint`) custa 0.

### 7.10 Códigos de erro de colocação e as fundações/decks/piscinas (rodada 9, binário)

**Tabela completa** do `cTSSimSystem::PlacementErrorString` (BV `0x58a6d4`):
código → índice 1-based do STR# 0x89 do `UIText.package` (o texto é o
`#índice−1` em inglês). Código fora da tabela (ex.: 2) → 1 "Location out of
bounds". Códigos cujo texto é vazio no jogo base (provavelmente só BV):
0x14, 0x15, 0x1d, 0x1e, 0x1f, 0x3c, 0x3d, 0x3e, 0x3f, 0x46, 0x47, 0x54, 0x55, 0x56, 0x57, 0x5d, 0x5e, 0x5f, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0xa5.

| código | STR# 0x89 | texto |
|---|---|---|
| `0x01` | 74 | Must be against a pool wall |
| `0x03` | 2 | Level out of bounds |
| `0x04` | 3 | Must be at center of tile |
| `0x06` | 5 | Cannot create level above highest level |
| `0x07` | 6 | Must be against wall |
| `0x08` | 7 | Can't intersect wall |
| `0x0a` | 9 | Must be placed outside |
| `0x0b` | 10 | Must be placed inside |
| `0x0c` | 11 | Can't intersect other objects |
| `0x0e` | 13 | Can't find a place to put it |
| `0x0f` | 14 | Can't support the weight |
| `0x10` | 15 | Can't support the size |
| `0x11` | 16 | Can't place on top |
| `0x12` | 17 | Can't place on slope |
| `0x13` | 32 | Needs something to support it |
| `0x16` | 21 | Insufficient funds |
| `0x17` | 22 | Must be against unused wall |
| `0x18` | 23 | Must be on diagonal |
| `0x19` | 91 | Must place in one of these places: |
| `0x1a` | 25 | Foundations only allowed on terrain |
| `0x1b` | 26 | Wall next to object in use |
| `0x1c` | 63 | No ground at bottom |
| `0x21` | 32 | Needs something to support it |
| `0x22` | 33 | Mantle rotation must match chimney |
| `0x23` | 34 | Hearth can have only one mantle |
| `0x24` | 35 | Can't delete object |
| `0x25` | 91 | Must place in one of these places: |
| `0x26` | 37 | Can't pick up |
| `0x27` | 38 | Can't select object in use |
| `0x28` | 39 | Can't pick up—location out of bounds. |
| `0x29` | 40 | Fireplace mantle must be all in one room |
| `0x2a` | 41 | Must connect to upper floor |
| `0x2b` | 42 | Can't intersect upper-floor object |
| `0x2c` | 43 | Can't rotate |
| `0x2d` | 44 | Can't delete pool with Sims in it |
| `0x2e` | 45 | Must first delete ladder or diving board |
| `0x2f` | 8 | Ceiling too low |
| `0x30` | 97 | Ceiling too high |
| `0x31` | 4 | Terrain too steep |
| `0x32` | 91 | Must place in one of these places: |
| `0x33` | 91 | Must place in one of these places: |
| `0x34` | 91 | Must place in one of these places: |
| `0x35` | 91 | Must place in one of these places: |
| `0x37` | 91 | Must place in one of these places: |
| `0x38` | 93 | Can't delete object with something on it or in it |
| `0x39` | 38 | Can't select object in use |
| `0x3a` | 38 | Can't select object in use |
| `0x3b` | 98 | Cannot be placed on a diagonal wall |
| `0x41` | 7 | Can't intersect wall |
| `0x43` | 7 | Can't intersect wall |
| `0x44` | 6 | Must be against wall |
| `0x48` | 32 | Needs something to support it |
| `0x49` | 17 | Can't place on slope |
| `0x4a` | 76 | Can't place wall |
| `0x4b` | 75 | Wall too close to Sim |
| `0x4c` | 47 | Cannot build diagonally. |
| `0x4d` | 48 | Can't place next to arch |
| `0x4e` | 49 | Can't delete arch |
| `0x4f` | 50 | Can't remove object on fence |
| `0x50` | 94 | Can't place next to wall |
| `0x52` | 32 | Needs something to support it |
| `0x53` | 17 | Can't place on slope |
| `0x58` | 85 | Must fill within enclosed area or on existing tiles |
| `0x5a` | 51 | Covered by floor tile |
| `0x5b` | 52 | Attached to wall |
| `0x5c` | 53 | Attached to wall |
| `0x61` | 54 | Can't move occupied terrain |
| `0x63` | 55 | Can't raise floor any closer to floor above |
| `0x64` | 56 | Can't lower floor any closer to floor below |
| `0x65` | 57 | Can't lower terrain further |
| `0x66` | 58 | Can't raise terrain further |
| `0x68` | 59 | Can't raise terrain above stairs |
| `0x69` | 60 | Can't raise floor above |
| `0x6a` | 61 | Can't lower floor below |
| `0x6b` | 55 | Can't raise floor any closer to floor above |
| `0x6c` | 62 | No ground at top |
| `0x6d` | 63 | No ground at bottom |
| `0x6e` | 64 | Can't level top landing |
| `0x6f` | 65 | Can't level bottom landing |
| `0x70` | 66 | Can't place top landing |
| `0x71` | 67 | Can't place bottom landing |
| `0x72` | 68 | Can't reserve space above stairs |
| `0x73` | 69 | Can't place step |
| `0x74` | 69 | Can't place step |
| `0x75` | 70 | Can't scoop out terrain |
| `0x76` | 7 | Can't intersect wall |
| `0x77` | 71 | Can't reach ground |
| `0x78` | 72 | Must have at least one step |
| `0x79` | 79 | Must be placed on a fence. |
| `0x7a` | 11 | Can't intersect other objects |
| `0x7b` | 73 | End points at different elevation |
| `0x7c` | 73 | End points at different elevation |
| `0x7f` | 11 | Can't intersect other objects |
| `0x80` | 73 | End points at different elevation |
| `0x81` | 12 | Can't level terrain |
| `0x82` | 76 | Can't place wall |
| `0x83` | 11 | Can't intersect other objects |
| `0x84` | 7 | Can't intersect wall |
| `0x85` | 77 | Can't place gate across arch |
| `0x86` | 78 | Can't extend pool |
| `0x87` | 95 | Can't delete pool |
| `0x88` | 79 | Must be placed on a fence. |
| `0x89` | 86 | Can't place gate on short fence |
| `0x8a` | 90 | Pools only allowed on terrain |
| `0x8b` | 89 | Only exposed terrain can be edited |
| `0x8c` | 91 | Must place in one of these places: |
| `0x8d` | 91 | Must place in one of these places: |
| `0x8e` | 91 | Must place in one of these places: |
| `0x8f` | 91 | Must place in one of these places: |
| `0x90` | 91 | Must place in one of these places: |
| `0x91` | 91 | Must place in one of these places: |
| `0x92` | 96 | Can't delete objects in rewards mode |

Os códigos 0x19, 0x25, 0x32–0x35, 0x37 e 0x8C–0x91 dão "Must place in one of
these places:" seguido da lista de lugares (§3.2).

**Fundações, decks e piscinas** (`cTSUserToolDeckFoundation`; no base a
piscina é um tipo de deck, `deckfoundations.txt`). Cada tile da seleção
passa por `CanPlaceTile` (BV `0x34f5a8`): tile com água (`IsPartWater`) →
`0x1E` (sem texto no base); praia (só BV) → `0x86`; um objeto no tile que
não tem o od 0x22 nem o bit 0x200 ("no ar") da od 42 → `0x0C` "Can't
intersect other objects"; e para cada um dos 4 lados, se o vizinho já tem
deck/fundação de um tipo que não casa com o lado (tabela por direção do tipo
do vizinho: 1 → só 0, 2 → 4/6, 4 → 2/4/6, 8 → 0/2/6) → `0x86` "Can't extend
pool". `MayPlaceQuadrilateral` (`0x352a30`) antes recusa seleção degenerada,
cantos fora do lote (código 2), e vértices cuja altura difere da altura da
ferramenta quando ela não pode aplainar ali (`FUN_009e9550`).

**Custo mostrado** (`PreviewCost`, `0x350830`): soma, para cada tile (ou
quarto) da seleção cuja altura atual difere da da ferramenta,
(`DeckFoundationCost(altura da ferramenta)` − `DeckFoundationCost(altura
atual)`) × fator (0,25 por quarto, 0,5 por metade diagonal, 1 por tile
inteiro; os preços por tile são os do §7.1: fundação 4, deck 2–3, piscina 75),
e arredonda; `CanAfford` decide o cursor.
O resto das regras (escavar piscina, telhados, aplainar terreno) não foi lido.

### 7.11 Terreno, escadas, telhados e piscinas (rodada 9, lido e inferido)

- **Pincel de terreno** (`cTSUserToolTerrainSculpt`): `ApplyTerrainSculptBrush`
  (BV `0x32a214`) roda a cada 100 ms (`OncePerFrameUpdate`). A largura do
  pincel é `W0 + (10 − W0) × 0,5 × escolha` (`SetBrushWidthChoice`, escolha
  0–2; tamanhos do cursor 2, 5,5, 10; aqui W0 = 2, inferido). O perfil é a
  imagem `profileTextureID` do `terrainSculptBrushes.txt`: TGA 32×32 cinza
  (tipo 3) no `terrain.package`, grupo `0x4B72DA28`, tipo `0x856DDBAC`.
  `RaiseBrushFn`/`LowerBrushFn`: altura nova = (altura ± perfil × força) / passo,
  arredondada para cima (subir) ou para baixo (descer), × passo; o passo é a
  vtable `0x158` do `WorldDB` (não lido; os lotes salvos têm alturas em
  múltiplos de 0,00625, que é o que usamos). `ChangeTerrainElevation` (BV
  `0x4010e0`): vértice que não pode mudar (`FUN_009e9550`) não muda;
  `UpdateFundsForHeightChange` acumula |Δ| e cobra `round(Σ|Δ| × custo por
  unidade)` − o já cobrado, recusando sem fundos; `ChangeFloorElevationCoupled`
  leva os andares de cima junto. As funções de nivelar/suavizar não têm nome
  (aqui: até a altura do início do traço / pela média dos vizinhos).
- **Pinturas**: `DiminishAllPaints` tira das outras o que a pintura ganha;
  máscaras de 4 pontos por tile (`2ARY 0x5CBC + i`, `LTTX` com a base e os
  nomes).
- **Escadarias**: as do jogo base são objetos ("Stairs - Straight - *", tipo
  de construção 1, subsort 0x20); as partes "Hi" ficam um andar acima pelo
  campo de OBJD 0xB0 "level offset" (o mesmo põe a parte da escada de piscina
  no fundo, −1). As escadas modulares (`modularstairs.txt`) são as de ligação.
- **Telhados**: o ROOF guarda id, retângulo, nível (o andar de cima das
  paredes), tipo, padrão (um só no lote), ângulo (v2) e estilo (v3). Empenas
  curtas/longas têm paredes de sótão (estilo 3) nas pontas; a regra "pontas
  não cobertas por outro telhado do andar" dá 788 das 905 paredes de sótão dos
  62 lotes (`MakeShedWalls` não foi lido).
- **Fundações**: o andar 1 fica a 0,75 acima do terreno nos vértices da
  fundação (e os de cima junto), paredes 23 na borda, piso
  "floor_plywoodFoundation".
- **Piscinas**: POOL = id, (x, y) do canto, nível 0, largura, altura, 1,
  profundidade da água (0,25), 0; o lote tem o andar −1 (elevações −3), o
  fundo tem o piso "floor_poolroundedlipblue" e as paredes 29 ficam no andar
  −1. Na colocação, tile de piscina sem od 42 0x800 → `0x8F`.
- **Rotação de piso**: 3ARY **`0x20`**, um `u16` por tile, o quarto q nos
  bits 4q..4q+3 (1/2/4/8; 0 lido como 1; `FloorPatternRotation` BV
  `0xd3c24`, `cWorldDB +0x94`). Nenhum lote do jogo o tem (o `Load` enche com
  `0x1111`). O 3ARY `0x15` é a **camada de visibilidade do piso** (§7.26;
  rodada n48: antes lido como a rotação, inferido pelos dados).

### 7.12 Escadas modulares, auto-telhado e a ferramenta Design (rodada 11, binário)

**Escadas modulares** (`cTSUserToolModularStairs`; o posicionamento é de
`TSUserModularStairsPlacement.cpp`: `UserPlaceModularStairs` = BV
`0x9edcf8`, `UserPlaceModularStairsFindEnd` = `0x9ece38`, o auto =
`0x9ef328`, sem nome no symxfer). Cada produto do `modularstairs.txt` dá
cinco objetos, nesta ordem (`produto→0xB0(i)`): 0 `landing` (patamar), 1
`placeholder` (reserva de espaço), 2 `step`, 3 `topStep`, 4 `bottomStep`
(0 = usa o `step`). Todos os produtos usam o mesmo patamar `0xAC400DE1` e
reserva `0x0C400DDD`.

- Clique no **tile de cima** (um tile de piso com borda livre): o tile tem
  de ser válido (`WorldDB→0xB8(tile, 0xF, 1)`; senão `0x6C`) e não pode ser
  água. A direção é a da borda livre do tile (byte `+0xC`: 1 → direção 2,
  2 → 0, 4 → 6, 8 → 4, as direções de 8 do mundo).
- **Sem arrastar** (`TryAutoPlaceStairs` → `FindEnd`): anda tile a tile na
  direção, andar por andar para baixo, até achar onde o último degrau toca
  o chão (para em água `0x1D`, piscina `0x1C`, parede `0x71`, objeto
  `0x74`); depois `UserPlaceModularStairs` até lá.
- **Arrastando até um ponto**: o ponto tem de estar na mesma linha ou coluna
  do tile de cima e na direção da borda livre (senão `0x71`); mesmo tile →
  `0x78`. **n degraus** = arredondado((d − 0,5) × 4 + 0,5), d = distância no
  plano do centro do tile de cima ao ponto: **4 degraus por tile, cada um
  com 0,25 de avanço**; com a opção de tiles inteiros (`param 3`) n sobe ao
  múltiplo de 4. Cada degrau desce `WorldDB→0x158` (altura de um degrau; o
  andar inteiro em 4 tiles, inferido), começando na altura do piso de cima
  (+0,75 se o tile de cima é de fundação).
- Por degrau: se o terreno fica a menos de 0,75 abaixo e há deck ali →
  `0x78`; se fica acima do degrau (folga −0,09375) o **terreno é rebaixado**
  (`ChangeFloorAndWaterElevationCoupled` nos 4 vértices do quarto de tile,
  na altura do degrau; vértice travado → `0x6F`/`0x75`); no último degrau os
  vértices do tile seguinte vão à altura final. Objetos: o `topStep` no
  primeiro, `step` no meio, `bottomStep` no último (falha: `0x73`/`0x74`);
  a cada andar atravessado, no degrau de índice 2 do tile, um `placeholder`
  em cada andar entre o de baixo e o de cima (falha: `0x72`).
- **Patamares**: um `landing` no tile de cima (direção da escada; falha
  `0x70`), outro meio tile além do último degrau, virado 180°
  (`(dir + 4) % 8`; falha `0x71`). O tile de baixo tem de ser válido e sem
  piso alto (`0x6D`), o último degrau a no máximo 0,09375 do chão (senão
  `0x77` "Can't reach ground"), sem parede no caminho (`0x76`), e o tile de
  baixo nivelável (`0x6F`).
- Os objetos vão para um `cTSUserToolUndoableModularStairs` (`AddObject`
  BV `0x3ffc28`: põe o od 13 e o od 41 do objeto, testa
  `TSObjectUserCanSetPositionAndOrientation`, gira e posiciona) e o
  `LinkObjects` (`0x3ff4a4`) liga cada objeto ao anterior e ao seguinte (os
  IDs vizinhos, −1 nas pontas) — é a cadeia que a VM e o roteamento usam.
  Apagar (marreta `DeleteModularStairs` → `UserRemoveModularStairs`, BV
  `0x9ebd54`: se `TSObjectGroupUserCanDelete(grupo)`, chama
  `RemoveFlight(grupo)`, BV `0x3fff3c`, vtable `0x70` do undoable) tira a
  escada inteira pelo grupo: para cada objeto do grupo cria a
  suboperação de apagar objeto (a devolução normal do objeto) e, se o
  **modelo do corrimão** do objeto (`ObjectModel`, BV `0x3ff17c`: o dado 0
  do objeto depois da mensagem `0x30`) é > 1, **soma à devolução o preço do
  corrimão** (o produto `0x2C7BF690`, vtable `0x5c`): 2× se o modelo é 4
  (corrimão dos dois lados), 1× nos modelos 2–3, nada nos 0–1 — o mesmo
  `RailingCost` do lote (`docs/neighborhood-mac.md`). (Corrige o "2× se o
  grupo tem 4 partes" da rodada 11.) O custo mostrado é
  `CalculatePreviewCost`.

**Auto-telhado** (`cTSUserToolAutoRoof::CreateAutoRoof(tipo, ...)`, BV
`0x2560ac` → `CreateAutoRoofs(andar, tipo, ...)` `0x9a0df0` para cada andar
1…topo):

1. Marca numa grade os quartos de tile dos **cômodos do andar de baixo**
   (`AppendRoomTilesIfValid`, `0x99947c`) que são dentro, cujo tile acima
   não é dentro, sem piso no andar e sem telhado já cobrindo
   (`RoofManager→0x7C`).
2. Tipo 5 (mansarda): só **põe o piso do telhado** (`roofFloorPattern` do
   `roofs.txt`, `CCC26098`) em cada tile marcado (`0x99977c`).
3. Outros: `cAutoRoofData::ComputeRoofRects` (`0x9a0534`) decompõe a grade
   em retângulos por varredura: em cada linha acha os trechos marcados;
   `AddToActiveRects` estende para baixo o retângulo ativo com o mesmo
   trecho x, corta os que se sobrepõem em parte; os que não continuam saem
   da lista ativa. `0x9a0a08` (com 2,0) tira os retângulos redundantes:
   conta a cobertura de cada célula e descarta um retângulo maior que 2,0
   cujas células todas continuam cobertas por outros.
4. Para cada retângulo de área > 0, o tipo do ROOF sai de `0x997f8c(tipo
   da ferramenta, retângulo)`: 1 → 5; 2 → 4; 3 → 5 se altura ≤ largura,
   senão 4; 4 → 3 se altura < largura, senão 4; outro → 0 (tipos do ROOF:
   tabela abaixo). Cria o telhado
   (`cTSUserToolUndoableRoof→0x6C(padrão, mín, máx, tipo)`) no andar; os
   tipos 3 e 4 (empenas) ganham as **paredes de sótão** nas pontas
   (`0x9990f4` → `MakeShedWalls`).
5. Antes, sem a flag "manter", remove os telhados auto anteriores
   (`0x99eafc`); custo total no fim, recusado sem fundos.

**Botões do painel de auto-telhado** (`cTSWinProcBuildPanel::
onBtnClickedID`, BV `0x287f18`…): cada botão manda à ferramenta o valor
`2·tipo + (C400 marcado)`, e o `cTSUserToolAutoRoof::Init` (BV `0x2571c4`)
usa `tipo = valor >> 1`, "manter os telhados" = bit 0 (valor 0 →
`RemoveAllRoofs`, `0xDEADBEEF` → nada). Conferido no código: **C100 → 1
(tudo de quatro águas, ROOF 5), C200 → 2 (tudo empena, ROOF 4), C500 → 3
(quatro águas onde altura ≤ largura, senão empena), C600 → 4 (empena curta
ROOF 3 onde altura < largura, senão empena longa ROOF 4), C700 → 5 (laje:
só o piso de telhado), C300 → 0 (remover)**, C400 = a caixa "preservar".
É o mapeamento que o `build_mode_panel.cpp` já usa.

**Tipos do ROOF** (`nTSSG::cRoofManager::AddRoof(id, mín, máx, tipo)`, BV
`0x6bf488`; classes conferidas pelo `ComputeGridElevation` que cada
construtor chama, porque o symxfer troca alguns nomes de construtor):

| tipo | classe | `roofs.txt` / nome no catálogo (STR# 0x8C do `Build.package`) |
|---|---|---|
| 1 | `cHalfGableRoofType` | produto 0 `roof-ui-gable-shed`, "Shed Gabled Roof" |
| 2 | `cHalfHippedRoofType` | produto 1 `roof-ui-hipped-shed`, "Shed Hipped Roof" |
| 3 | `cTallGableRoofType` | produto 2 `roof-ui-gable-tall`, "Short Gabled Roof" (cumeeira no eixo curto) |
| 4 | `cGableRoofType` | produto 3 `roof-ui-gable`, "Long Gabled Roof" (cumeeira no eixo longo) |
| 5 | `cHippedRoofType` | produto 4 `roof-ui-hipped`, "Hipped Roof" |
| 6 | `cMansardRoofType` | produto 5 `roof-ui-mansard`, "Mansard Roof" (tem `roofFloorPattern`) |
| 7 / 8 / 9 | cônico / cúpula / octogonal | (não estão no `roofs.txt` do base) |
| 10–14 | diagonais: empena, empena alta, quatro águas, meia-empena, meia-quatro-águas | `IsDiagonalRoofType` (BV `0x163c6c`): máscara `0x387C00` = 10–14 e 19–21 |
| 15 | `cGlassGableRoofType` (estufa) | `IsGlassRoofType` (`0x163c5c`) |
| 16–18 | pagode: quatro águas, empena, meia-empena | |
| 19–21 | pagode diagonal: quatro águas, empena, meia-empena | |
| 22 | octogonal (outra variante, mesmo construtor do 9) | |

O produto *k* do `roofs.txt` é o tipo *k* + 1 (inferido da ordem:
catálogo e `AddRoof` batem um a um, e o produto 5, a mansarda, é o único
com `roofFloorPattern`). Os lotes do base só usam 1–6.

**Águas-furtadas** (roofs 6–11 do `roofs.txt`: 1, 2 e 3 de largura, com
empena ou de quatro águas) são produtos de telhado comuns
(`cTSWinProcCatalog::LoadDormers`); diferente dos telhados normais, têm
preço (§7.9).

**Ferramenta Design** (botão `0xCC607030` do modo compra;
`cTSUserToolObjectDesign`, BV `0x3022fc…`): cursor `0x39`. Botão apertado
sobre um objeto: se ele **não tem partes recoloríveis** → erro (STR# 0x8D
#16 do grupo `0x7FA7DC7C`); se não há dinheiro para o **preço da
ferramenta** (o produto do catálogo da ferramenta, `+0x4C`, lido no
`Init`) → erro #15. Textos (rodada 12, `Text/UIText.package`, STR# 0x8D, grupo local
= `0x7FA7DC7C`; os números são 1-based como os do código, entradas 14 e 15
do dump): #15 "Insufficient Funds!", #16 "This object is not designable."
(o comentário da EA: "message appears when trying to use design mode tool
on non-designable object"). Soltar sobre o mesmo objeto abre o
`cTSWinProcObjectDesignDialog::Show(objeto, preço, x, y)` (BV `0xa16e64`).

- "Recolorível" (`nTSSG::GetDesignModeInfo`, BV `0x9e2904`): percorre os
  shapes do modelo; um subconjunto (grupo de material) entra se a
  propriedade **`tsDesignModeEnabled`** do shape lista o nome dele e o
  nome não contém "shadow"; o primeiro vira o **material 1** e o segundo
  (de nome diferente) o **material 2**; para cada um, o nome da malha de
  materiais vem da propriedade **`tsMaterialsMeshName`** do shape (+
  `_cres`). No máximo dois.
- O diálogo: título = STR# 0x93 #0x15 com o nome do objeto (`sprintf`),
  nome em `0xA3000`, duas grades de opções (controles `0xA1000`/`0xA2000`;
  a segunda some se o objeto só tem um material), carregadas pela malha de
  materiais e o subconjunto (`→0x28(objeto, malha, subconjunto)`); a
  janela é arrastável pela barra `0xA7000`. **Cada clique numa opção
  (comando `0xEC58BA86`) já aplica no objeto** (`ApplyChanges`: os dois
  materiais escolhidos como override, `objeto→0x478`, e redesenho
  `→0x490(1)`): pré-visualização ao vivo; `0xA6002` (aceitar) fica
  habilitado depois de uma mudança.
- Aceito: um `cTSUserToolUndoableObjectDesign` (`0x8D0FA620`) guarda os
  overrides antigos (`SetObject`: a lista `objeto→0x47C/0x480`) e os novos
  (`ApplyMaterialOverride`), cobra **−preço** dos fundos no ponto do
  clique e entra no desfazer; cancelado, volta os materiais
  (`ResetChanges`). O objeto com o override salva o material escolhido
  (lista de overrides do objeto; ver `docs/save-mac.md`).

### 7.13 Escadas modulares no código (rodada 12, `vm/build_edit_stairs.cpp` `placeStairs`)

Lido de `UserPlaceModularStairs` (BV `0x9edcf8`), `FindEnd` (`0x9ece38`),
`CalculatePreviewCost`, `AddObject`, `LinkObjects` e `RemoveFlight`, e
conferido com os lances salvos (`N001_Lot38`: refazer o lance dá os mesmos
objetos, níveis, elevações, direções e elos):

- Constantes: degrau = 0,25 de avanço e 0,1875 de altura (`WorldDB→0x158`;
  inferido dos lotes); tolerâncias `0x274b870` = 0,09375 (último degrau até o
  chão), `0x274bbb8` = −0,09375 (terreno acima do degrau → cavar),
  `0x274b474` = 0,001 (regra do deck).
- n degraus = ceil((d − 0,5) × 4), d = distância do centro do tile de cima ao
  ponto do arraste (o truque de conversão `+ 1,5·2²³` com o ajuste de inteiro
  exato do código); a prévia de custo usa round e multiplica pelo preço **por
  degrau** do produto (STR# 139 3i; o PT-BR deixa vazio o preço de DeFumê e
  Forja: vale o inglês, 80 e 85).
- O ponto do arraste é onde fica o **último** degrau. O degrau i fica em
  borda + u × 0,25 × (i + 1), z = topo − 0,1875 × (i + 1); o de cima está no
  nível do tile de cima, os outros no nível de baixo; um `placeholder` no
  degrau de índice 2 de cada tile, em cada nível de baixo+1 até o de cima;
  patamar no centro do tile de cima (direção da escada) e meio tile depois do
  último degrau (direção + 4).
- Cavar: se o terreno a ±0,5 do degrau (de lado) está mais de 0,09375 acima
  dele, os cantos do tile do degrau descem: os de baixo à altura do último
  degrau do tile, os de cima (salvo k = 0) à do primeiro. O tile depois do
  último degrau vai **sempre** à altura dele (também sobe: um arraste curto
  ergue o chão). Vértice preso → `0x6F`.
- Elos: `LinkObjects` só liga os degraus (lista `+0x68`): od **80 = próximo,
  81 = anterior** (−1 nas pontas); patamares e placeholders ficam na lista
  `+0x74`, sem elos. O degrau de cima é XMTO. Valor (od 41) dos degraus = o
  preço por degrau (os salvos têm 40 no Madeira); a marreta devolve o od 41.
  `RemoveFlight` soma o preço do corrimão (`0x2C7BF690`): 2× com o modelo de
  corrimão 4, 1× com 2–3, nada com 0–1 (rodada 13: feito, ver §7.13.1).
- Estado salvo: a "elevation" do degrau é a altura sobre o piso do nível de
  baixo, também a do degrau de cima (inferido: o terreno é 0 sob todos os
  lances salvos); patamares e placeholders 0.
- Inferido aqui: os pisos do andar de cima sobre os degraus são tirados (como
  a escadaria-objeto); a parede no caminho conta só paredes e cercas (as bordas
  de fundação/deck não); objetos que bloqueiam = camada de piso (od 63 0 ou
  bit 1; janelas não); a direção sem arraste = o lado do tile mais perto do
  clique (o quarto do `cFloorTile`); a busca do fim (`FindEnd`): ver
  §7.13.2 (rodada 14).
- `FUN_009e9550` (pode o vértice ir a essa altura?) tem uma lista de testes por
  máscara: limites de altura do terreno `WorldDB→0x150/0x14C` (`0x65`/`0x66`;
  rodada 13, `FUN_009e7700`: `MinTerrainElevation` = −150 ×
  `FloorElevationUnit` e `MaxTerrainElevation` = +150 ×
  `FloorElevationUnit`, com a unidade = **0,1875** (`cWorldDB +0x44`, posta no
  construtor `FUN_000d4c98`), ou seja **−28,125 / +28,125**; altura <
  mínimo → `0x65` "Can't lower terrain further", > máximo → `0x66` "Can't
  raise terrain further"; o mesmo construtor põe `+0x3c` = 4, `+0x40` = 5),
  folga para o andar de baixo/de cima (`0x69`/`0x6A`), objetos que prendem o
  chão (`0x61`), objetos de teto (`0x60`), praia. Aqui: "livre de piso,
  parede e fundação" e, desde a rodada 13, os limites de altura
  (`terrainLimitError`): o pincel de terreno e de água deixa o ponto onde está
  e dá `0x65`/`0x66` quando nenhum se move, as escadas modulares recusam com
  `0x65`/`0x66` em vez de `0x6F`, e o nivelamento das paredes, fundações e
  piscinas não move um ponto além deles.

### 7.13.2 `FindEnd` no binário (rodada 14, BV `0x9ece38`)

Sem arraste, `UserPlaceModularStairs` pede o ponto final ao `FindEnd`
(argumentos: o tile de cima com o byte de direção — 1 −x, 2 +y, 4 +x, 8 −y —,
a saída, o undoable e a lista de erros) e conta os degraus dele como de um
arraste (ceil((d − 0,5) × 4)). Ordem:

1. tile de cima sem piso (`WorldDB 0xb8(tile, 0xF, 1)`) → `0x6C`; água →
   `0x1D`; não plano → nivela (`FUN_009ec428` modo 0) ou `0x6E`; senão a
   altura de cima = `SampledFloorElevation` do tile (+0,75 se o tile é de
   fundação, `IsDeckFoundationOfType 4`; não feito: no opensims2 o piso da
   fundação já está no nível 1); o objeto de cima não cabe → `0x70`; parede
   → `0x76`; o tile seguinte fora do lote → `0x74`.
2. Para cada nível do de cima ao mínimo (`WorldDB 0x140`), z = a altura de
   cima, tile t = o seguinte; enquanto t está no lote: depois = z − 4 ×
   `WorldDB 0x158` (0,1875 → 0,75 por tile); água → pendente `0x1D`, piscina
   → `0x1C`, o degrau não cabe (`FUN_009ebdc8`) → `0x71`, e acaba o nível;
   com piso no nível: os cantos do tile girados pela direção
   (`FUN_009ec03c`: perto e longe), altura de cada canto = elevação do
   vértice (+0,75 se encosta numa fundação, `FUN_00164a7c`); perto = z
   depois que um tile anterior foi "cavado"; longe = o maior dos dois, salvo
   se ele pode descer até "depois" (`FUN_009e9550`, que devolve sim logo se
   o vértice já está nessa altura), então o menor. O piso **encontra** o
   lance no tile se um canto de perto está abaixo de z + 0,001 e o longe
   acima de depois − 0,001:
   - no primeiro tile: os dois cantos de longe podem descer → segue
     ("cavado"); senão `0x78`;
   - tile não plano: nivela (modo 1) ou, se os cantos de longe podem descer,
     segue; senão pendente `0x6F` e acaba o nível;
   - senão **fim** = centro do tile + 0,5·u − k·0,25·u com k =
     round((longe − depois) / 0,1875) (truque `+1,5·2²³`), no máximo 4; se o
     longe está abaixo dos dois de perto (o piso cai dentro do tile), fim =
     a borda de longe; o tile meio tile depois do fim tem de receber o
     patamar: fora do lote/nivelar (modo 3)/o objeto de cima não cabe →
     `0x71`, água `0x1D`, piscina `0x1C`.
   Sem fim no tile: pendente `0x77` (se não havia outro), z = depois, t
   avança.
3. Sem fim em nenhum nível: o primeiro pendente.

No código (`planStairs` sem `end`): isso, com o degrau que não cabe = parede
no caminho ou objeto na camada de piso (inferido) e os vértices de andares
de cima nunca "cavados" (inferido); o fim alimenta a mesma conta do
arraste. `N001_Lot38 --delete-stairs 147 --stairs 0,18,16,2,0 --check` refaz o
lance salvo (4 degraus); `test_build` (terreno erguido: 8 degraus; chão a
4 degraus do topo: o primeiro tile segue, 4).

### 7.13.1 O modelo do corrimão e a ferramenta de corrimão (rodada 13, binário)

O "modelo do corrimão" não é um campo do objeto: `RailingObjectModel` (BV
`0x523d10`, igual ao `0x3ff17c` do undoable das escadas) roda a **entrada 48
do OBJf** (`cTSObject::RunTree(0x30, …)`, vtable `0x3b4`) e, se ela devolve
verdadeiro, lê o **temp 0** do objeto (vtable `0x4c` = `GetTemp`, os 8 temps
em `+0x8a`; o vizinho `0x60` é `SetTemp`); senão 1. Nas escadas modulares a
entrada 48 é o semiglobal `0x2018` "Function - Get Model Type" do
`StairModularGlobals`: gráfico (od 0) = 0 → 1, senão (gráfico − campo `0x48`
do OBJD) / 3 + 1. A entrada 47 (`0x2001` "Function - Set Model by Type",
parâmetro 0 = modelo 1–4) põe gráfico = campo `0x48` + 3·(modelo − 1): o
gráfico é o índice no STR# `0x85` (1–3 sem corrimão, 4–6 "Left Railing",
7–9 "Right Railing", 10–12 "Both Railing"; meio, base e topo). Mudar o od 0 é
o "change graphic" do motor (erro `0x48` da VM): o modelo 3D passa a ser o
dessa entrada. `RailingCost` (BV `0x523da4`): 0/1 → 0, 2–3 → 1×, 4 → 2× o
preço do produto `0x2C7BF690` = a "Railing Tool" do `userToolProxies.txt`
(STR# 139 entrada 0: §3). `ComputeStats` o soma ao valor das escadas (tipos
0xB/0xC) e `RemoveFlight` à devolução de cada parte.

**Ferramenta de corrimão** (`cTSUserToolStairsRailing`, botão B620, ferramenta
`0xF`; `Init` lê o preço do produto `0x2C7BF690`, cursor `0x3A`):
`OnAnyMouseEvent` acha o objeto sob o mouse; tipos 10–12 (escadaria-objeto,
degrau modular, portal) e o lado = `PointIsOnLeftSideOfObject` (para o tipo 10,
o contrário). `ChangeRailings`: se o objeto tem grupo (lance), todos os do grupo
de tipo 0xB–0xC; senão só ele. `ChangeOneRailing`: modelo atual m
(`ObjectModel`: o temp 0 qualquer que seja o retorno; 0 → 4 no tipo 10, senão 1);
pôr (sem Ctrl): m = 1 → 2 (lado falso) ou 3 (verdadeiro); m = o outro lado → 4;
já tem → nada; tirar (Ctrl, preço negativo): m = 4 → 3 (lado falso) ou 2
(verdadeiro); m = o do lado → 1. Cada troca é um `TrySwapModel` (roda a
entrada 47 com o modelo novo e soma o preço ao custo do undoable). Os lances
dos lotes salvos têm gráfico 0 com modelos de corrimão no cObject (feitos antes
desse mecanismo): para o jogo são modelo 1 (sem devolução de corrimão).

No código (`vm/placement` `railingObjectModel`/`railingCost`/`setRailingModel`,
`BuildEditor::changeRailing`, `VM::railingPrice`): a marreta devolve o
corrimão, o valor do lote o conta, a ferramenta de corrimão (B620 no app,
headless `--railing id,lado[,delete]` no app e no `lot-save`) põe e tira com
desfazer; o app troca o modelo quando o od 0 muda durante a sessão
(`VmObjectView::updateGraphic`) e o `lot-save` grava o modelo do gráfico num
cObject novo. Inferido: o lado é o do eixo da direção do objeto (o binário
compara x ou y conforme a direção, nos eixos dele), e o cObject já salvo de um
degrau não troca o nome do modelo (o od 0 vai no estado).
`docs/screenshots/r13-construcao-corrimao.jpg` (antes/depois: corrimão dos
dois lados).

### 7.14 Águas-furtadas no código (rodada 12, `placeDormer`)

`cTSUserToolRoof`: o estilo da ferramenta (`+0x88`, o id do produto do
`roofs.txt`) é água-furtada se `FUN_00995400` (= `IsStyleDormer`) diz que sim
(6–11). `Init` põe como cursor o modelo do `defaultRoofPattern`
("dormerpreview-2wide") deslocado de largura × 0,5 e liga as teclas de girar;
no soltar, `FUN_001d5d20` (= `MakeDormer`):

- largura w = (estilo − 6) % 3 + 1, fundo w + 2; nível = o do local clicado
  (o sótão); se o de cima não existe é criado (WorldDB `0x1C`), acima do topo
  → `0x06`;
- direção (`+0xB4`): 6 (padrão) w × fundo em y × x = fundo × w; 2 igual
  começando `fundo` tiles a −x; 0 = w em x × fundo em y; 4 igual começando
  `fundo` a −y;
- `FUN_001d5a5c` testa as 4 linhas (vértice com parede → `0x08`;
  `UserMayPlacePartition`), `FUN_00998cd8` põe paredes **estilo 1** nas 4
  linhas no nível do sótão (dir 0 anda +y, 6 anda +x) e depois as paredes de
  sótão (estilo 3) nas pontas curtas;
- `UserMayAddRoof(ângulo, canto, canto oposto, tipo)` no nível de cima: tipo
  **4** (empena longa) para 6–8, **5** (quatro águas) para 9–11.

Não há "água-furtada" no lote salvo: ela vira paredes e um ROOF comum (nenhum
dos 62 lotes tem estilo ≠ 0). O preço do catálogo (560/840/1120) é igual a
8/12/16 trechos × §70: as paredes entram como pré/pós-operações do undoable do
telhado e o `TotalCost` soma o custo delas (o telhado é de graça); aqui cada
trecho novo custa §70. Inferido: o teste de suporte da parede fica de fora
(a água-furtada fica sobre o telhado), o topo é o nível 4 do lote, e o canto é
o ponto de grade sob o mouse (o jogo usa o cursor deslocado).

### 7.14.1 Miniaturas das escadas modulares e das águas-furtadas (rodada 13, binário)

O cache de miniaturas não as tem: o jogo as renderiza. `cTSUIThumbnailFactory
ModularStairs::SetupScene` (BV `0x426014`, vtable `0x50`; cache
"UIModularStairThumbnails") carrega três modelos, o do `topStep`, o do `step` e
o do `bottomStep` (o do `step` quando vazio), cada um num nó de transformação
com a translação (0, 0,25·i, −0,1875·i) (`0x274b54c` = 0,25, `0x274b7a8` =
0,1875): três degraus descendo. A câmera é fixa (rotação 2,199 rad e escala
1,25, não decodificadas). `cTSUIThumbnailFactoryDormer::SetupScene` (BV
`0x3773ec`, "UIDormerThumbnails") carrega o modelo do produto (o
`defaultRoofPattern`: "dormerpreview-2wide", "dormerhippreview-1wide", …) com
a câmera a 115° (90° em outro caso, `FUN_009d385c`). No app
(`BuildMode::stairsThumbnail`/`dormerThumbnail`, `renderPartsThumbnail`): os
mesmos modelos e translações, desenhados como as miniaturas da decoração do
bairro (de sudeste e de cima); o enquadramento corta a perna de um andar dos
degraus (no máximo 1,2 × a largura de altura, inferido).
`docs/screenshots/r13-construcao-miniaturas-escadas-aguas-furtadas.jpg`.

### 7.15 Retângulos do auto-telhado (rodada 12, `autoRoofRects`)

Lido no código (`ComputeRoofRects` `0x9a0534`, `AddToActiveRects`
`0x99dd80`, `MoveInactiveRects`, `0x9a0a08`, `FUN_01969950`):
`cRoofRect` = [x0, x1) × [y0, y1) + a linha em que cresceu por último; os
ativos num `set` (`cRectLessThan`). Para cada trecho marcado da linha y:
todo ativo **contido** no trecho cresce uma linha; todo ativo que só
**cruza ou encosta** (x0 ≤ fim e início ≤ x1) dá um retângulo novo com a
interseção em x, do topo dele até esta linha (se nenhum que cresceu tem o
mesmo x0/x1); se sobra célula do trecho em nenhum dos dois, o trecho inteiro
entra como ativo. Depois da linha, os ativos que não cresceram nela vão para a
lista final. Ou seja, os retângulos **se sobrepõem** (um L dá o braço largo e
o estreito do topo até o fim). Depois, com 2,0: a cobertura de cada célula
é contada sobre todos; a lista é ordenada pela razão de aspecto
(max(l/a, a/l), lados ≥ 1) decrescente e percorrida da frente: um retângulo
com razão ≤ 2 fica; um mais alongado tem a cobertura das suas células
diminuída e só fica se alguma célula chega a < 1.

### 7.16 Telhados diagonais (rodada 12)

`cTSUserToolRoof::MakeRoof` (BV `0x1e0480`) escolhe a geometria pelo
**estilo** da ferramenta (o id do produto do `roofs.txt`): 0–5 → tipos 1–6;
`0x0C`/`0x11` cônico, `0x0D` cúpula, `0x0E–0x10` octogonal; `0x12–0x16` →
diagonais 10–14 (`DiagonalRoofFromType`, `IsDiagonalRoofStyle`); `0x17–0x19`
vidro; `0x1A–0x20` pagode. O `roofs.txt` do jogo base só tem os produtos 0–11:
**no jogo base não há como fazer telhado diagonal** (nem o auto-telhado: a
grade dele é de tiles inteiros). Aqui a ferramenta aceita os tipos 10–14 só
pelo headless (`--roof x0,y0,x1,y1,10..14`: arrastar de um canto do losango
ao oposto), com a geometria do OpenTS2 que já desenhava os tipos; as paredes
de sótão diagonais (`CheckWallLegalityDiagonal`) não são feitas.

### 7.17 Lagos: a ferramenta de água (rodada 12)

`cTSUserToolWater` (BV `0x1e600c…`) é um pincel de terreno: só no térreo
(senão `0x8B`), aplicado a cada 100 ms com o botão apertado
(`OncePerFrameUpdate` → `ApplyWaterBrush`, temporizador + 100), largura
`W0 + (10 − W0) × 0,5 × escolha` e cursor 2/5,5/10 (`DAT_02732980`), perfil
`spWaterBrushProfile` (TGA `0x8B731811`). `WaterBrushFn` (`0x1e61a0`):
altura nova = floor((h − (perfil − limiar) × força) / passo) × passo, com o
contexto do `InitBrushContext` (limiar 0, força 1e-5) — **cava**;
`cTSUserToolUndoableWater::ChangeTerrainHt` testa o vértice
(`FUN_009e9550`), move o terreno (`ChangeFloorElevationCoupled`) e anota o
vértice; `UpdateFundsForVertexEdit` cobra pelo preço do catálogo (STR# 133:
§1). Depois de cada aplicação `AdjustWaterElevations` →
`cWorldTerrain::UpdateWaterElevations` (`0x8b185c`): os vértices anotados mais
a água já ligada a eles (8 vizinhos, `DAT_02737e00/20`);
`ChooseWaterElevationsForPondWithVertex` (`0x8b11ec`) inunda cada grupo e põe a
água 0,25 (`DAT_0277a578`) abaixo do menor terreno da borda (vizinhos fora
do grupo); vértice com terreno mais de 0,25 acima dessa água fica seco
(água = terreno − 1,0, `DAT_0277a57c`); `AdjustWaterElevationsAtNonPondVertices`
(`0x8afbe4`): o seco ao lado de um lago fica com a água do vizinho − 0,1
(`0x274b680`), os outros com terreno − 1,0. A água é o 2ARY `0x3B76`
([y][x], (L+1) × (A+1)), agora gravado (os 62 lotes voltam iguais).
Inferido: o botão da ferramenta no painel (B740); o que `UpdatePondFlag` faz
além de marcar o lago.

### 7.18 Piscinas que não são retângulos (rodada 12)

No jogo base a piscina é um tipo do `cTSUserToolDeckFoundation` (tile a
tile, `CanPlaceTile`, o estilo por tile no `WorldDB::DeckFoundationStyle`);
as piscinas curvas (`cTSUserToolCurvedPool`) são só BV. Os 7 POOL dos lotes
salvos são todos retângulos. Aqui: um retângulo arrastado sobre ou junto de
uma piscina a **estende** (os tiles que já são piscina ficam e não se pagam;
o chão vai à altura da piscina), as paredes de piscina seguem a borda da união
e os tiles novos viram retângulos POOL sem sobreposição (linhas de mesmo
trecho juntadas), para a água cobrir só os tiles; Ctrl apaga a piscina inteira
(os POOL ligados por uma aresta). Inferido: o POOL de uma piscina em L no jogo
(nenhum lote salvo tem); a recusa `0x86` só vale para deck/fundação junto de
piscina (não feita).

**Rodada 13 (binário): como o jogo guarda os POOL.** O `nTSWorld::cWorldPool`
(vtable `0x281b788`) guarda as piscinas num hash de `tPoolObject` (id, x, y
como float em `+0xc/+0x10`, nível, largura e altura inteiras em `+0x18/+0x1c`,
mais dois floats); o `Save` (BV `0xc21154`, vtable `0x58`) escreve versão,
1, o próximo id e **uma entrada por `tPoolObject`** (id, x, y, nível,
largura, altura, `+0x20`, `+0x24`, `+0x28`): o formato que o opensims2 lê. O
`AddPool` (vtable `0x30`, BV `0xc21984`) recebe dois cantos e cria **um**
retângulo (e o `cTSUserToolUndoablePool::TryAddPool` só o chama). Os testes do
gerente ao pôr um retângulo R:
- vtable `0x44` (`0xc22068`): as piscinas que R cruza; `0x40` (`0xc21d24`):
  as que só encostam (caixa ±0,01);
- vtable `0x48` (`0xc22358`): falso se R cruza uma piscina P **sem a
  atravessar** (a interseção não tem nem a largura nem a altura de P) ou se
  a interseção fica a exatamente **1 tile** de uma borda de P (sobraria uma
  faixa de 1 tile);
- vtable `0x4c` (`0xc22810`): para cada P cruzada, os pedaços de P fora de R
  (um ou dois retângulos no eixo em que R a atravessa);
- vtable `0x50` (`0xc22dfc`): falso se a área de R mais a das piscinas que ele
  toca passa de **900** ou se R com uma delas não forma um retângulo (uso
  não achado: talvez a junção em um só retângulo).
Ou seja, uma piscina em L ou U são **vários POOL que se tocam, sem
sobreposição**: o retângulo novo inteiro e o que sobra dos antigos. No código
(`drawPool`, rodada 13): R cruzando uma piscina sem atravessá-la ou deixando
uma faixa de 1 tile → `0x86` "Can't extend pool" (o código é inferido: quem
chama o `0x48` não foi achado); senão as piscinas cruzadas viram os pedaços
de fora e R entra inteiro (antes eram linhas juntadas dos tiles novos). Não
feito: o limite de 900 e a junção do `0x50`. `lot-save N001_Lot38 --pool
2,2,3,7 --pool 2,2,7,3 --pool 6,2,7,7 --check` volta igual;
`docs/screenshots/r13-construcao-piscina-em-u.jpg`.

**Rodada 14 (binário + `baserom.sym`): os nomes certos dos slots.** O
`baserom.sym` dá os nomes: a vtable `0x48` (primária; `0x38` na interface
`cIWorldPool`, `+8`) é **`IsQuadLegalForPoolErase`** e a `0x4c` (`0x3c`)
**`QuadListOnPoolErase`**: são da **borracha** (Ctrl-arrastar), não de pôr.
A `0x50` (`0xc22dfc`, `0x40` na interface) é
**`cWorldPool::AreExpansionBoundsLegal(v0, v1)`**: R = o retângulo dos dois
vértices; para cada piscina P cuja caixa cruza R crescido de **0,01**
(`FLOAT_0274b530`; encostar conta, inclusive pela quina), soma a área de P e
exige que a caixa de R ∪ P tenha área = área(R) + área(P) (sem sobrepor e
com um lado inteiro em comum: os dois formam **um retângulo**), senão falso;
no fim, falso se a soma (R + as tocadas) passa de **900** (`FLOAT_0274bc2c`
= `0x44610000`). O `cTSUserToolUndoablePool::TryAddPool` (vtable `0x6c`, BV
`0x309c44`; a piscina vem de `FUN_000c8190()` vtable `0x8c`) só chama o
`AddPool` (vtable `0x20` da interface) e guarda a troca; o
`TryRemovePool` (vtable `0x70`, `0x309ac8`) o `RemovePool`. Quem chama o
`AreExpansionBoundsLegal` não foi achado (chamada virtual pela interface).
Ou seja, **no jogo uma piscina só cresce em retângulo**: um arraste que
sobrepõe uma piscina, ou que encosta nela sem formar um retângulo (um L),
é recusado; o que encosta ao longo de um lado inteiro vira um POOL próprio
ao lado (os dois juntos são um retângulo); 900 tiles no máximo. A borracha
corta: atravessando (`IsQuadLegalForPoolErase`) e sem deixar faixa de 1 tile;
o que sobra de cada piscina são os pedaços de fora (`QuadListOnPoolErase`).

No código (rodada 14): `drawPool` testa o `AreExpansionBoundsLegal` (`0x86`
"Can't extend pool", código inferido) e põe R como um POOL novo;
`erasePool` (Ctrl-arrastar no app, headless `--erase-pool x0,y0,x1,y1` no
app e no `lot-save`) faz a borracha (`0x87` "Can't delete pool", código
inferido; Sim dentro `0x2D`), devolvendo int(75 × 0,8) por tile;
`deletePool` (clique, `--delete-pool`) continua tirando a piscina inteira
(inferido). A piscina em U da rodada 13 não se faz mais (era o corte da
borracha usado ao pôr). Visual: a parede de piscina não desenha a face
virada para a terra, e com um nível sob o chão o lote ganha lados de terra
nas bordas (o fundo da piscina aparecia além da borda do lote como uma
caixa ciano; inferido, o jogo tem o `cLotSkirt` do bairro em volta).
Rodada 20 (`r20-underground`): os lados de terra valem para todo lote
(`lotSkirtBottom`: 1,5 tile sob o ponto mais baixo do terreno, ou 0,5 sob o
piso da piscina se for mais fundo) e o renderizador corta tudo abaixo disso
(`Renderer::setGroundClip`, segundo `clip_distance` do `vertex_main`). O
jogo deixa coisa sob o chão que o terreno do bairro esconde: o
`deckSupport` (objeto "Column - Square Brick") desce 22,5 tiles por
desenho, para sempre alcançar o terreno, e o `N002_Lot23` tem uma escada
modular verde enterrada até z −2,6 (já no lote de fábrica); de lado, sem o
bairro em volta, apareciam como postes compridos e triângulos escuros sob o
lote. Visual, inferido. `--tilt <graus>` abaixa a câmera nas capturas sem
tela; `docs/screenshots/r20-underground-*.jpg` (antes × depois).
`lot-save N001_Lot38 --build-funds 20000 --pool 2,2,3,7 --pool 4,2,5,7
--erase-pool 2,5,5,5 --check` volta igual (`--pool 4,2,7,3` entre eles é
recusado); `docs/screenshots/r14-construcao-piscina-borda.jpg` (antes ×
depois).

**Rodada 15 (front "engine-re", binário): os códigos de recusa da piscina.**
O BV não tem `cTSUserToolPool`: a piscina retangular é o
`cTSUserToolDeckFoundation` com o byte **`+0xbd` = piscina** (`+0xbe`
palco, `+0xbc` fundação; `+0x80` = 0 retângulo, 1 losango; `+0x84` 0/2
pôr, 1/3 apagar). Os cantos (vértices) ficam em `+0xc4` (x, y, nível) e
`+0xd0`; o ponto onde o arraste começou em `+0x8c`. O `OnDrag` (BV
`0x355b94`) só arredonda o ponto (`floor(x + 0,5)`, `FUN_0015dc5c`) e o
`PreviewQuadrilateral` (BV `0x3551b0`) faz o retângulo (mín./máx. com o
começo) **sem prender nada ao lote**; ele chama o `MayPlaceQuadrilateral`
(ou, apagando, o `FUN_00351104`) com uma lista de erros, pinta o cursor
`DeckFoundationPreviewNotPlaceable` e mostra os erros (`ShowErrors`) quando
falha; o `OnRelease` só cria se o último preview passou (`+0xbb`). Ou seja,
**um canto fora do lote é recusado, não preso** (o r15-pool-fixes acertou).
Os códigos, na ordem (texto pelo `PlacementErrorString`, §7.10):

| teste (pôr piscina) | código | texto (STR# 0x89) |
|---|---|---|
| `SelectionIsDegenerate` (BV `0x34ed54`): algum canto com nível > 0 | `0x8A` | #90 "Pools only allowed on terrain" |
| nível fora de [mín., máx.] do lote | — (silencioso) | — |
| x0 ≥ x1 ou y0 ≥ y1 (seleção vazia) | `0x80` | #73 (texto de escada; na prática não aparece) |
| `MayPlaceQuadrilateral` (BV `0x352a30`): um canto fora do `IsInBounds(x, y)` do mundo (BV `0xd1944`: Mín + 1 ≤ x ≤ Máx − 1) ou no tile travado (`cIWorldDB` `0xc4`) | `0x02` | #1 "Location out of bounds" |
| vértice dentro do retângulo com altura ≠ a da piscina que o `FUN_009e9550` não consegue mexer | `0x81` (`0x2F` acima do térreo) | #12 "Can't level terrain" |
| `FUN_009dae20` num tile (o terreno do tile tem uma forma que não aceita; sem código próprio) | — (silencioso) | — |
| `CanPlaceTile` (BV `0x34f5a8`), por tile: água (`IsPartWater`) | `0x1E` | vazio no jogo base |
| … praia (`IsBeachLocation`, só BV) | `0x86` | #78 "Can't extend pool" |
| … objeto com od `0x22` = 0 e sem od 42 bit `0x200` | `0x0C` | #11 "Can't intersect other objects" |
| … nos 4 cantos do tile, um item do gerente `0x90` do `cIWorldDB` (thunk `0x150f204` → `0xd1530`, não identificado) de tipo 0/2/4/6 conforme o canto | `0x86` | #78 |
| o tile onde o arraste começou já tem deck, fundação ou piscina (`IsDeckFoundationOfType` 2, 1, 4) | `0x86` | #78 |
| cada parede de piscina nova (`UserMayPlacePartition`, com a parede que já existe aceita: `0x41`) recusada: os dois tiles dentro e destravados → | `0x86` | #78 |
| … senão | `0x02` | #1 |
| parede que atravessa o retângulo (`FUN_00352750` com o modo da piscina) | `0x46` | vazio no base |
| parede que não pode sair (`LevelRoomToolMayEraseWallStyle` falso) | `0x84` | #7 "Can't intersect wall" |

Apagar (Ctrl, `FUN_00351104` → `MayEraseStage`, BV `0x350290`): por tile,
`CanDeletePoolTile` (BV `0x165008`): tile sem piscina (`FUN_001649e4`)
passa; algo do mesmo gerente `0x90` no canto do tile → **`0x87`** #95
"Can't delete pool"; objeto do tile de tipo 2 (pessoa) → **`0x2D`** #44
"Can't delete pool with Sims in it"; parte de multi-tile cujo líder tem o
campo `0x4a` = `0x40` (inferido: escada/trampolim) → **`0x2E`** #45 "Must
first delete ladder or diving board". O limite de 900 tiles e a junção em
retângulo (`AreExpansionBoundsLegal`) não aparecem nesses testes (quem os
chama continua sem achar).

**Correção para o r15-pool-fixes:** os códigos que ele escolheu pelo texto
(`0x5B`, `0x5A`, `0x4F`, `0x60`, `0x20`, `0x0D`) só dão o texto certo porque o
`vm::placementerror::textIndex` do app é `código − 2`, que não é a tabela do
binário (§7.10: `0x86` → #78, `0x8A` → #90, `0x81` → #12, `0x87` → #95, `0x2F`
→ #8, `0x13` → #32...). O certo é o `textIndex` seguir a tabela do
`PlacementErrorString` e a piscina voltar aos códigos do binário: `0x8A`
(nível), `0x02` (canto fora/travado), `0x81` (terreno), `0x1E` (água),
`0x0C` (objeto), `0x86` (estender/parede), `0x87`/`0x2D`/`0x2E` (apagar).
O "not editable" (borda/faixa) do app é o `0x02` nos cantos e o `0x81` nos
vértices de dentro.

**Feito (r15-pool-draw):** `vm::placementerror::stringNumber` é a tabela do
§7.10 (código → entrada 1-based do STR# 0x89; fora dela → 1; os códigos de
texto vazio no base → 0, sem texto: o app cai no texto genérico) e o
`textIndex` é `stringNumber − 1`. A piscina usa `0x8A` (sem terreno / piso no
térreo, este inferido), `0x02` (canto fora ou travado), `0x81` (tile de dentro
travado, terreno que não aplaina), `0x1E` (água, sem texto), `0x0C`
(objeto), `0x86` (deck/fundação no tile, junção em retângulo e 900 tiles,
estes dois inferidos), `0x46` (parede atravessando, sem texto), `0x87`/`0x2D`/`0x2E`
(apagar); um arraste que começa num tile com piscina ou fundação é `0x86`, e o cursor vermelho testa a partir desse começo. Os outros códigos do app já eram os do binário e agora mostram o
texto deles: paredes `0x48` "Needs something to support it" (era "Can't
reach ground"), `0x49` "Can't place on slope", `0x4B` "Wall too close to
Sim", `0x4C` "Cannot build diagonally.", `0x4F` "Can't remove object on
fence", `0x31` "Terrain too steep", `0x40`/`0x45` "Location out of bounds";
terreno `0x65`/`0x8B`, pisos `0x52`/`0x53` e escadas `0x70`–`0x72`/`0x1C`
ganham os textos que antes saíam vazios. O passo headless imprime o código e
o texto: `opensims2: build: --pool ...: FAILED: ... [0x86 "Impossível
estender a piscina"]`.

### 7.19 Arcos de cerca (rodada 12)

`fencearches.txt` tem um só arco no jogo base (`fenceArch 26`, "Arco de
Madeira em Treliça", modelos `fenceDeckAgedWoodArchTrellis{Center,East,West,Post}`,
STR# 143 índice 18: §50). É uma partição de `PartitionType` 2:
`AddWallToGraph` a põe só no grafo `+0x58` (WGRA **0x13**), em nenhum outro
(nem no 5/0x18 com os IDs da camada de paredes), então ela fica na aresta de
uma cerca sem tirar a cerca; nenhum dos 62 lotes tem uma. Erros: `0x79`/`0x88`
"Must be placed on a fence.", `0x4D` "Can't place next to arch"
(`cPartitionManager::FenceArchAdjacent`), `0x4E` "Can't delete arch",
`0x85` "Can't place gate across arch". O corpo de `UserMayPlaceFenceArch` (rodada 13,
§7.19.1) está em `0x9a92fc`. Aqui: o arco
vai numa aresta com cerca (`0x79`, inferido: o binário não o testa ali), depois
os testes do `UserMayPlaceFenceArch` (rodada 13, `fenceArchBlocked`: `0x45`
nenhum dos dois tiles dentro do `IsInBounds` estrito e da área editável;
`0x4F` objeto no moirão de uma ponta; `0x0C` arco numa ponta ou objeto de um
dos 6 tiles da aresta e das extensões cuja pegada encosta num poste: as
pegadas são tiles inteiros aqui, então qualquer objeto dos quatro tiles em
volta de um poste, inferido), custa o preço do catálogo, Ctrl tira (0,8 de volta) e (até a rodada 13)
apagar a cerca levava o arco; gravado como linha do grafo 0x13 (lido de volta como o
estilo 26: inferido). Desenho (inferido pelos nomes): o centro sobre a aresta,
um poste em cada ponta, a parte oeste no poste do início e a leste no do fim.

#### 7.19.1 `UserMayPlaceFenceArch` no binário (rodada 13)

`UserMayPlacePartition` (BV `0x9a95cc`) manda os estilos de arco para
`UserMayPlaceFenceArch` (BV **`0x9a92fc`**, o "near" do symxfer em
`0x2972fc` era outra função), que **não olha se há cerca** (o `0x79`/`0x88`
"Must be placed on a fence." e o `0x4D` "Can't place next to arch" não
saem daqui nem de nenhum `li r4, código` antes dos construtores de erro do
binário; inferido: vêm da ferramenta da interface, por outro caminho).
Ordem (com lista de erros, segue juntando erros; sem lista, sai no primeiro):

1. os tiles dos dois lados da aresta (`FUN_008b4e9c`/`FUN_008b51d4`):
   nenhum passa no `WorldDB` vtable `0x108` (na interface `cIWorldDB`, o
   nome do symxfer é `IsInBounds(tile)`; `0xf8` é `IsOnLot(tile)`) → `0x45`;
2. para cada ponta da aresta, um **objeto no moirão** daquele vértice
   (`FUN_00163758`, a mesma busca de `EraseFencePostObject` e
   `TryPlaceOnFencePost`) → `0x4F` "Can't remove object on fence" (um erro
   por ponta);
3. `MayPlaceFenceArchWithRespectToObjects` (BV `0x9a8cb8`) falso → `0x0C`
   "Can't intersect other objects" e falha.

`MayPlaceFenceArchWithRespectToObjects`:

- falha se o módulo de cercas do grafo de cena (`TSSGSystem` vtable `0x8c`,
  depois vtable `0x48(vértice)`) diz sim para uma das duas pontas (inferido:
  já há um arco no vértice, `nTSSG::FenceArchAtVertex`; é aqui que um arco
  vizinho é recusado, com `0x0C`);
- três trechos: a própria aresta e um trecho de **1 tile** que a prolonga
  em cada ponta (pelo eixo dela: se |x0 − x1| < 1, a aresta corre em y);
  para os dois tiles de cada trecho (um de cada lado), cada objeto, salvo os
  de od `0x22` ≠ 0 ou com o bit `0x400` da od `0x2a`: se a pegada dele
  (`GetFootprint` na posição e orientação dele) cruza a caixa de **±0,05**
  em volta de uma das duas pontas → falha. Ou seja, só os objetos que
  encostam nos **postes** do arco o impedem; o meio da aresta pode ter
  objetos.

`TryRemoveFenceArch` (BV `0x30c514`): apaga os objetos do tile da aresta
presos àquela aresta (vtable `0x26c(aresta)`), salvo os de od `0x3b` =
`0x7d` com a vtable `0x348` verdadeira. `AddFenceArch` (BV `0xc348cc`): os
dois tiles têm de existir; `AddWallToGraph`, guarda o estilo do arco num
mapa (`this+0xa4`, chave = o ID da aresta no grafo `+0x58`) e, com a
mensagem ligada no `WorldDB`, manda `0x0B4CF313`. `FenceArchAdjacent(vértice)`
= o grafo `+0x58` tem algo no vértice (vtable `0x24`).

#### 7.19.2 De onde vêm `0x79`, `0x88` e `0x4D` (rodada 14, binário)

- **`0x79`** sai só do `TestPlacementCommon` (`li r4,0x79` em `0x8abbc4`): é
  o objeto que exige moirão de cerca (od 42 bit `0x2000`, §3.2); **`0x88`**
  sai do `FUN_008a7a60` (chamado por `TestPlacementWithRespectToWalls`): o
  objeto de parede com a flag `0x1000` sem cerca na aresta. Os dois são de
  **objetos**, não do arco: o arco **não exige cerca**.
- **`0x4D`** sai do `FUN_009a6b20`, o teste de partições do teste comum de
  `UserMayPlacePartition` (BV `0x9a7384`, que o symxfer chama de
  `cTSUserToolAutoRoof::RemoveAllRoofs`; `UserMayPlacePartition` o roda para
  todos os estilos e só depois manda os arcos — `PartitionType` 2 — para
  `UserMayPlaceFenceArch`). Para cada grafo i = 0..5 (o grafo é o
  `PartitionType`: 0 paredes, 1 cercas, 2 arcos; 3 meia parede, que é
  parede e cerca): se o bit i de `PartitionsBlocked` (produto `+0xd0`) está
  ligado e há uma partição do grafo na aresta → **`0x41`**; se o bit i de
  `AdjacentPartitionsBlocked` (`+0xd4`) está ligado e há uma partição do
  grafo terminando numa das pontas (`FUN_009a6154`) → uma parede
  (`PartitionStyleIsWall`, tipos 0 e 3) no grafo 2 dá **`0x4D`** "Can't
  place next to arch", um arco no grafo 0 dá **`0x50`** "Can't place next
  to wall", o resto 0. Valores (construtores): `cTSProductPartition` padrão
  `0xF`/0; `cTSProductWall` (BV `0xf1c9d4`) adjacentes **4**; cerca
  (`FUN_00e4fc68`) bloqueados **`0xB`**; `cTSProductFenceArch` (BV
  `0xf20544`) tipo 2, bloqueados **5**, adjacentes **1**, `requiresFlatBottom`
  1, `mayPlaceDiagonally` 0. Ou seja: parede encostada no poste de um arco
  → `0x4D`; arco sobre parede ou outro arco → `0x41`; arco encostado numa
  parede → `0x50`; arco sobre cerca pode (o grafo 1 não está nos 5).
- O resto do teste comum, na ordem: lado fora do lote (`IsOnLot`, slot
  `0xf8`) → `0x40` (uma cerca — `PartitionStyleIsFence`, tipos 1 e 3 —
  precisa de um lado só; parede e arco dos dois); `FUN_009a6b20`; diagonal
  sem `mayPlaceDiagonally` → `0x4C`; lado fora do lote ou **travado** (slot
  `0xc4`, §7.20) → `0x45` (os dois lados para a cerca), pulado com a trapaça
  `TilesUnlocked` da barra (`FUN_000c8028` vtable `0xd4`); fundo não plano →
  `0x49`; sem suporte → `0x48`; piscina dos dois lados → `0x46`; água sem
  `submersible` → `0x47`; praia → `0xA3`.

No código (rodada 14, `BuildEditor::partitionCommonError`): `0x40`, `0x41`,
`0x4D`, `0x50`, `0x4C`, `0x45` e (arco) `0x49` como acima, para paredes,
cercas e arcos; o arco vai numa aresta sem cerca, e apagar a cerca deixa o
arco (antes: `0x79` inferido e o arco ia junto com a cerca). Inferido: a
ordem entre o `0x45` e o `0x41/0x4D/0x50` (o binário junta todos na lista).

### 7.20 A área editável do lote (rodada 12)

`cWorldDB::IsOnLot(tile/vertex/location)` (BV `0xd1538…`) compara com os
limites que `SetLotBounds(x0, y0, x1, y1)` guarda (`+0x24…+0x34`, mais os
níveis); quem chama `SetLotBounds` vai pela vtable (não achado). O lote do
bairro é o tamanho mais uma faixa de rua de um tile do bairro (10 tiles do
lote) em cada borda com rua (`emptyLotRect`, `docs/neighborhood-mac.md`); nos
lotes salvos essa faixa é calçada + rua + calçada (ex.: `N001_Lot38`, linhas
20–29). Aqui: uma borda cujos 10 tiles de fora têm piso "road_*" sai da área
editável (inferido dos dados), e paredes/cercas (`0x45`), pisos, fundações,
piscinas, terreno e escadas ficam dentro dela. Os objetos (modo compra) não
foram mudados.

**Rodada 13 (binário + dados).** Os limites vêm do **save**: o
`cWorldDB::Load` (BV `0xd7758`) lê o recurso `0x49FF7D76` (o "cWorldDB" do
lote, que o opensims2 listava como sem uso) e chama `SetLotBounds` (vtable
`0x4c`, BV `0xd65e4`) com os valores lidos. Formato (`cWorldDB::Write`, BV
`0xd91c8`; o base grava a versão 3, o BV a 5): 64 bytes zerados, u32 TGI
tipo (`0x49FF7D76`), u32 versão, string (u8 tamanho + "cWorldDB"), depois
s32 x0, s32 y0, s32 nível mínimo, s32 x1, s32 y1, s32 nível máximo, u8
flag (≠ 0 → segue uma string, só no BV). Exemplos de N001: lote 38 →
(0, 0, 0)–(29, 29, 2); lote 11 → (0, 0, 0)–(39, 29, 4); lote 16 → (0, 0,
0)–(29, 19, 3). Ou seja, **os limites são o mundo inteiro do lote,
inclusive a faixa de rua** (no lote 38 as linhas 20–29 são a rua e estão
dentro).

- `IsOnLot(tile)` (BV `0xd1604`): x0 ≤ x ≤ x1, y0 ≤ y ≤ y1, nível no
  intervalo (inclusivo).
- `IsInBounds(tile)` (BV `0xd1a28`, vtable `0x108` da `cIWorldDB`, a
  interface que `nTSWorld::WorldDB()` devolve): **estrito** em x e y (x0 <
  x < x1, y0 < y < y1), inclusivo no nível: tira o anel de tiles da borda
  do mundo. É esse o teste do `0x45` de paredes, cercas e arcos.
- **Atenção às vtables:** `WorldDB()` devolve o subobjeto `cIWorldDB` (o de
  deslocamento −28 da `cWorldDB`, vptr `0x27a3420`), não o primário; os
  slots desta seção e do §7.19.1 são dessa interface (`0x94`
  `PartitionManager`, `0xc4` `HasFloorTest(tile, nível, bool, bool)`, `0xe4`
  `NumObjectsOnTile`, `0xf8` `IsOnLot`, `0x108` `IsInBounds`, `0x14c`/`0x150`
  `Max`/`MinTerrainElevation`). O "travados (`WorldDB` vtable `0xc4`)" do
  §7 (paredes) foi lido no vtable primário; nessa interface o slot `0xc4` é o
  `HasFloorTest` (a conferir).
- **O que tira a faixa de rua (rodada 14, binário + dados): os tiles
  travados.** O slot `0xc4` da `cIWorldDB` é o thunk `0x1510e54` para
  `0xd2734` (o "`HasFloorTest`" do symxfer é nome errado): lê o byte do
  `c3DArray<uchar>` em `cWorldDB +0xa0` no tile (x − x0, y − y0, nível − n0)
  e diz se ≠ 0 = **`IsLocked(tile)`**. O array é o **3ARY `0x03`** do lote
  ([nível][x][y], um byte por tile): o `cWorldDB::InitForNewLot` (BV
  `0xd6a84`) o lê do **modelo de lote** (`LotTemplates/LotTemplate*.package`,
  `cTSSimSystem` vtable `0x124`, recurso 3) e o gira com a frente do lote; o
  lote salvo o guarda. Os modelos travam a faixa de rua de 10 tiles; nos 67
  lotes com o array (todos menos `N002_Lot32`), o travado é exatamente a
  faixa (colunas ou linhas inteiras), em alguns lotes só nos níveis de
  baixo (o `AddLevels` enche os níveis novos com 0). Quem o lê:
  - o teste comum das partições (`0x45`, §7.19.2);
  - o teste dos vértices `FUN_009e9550` (máscara bit 0 → `FUN_009e8780`):
    vértice térreo na borda do mundo (x = `MinTileX` ou `MaxTileX` + 1, idem
    y) ou com um dos 4 tiles em volta fora do lote ou travado → **`0x62`**
    (sem a trapaça); antes disso `FUN_009e75e0`: fora do lote → `0x59`;
  - os objetos: `TSObjectIsInLockedArea` (BV `0x8a9d00`, chamado por
    `TSObjectUserCanSetPosition`, `...CanDrop`, `...CanPlaceInSlot` e o conta-
    gotas) → `IsOnLockedTile` (BV `0x8a66b0`): no lote e travado → **`0x02`**;
    od 42 bit `0x40` pula o teste; o objeto com `AllowOnlyOnLockedTile`
    (BV `0xaff268`, vtable `0x230`: **od 40 bit 10**, `+0xea`; o rótulo do
    editor, STR# 0xD6 #10, é "(unused)Generates Light") tem de ficar **só**
    em tiles travados: fora deles é recusado **sem erro na lista**; nos
    outros, tile travado → `0x02`. Multi-tile (`0x5A6216F6`): cada parte no
    seu tile, com o bit da parte; objeto com pegada estendida (vtable
    `0x1a8`): os 4 cantos de cada retângulo, todos travados para o bit e
    nenhum para os outros. **Rodada 15: nenhuma árvore do jogo base liga
    esse bit** (nenhum Expression liga a flag 11 do od 40 em 22.396 BHAVs) e o
    `Reset` o zera (od 40 não está entre os dados guardados): no base ele
    nunca vale, e o app está certo em não o ter;
  - fundações/decks/piscinas (`MayPlaceQuadrilateral`, `0x353c6c`: tile fora
    do `IsInBounds` ou travado → `0x02`).
- No código (rodada 14): `Lot::locked` (lido do 3ARY 0x03, níveis novos
  copiam o vizinho, gravado como estava), `vm::lotTileLocked`,
  `World::locked` (modo compra: `0x02`, salvo od 42 `0x40`),
  `BuildEditor::tileLocked`/`editableTile` (tiles do lote e destravados) e
  `editablePoint` (os 4 tiles em volta destravados: o pincel de terreno
  não mexe em vértices da borda nem da faixa). Só um lote sem o array usa a
  regra inferida antiga (`inferredRoadStripArea`, faixa com piso "road_*").
  Saiu o teste inferido de piso de rua das paredes.

### 7.21 Suporte: `HasSupport`, `SupportedUnderneath`, `IsFloorable` e paredes no alto (rodada n07, Ghidra MCP)

Tudo em `nTSWorld` (BV). O tile é um `cFloorTile` = (x, y, nível, máscara
de quartos); os bits da máscara são os do app: 1 oeste (−x), 2 norte (+y),
4 leste (+x), 8 sul (−y). As direções dos vizinhos (`FUN_0015d680`) usam os
mesmos bits (1 → −x, 2 → +y, 4 → +x, 8 → −y) e `FUN_0015d3e8` dá o quarto
oposto (1↔4, 2↔8, 3↔0xC, 6↔9). O nível é o do lote: 0 = térreo.

- **`SupportedUnderneath`** (`0x16a5b8`): nível ≤ 0 → sim. Senão, sim se
  (a) `FUN_00164d30`: algum quarto da máscara, no nível de baixo, está
  **dentro de uma sala do mapa de carga** (`PartitionManager` vtable `0x13c`
  = `FUN_00c30948`, sem nome: escolhe o gerenciador de salas da definição
  pedida, aqui **1 = `bearsLoad`, o 3ARY 0x0A**, converte o centro do quarto,
  x + 0,5 ± 1/3, em tile e pergunta a sala; ≠ 0 = dentro). Ou seja, um piso
  embaixo não segura nada: o que segura são as paredes de carga em volta
  (fundações inclusive). Ou (b) **`HasObjectSupport`**
  (`0x16a328`): um objeto no tile de baixo (`NumObjectsOnTile`/vtable `0xec`)
  com `cTSObject::IsSupport` (`0xaff018`, vtable `0x1cc`: `GetType` = campo
  9 do OBJD **== 5**; no jogo base, as 12 colunas), que não seja o objeto
  testado (vtable `0x2b0`) e cuja posição não seja exatamente o canto do tile.
- **`FUN_001654b4`** (apoio de parede): no nível de baixo, uma parede com
  `PartitionStyleBearsLoad` (`0x8b5bf0`, o `bearsLoad` do produto; sem produto
  = sim) numa das **duas diagonais** do tile ou na **aresta de um quarto da
  máscara** (1 oeste x, 2 norte y+1, 4 leste x+1, 8 sul y).
- **`HasSupport`** (`0x16a640`) = `SupportedUnderneath` ou `FUN_001654b4` ou
  `WorldDB::Roof()` (vtable `0x88` → `cWorldDB +0x7c`; os acessores `+0x78..+0x84`
  são `Loaded`, `Roof`, `Pool`, `CurvedPool`, pela ordem do base) vtable `0xa4`
  = **`cWorldRoof::DoesTileHasRoofSupport`** (`0xc245d4`, pela vtable
  secundária `-8` do `cWorldRoof`): os telhados do nível de baixo de tipo 6
  (**mansarda**; o 6 do `GetRoofIDs` lido como o tipo) cujo retângulo, menos
  `int(StandardLevelHeight)` = 3 tiles de cada lado, contém o tile (o topo
  plano da mansarda).
- **`IsFloorable`** (`0x16a990`), usado pelo piso (`0x52`), `PlaceTileOutlines`,
  `RemoveFloorTile` e o raio do mouse:
  1. nível ≠ 0: o tile precisa ser plano (`IsFlat`) e `IsInBounds` (WorldDB
     `0x108`), senão não;
  2. `HasSupport(tile inteiro)` → sim;
  3. para cada vizinho (1, 2, 4, 8) que está no lote (`IsOnLot`, `0xf8`):
     `SupportedUnderneath(vizinho, quarto voltado para o tile)` → sim; se o
     vizinho tem piso nesse quarto neste nível (`0xb4`) → "junto de piso";
  4. sem piso junto → não. Com piso junto: algum tile a |dx| + |dy| < 4
     (dx, dy em −2..2) `SupportedUnderneath` (inteiro) → sim; senão algum tile
     do 3×3 em volta com `FUN_001654b4` (inteiro) → sim; senão não.

  Antes (inferido) o app contava o piso de baixo e qualquer parede (pelo
  meio do trecho) no tile e nos vizinhos até 2 tiles. Agora é o do binário:
  segura a sala de carga de baixo, a coluna, a mansarda, e a parede de carga
  só no próprio tile ou a 1 tile (junto de piso); sótão e cerca não seguram.
  **Conferido nos 63 lotes de N001–N003** (`s2pkg lot-save <lote> <saída>
  --check-build 1`): de 14.098 tiles com piso acima do térreo, 14.071 passam
  no `IsFloorable` (antes da sala de carga, 2.515 falhavam, quase todos em
  fundações); os 27 que sobram (N001_Lot11, Lot82, N002_Lot4, N003_Lot183,
  Lot192) não foram explicados.
- **Paredes no alto** (`FUN_0016b740`, chamado pelo teste comum das
  partições `0x9a7384` quando o estilo pede `requiresSupportUnderneath`,
  erro `0x48`): para cada lado da parede (`FUN_008b4e9c` e o
  `OppositeWallLocator`: o tile e os quartos voltados para a parede; na
  diagonal, as duas metades do mesmo tile: "/" 0x3 e 0xC, "\\" 0x6 e 0x9)
  que está no lote: todos os quartos `SupportedUnderneath` → sim; ou piso
  nesses quartos neste nível (`0xb0`) ou o topo de mansarda
  (`DoesTileHasRoofSupport`) → sim;
  ou, nível > 0, uma parede no mesmo trecho um nível abaixo com estilo 1, 3,
  5–10, 0x0F, 0x16 ou 0x17 (parede, sótão, fundação; não deck 0x10, piscina
  0x1D, saia 4 nem cerca) → sim. Senão `0x48`. O app (inferido antes:
  "parede embaixo ou piso de um lado") passou a aceitar uma parede sobre a
  sala de carga do andar de baixo sem piso no andar dela, e a recusar sobre
  deck, piscina ou cerca.

### 7.22 A ferramenta de telhado: `cTSUserToolRoof::IsRoofValid` (rodada n07, Ghidra MCP)

`IsRoofValid(bool testaObjetos)` (BV `0x1d9fd4`, nome "near" do symxfer; o
`TryAddRoof`/`UserMayAddRoof` só repassam para o undoable). Os dois cantos do
arraste (`+0x98` início, `+0xa4` fim, cada um x, y, nível). Os erros são
juntados numa lista; o primeiro aparece:

1. `FUN_0099545c`: |Δx| × |Δy| **≥ 1840** → `0x10` "Can't support the size"
   (nos dois cantos).
2. Cada canto (`ApplyFloorPattern`, nome errado do symxfer, `0x9992d0`): os
   tiles em volta do vértice precisam estar no lote (`IsOnLot`) e destravados
   (WorldDB `0xc4`), senão `0x7E` (sem texto próprio: "Location out of bounds").
3. (BV) cruzar telhado de vidro → `0x0C`.
4. Área < 1 (e não diagonal) → inválido, sem erro.
5. Com `testaObjetos`: `RoofIntersectWithObjects` (`0x1d9908`) monta o
   telhado (`RoofFromType`/`DiagonalRoofFromType`) e, para cada tile da caixa
   (a do losango nos diagonais), `DoesTileHasObjectIntersectingRoof`
   (`0x1d4354`): se o telhado cobre o centro do tile, cada objeto do tile com
   od 0x22 = 0 e od 42 sem o bit 0x8000 (telhado) — salvo quando um teste antes
   (membro `+0x6c` da ferramenta, `cWorldRoof` `0x7c` `IsAnyRoofAtLocation` e
   `0xa0` com `StandardLevelHeight`; não feito) libera o tile — recusa com
   `0x7F` ("Can't intersect other objects") se a altura do telhado no centro
   é menor que o piso ali + `StandardLevelHeight` (3).
6. Nível do início < 0, ou o tile do início sobre piscina (`FUN_001648ac` = o
   tile um nível abaixo é piscina, `IsPool` do `DeckFoundationStyle`) → `0x1C`.
7. Se valeu, `AreWallsLegalToBePlaced` (as paredes de sótão).

No app: `BuildEditor::roofError` faz 1, 2, 5 e 6 antes do `drawRoof`. Nos 63
lotes de N001–N003, 32 dos 275 telhados seriam recusados hoje pelo 5: todos
têm janelas (e uma escada e uma cadeira) no nível do telhado, embaixo da
beirada, postas depois do telhado (o teste só roda ao pôr o telhado).

**O pré-teste (rodada n13, Ghidra MCP):** o membro `+0x6c` da ferramenta é o
`cIWorldRoof` do mundo (vtable secundária do `cWorldRoof` em `0x281b948`,
`this − 8`). No `DoesTileHasObjectIntersectingRoof`, no **primeiro** objeto do
tile que passa no filtro (od 0x22 = 0, od 42 ≥ 0) a função decide e retorna:
`IsAnyRoofAtLocation(centro, 1)` (slot `0x7c` → `0xc25398`: algum telhado
**já posto** no andar do centro o cobre, `Contains` com beiral) e
`DoesLocationHasClearanceHeight(3, centro, 1)` (slot `0xa0` → `0xc2435c`)
liberam o tile; senão `0x7F` se o telhado novo no centro
(`GetElevation(x, y, 0)`) < piso + 3. `DoesLocationHasClearanceHeight(h, loc)`:
livre se nenhum telhado cobre `loc` nem o ponto um andar abaixo; senão `m` =
o máximo (desde 0) das alturas dos telhados dos andares L−1..L+2
(`AddRoofIdToArray(…, true)`), cada uma por
`GetSampledElevationByRoofIDForClearance` (`0xc236b4`: `GetElevation(x, y,
margem)`, margem 0,5 para telhados de andar acima de L; tipo 0xF = piso +
0,2), e livre se `m ≥ piso + h` ou `m ≤ piso + 0,1`. O telhado sendo posto não
está no mundo nessa hora (o `RoofIntersectWithObjects` o cria à parte). No
app (`BuildEditor::roofError`) o pré-teste usa os outros telhados do lote; a
contagem dos telhados do jogo aceitos (`s2pkg lot-save <lote> <saída>
--check-build 1`) testa cada telhado salvo com o lote como está, ele próprio
incluído entre os telhados já postos. A caixa de tiles é a do jogo: os limites
do telhado **com o beiral**, de `int(mín)` a `int(máx − 1)` (antes o app
olhava só os tiles dentro das paredes). Resultado nos 63 lotes: **245 de 275**
aceitos (antes 243). O pré-teste só libera tiles onde o telhado que já está lá
passa a ≥ 3 do piso ou a ≤ 0,1 dele (o beiral do lado de fora); os 30 que
sobram têm todos uma **janela** no andar do próprio telhado num tile de dentro
das paredes onde ele está a 0,58 do piso (a primeira fileira sob a parte
baixa): o jogo também os recusaria se fossem refeitos, porque o teste só roda
ao pôr o telhado e as janelas foram postas depois (N001_Lot59, 64, 68, 77, 82;
N002_Lot10, 22, 25, 29, 31; N003_Lot133, 157, 166, 171, 179, 181).

`FUN_001648ac` (tile de cima de piscina) também dá o `0x46` das paredes (os
dois lados sobre piscina) e o teste 9 do `FUN_009e9550`.

Piscinas (`cTSUserToolDeckFoundation`, a piscina é um deck): no
`SelectionIsDegenerate` (`0x34ed54`) o `0x8A` "Pools only allowed on terrain"
é **piscina com um canto acima do nível 0** (e `0x1A` o mesmo para
fundações; `0x80` seleção degenerada). O `MayPlaceQuadrilateral` (`0x352a30`)
pede o `FUN_009e9550` a cada vértice cuja altura difere da ferramenta e dá
`0x81` "Can't level terrain" (`0x2F` acima do térreo) a qualquer recusa; o
`CanPlaceTile` não olha piso. O app passou a dar `0x81` (não `0x8A`) para piso
no chão sob a piscina (qual teste do vértice recusa segue inferido). E o
`0x86` "Can't extend pool" vem do `MayPlaceQuadrilateral`: cada piscina que o
arraste toca passa pelo `AreExpansionBoundsLegal`; se falha, `0x86` (ou `0x02`
com um canto fora/travado) — confirmado.

### 7.23 Terreno: o teste de vértice `FUN_009e9550` (rodada n07, Ghidra MCP)

`FUN_009e9550(altura, vértice, flags, erros)` (o "pode mudar este vértice"
de terreno, piso e escadas) tem um pré-teste (vértice no mundo, `0xfc`; senão
`0x59`) e testes ligados por bit de `flags`:

| bit | função | recusa |
|---|---|---|
| 0 | `0x9e8780` | vértice na borda do lote (nível 0) ou tile em volta fora do lote/travado → `0x62` |
| 1 | `0x9e7700` | altura < `MinTerrainElevation` → `0x65`; > `MaxTerrainElevation` → `0x66` |
| — | sim system vtable `0xd0` | desligado (cheat): pula o resto |
| 3 | `0x9e8378` | um tile em volta `IsFloorable` → `0x6B` |
| 2 | `0x9e856c` | um tile em volta com piso (`0xb4`, inteiro) → `0x5A` "Covered by floor tile" |
| 7 | `0x9e8e0c` | objetos: `HasGroundPinningObjects` → `0x61`; subindo perto do nível de cima, `HasNonCeilingConformingObjects` → `0x60`; od 42 bit 0x80 (escadas 11/12 pelos cantos) acima do novo piso → `0x68` |
| 4 / 5 | `0x9e9c98` / `0x9e9a78` | o vértice do nível de cima / de baixo teria de andar junto e não pode → `0x69` / `0x6A` |
| 8 | `0x9e8048` | parede presa ao vértice (`0x9e7d08`, neste nível e no de baixo) |
| 9 | `0x9e8268` | um tile em volta sobre piscina |
| 10 | `0x9e7890` | partição no vértice que não é `submersible` → `0x1E` |
| 11 | `0x9e8ad0` | cerca ficaria com inclinação > √3 → `0x31` |
| 13 | `0x9e8124` | tile em volta com deck/fundação (`0xcc` ≠ −1) |

O pincel de terreno chama com `flags` = a global `0x02bafd10`, que **nenhuma
instrução escreve** (BSS, só lida em `ChangeTerrainElevation` `0x4010e0`):
pelo que se lê, o pincel passa só o pré-teste. E o `0x8B` "Only exposed
terrain can be edited" do `ApplyTerrainSculptBrush` (`0x32a214`) é só quando
o ponto do mouse não está no nível 0. O app segue com a regra inferida
(`terrainFree`: piso, fundação ou ponta de parede em volta não deixam mexer);
não foi trocada porque a global pode ser escrita por um caminho que o Ghidra
não liga.

**Correção (rodada n20):** a global é escrita, sim, por um inicializador
estático que o Ghidra não analisou (seção `__StaticInit`, `0x1f66394`: `lis
r3,0x2bb; addi r3,r3,-0x2f0; li r4,11255; bl 0x1598c50`, o construtor
`std::bitset(unsigned long)`, que só guarda o valor). O pincel usa **`0x2BF7`**.
As outras chamadas do teste de vértice também leem bitsets assim: o nivelador
das paredes `0x02c761b0` e as escadas modulares `0x02c81020` valem
**`0x2BB7`** (inicializadores em `0x2228a0c` e `0x22502e8`); cada unidade de
tradução tem três (`0x2BB7`, `0x3BF7`, `0x2BF7`, nesta ordem). Ver §7.24.

### 7.24 Colocação, cômodo, nivelador, desfazer e escadas (rodada n20, Ghidra MCP)

Leitura do BV com o Ghidra MCP (só leitura); onde o Ghidra não tinha função
(inicializadores estáticos, o gerente de desfazer) os bytes foram lidos do
próprio `sims2_bv.ppc` com um desmontador simples (scripts de rascunho).

**O teste de vértice `FUN_009e9550` por inteiro** (corrige a tabela de §7.23).
`flags` é um `std::bitset` testado bit a bit (`FUN_01794c88`):

1. pré-teste `FUN_009e75e0`: vértice no mundo (`WorldDB 0xfc`), senão `0x59`;
2. bit 0 (`FUN_009e8780`): no nível 0, vértice na borda do mundo, ou um dos
   tiles em volta fora do lote ou travado (sem o cheat de construir em
   qualquer lugar, sim system `0xd4`) → `0x62`;
3. o vértice **já está na altura** → devolve o resultado do bit 0 (os outros
   testes nem rodam);
4. bit 1: limites → `0x65`/`0x66`;
5. sim system `0xd0` desligado → para aqui (cheat; ligado por padrão:
   inferido, o `constrainFloorElevation`);
6. bit 3 (`IsFloorable` em volta, `0x6B`), bit 2 (`FUN_009e856c`: algum
   tile em volta, no nível do vértice, com piso em algum quarto,
   `HasFloorTest(t, 0xF, qualquer)` → `0x5A`), bit 7 (objetos, `FUN_009e8e0c`:
   `HasGroundPinningObjects` → `0x61`; subindo a menos de
   `StandardLevelHeight` do nível de cima, `HasNonCeilingConformingObjects`
   → `0x60`; objetos com od 42 `0x80` cujo piso subiria acima deles →
   `0x68`), bit 6 (só muda uma cópia local: sem efeito), bit 4/5
   (`FUN_009e9c98`/`0x9e9a78`: se o vértice do nível de cima ficaria a menos
   de `MinLevelHeight` + ¼ de `FloorElevationUnit` da altura nova, ele tem de
   poder ir a altura + `MinLevelHeight` (o mesmo teste, sem o bit 5), senão
   `0x69`; o de baixo, ao contrário, `0x6A`; **`MinLevelHeight` = 0,75**, global
   `0x02b14460` posta pelo inicializador `0x1d3da8c`), bit 8 (`FUN_009e8048` →
   `FUN_009e7d08`: uma partição numa das quatro arestas que saem do vértice,
   neste nível com `requiresFlatBottom` → `0x5B`, no de baixo com
   `requiresFlatTop` → `0x5C`; antes, arcos/postes `FUN_009e7a44`), bit 9
   (`FUN_009e8268`: um tile em volta sobre piscina, `FUN_001648ac`; sem
   código), bit 10 (`0x1E`), bit 11 (cerca íngreme `0x31`), bit 13
   (`FUN_009e8124`: um tile em volta com deck/fundação/piscina,
   `DeckFoundationStyle` ≠ −1; sem código), e no fim praia (BV).

Com `0x2BB7` (paredes, cômodo, escadas) rodam os bits 0, 1, 2, 4, 5, 7, 8, 9,
11 e 13; com `0x2BF7` (pincel) os mesmos e o 6, sem efeito: **o pincel, as
paredes e as escadas testam igual**.

**O nivelador das paredes** (`FUN_009a5a68(altura, vértice, erros,
undoable)`): `WorldDB 0xc0` = `IsWaterVertex` → `0x47` (sem texto no base);
depois `FUN_009e9550(altura, vértice, 0x2BB7, erros)`; passou (e não é
água) → `ChangeFloorAndWaterElevationCoupled` (os andares de cima juntos).
`FUN_009a79d0` chama o nivelador quando o estilo `requiresFlatBottom` e,
com `requiresFlatTop`, também o vértice do andar de cima (se no lote, `0xfc`)
à altura do de baixo + `StandardLevelHeight`. A altura é a do **vértice
onde o traço começou** (`WorldDB 0xd8` = `FloorElevation` no
`UserAddPartitions`; era inferido).

**O fechamento do cômodo (`FUN_009a7dd8`)** não fecha nada: depois que os dois
"L" do retângulo passam, ele chama `FUN_009a79d0` em **todo vértice do
retângulo** (cantos inclusive, x por fora e y por dentro, no nível do
início) e faz o E dos resultados: o chão de dentro do cômodo é aplainado na
altura do começo, e um vértice que não pode ir (água, piso em volta, …)
reprova o cômodo inteiro (`UserAddPartitions` devolve 0).

**Tudo ou nada.** `TryPlacePartition` (`0x9a9774`) zera a lista de erros a cada
trecho, nivela (os erros do nivelador entram na lista), testa e insere; com
algum erro que não seja `0x41` (já há partição) devolve 0. `PlaceWallLine`
(`0x9a9fec`) e `EraseWallLine` (`0x9ac0e8`) devolvem 0 no primeiro trecho que
falha. `ChangeWallsAndPlaceCursorAtEnd` (`0x1e1828`) guarda esse resultado em
`+0xc4` (e zera com `CanAfford` falhando, `+0xc5` = sem dinheiro) e mostra o
erro salvo `0x41`/`0x42`; `OnRelease` (`0x1e1fd0`) com `+0xc4` = 0 **desfaz
a prévia inteira** (vtable `0x20` dos três undoables) e não submete nada.
Ou seja: uma parede, cerca ou demolição arrastada que esbarra em algo não
constrói nem apaga **nenhum** trecho (antes: os trechos antes do problema
ficavam, inferido).

**Colocação de objetos: onde entra o suporte.** `TestPlacementCommon`
(`0x8aad90`) **não** usa `HasSupport`/`SupportedUnderneath`. O piso é
`FUN_0016d518(tile, 0, 0, visível, porObjeto, piscina)`: `WorldDB 0xb8(tile,
0xF, visível)` = no nível 0 sempre sim, acima `HasFloorTest(tile, 0xF,
todos, visível)` (`FUN_000d2798`: **os quatro quartos com piso** e, com
`visível`, os quatro visíveis), ou `TileHasFloorFromObject`; e, com
`piscina`, o tile de baixo não é de piscina (`FUN_00164c40`). Para objetos
comuns o teste é `(tile inteiro, 0, 0, !HidesFloorOnPlace, 1, 1)` (vtable
`0x220` = `cTSObject::HidesFloorOnPlace`, `+0xaa` bit 14), salvo p&`0x200` ou
p2&4 → `0x8E`; multi-tile: cada retângulo da pegada, pelo canto, com
`(.., 1, 1, 1)` → `0x13`. Pessoas: p&`0x200`, o mesmo teste sem piscina, ou
`FUN_00165924`. **Rampa** (passo 6) é `nTSWorld::IsFlat` (`0x165af8`): as
quatro alturas do tile **exatamente iguais**, salvo p&`0x100` → `0x12`
(multi-tile: cada tile de cada retângulo). `HasSupport` só aparece no
`FUN_008a7164`: objeto de **tipo 10** (escadaria) → `CeilingTooHighForObject`
(`0x16a6f8`): o tile de cima (nível + 1, no lote) com `HasSupport` e algum
vértice dele mais de `StandardLevelHeight` + 0,001 acima do vértice de baixo
→ `0x30` (salvo p2&`0x20`). Fora da colocação comum, a ferramenta de lareira
(`cTSUserToolFireplace::TryPlace`, BV `0x27a38c`, que chama a do objeto antes)
sobe a chaminé andar por andar enquanto o tile de cima tem `HasSupport` (não
feito: aqui a lareira é um objeto comum). O teste de altura do fim do `TestPlaceInWorld`
(`0x8ad754`) é `od 4 & m` com `m` = vtable `0x2c(1)` de um objeto
`0xFBA4DA53` criado na hora (o mesmo que o `ReadSlots` cria para as alturas
dos slots): `0x19` sem o bit.

**Desfazer, peça por peça.** O gerente (`cTSUserToolUndoManager`, vtable
`0x2807200`, construtor `0x970710`: `+8` lista de desfazer, `+0x14` de refazer,
vetores de ponteiros):

| vtable | função | o que faz |
|---|---|---|
| `0x0c` | `CreateUndoable` (`0x970180`) | pelo CLSID |
| `0x10` | `SubmitUndoable` (`0x970aa8`) | se o estado (`0x2c`) ≠ 2: som de pagar/receber pelo sinal do `TotalCost` (`0x34`), `CommitIt` (`0x14`); erro → `FlushCommandQueues` e sai. Estado 2 → entra no fim da lista (sem limite: só tira o primeiro se chegar ao `max_size`) e **a lista de refazer é solta** |
| `0x14` | `UndoLastCommand` (`0x970910`) | o último: `UndoIt` (`0x18`); erro → **solta a lista de desfazer inteira**; senão vai para a de refazer, som `PTR 0x278a2c0` |
| `0x18` | `RedoLastCommand` (`0x970798`) | o último da de refazer: `RedoIt` (`0x1c`); erro → **solta a de refazer**; senão volta à de desfazer, som `0x278a2b4` |
| `0x1c` | `FlushCommandQueues` (`0x970568`) | solta as duas |
| `0x20` / `0x24` | `CanUndo` / `CanRedo` (`0x1958e4c`/`0x1958e10`) | lista não vazia (os botões `0xA1`/`0xA2`) |

Soltar (`0x9703a0`/`0x970484`) chama a vtable `0x10` de cada undoable (o
`ResetSelf`, que mata os objetos vendidos) e o `Release`. O undoable
(`cTSUserToolUndoable`, vtable `0x2808558`) guarda o estado em `+0x28`: 1
novo, 2 feito, 3 desfeito, 4 falhou. `CommitIt` (`0x9b4ff0`): pré-operações,
a própria (`0x58`), pós absolutas, pós; `UndoIt` (`0x9b65bc`; se ainda 1, faz
antes): pós, a própria (`0x5c`), pós absolutas, pré; `RedoIt` (`0x9b519c`):
como o `CommitIt` com `0x60`. Cada um termina com a vtable `0x68(±0x30)`
(`0x9b4a6c`, `cTSUserToolUndoable::AdjustFunds`: manda ao simulador,
vtable `0xb0(6, valor)` = `cTSSimulator::Spend`; rodada n37, §7.25: é ela que
cobra, devolve no desfazer e cobra de novo no refazer). O
`cTSUserToolBuyBuild::SubmitUndoable` (`0x9b3b94`) ainda chama, sem "não
cobrar", a vtable `0xa0(−TotalCost)` do objeto em `+4` da ferramenta (§7.25:
não é o simulador). Nem o gerente nem `RedoIt` testam o dinheiro: o refazer
não é recusado por falta de fundos. O `0x30` é o custo do próprio
undoable (o `float +0x58` arredondado; 0 sem cobrança) e o `TotalCost` (`0x34`,
`0x9b5568`) soma o dele e o de cada pré/pós-operação.

**Um gerente só, esvaziado ao entrar no modo Vivo.** Toda ferramenta de compra
e construção pega o mesmo gerente (`cTSUserToolBuyBuild +0x44` = sim system
vtable `0xb8`, no `0x9b36e0`; as escadas modulares também, no `AddObject`). O
`cTSMainToolbar::SetMode` (`0x1ade24`) cria o objeto do modo novo: o do modo
**2 (Vivo)** (`0x159ff64`) manda a mensagem `0x8B` (o lote mudou: valor do lote)
e chama `FlushCommandQueues`; os dos modos 8/9 (`0x159b7a8`) e 11 (`0x159c05c`)
também esvaziam. Os de Compra (3, `0x159ebc4`) e Construção (4, `0x159e9dc`) e
seus destrutores (`0x159726c`…) não mexem no histórico: **trocar entre Compra e
Construção mantém uma pilha só**, e um Ctrl-Z na Construção desfaz a compra
feita antes. No app (rodada n20): `app/undo_order.hpp` guarda de quem é a
última mudança (cada modo desfaz as suas), uma mudança nova em um solta o
refazer dos dois, e só a entrada no modo Vivo esvazia (antes: duas pilhas,
zeradas ao sair de cada modo).
Os objetos vendidos só morrem (`ResetSelf`) quando o histórico é solto, ou seja
ao voltar ao modo Vivo.

**Escadas modulares: o +0,75 e a "elevation".** `UserPlaceModularStairs`
(`0x9edcf8`) soma 0,75 à altura de cima quando `FUN_0016854c(tile de cima)`:
algum quarto do tile, **no nível dele**, com `DeckFoundationStyle` do tipo 4
(`nTSWorld::HasDeck` = `IsDeckFoundationOfType(.., 4)`; `IsPool` = 3 ou 5).
**Correção (rodada n37, §7.25):** o tipo 4 é o **palco** do BV (deck é 1,
fundação 2, piscina 3), apesar do nome `HasDeck`: no jogo base nenhum lance
ganha o 0,75. Nos lotes (3ARY `0x14`, `u32` por quarto,
−1 nenhum, níveis a partir do −1 como as alturas) os marcadores ficam no
**nível 0**, debaixo do piso levantado (o nível 1, já a +0,75: `N001_Lot68`,
decks 1 e fundações 0 no nível 0, piscinas 3 no −1): um lance que sai do piso
levantado (nível 1) não ganha o 0,75. Só ganha quem começa no tile do nível 0
de um deck (no fim, `FUN_00164a7c` tira 0,75 do resto se o ponto final toca
um quarto de deck). A "elevation" que o jogo salva é o `float +0x20c` do
objeto (`WriteHeader` `0xb07790`), o valor que `SetPosition` (`0xafd048`)
recebe: para os degraus, **a altura absoluta** que o lance calcula
(`SampledFloorElevation` do topo − k × 0,1875), passada a `AddObject`
(`0x3ffc28`) → `TrySetPosition` (`0x35cdd0`); patamares e reservas, 0.

**Portas e janelas sem máscara.** `ComputeWallCutoutLocators` (`0x6612d8`):
com o texto do STR# `0x85` vazio **não há recorte nenhum**; senão, para cada
parte e lado, o nome da máscara vai ao `SetCutoutMaterial` (`0x6c5d6c`), sem
caixa retangular: o jogo só abre a parede pela máscara (rodada n36: feito,
`walls-floors-mac.md` §6.3).

**No código (rodada n20).**

- `BuildEditor::vertexMoveError` (`vm/build_edit_ground.cpp`) é o
  `FUN_009e9550` com as máscaras do jogo (`vertextest::kWalls` = `0x2BB7`):
  borda/travado `0x62`, limites, piso em volta `0x5A`, o andar de cima/baixo a
  menos de 0,75 (`0x69`/`0x6A`), parede com fundo/topo plano no vértice
  (`0x5B`/`0x5C`), piscina, fundação ou deck em volta (sem código), cerca
  íngreme `0x31`. Falta o bit 7 (objetos que prendem o chão). Usam: o
  nivelador das paredes e do cômodo (`levelForWall`: água `0x47`, os andares de
  cima juntos), as escadas modulares e o pincel de terreno (`ChangeTerrainElevation`
  com `0x2BF7`, os mesmos testes: a ponta de uma parede fica, a de uma cerca
  anda; antes `terrainFree`, inferido). As fundações e piscinas
  (`MayPlaceQuadrilateral`, não lido) seguem com a regra antiga (`levelPoints`).
- Paredes, cercas e demolição arrastadas: tudo ou nada; o cômodo aplaina o
  retângulo inteiro; a altura é a do vértice de início no nível dele.
- Colocação: `IsFlat` (`0x12`) e os quatro quartos com piso, pelo
  `VM::placementLot`; escadarias com o `CeilingTooHighForObject` (`0x30`,
  `BuildEditor::stairCeilingError`, no modo Construção e no `s2pkg`).
  `s2pkg placement-sweep`: dos 12.930 objetos dos 62 lotes testados onde estão,
  o relevo recusa 14 que cabiam (13 oleandros e uma escada de piscina em
  rampa: inferido, terreno mexido depois deles) e nenhuma escadaria; o teste de
  altura do od 4 (bit 0 no chão) não recusa nenhum (o `0x2c(1)` do objeto
  `0xFBA4DA53` segue não lido).
- Escadas modulares: a "elevation" salva é a altura absoluta; sem o +0,75
  (deck no próprio nível do tile de cima: o editor só aceita topo acima do
  térreo).
- Desfazer: uma pilha para Compra e Construção (`app/undo_order.hpp`),
  esvaziada ao entrar no Vivo; soltar o refazer mata o que as mudanças
  desfeitas tinham tirado do mundo. Prova: `tests/build_undo.sh` (desfazer e
  refazer gravam o lote byte a byte igual: 9 edições no `N001_Lot11`, parede
  com porta no `N002_Lot23`, o lance do `N001_Lot38`).
- Portas e janelas sem máscara: o app segue com a caixa retangular quando não
  acha o TXMT (`formats/lot_model.cpp`, área da rodada 19); no jogo não haveria
  buraco (TODO).

### 7.25 Fundações, piscinas, marcadores, objetos no vértice, dinheiro do desfazer, salas e lareira (rodada n37, Ghidra MCP)

Leitura do BV com o Ghidra MCP (só leitura). Onde o Ghidra não tem função
(inicializadores estáticos, os handlers do parser, o gerente de desfazer) os
bytes foram lidos do `sims2_bv.ppc` com um desmontador mínimo (rascunho). Os
nomes entre aspas vêm do `baserom.sym` (`sims2_mac_decomp`), que dá os nomes
das funções anônimas da ferramenta (`UserPlaceDeckFoundation`,
`MoveFloorElevations`, `SetDeckFoundationMarkers`, `cTSUserToolUndoable::AdjustFunds`).

**Os tipos dos produtos.** O parser do `deckfoundations.txt` cria o produto e
chama `SetType` (vtable `0xc8`, `0xf21c40`: `+0xbc`) com **1 para `deck`, 2
para `foundation`, 3 para `pool`** (handlers `0x1bf3930`, `0x1bf4158`,
`0x1bf3ea0`; `li r4,N` antes da chamada); o `stage` do BV é o 4 (e a piscina
curva, 5: `IsPool` = 3 ou 5). `cTSProductDeckFoundation::Type` (`0xf220b0`) e
`StyleID` (`0xf220b8`, `+0xc0`: o número do produto, o que vai no 3ARY `0x14`).
A ferramenta guarda o tipo em `+0x70` e três bytes no `Init` (`0x354a64`):
`+0xbc` fundação (tipo 2), `+0xbd` piscina (3), `+0xbe` palco (4).
**Correção da §7.24:** o `nTSWorld::HasDeck` (`0x168400`) e o `FUN_0016854c`
perguntam o **tipo 4, o palco** (só BV), não o deck: o +0,75 das escadas
modulares e o `0x99` são de palco; `FUN_00168140` é o "tem deck" (tipo 1) e
`FUN_001688f8` o "tem fundação" (tipo 2). No jogo base nada é do tipo 4.

**A altura da ferramenta** (`+0xa8`). `OnClick` (`0x34f214`) arredonda o
ponto ao vértice (`FUN_0015dc5c`) e `StartOperation` (`0x34eb18`) guarda o
começo em `+0x8c` e a altura em `+0xa8` = `UserDeckFoundation::CalculateTopElevation`
(`0x9ad4e4`, nomeado errado `cTSUserToolAwning::OnRelease`): palco, a altura
do piso ali; senão, no térreo, se nenhum tile em volta do vértice do começo
tem marcador (`FUN_001633fc` = `HasDeckFoundationVertex`: algum dos quatro
tiles no lote com `DeckFoundationStyle` ≠ −1) ou algum quarto em volta é de
palco (`FUN_00164a7c`), **altura do vértice + 4 × `FloorElevationUnit` (0,75)**,
e piscina **+ 0**; com marcador em volta, **a altura do piso do andar de
cima** ali (o deck novo continua na altura do que já existe).

**`MayPlaceQuadrilateral` (`0x352a30`), na ordem** (os cantos: `+0xc4` o
mínimo, `+0xd0` o máximo, vértices; os tiles vão de `+0xc4` a `+0xd0` − 1):

1. `SelectionIsDegenerate`; cada canto fora do `IsInBounds` ou travado → `0x02`;
2. **cada vértice do retângulo** (`+0xc4` a `+0xd0`, inclusive), no andar de
   cima para deck e fundação e no próprio andar para piscina, cuja altura
   (`WorldDB 0xd8`) não é a da ferramenta tem de passar o teste de vértice
   `FUN_009e9550(+0xa8, vértice, flags)` com **`0x2BB7`** (a global
   `0x02b8a698`, posta pelo inicializador `0x1ee033c`), senão **`0x81`** "Can't
   level terrain" (`0x2F` fora do térreo);
3. `FUN_009dae20` em cada tile (alguma forma do tile, sem código; não lido);
4. fundação: cada objeto do tile que não `ObjectMayBeInFoundation` (`0x34e380`:
   um dos marcadores da pasta de objetos — vir ver, vá aqui, túmulo, §7.26 —
   ou `IsSupport` — OBJD tipo 5 — com `ConformsToCeiling`) → **`0x83`**; palco
   no tile, neste andar ou no de cima → `0x99`; deck: só o palco (`0x99`),
   **nenhum teste de objeto** (os objetos no chão em volta pegam o `0x60` do
   teste de vértice, abaixo); piscina: `CanPlaceTile` por tile e o tile do
   começo com fundação, deck ou palco → `0x86` (§7.18);
5. a lista `+0x9c` (visibilidade do piso; piscina: `0x02`/`0x86`);
6. paredes: de piscina atravessando → `0x46`; uma parede dentro do retângulo
   cujo estilo o nivelador de cômodo não pode apagar (`FUN_009ac8d0`, lido na
   rodada n48: o produto do estilo, `cTSProduct` vtable `0x154(4)`, o
   `deleteTool levelRoom` do `walls.txt`, §7.26) → `0x84`.

**`UserPlaceDeckFoundation` (`0x9b1bc0`), na ordem:** `SetDeckFoundationMarkers`
(`0x9ad76c`), `MoveFloorElevations` (`0x9af1c4`), piscina: `ModifyFloorVisibility`
(`0x9adec8`), `RemoveOldWalls`, as paredes novas pela borda (`0x9b1204`,
`AddNewWalls` `0x9ad054`), `ChangeFloorPatterns` (`0x9ae5b4`), e, fora o palco,
os suportes (`ChangeSupportsInInterior` `0x9af9ac`, `ChangeSupportsAlongEdge`
`0x9b0708`). Apagar (`UserRemoveDeckFoundation`, `0x9b17dc`): marcadores a −1,
suportes, pisos, `RemoveOldWalls` (falhou → desfaz), paredes, visibilidade e
`MoveFloorElevations` de volta.

**Os marcadores (3ARY `0x14`).** `SetDeckFoundationMarkers`: cada quarto (o
iterador `FUN_0015c3e4`, máscaras 1, 2, 4, 8) de cada tile do retângulo, **no
andar do canto** (o térreo para deck e fundação, onde os lotes os têm) e **um
abaixo para a piscina**, recebe o `StyleID` do produto pelo
`cTSUserToolUndoableDeckFoundation::SetDeckFoundationStyle` (`0x41817c`): se
o marcador do quarto muda, `WorldDB 0x6c` o grava, a troca vai para a lista do
desfazer e o custo do undoable (`float +0x58`) soma
**(`DeckFoundationCost(novo)` − `DeckFoundationCost(velho, devolução)`) × 0,25**.
`DeckFoundationCost` (`0x417c30`): o preço do produto do estilo (nenhum
produto: 0), × `RefundRate` (0,8) na devolução, arredondado. Ou seja: o preço é
por quarto que muda, e um quarto de outro produto devolve 80% do dele.

**`MoveFloorElevations` (`0x9af1c4`).** Os vértices de **`x0 − 1` a `x1 + 1`
e `y0 − 1` a `y1 + 1`** (uma volta a mais em cada lado), no andar de cima
(deck, fundação) ou no próprio (piscina, palco), passam a
`TryMoveFloorElevation` (`0x9aef18`): no mundo e dentro dos limites do lote;
apagando, a altura é a do vértice de baixo + `StandardLevelHeight`; se a altura
muda (ou é piscina), o teste de vértice com **`0x2BB7`** (outra global,
`0x02c769d0`, inicializador `0x222a840`) **sem o bit 9** na piscina e **com o
bit 3** (`IsFloorable`) ao apagar fora do andar 1 (ou com `FUN_009aec5c`);
passou → `ChangeFloorVertex` (`0x9acee0`): piscina, o vértice de baixo vai a
altura − 3 (sempre); deck/fundação, o de baixo (o terreno) **só desce**, até a
altura − 0,75, se estiver acima disso; depois o vértice vai à altura com os
andares de cima juntos (`ChangeFloorAndWaterElevationCoupled`). Um vértice que
não passa fica onde está, sem erro (os de dentro já passaram no passo 2). **A
volta de fora sobe também:** nos lotes salvos, 3.139 dos 3.410 vértices do
andar 1 logo fora de uma fundação ou deck estão a +0,75 do terreno (o resto, a
+3); dois tiles fora, quase todos a +3. Por isso um tile vizinho à fundação tem
os quatro vértices a +0,75 e só o marcador diz quem é fundação.

**O bit 7 do teste de vértice (`FUN_009e8e0c`; devolve 1 = recusa).** O bit 12
das flags vai como o `bool` do `HasGroundPinningObjects` (zero em `0x2BB7` e
`0x2BF7`). Para cada um dos quatro tiles em volta do vértice, no andar dele e
no lote (`0xf8`):

- `HasGroundPinningObjects` (`0x16c5b4`): algum objeto do tile que é pessoa
  (OBJD tipo 2) ou **não tem o bit `0x100` do od 42** (com o `bool`, também a
  vtable `0x22c`, `FlattensTerrainOnPlace`, `+0xea` bit 11) → **`0x61`**
  "Can't move occupied terrain";
- se o vértice está acima do andar mais baixo e a altura nova fica **menos de
  `StandardLevelHeight` (− 0,001) acima do vértice de baixo**, o tile um andar
  abaixo com `HasNonCeilingConformingObjects` (`0x16c7a8`: algum objeto que não
  é marcador da pasta de objetos e não `ConformsToCeiling`, vtable `0x1f0` →
  `AggregateConformsToCeiling` `0xafdd94`: o nó `post_trans` do modelo, §7.26)
  → **`0x60`**;
- cada objeto do tile com od 42 bit `0x80`: escada modular (OBJD tipo 11/12),
  os quatro cantos da pegada pela direção (±0,5 de lado, 0,25 para a frente,
  `FloorElevationUnit` de altura nos dois de cima); outros, a posição; se o
  `HypotheticalFloorElevation` (o piso com o vértice na altura nova) passa da
  altura do objeto (+ o do canto) → **`0x68`** "Can't raise terrain above stairs".

**Quem devolve o dinheiro no desfazer.** A vtable `0x68` do undoable é
**`cTSUserToolUndoable::AdjustFunds(valor)`** (`0x9b4a6c`): o simulador
(`FUN_000c7fb0()` vtable `0xc`, a interface `cISimulator` do `cTSSimulator`:
`0x74` `GetFunds`, `0x78` `SetFunds`) vtable `0xb0` = **`cTSSimulator::Spend(6,
valor)`** (`0x499624`: fundos −= valor, o P&L de negócio do BV pela categoria).
`CommitIt` e `RedoIt` gastam `+Cost()` e `UndoIt` gasta **`−Cost()`**
(`neg r4,r3` em `0x9b66a8`): **o desfazer devolve o preço inteiro** e o refazer
cobra de novo **sem testar o dinheiro** (os fundos podem ficar negativos;
`RedoLastCommand` também não testa). `Cost()` (vtable `0x30`, `0x9b48f0`) =
`round(+0x58)`, 0 com "sem custo". O pincel de terreno cobra pela mesma
`AdjustFunds` a cada aplicação (`UpdateFundsForHeightChange`). O
`SubmitUndoable` da ferramenta (`0x9b3b94`) ainda chama a vtable `0xa0(−TotalCost)`
do objeto em `+4` da ferramenta (não é o simulador; não identificado: talvez o
aviso de dinheiro da UI). O `UndoLastCommand` (`0x970910`) só manda a mensagem
`0x98` com os campos `+0x5c/+0x60` do undoable (`0x38`/`0x3c`). Corrige a §7.24
("inferido: a estatística de gastos").

**`SetRoomsPostAttach` em detalhe** (`0xc1f1bc`, §7.3). A aresta nova `e`
(`QuadEdge 0`): a face da esquerda é a vtable `0x18` da aresta e a da direita a
`0x1c`. Ponte (o ciclo da esquerda passa pela `Sym`): as duas faces ficam com a
sala que contém o localizador da aresta. Senão `CreateRoom` (o menor ID livre)
e, se a sala velha não é 0 **ou** `FUN_00c1c480(e)` = 0, a direita fica com a
velha e o **ciclo da esquerda** (de `e`) ganha a nova; senão a esquerda fica
com a velha e o ciclo de `e.Sym` (a direita) ganha a nova. Cada aresta do
ciclo: a face da esquerda vira a sala nova e semeia o lado dela
(`AddSeedToRoomManager`); `RefreshMap` e `AttachWallsToRoom`.
`FUN_00c1c480` é o teste de paridade: pelo lado do localizador de `e` escolhe
um eixo (lado 1 → direção 6; 2, 9, 12 → 4; 3, 6, 8 → 0; 4 → 2) e conta as
arestas do ciclo de `e` (sem ela) que cruzam o raio que sai do localizador
nessa direção (mesma linha, adiante; empate pelo lado); **par = fora** (devolve
1). Ou seja: com sala velha 0, a sala nova é do lado que o ciclo cerca.

**Colocação: a piscina sob o tile.** `FUN_0016d518(tile, testarLote, alt,
visível, porObjeto, piscina)`: no lote (se pedido), `WorldDB 0xb8` (ou `0xbc`)
com a máscara e o `visível`, ou `TileHasFloorFromObject`; e, com `piscina`, o
tile **um andar abaixo** sem quarto de piscina na máscara (`FUN_00164c40`:
`IsPool` do marcador de cada quarto). Os objetos comuns pedem `piscina` = 1:
**no térreo, um tile sobre uma piscina (marcador 3 no andar −1) não tem piso**
(salvo p&`0x200` ou p2&`4`). A camada de visibilidade do piso é do mundo, posta
pelo `ModifyFloorVisibility` da piscina e pelos objetos que escondem o piso
(`HidesFloorOnPlace`); o 3ARY `0x0C` dos lotes (um byte por tile, valores 0–54)
não é ela.

**A ferramenta de lareira** (`cTSUserToolFireplace::TryPlace`, `0x27a38c`).
Põe a cornija como objeto (`cTSUserToolObject::TryPlace`); recusa se a pegada
não fica num cômodo só (`AreObjectBoundsInSameRoom`: erro 3, `0x29`) ou se já
há chaminé atrás que não serve (`IsValidChimneyBehindHeldMantle`: erro 5). O
tile da chaminé é o de trás da cornija (pela direção `0xac`: 0 → y−1, 2 → x+1,
4 → y+1, 6 → x−1). Sobe andar por andar a partir do da cornija enquanto o tile
de cima tem **`HasSupport`**, até o último andar do lote. Põe, de cima para
baixo: o topo da chaminé (`ChimneyTopFP` na última peça, `ChimneyTop` nas
outras; acima do último andar a ferramenta cria um andar novo, `0x1c`), uma
peça de chaminé (`chimneyhearth`) por andar, e, no térreo sobre fundação
(`HasFoundation`) ou terreno baixo (`FUN_0027741c`), o `chimneyFoundation`;
cada peça é ligada à de baixo e à de cima (os mapas `+0x244`/`+0x258`,
`LinkObjects`). Uma peça que não cabe desfaz tudo (`RevertChanges`). Feito na
rodada n48 (§7.26, que corrige o andar de cima: o `HasSupport` é de um quarto).

**Correção do bit 13 do teste de vértice.** O `FUN_009e8124` não lê os
marcadores de deck: pergunta a vtable **`0xcc`** do `WorldDB` (thunk
`0x1511a3c` → `0xd2f34`), que lê outro array por tile (`cWorldDB +0xd0`, um
`u32` por tile, −1 por padrão) ao lado dos marcadores (`0xc8`, `+0xcc`, por
quarto). Nada do jogo base o grava (inferido: os toldos do BV): no base o bit
13 nunca recusa. Sem isso, o "pincel não mexe em volta de fundação" que a
§7.24 tirava daqui não existe; o que segura o chão perto de uma fundação é o
bit 4 (o andar de cima teria de subir junto e tem piso em volta, `0x69`).

**No código (rodada n37).**

- Marcadores: `formats/lot_edit.hpp` `setDeckFoundationMarker` /
  `fitDeckFoundationMarkers` (um plano por andar, do mais baixo ao de baixo do
  topo, seguindo os andares novos), gravados pelo `lot_write` (o 3ARY `0x14`
  sai byte a byte igual ao do `N001_Lot68` quando nada muda). A fundação, o
  deck e a piscina gravam o produto em todo quarto do retângulo (a piscina um
  andar abaixo); apagar grava −1. `BuildEditor::foundationTile` lê os
  marcadores do térreo quando o lote os tem (os vértices da volta também sobem,
  então as alturas não distinguem o tile vizinho); um lote lido sem o array
  ganha um das alturas e das piscinas na primeira mudança.
- Nivelamento: `deckTopHeight` (`CalculateTopElevation`; o começo do
  arraste é o canto do primeiro tile longe do outro extremo, mapeamento
  inferido), `deckLevelError` (os vértices do retângulo → `0x81`),
  `moveDeckFloor` (`MoveFloorElevations` com a volta, `ChangeFloorVertex`),
  `markerCost` (o preço por quarto que muda). Sai o `levelPoints` inferido.
  Fundação: objetos `0x83` (aqui passam suportes e objetos no ar, od 42
  `0x200`: os três tipos do serviço `0x60` não foram identificados); paredes
  estritamente dentro `0x84` (aqui toda parede que não é borda levantada:
  `FUN_009ac8d0` não lido); deck, nenhum teste de objeto; o andar de cima é
  criado se faltar. Ficam do app (inferido): tile travado `0x45`, piso no
  térreo `0x0C`, água `0x1E` (o `CanPlaceTile` só é da piscina). A água do
  chão segue as mudanças de altura (`ChangeFloorAndWaterElevation` →
  `UpdateWaterElevations` com a flag de piso, ponto por ponto: lido na §7.26).
- Bit 7 do teste de vértice: `BuildEditor::vertexObjectsError` (`0x61`,
  `0x60`, `0x68`); o `0x60` com os marcadores e o `ConformsToCeiling` da §7.26. Vale para o nivelador das paredes, o cômodo,
  as escadas modulares, o pincel e o deck. `OS2_DEBUG_VERTEX=1` diz quem
  recusa. O `tests/door_cut_wall.sh` e o `build_undo.sh` vendem antes as
  escadas do deck do `N002_Lot23` (o degrau de cima delas ficava onde o cômodo
  levantaria o andar do deck: `0x68`, como no jogo).
- Dinheiro: refazer não testa os fundos (editor, objetos e Design do modo
  Construção, modo Compra); desfazer devolve o preço inteiro, como antes.
- Salas: `formats/lot_edit.hpp` `postAttachRooms`, chamado pelo `addWall` em
  cada grafo com mapa de salas: dividir uma sala dá o ID novo ao lado
  esquerdo da linha que fecha o ciclo (o `a → b` da parede como gravada; a
  direção que o jogo usa para a aresta é inferida), dividir o lado de fora
  (0) dá ao lado que o ciclo cerca (o ciclo pelo `LNext`, paridade); o
  `updateRooms` mantém o que cada parede deu. O "quartos sem piso nos andares
  de cima ficam fora" do `updateRooms` segue inferido.
- Colocação: `poolUnder` (`FUN_00164c40`): sobre piscina não há piso,
  `0x8E`, salvo p&`0x200`/p2&`4`; objetos de piscina (p&`0x800`) passam
  (inferido: a luminária de piscina do `N001_Lot82`, od 42 `0x0800`, od 82 0,
  fica no térreo sobre a piscina; o jogo passa o teste para todo objeto). O
  `s2pkg placement-sweep` dá as mesmas contas nos 12.930 objetos. A camada de
  visibilidade do piso segue sem fazer (o app não a tem).
- Antes e depois numa encosta do `N003_Lot159` (fundação 23,20–26,22,
  piscina 21,15–24,17): `docs/screenshots/n37-fundacao-encosta-antes.jpg` /
  `-depois.jpg` (a fundação mais alta do lado de baixo, o chão do lado de cima
  cortado), `n37-piscina-encosta-*.jpg` (a piscina num patamar plano, a volta
  aplainada; antes a borda da piscina saía do morro e o chão rebaixado ficava
  sob a água velha, desenhado como fundo de lagoa).

### 7.26 Colocação, resto: objetos, piso visível, altura, água e moirão (rodada n48, Ghidra MCP)

Leitura com o Ghidra MCP (só leitura), no base 1.0 (sem nomes; achado pelo
`0,283` em `0x1581c68`) e no BV (nomes do symxfer). O base vale.

**`TestPlaceInWorld`** (base `FUN_00ffd864`, BV `0x8ad754`): `TestPlacementCommon`
(base `FUN_00ffc9b0`), `TestPlacementWithRespectToWalls` (não para pessoa,
base `FUN_00ffb528`), `TestPlacementWithRespectToObjects` (base
`FUN_00ffb990`) e o teste de altura. Os multi-tile passam parte por parte
(`TSMTObjectCanSetPosition` BV `0x8add68` → `TSObjectCanSetPositionImpl` de
cada parte no seu deslocamento). A ferramenta de compra (`FUN_009eb860`) e o
Create New Object Instance (`TryCreateObject` BV `0xe34b2c`, em `0xe353f0`)
chamam `TSObjectCanSetPosition` com **flags 0** e os dois floats 0 (pelo ABI
do Darwin os floats ocupam r5/r6; as flags vão em r7 = 0): sem o círculo das
portas (flag 2) e sem o círculo da pessoa (flag 1), tolerância 0 nas pegadas.

**O teste de altura.** O objeto `0xFBA4DA53` (CLSID; interface `0xFBA4DA4D`,
fábrica do base em `0xbc3550`) tem a vtable da interface em `0x15dab60`
(thunks `this − 0x20`); o slot `0x2c` → `0xbc362c` → `0xbc3650`:
**`1 << (altura − 1)`**. O `0x2c(1)` do `TestPlaceInWorld` é **1**: o od 4
precisa do bit 0 "chão", senão `0x19` (lista de lugares). O app já fazia
isso; agora confirmado.

**`TestPlacementWithRespectToObjects`, base 1.0 `FUN_00ffb990`** (o BV
separa em `TestPlacementWithRespectToObjects` `0x8ace04` e
`TestPlacementWithRespectToObject` `0x8ac158`, mesma lógica; o BV junta os
objetos num conjunto e, com pegada estendida, olha os tiles da pegada):

1. pessoa com o dado de pessoa 68 bit `0x02` → passa tudo;
2. a pegada do objeto no lugar e na direção testados (`GetFootprint`,
   vtable base `0xe8` / BV `0x100`);
3. para cada objeto nos 3×3 tiles em volta (o nível do lugar, no lote,
   `ObjectOnTileAtIndex`), que não seja parte dele (`IsPartOfMe`):
   - **no tile do próprio lugar**, o teste exclusivo (`FUN_00ffb7b0`; BV
     `0x8a74fc`, com o nome errado `HasEnoughClearanceFromRoof`) com os od
     63 (STR# `0xFB`: 1 chão, 2 parede, 4 "no other object of same type", 8
     teto) `a` e `b`: `a & b & 9` → `0x0C`; com 4 em algum, o outro **no
     mesmo lugar** (x e y a menos de 32 ULPs, mesmo nível: `FUN_0142e734`),
     com a mesma `Elevation` (vtable `0xb0`) e o mesmo produto (base `0xd8` →
     `0x6c`; BV `GetSelector` `+0x224` → `0x70`) → `0x0C`; se os dois exigem
     uma parede em comum (`FUN_00ffb62c`: cada `RelativeWallOrFenceRequired`
     — a n-ésima das flags 1, 2, 4, 8, `0x10`, `0x40` do od 13, BV
     `0xaf61b8`/`NumWallOrFenceRequiredFlags` `0xaf50dc` — virada pela
     direção, e o `OppositeWallLocator` dela, contra as do outro): os dois
     portas ou janelas (`ExclusiveWallPlacementCategory` BV `0xaf57a4`: 1
     para os tipos 8 e 9) → **`0x17` "Must be against unused wall"**, senão os
     dois com 2 → `0x0C`. O BV acrescenta testes do od 82 (expansão);
   - objeto pessoa: passa se o outro ou ele `CanIntersectPeople` (od 8 &
     `0x14`, BV `0xaf56f0`); objeto contra pessoa: passa se ele
     `CanIntersectPeople`; objeto contra objeto: passa se um dos dois
     `CanIntersectObjects` (od 8 & `0x06`, BV `0xaf56b8`; o BV tira as
     janelas de sacada, expansão);
   - a árvore **"allow intersection"**: `RunTree(5, ID do outro, nenhum
     parâmetro, true, false)` (BV `0xafe524` → `0xb02278`: o outro é o stack
     object, sem parâmetros, rodada até o fim), primeiro a do objeto, depois
     a do outro; verdadeiro → passa;
   - camadas: só testa se os dois `FootprintBlocksCeiling` (od 42 `0x1000`)
     ou os dois `FootprintBlocksFloor` (o bit apagado);
   - duas pessoas: centros a menos de `0,283` → `0x0C`; senão, se o objeto é
     porta diagonal (`FUN_00fffc88`: tipo 8 com od 13 & `0x50`): a pegada do
     outro contra as pegadas **0 e 2** da porta (`DoesFootprintIntersectDoor`
     BV `0x8a8fdc` → `TSObjectIndexedWorldSpaceFootprint` `0x8a8c64`: a lista
     "footprint" de nome "0" / "2" do modelo, todos os tiles, vtable `0x2cc`;
     sem a lista, não testa); senão, o outro pessoa e o objeto sem o od 40
     `0x4000` ("Low Small Object"): **o círculo de raio 0,3** em volta da
     pessoa contra a pegada do objeto (`FUN_0154da9c` / BV `0x15b2448` →
     `0x15b2090`: dentro de 0,3 nos dois eixos e, no canto, a menos de 0,3 do
     canto); senão (flags 0), o objeto não pessoa contra uma porta diagonal:
     a pegada dele contra as 0 e 2 da porta; senão **as duas pegadas**
     (`FUN_0154d88c` / BV `0x191f914` → `0x191f43c`: algum retângulo não vazio
     de uma cruza, estrito, um não vazio da outra) → `0x0C`;
4. objeto que não é pessoa (ou com a flag 2): o anel 5×5 em volta do 3×3,
   **só portas diagonais**: o objeto ou o outro porta diagonal, nenhum dos
   dois `CanIntersectObjects`, nenhuma árvore 5 deixa, uma camada; a pegada
   contra as 0 e 2 da porta → `0x0C`.

**A camada de visibilidade do piso** (`cWorldDB +0x9c`, um byte por quarto;
`SetFloorVisibility` BV `0xd8ed4`, slot `0x48`; `FloorVisible` `0xd29bc`).
Quem a escreve: o objeto do grafo de cena, em
`nTSSG::cObject::UpdateVisibilityAndRendering` (BV `0x661e5c`), quando o od 34
& `0x25` muda: `SetFloorVisibilityForHiddenObjects(visível = od 34 & 0x21 ==
0)` (`0x661c28`) põe, para cada parte do grupo que `HidesFloorOnPlace`
(vtable `0x220`, `+0xaa` bit 14 = **od 8 bit `0x4000` "Hide Floor"**, STR#
`0x8E` #14), os quatro quartos do tile dela **invisíveis** enquanto o objeto
está visível (e visíveis quando ele se esconde). No jogo base: a banheira
de hidromassagem embutida (as 9 partes) e as janelas de dois andares. A
piscina: `ModifyFloorVisibility` (`0x9adec8`; undoable `0x4c16786f`,
`cTSUserToolUndoableFloorVisibility`) põe invisíveis os tiles do retângulo no
andar dos cantos ao pôr (`UserPlaceDeckFoundation`, `b = 1` → `!b`) e visíveis
ao apagar. Há também uma ferramenta `cTSUserToolFloorVisibility` (BV
`0x3644cc`). Quem a lê na colocação: `HasFloorTest(tile, máscara, todos,
visível)` (`FUN_000d2798`): com "todos", cada quarto da máscara com piso **e**
visível; com "algum", o primeiro quarto com piso basta (a visibilidade nem é
lida). **Onde ela fica** (`cWorldDB::Load`, BV `0xd7758`): a camada é o **3ARY
`0x15`** do lote (um byte por quarto, 1 visível, 0 escondido; um lote sem ele
lê o antigo 3ARY 2 e o traduz, `TranslateDeprecatedFloorVisibilityLayer`).
Nos lotes do jogo só há 0 e 1: 2.940 quartos escondidos, nos lotes com
piscina (o térreo sobre a água) e com banheira embutida (os 9 tiles dela,
com o padrão de piso guardado por baixo: N001_Lot82, N002_Lot29,
N003_Lot192). O `SetPosition` (BV `0xafd048`) esconde o tile de um objeto
`HidesFloorOnPlace` e o `RemoveFromWorld` (`0xafc508` → `FUN_00af75b8`) o
mostra de novo. Os outros arrays que o `Load` lê: `+0x90` o 0 (pisos),
`+0x98` o 1 (alturas), `+0xa0` o 3 (travados), `+0xa4` o `0x0C`, `+0xcc` o
`0x14` (marcadores), `+0x94` o **`0x20` (rotação dos pisos)**, `+0xd0` o
`0x1b` (BV). Os slots do `cWorldDB` (vtable `0x27a326c`): `0xb0` = (todos,
visível do argumento), `0xb4` = (algum), `0xb8` = no nível 0 sempre sim, senão
(todos), `0xbc` = no nível 0 sempre sim, senão (algum). O piso do
`TestPlacementCommon` é `0xb8(tile, 0xF, !HidesFloorOnPlace)` (base `0x90`):
fora do térreo, um tile sob um objeto "Hide Floor" não tem piso para outro
objeto (`0x8E`). Os outros bits do od 8: `0x1000` "Remove Floor"
(`RemovesFloorOnPlace`, vtable `0x224`: as partes de cima das escadarias) e
`0x2000` "Add Floor if Required" (portas e portões).

**`TestPlacementCommon`, base 1.0 `FUN_00ffc9b0`** (os códigos do base são
os do BV com 4 a menos de `0x88` em diante: base `0x8A` = BV `0x8E`, `0x88/0x89`
= `0x8C/0x8D`, `0x8B` = `0x8F`, `0x8C` = `0x90`, `0x75` = `0x79`; o app usa os
do BV): 1 `WorldDB 0xc4(x, y)` (no mundo) → 2; 2 nível do lote → 3; 3 pessoa:
piso (`0x90(tile, quarto, 1)`) salvo p&`0x200`, e não sobre piscina; objeto:
salvo p&`0x200`, `0x90(tile, 0xF, !HidesFloorOnPlace)` e não sobre piscina
(`FUN_01036c90`, o andar de baixo com marcador de piscina) → `0x8E` (o
`TileHasFloorFromObject` do BV, od 98 bit 1 dos elevadores, e o p2&4 não
existem no base); com p&`0x2000` e um moirão de cerca no lugar, os testes 4,
5 e 7 não se aplicam; 4 parte na água (`IsPartWater`: algum vértice
`IsWaterVertex`, só no térreo, BV `0xd25d8`) sem p&4 nem p&`0x4000` →
`0x90`; 5 tile de piscina sem p&`0x800` → `0x8F`; 6 `IsFlat` sem p&`0x100` →
`0x12`; 7, salvo tile todo na água (`IsAllWater`), de piscina ou sobre
piscina: algum quarto com piso (`0xb4`) sem p&1 → `0x8C`, terreno (`0xb8` e
não `0xb0(tile, 0xF, visível)`: no térreo, sem os quatro quartos com piso e
visíveis) sem p&2 → `0x8D` (sem a exceção de p&`0x200` que o app tinha); 8 od
30 (1 só fora → `0x0A`, 2 só dentro → `0x0B`: a sala de definição 0 do
gerente de partições no lugar, `0xe4(lugar, 0, 0)`, fora quando é 0,
`FUN_0154dda4`; o BV usa a definição 1 para o "só dentro", `0x13c(lugar, 1,
0)`; feito na n48 com o 3ARY `0x0B` no quarto do lugar); 9 tile travado → 2; 10 p&`0x2000` sem moirão de cerca no lugar (o
`0xec` do gerente de partições e `PartitionStyleIsFence`) → `0x79`; 11
altura livre e teto (`0x2F`, `0x30`, não feitos salvo a escadaria). O
`IntersectsWalls` das paredes é pulado com p&`0x400` ou p&`0x2000`.

**No código (rodada n48).** `vm::checkPlacement` faz o
`TestPlacementWithRespectToObjects` do base para a compra **e** para o Create
New Object Instance (antes a compra usava camadas por tile, inferido): as
pegadas de cada parte são tiradas no lugar testado (o grupo é posto lá só
para a leitura; uma parte já no tile testado fica onde está),
`VM::indexedFootprint` dá as listas 0 e 2 das portas; o piso visível vem dos
objetos "Hide Floor" no mundo (`floorHidden`); a água, o piso/terreno e o
moirão (as `fencePosts` do lote) com a geometria do lote
(`VM::placementLot`); uma parede com porta conta como parede (o mundo do VM
a abre para as rotas). `s2pkg placement-sweep` (os 12.930 objetos dos 62
lotes testados onde estão, com a geometria; `S2PKG_SWEEP_WHY=all` lista cada
recusa): **antes** cabiam 11.362 (`0x02` 571, `0x07` 781 — as portas,
`0x08` 19, `0x0C` 122, `0x12` 14, `0x8E` 61); **depois** 12.266 (`0x02` 571 —
os portais, a caixa de correio e o lixo da faixa da rua, que o jogo põe sem
teste —, `0x07` 1, `0x08` 12, `0x0C` 39 — flores e arbustos no mesmo tile,
postos pelos autores dos lotes —, `0x12` 14, `0x8C` 15 — flores sobre piso —,
`0x8D` 10, `0x8E` 2). As plantas de lago passaram (o tile todo na água), as
lâmpadas de moirão também. Com o od 30 (a sala de definição 0, 3ARY `0x0B`, em
que o quintal cercado é fora): 12.231, os 35 novos `0x0B` são luminárias de
teto e plantas penduradas "só dentro" em varandas sem paredes (as três salas
do lote dão 0 ali), postas pelos autores dos lotes; pela sala do 3ARY `0x09` (o
quintal cercado é sala) 604 árvores e arbustos davam `0x0A`. Sem a geometria
do lote o teste não é feito. No `tests/buy_place.sh` a cadeira não vai mais no
tile da porta do tutorial (as pegadas das partes da porta tomam os dois
tiles, como no jogo). Bateria (14 sementes × 12.000 ticks, `s2pkg autonomy`):
N001_Lot68 autônomas terminadas verdadeiras 638 → 641, falsas 19 → 19;
N001_Lot11 a mesma saída. A diferença vem dos `ReserveTile (Passable)` que o
Create New Object Instance agora põe sob um Sim (a árvore 5 deixa).

**A ferramenta de lareira, lida de novo** (`cTSUserToolFireplace::TryPlace`
BV `0x27a38c`; corrige o parágrafo da §7.25). Depois do `TryPlace` de objeto e
do `AreObjectBoundsInSameRoom` (erro 3), uma cornija **multi-tile** (vtable
`0x19c`) com chaminé atrás (`IsChimneyBehindHeldMantle` `0x278a34`: uma peça de
tipo 2 ou 3 no tile atrás de alguma parte, pela direção `0xac` de cada parte)
não faz peças: vai na frente da chaminé (abaixo). Senão:

- **O andar de cima.** Do andar da cornija para cima enquanto
  `HasSupport` vale no **ponto da cornija** (`FUN_0015d84c` faz o `cFloorTile`
  do ponto: o tile e **um quarto só**, o do ponto, `FUN_0015d2f0`: 1 oeste,
  2 norte, 4 leste, 8 sul; o centro do tile cai no **norte**), até o último
  andar do mundo (`0x144`); T = esse andar, não abaixo do da cornija. (Era o
  tile todo: uma parede de outro lado do tile subia T um andar, 11 das 48
  lareiras salvas.)
- **Um andar só (T ≤ andar da cornija):** a altura do andar (`SampledFloorElevation`
  de T+1 menos a de T no tile da chaminé) tem de ser ≥ `StandardLevelHeight`
  (3), senão nada; abaixo do último andar o `ChimneyTopFP` em T+1 com elevação
  0; no último, se T < `FUN_00163808` (o máximo do mundo, 4 sem `WorldDB`), o
  mundo ganha um andar (`SetLevelRange`, vtable `0x1c`) e o `ChimneyTopFP` vai
  em T+1; senão um `ChimneyTop` em T com a elevação da altura do andar.
- **Mais andares:** as tampas em T, de `floor(max(2, RoofHeightAt − piso de T + 2))`
  até 0 de 1 em 1 (`ChimneyTopFP` na elevação 0, `nearlyEqual`, as outras
  `ChimneyTop`); `RoofHeightAt` (`0xc23a18`, slot `0x60` da vtable de interface
  `0x281b948` do `cWorldRoof`; a primária `0x281b870` o tem em `0x6c`) é o
  máximo, por andar com `IsAnyRoofAtLocation(ponto, true)`, do
  `RoofHeightAtLocation(ponto, false)` (`0xc23f8c`: o maior
  `GetSampledElevationByRoofID(id, x, y, andar, true)` dos telhados do andar,
  a partir de 0; o piso de cima sem telhado).
- **As peças de lareira** (`chimneyhearth`) de T (ou T−1) para baixo até o chão
  (o fogão, tipo 4: até o andar acima dele); abaixo do andar da cornija para
  num tile de cômodo que carrega peso (gerente de partições `0x13c`) ou sob
  telhado (`0x7c` = `IsAnyRoofAtLocation`); no chão, fora do andar da
  cornija, `HasFoundation` do tile todo ou `FUN_0027741c` (o térreo não está
  a `StandardLevelHeight` do chão) dão o `chimneyFoundation`. Só as peças de
  lareira passam pelo `FUN_0027664c` (no chão, não sobre piscina; nenhum
  objeto no tile sem od 42 bit 0x01); as tampas não.
- **Elos:** cada peça abaixo da anterior; a cornija fica **abaixo** da peça do
  seu andar e acima da de baixo (o fogão: abaixo da primeira tampa sem cano).
  `LinkObjects(true)` em cada parte, duas vezes.

**Cornija na frente de uma chaminé** (`IsValidChimneyBehindHeldMantle`
`0x2784a8`; `OnRelease` `0x27e0ac`): atrás de cada parte uma peça de lareira
(tipo 2) ou a tampa com elevação 0 (tipo 3, `nearlyEqual(0xb0, 0)`), e a peça
de baixo dela não é cornija (erro **6**); contagem diferente → erro **5**. O
`CreateFireplaceLinkedListFromPart` (`0x277524`) lista a chaminé de baixo
para cima e o `CreateClonedUndoableListFromIDLinkedList` (`0x27d3ac`) a refaz
peça por peça (cópias com o od 41 da original, viradas para a cornija) e
religa em sequência, a cornija **abaixo da peça atrás dela**. Se a peça é a
tampa (um andar construído sobre o topo da chaminé), a tampa e as de cima
saem e `CreateUndoableListBehindObjectHeld` (`0x27cab8`) refaz o topo atrás
da cornija: o mesmo laço de suporte e as mesmas tampas, peças de lareira de
T (ou T−1) até o andar da cornija, sem fundação e sem `FUN_0027664c`.

**A marreta** (`cTSUserToolSledgeHammer::DeleteFireplaces` `0x3065a0` →
`EraseLinkedFireplace` `0x306454`, único chamador `OnButtonUp` `0x306b04`): a
corrente de cima da peça, depois a de baixo, depois ela, cada uma vendida
(`EraseObjectRecursive`).

**O preço.** Os OBJD das peças custam 0; a cornija salva tem o preço do
produto no od 41 (Loft 2525 nos lotes de N001, Colonial 3000, Pot Belly 1000;
o catálogo dá 2500, 3000, 1000, 600, 2000); onde a ferramenta põe o valor não
foi lido (inferido: o preço do produto na cornija).

**No código (rodada n48).** `BuildEditor::chimneyPlan` / `placeFireplace` /
`deleteFireplace` (`src/vm/build_edit_fireplace.cpp`); o `Op` do desfazer
guarda os elos mudados (`links`). No app, o BA30 (categoria 10, subsort 3) lista
as cinco lareiras de `fireplaces.txt` (sem miniatura: a
`cTSUIThumbnailFactoryFireplace` não foi portada); a cornija vai na mão como
uma porta (`,`/`.` giram), o clique a põe com a chaminé e pega outra (como a
ferramenta de objeto; inferido para a lareira), Ctrl num pedaço é a marreta.
Os erros próprios da ferramenta (3, 5, 6 em `+0x1a0`) não são os do STR#
`0x89`: o app mostra a razão. O VM carrega a elevação salva
(`cTSObject +0x20c`) em `Position::elevation` (antes 0 nos objetos lidos). `s2pkg
fireplace-sweep` (cada lareira salva contra a corrente que a ferramenta faria
onde a cornija está; uma corrente com outras cornijas é comparada sem elas):
das 48 lareiras dos 62 lotes, **36** iguais (antes da correção do quarto, 25);
das 15 em corrente com outras cornijas, 11 iguais (as 4 outras são as
cornijas postas depois, na frente da chaminé); as 8 diferentes restantes são
de N003 (Strangetown) e o lote mudou depois da lareira: as duas de
N003_Lot133 são iguais em N003_Lot179, a mesma casa com as lareiras postas de
novo. `lot-save --delete-fireplace 525 --fireplace 0,16,11,2,2` em N001_Lot68
refaz a corrente salva peça por peça (§2500); em N001_Lot59, a cornija do
andar 2 e depois a do 3 refazem a corrente com as duas; em N001_Lot82, com um
cômodo no andar 4, a cornija nova na frente da tampa refaz o topo. Desfazer
volta os elos. Inferido: o preço na cornija; a pessoa no tile da peça (o
`ObjectOnTileAtIndex(…, true)` do `FUN_0027664c` não lido: a pessoa não
conta); as cópias da chaminé (o jogo troca as peças por cópias novas; aqui
as mesmas ficam) e uma chaminé virada para outro lado (recusada, não feito).
As tampas de N001_Lot11 não cabem onde estão (`0x08`: a cerca do terraço do
andar 4 corta as duas partes), por isso a lareira de lá não se refaz.

**Terreno: os marcadores, o `ConformsToCeiling` e a água ligada** (rodada
n48). O serviço `0x60` do `FUN_000c7fb0` (GZCOM `0x7a3e667d`) é a **pasta de
objetos** (`cTSObjectFolder`, vtable de interface BV `0x27c29e8`): o `0xac` é
`IsSelectorComeAndSee` (`0x4e483c`: o seletor é o "vir ver" normal
`0x4C859106`, o apagado `0xACC53DFF` ou o de decorar `0xEC85F0F3`), o `0xc4`
`IsSelectorGoHere` (`0x4e49fc`: o "vá aqui" `0x000007C4` ou o da piscina
`0xECFEDB37`) e o `0xcc` `IsSelectorGraveMarker` (`0x4e4a74`: o túmulo
`0x6CD4A8BC`); o `0xc8` (`IsSelectorPondGoHere`) fica de fora. No base 1.0 os
mesmos GUIDs (`0xb14a34`–`0xb14ca8`, vtable primária `0x15d7ce8`: `0xbc`/`0xc0`/`0xc4`,
`0xd0`/`0xd4`, `0xd8`; um quarto `GetXxx` de `0x7FD96B54` fora do teste). O
`ConformsToCeiling` (`cTSObject` `0xaff1a4` → `0x508` →
`AggregateConformsToCeiling` `0xafdd94`) pergunta às partes da cena do objeto a
interface `0xaae90adb` (o `QueryInterface` `0x6544b4` do `nTSSG::cObject` dá
`+0x70`); o slot `0x1c` dela é **`nTSSG::cObject::AttachedToCeiling`**
(`0x6542f0`): o modelo tem um nó **`post_trans`** (`FUN_009466bc`, busca por
nome na árvore). Dos suportes (OBJD tipo 5) dos 62 lotes só a coluna do deck
(`Column - Square Brick`, modelo `deckSupport`) o tem; as outras sete colunas
(Art Nouveau, Club, Colonial 1, Designer, Moroccan, Quaint, Value1) não: não
entram numa fundação (`0x83`) e um tile com elas sob um teto baixo recusa o
vértice (`0x60`; antes todo suporte passava, inferido).

`UpdateWaterElevations` (`0x8b185c`) com a flag de piso vem de
`UpdateWaterElevation` (`0x419000`) **um vértice por vez**, logo depois de
`ChangeFloorElevation` (só no térreo). As entradas (`GetWaterHtChangeEntry`
`0x8b1098`: terreno, água antes e depois, flags 1 feito / 2 lagoa);
`FUN_008b02d4`: um ponto que **não tinha** a flag de lagoa (`+0xd8`, posta pelo
`UpdatePondFlag` `0x8aee90` da vez anterior: terreno − água ≤ 0,25) fica com a
água 1 abaixo do chão; o que fica com a água até 0,25 abaixo do chão é lagoa.
Depois, para os dois caminhos: `ExpandWaterHtChangeArrayToIncludeConnectedPreExistingWater`
(`0x8b16cc` → `AddPondNeighborsOfVertsToChangeArray` `0x8b150c`: os vizinhos
`IsPondVertex`, `0x8af560`, terreno − água ≤ 0,25, como lagoa, de novo em
volta dos novos); cada entrada de antes: sem lagoa, feita; lagoa,
`ChooseWaterElevationsForPondWithVertex` (`0x8b11ec`: o grupo de lagoa, **cada
vizinho vira entrada**, a água 0,25 abaixo do menor chão dos vizinhos que não
são lagoa, o ponto mais de 0,25 acima dela seco); por fim
`AdjustWaterElevationsAtNonPondVertices` (`0x8afbe4`) em **todas** as entradas
(também as da borda criadas no passo anterior): o que não é lagoa recebe a
água do primeiro vizinho lagoa (ordem `DAT_02737e20`/`e00`) − 0,1 ou o chão − 1.
Antes o app tratava só os pontos secos e não mexia nas bordas; a ferramenta
de água (sem a flag) também ganhou as bordas: dois traços de água em
N001_Lot68 (`lot-save --terrain water,8,30,60,2 --terrain water,10,30,40,1`)
mudam 61 dos pontos da água (a borda 0,1 abaixo da lagoa, −0,35, em vez de 1
abaixo do chão, −0,5; um ponto da borda passa a lagoa, −0,25). Inferido: as
flags de lagoa no começo de cada mudança (aqui: terreno − água ≤ 0,25, como o
`UpdatePondFlag` deixaria); um lote sem o array de água não ganha um com a
flag de piso.

**`FUN_009ac8d0`** (BV; chamado pelo `MayPlaceQuadrilateral`, pelo
`SetAwningMarkersRecursive` e por `FUN_002af4f4`): o produto do estilo da
parede (`FUN_000c7fb0()` `0x40` → `0x5c(estilo)`) com a vtable `0x154(4)`. O
`4` é o bit de `levelroom` na tabela do `deleteTool` (`0x28660d0`: `partition`
1, `roof` 2 — o ponteiro `0x102c6c4` —, `levelroom` 4, `modularstairs` 8):
o nivelador de cômodo apaga a parede. No `walls.txt`: a fundação (23), a 16,
as saias de deck (24, 4) e a parede de piscina (29). O app tinha só as bordas
levantadas (23, 16, 4): a saia 24 e a parede de piscina dentro do retângulo
davam `0x84`.

---

## 8. Base vs BV

Base (strings no Mac 1.0): `ObjectThumbnails.package`, `UIObjectThumbnails`,
`showCatalogID`, `moveObjects`. As tabelas de rótulos e os textos de erro estão
nos dados base. Só BV: modo atacado/negócios (OFB: `nTSBusiness`, od 105/106),
coleções, praias, lagoas, estufas, toldos, saunas e portões de garagem, a
máscara de EP no campo 105, as notas 6/7 do catálogo e os seletores
diagonal/alinhado (campos 21–24, `SwapDiagonalObject`). Comunitário: os botões
existem no script base. O campo 100 é lido no BV, sem conferência no base.

Construção (constscan e strings no base 1.0): meias-paredes, estufas,
paredes de palco/invisíveis, espaçamento de postes, toldos (CLSID
`0x303AB8DF`), os cômodos diagonal e octogonal (teclas `0x714798EE` etc.) e o
modo de quarto de tile (`0xB1265EF3`) são só BV. Existem no base: a ferramenta
de parede e a de cômodo, o recorte `_%s_%s_%d_%s_%s` + `wallMask`, os
undoables de parede, elevação e piso (`0xEC167825`, `0x6C167887`,
`0x4C16787B`), a rotação de piso (`0xACB3A629`), os arcos de cerca e os
scripts de catálogo.

## 9. Para implementar (ordem sugerida)

1. Catálogo: produtos de todos os OBJDs líderes com campo 39 ≠ 0, nome e
   descrição do CTSS (grupo, campo 41), preço = campo 18, ordem por preço,
   filtros §1.3 e botões §1.4.
2. Ferramenta: criar o objeto ao escolher, seguir o cursor com passo de
   tile, girar em passos de 2 (90°), testar com §3.2–3.4 (começar por 1, 2,
   6, 8, paredes e pegadas), slots (§3.1 passo 4), e cobrar ao soltar.
3. Vender = reembolso do od 41 (metade se quebrado). Desfazer guardando o
   objeto fora do mundo até sair do modo.
4. Depreciação diária no `DayPassed` (§2).
5. Construção: parede em linha de 8 direções a partir de vértices (§7.2), 70
   por trecho, apagar com Ctrl a 56 por trecho vendendo o que está preso;
   cômodo com Shift; testes `0x4C`, `0x4B`, `0x0C`, `0x1B`; terraplanagem sob
   a parede. Depois pisos (§7.4: retângulo, sala com Shift, 0,25 por quarto,
   aplainar ao soltar), revestimentos (§7.5: face, flood com Shift), salas
   (§7.3) e recortes (§7.6).

## 10. Aberto

- O papel do campo 34 "sale price" (35 em todos): no BV, a única chamada
  `GetField(0x22)` achada (`ArgCall`, `li r4,0x22` + vtable `0x10`) é de outra
  classe (`FUN_00e998bc`, cópia de propriedades); o caminho de compra/venda não
  o lê. Provavelmente não usado.
- Mapa exato B7/B9 (Diversos, Hobbies "recreação") e os IDs dos sons.
- Conferir `TestPlacementCommon` no base 1.0 (sem símbolos).
- O que o teste de altura od 4 compara (máscara do serviço `0xBA4DA53`, vtable `0x2c`).
- Construção (o cômodo, o nivelador e o suporte foram lidos na rodada n20,
  §7.24; `HasSupport` na §7.21): o corpo do "preso
  à parede" (vtable `0x26c`); o custo por
  unidade de altura do terreno (`SetCostForUnitHeightChange`, não decompila).

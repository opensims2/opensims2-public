# Primitivas de SimAntics: o que o binário de Mac confirma

Fonte: o executável da Aspyr para Mac, **The Sims 2 Bon Voyage 1.10
(v61383)**, universal i386 + ppc e **sem SafeDisc** (fora do repositório, em
`references/bv_mac/`). Usamos a fatia ppc, importada no projeto Ghidra
`~/sims2` como `/mac/sims2_bv.ppc`. Os nomes das funções vêm de
`tools/symxfer/` (ver o fim deste documento). Quando nada é dito, cada
afirmação abaixo é **lida no código decompilado** (marcada "binário"); o que
é dedução nossa aparece como "inferido". Onde o comportamento pode ser de
expansão, ele é marcado **"só BV"** (o opensims2 mira o jogo base de 2004).
Para o jogo base, o executável da 1.0 de Mac (`references/base_mac/
sims2_base_1.0.ppc`, Aspyr v2920, 2005) serve de conferência: está quase sem
símbolos, mas tem RTTI.

Convenção: "byte n" é o byte n dos 16 bytes de operandos da instrução (como
no resto do projeto e no SimPE); "u16 n" é um u16 little-endian que começa no
byte n; (dono, dado) é uma referência a variável, lida por
`cTSObject::InterpValue(dono, dado)`.
"Índice + 1 no STR#" quer dizer que o código soma 1 porque a API de STR#
do jogo conta a partir de 1: o operando é a entrada **a partir de 0**.

---

## 1. Como o jogo guarda e lê os operandos

- **Cabeçalho da árvore**: 64 bytes de nome, u16 assinatura (0x8000–0x8009), u16 nº de
  instruções, u8 tipo, u8 nº de argumentos, u8 nº de locais, u8 flags, u32 *tree version*.
  A assinatura **0x8009** (Apartment Life em diante) põe **1 byte a mais** logo depois do
  *tree version* (0 nas 28.988 das 29.014 árvores 0x8009 do `objects.package` do AL; as 26
  restantes valem 0x01/0x07/0x08/0x0C e todas têm a flag 0x10 do cabeçalho) — desconhecido,
  a VM não lê; pulado, todas as 29.014 caem nos alvos (medido na varredura do pacote: alvo
  < contagem ou ≥ 0xFFFC), sem ele nenhuma cai — e as 0x8007/0x8008 do mesmo pacote só caem
  sem o byte. Era o que derrubava a carga com as 17 árvores (item 9 do
  `goal-port-fidelity.md`).
- **`XPrimParam`** (16 bytes de operandos em `+4`, a "node version" em
  `+0x14`). `XPrimParam::Read` lê a versão de nó (só se o formato do BHAV é
  ≥ 0x8005; senão 0), 8 bytes e mais 8 (formatos < 0x8003 só têm 8: o resto
  fica 0). Binário.
- Cada primitiva tem uma subclasse (`SetToNextParam`, `NewReachParam`, ...),
  criada por `cTSBehaviorNode::GetNewPrimitiveByID(short)` (0x004885E0). O
  vtable tem 5 virtuais: `Write`, `Read`, `UpgradeVersion`, `GetVersion` e
  **`SwizzleParams`**. No Mac, `SwizzleParams` troca a ordem dos bytes dos
  campos de 16 e 32 bits (o arquivo é little-endian); `Write` chama ela antes
  e depois de gravar. Por isso **o corpo de `SwizzleParams` de cada classe dá
  exatamente quais bytes são u16/u32 e quais são u8** (tabela na §3).
  Binário.
- `UpgradeVersion` converte operandos de versão de nó antiga para a atual
  (só algumas classes: Relationship, Set to Next, Manage Inventory, Look At,
  Animate Overlay, Change Material, Budget, Dialog, Make Action String, Run
  Tree (chamada de árvore)). As versões antigas não aparecem nas árvores do
  jogo base que já olhamos, mas a VM precisa saber que o byte de versão de nó
  existe. Binário.
- Endereço útil: `cTSObject::InterpValue` é o slot `0x4D4` do vtable de
  `cTSObject`; os temps são lidos/escritos pelos slots `0x4C` (get temp n) e
  `0x50` (set temp n, valor) (inferido pelo uso: Test Object Type grava o GUID
  nos temps 0 e 1 por eles).

**Códigos de retorno** de um `Try<Primitiva>` (`cITSTreeSim::ReturnCode`):
0 = falso, 1 = verdadeiro, 2 = espera (rodar de novo no próximo tick),
3 = empilhou uma árvore (gosub). Binário (Sleep devolve 2 enquanto conta,
Run Functional Tree devolve 3 depois de `GosubObjectTree`).

**Erros**: a primitiva lança um `short` (código de erro). A tabela código →
texto está em `cTSObject::GetErrorString` (0x00B37720, um switch de 0x453
casos); a lista completa está no apêndice A. Exemplos: 0x05 "Primitive not
supported for this type of object", 0x08 "Stack number out of range", 0x15
"No stack object present when required", 0x17 "Could not find object
reference", 0x1B "Slot number out of range", 0x69 "Cant run Reach/Drop on
non Sim object".

## 2. Despacho: quem trata cada opcode

`cTSObject::TryElement` primeiro chama o agregado do objeto (objeto 3D) e
depois `cTSObject::OnTryElement` (0x00B07FB8). Se ninguém trata o opcode,
erro 0x05. Binário.

| opcode | primitiva (rótulo do editor) | handler no binário |
|---|---|---|
| 0x00 | Sleep | `cTSObject::TryIdle` |
| 0x01 | Generic Sims Call | `cTSObject::TryGenericSimCall` |
| 0x02 | Expression | `cTSObject::TryExpression` (o `nTSSG::cObject` antes só observa donos 3/4/0x12/0x13) |
| 0x03 | Find Best Interaction | `cTSPerson::TryFindBestAction` (só Sims) |
| 0x07 | Refresh | `cTSObject::TryUpdate` |
| 0x08 | Random Number | `cTSObject::TryRandom` |
| 0x0A | Tutorial | devolve verdadeiro sem fazer nada |
| 0x0B / 0x0C | Get Distance / Direction To | `TryDistanceTo` / `TryDirectionTo` |
| 0x0D | Push Interaction | `cTSObject::TryPushAction` |
| 0x0E | Find Best Object for Function | `TryFindFunctionalObject` |
| 0x0F | Break Point | `TryTreeBreak` |
| 0x10 | Find Location For | `TryFindGoodLocation` |
| 0x11 | Idle for Input | `cTSPerson::TryIdleForInput` (só Sims) |
| 0x12 | Remove Object Instance | `TryKillObject` |
| 0x13 | Make New Character | `nTSSG::cObject` (0x0065CB14, `TryMakeNewCharacter`) |
| 0x14 | Run Functional Tree | `TryCallFunctionalTree` |
| 0x16 | Turn Body Towards | `cTSRoutingObjectAggregate::TryTurnTowards` |
| 0x17 | Play / Stop Sound Event | `TryPlaySound` |
| 0x19 | Alter Budget | `TryBudget` |
| 0x1A | Relationship | `TryNewRelationship` |
| 0x1B | Go To Relative Position | `cTSRoutingObjectAggregate::TryGotoRelative` |
| 0x1C | Run Tree by Name | `TryCallNamedTree` |
| 0x1D | Set Motive Change | `cTSPerson::TrySetMotiveDelta` |
| 0x1E | Gosub Found Action | `cTSPerson::TryGosubAction` |
| 0x1F | Set to Next | `TrySetToNext` |
| 0x20 | Test Object Type | `TryTestObjectType` |
| 0x21 | Find 5 Worst Motives | `TryFind5WorstMotives` |
| 0x22 | UI Effect | `TryUIEffect` |
| 0x23 | Camera Control | `TryUserEvent` |
| 0x24 | Dialog | `TryDialog` |
| 0x25 | Test Sim Interacting With | `cTSPerson::TryTestInteractingWith` |
| 0x2A | Create New Object Instance | `TryCreateObject` |
| 0x2D | Go To Routing Slot | `cTSRoutingObjectAggregate::TryGotoRoutingSlot` |
| 0x2E | Snap | `TrySnap` |
| 0x30 | Stop ALL Sounds | `TryKillSounds` |
| 0x31 | Notify the Stack Object out of Idle | `TryNotifyStackObject` |
| 0x32 | Add/Change the Action String | `TryMakeActionString` |
| 0x33 | Manage Inventory | `TryInventoryAction` |
| 0x69–0x6C, 0x6E | Animate Object/Sim/Overlay/Stop, Look At | `nTSSG::cAnimatable::try_animate_obj/_sim/_overlay/_stop`, `try_look_at` |
| 0x6D | Change Material | `TryChangeMaterial` |
| 0x6F | Change Light | `nTSSG::cObject::TryChangeLight` |
| 0x70 | Effect Stop/Start | `TryChangeEffect` |
| 0x71 | Snap Into | `TrySnapInto` (o `cObject` antes atualiza o modelo) |
| 0x72 | Assign Locomotion Animations | `nTSSG::cLocomotable::try_assign_locomotion` |
| 0x73 | Debug | `TryDebug` |
| 0x74 | Reach/Put | `nTSSG::cAnimatable::try_new_reach` |
| 0x75 | Age | `nTSSG::cPerson::TryAge` |
| 0x76 | Array Operation | `TryArrayOperation` |
| 0x77 | Message | `TryMessage` |
| 0x78 | RayTrace | `nTSSG::cObject::TryRayTrace` |
| 0x79 | Change Outfit | `nTSSG::cObject::TryChangeOutfit` |
| 0x7A | On Timer | `TryOnTimer` |
| 0x7B | Cinematic | `TryCinematic` |
| 0x7C | Want Satisfy | `TryWantSatisfy` |
| 0x7D | Follow Sim (sem rótulo no editor do base) | `cTSRoutingObjectAggregate::TryFollowSim` — **só BV** |
| 0x7E | Run Script (Lua) | `TryRunScript` — **só BV** |

No **exe do PC** (o build do AL) as duas existem na **classe base**, ao lado do
RayTrace (0x78) e do Change Outfit (0x79) — o gate de tipo de objeto é
"qualquer objeto", não só o routing aggregate (`docs/re-pc-ghidra.md` §(d)).

Sem handler (erro 0x05 se aparecer): 0x04–0x06, 0x09, 0x15, 0x18, 0x26–0x29,
0x2B, 0x2C, 0x2F, 0x34–0x68. Binário. `FollowSimParam` e `RunScriptParam` não
existem na RTTI do executável base 1.0 (as outras 62 classes de operando
existem), daí o "só BV". Binário (base e BV).

Nos opcodes 0x16, 0x1B, 0x2D (rotas) e 0x6A, 0x74 (animação/reach), se uma
flag do sistema de simulação (bit 0 do estado) está desligada, o handler não
roda: a primitiva devolve 2 (espera) e o objeto é marcado para tentar de
novo. Binário (o significado da flag é inferido: "o mundo 3D está pronto").

## 3. Largura dos campos de cada primitiva (`SwizzleParams`)

Bytes que não aparecem como u16/u32 são u8 (ou não usados). Binário (BV).

| opcode | classe | u16 (LE) nos bytes | u32 (LE) nos bytes |
|---|---|---|---|
| 0x00 | `IdleParam` | 0–1 | — |
| 0x01 | `GenericSimCallParam` | 0–1 | — |
| 0x02 | `ExpressionParam` | 0–1, 2–3 | — |
| 0x03 | `FindBestActionParam` | 0–1 | — |
| 0x07 | `UpdateParam` | 0–1, 2–3 | — |
| 0x08 | `RandomParam` | 0–1, 2–3, 4–5, 6–7 | — |
| 0x0A | `TutorialParam` | — | — |
| 0x0B | `DistanceToParam` | 0–1, 4–5 | — |
| 0x0C | `DirectionToParam` | 0–1, 2–3, 6–7 | — |
| 0x0D | `PushActionParam` | 6–7, 9–10, 12–13 | — |
| 0x0E | `FindFunctionalObjectParam` | 0–1, 4–5 | — |
| 0x0F | `TreeBreakParam` | 0–1, 2–3 | — |
| 0x10 | `FindGoodLocationParam` | 5–6, 8–9, 10–11, 12–13 | — |
| 0x11 | `IdleForInputParam` | 0–1, 2–3 | — |
| 0x12 | `KillObjectParam` | 0–1 | — |
| 0x13 | `MakeNewCharacterParam` | 4–5, 7–8 | 10–13 |
| 0x14 | `CallFunctionalTreeParam` | 0–1, 2–3 | — |
| 0x16 | `TurnTowardsParam` | 0–1, 3–4, 6–7 | — |
| 0x17 | `PlaySoundParam` | 0–1, 2–3, 7–8 | — |
| 0x19 | `BudgetParam` | 2–3, 4–5, 7–8 | — |
| 0x1A | `NewRelationshipParam` | 3–4, 6–7, 9–10 | — |
| 0x1B | `GotoRelativeParam` | 0–1, 4–5 | — |
| 0x1C | `CallNamedTreeParam` | 0–1, 2–3, 7–8, 10–11, 13–14 | — |
| 0x1D | `SetMotiveDeltaParam` | 4–5, 6–7 | — |
| 0x1E | `GosubActionParam` | 0–1 | — |
| 0x1F | `SetToNextParam` | 9–10, 11–12 | 0–3 |
| 0x20 | `TestObjectTypeParam` | 4–5 | 0–3 |
| 0x21 | `Find5WorstMotivesParam` | 4–5, 6–7 | — |
| 0x22 | `UIEffectParam` | 1–2, 3–4, 6–7, 8–9, 11–12 | — |
| 0x23 | `UserEventParam` | 0–1 | — |
| 0x24 | `DialogParam` | 13–14 | — |
| 0x25 | `TestInteractingWithParam` | 2–3 | — |
| 0x2A | `CreateObjectParam` | 7–8 | 0–3 |
| 0x2D | `GotoRoutingSlotParam` | 0–1, 2–3, 5–6, 7–8, 9–10 | — |
| 0x2E | `SnapParam` | 0–1, 2–3, 4–5, 6–7, 8–9 | — |
| 0x30 | `KillSoundsParam` | 0–1 | — |
| 0x31 | `NotifyStackObjectParam` | — | — |
| 0x32 | `MakeActionStringParam` | 0–1, 2–3, 12–13, 14–15 | 5–8 |
| 0x33 | `ManageInventoryParam` | 2–3, 11–12, 14–15 | 5–8 |
| 0x69 | `AnimateObjParam` | 0–1, 4–5, 7–8 | — |
| 0x6A | `AnimateSimParam` | 0–1, 4–5, 10–11 | — |
| 0x6B | `AnimateOverlayParam` | 0–1, 4–5, 7–8 | — |
| 0x6C | `AnimateStopParam` | 0–1, 4–5 | — |
| 0x6D | `MaterialChangeParam` | 0–1, 3–4, 6–7, 9–10, 11–12 | — |
| 0x6E | `LookAtParam` | 2–3, 6–7, 9–10, 12–13 | — |
| 0x6F | `ChangeLightParam` | 3–4, 5–6 | — |
| 0x70 | `EffectParam` | 2–3, 4–5, 7–8, 10–11, 13–14 | — |
| 0x71 | `SnapIntoParam` | 1–2, 4–5, 7–8 | — |
| 0x72 | `LocoParam` | 0–1 | — |
| 0x73 | `DebugParam` | 1–2, 4–5, 7–8, 10–11 | — |
| 0x74 | `NewReachParam` | 1–2, 4–5, 7–8, 11–12, 13–14 | — |
| 0x75 | `AgeParam` | — | — |
| 0x76 | `ArrayOperationParam` | 3–4, 6–7, 9–10 | — |
| 0x77 | `MessageParam` | 1–2, 6–7, 10–11, 13–14 | — |
| 0x78 | `RayTraceParam` | 2–3, 6–7, 9–10, 13–14 | — |
| 0x79 | `ChangeOutfitParam` | 2–3, 10–11 | 4–7 |
| 0x7A | `OnTimerParam` | 0–1, 3–4, 7–8, 10–11, 13–14 | — |
| 0x7B | `CinematicParam` | 0–1, 3–4, 7–8, 10–11 | — |
| 0x7C | `WantSatisfyParam` | 1–2, 8–9, 11–12 | 3–6 |
| 0x7D | `FollowSimParam` | 1–2, 3–4, 6–7 | — |
| 0x7E | `RunScriptParam` | 0–1, 2–3, 4–5, 7–8, 10–11, 13–14 | — |
| — | `AnimateOverlayVersion0Param` | 0–1, 4–5, 7–8, 10–11, 12–13 | — |
| — | `LookAtVersion0Param` | 2–3, 4–5, 6–7, 9–10, 12–13 | — |
| — | `NewRelationshipVersion0Param` | 6–7 | — |
| — | `RunPrivateTreeParam` | 1–2, 4–5, 7–8, 8–9, 10–11, 12–13, 14–15 | — |
| — | `UnusedParam` | 0–1, 2–3, 4–5, 6–7, 8–9, 10–11, 12–13, 14–15 | — |
| — | `XPrimParam` | 0–1, 2–3, 4–5, 6–7, 8–9, 10–11, 12–13, 14–15 | — |

Diferenças que pegam a VM hoje (ver as seções abaixo): Random Number tem os
**donos em u16** (dado u16 0, dono u16 2, dado u16 4, dono u16 6), como no
The Sims 1; Snap Into tem um u16 nos bytes 7–8; Reach/Put tem u16 em 1–2 e
7–8.

## 4. Primitivas em detalhe

### 0x74 Reach/Put (`try_new_reach`, `NewReachParam`)

**A animação do corpo vem das listas "Reach Animations" (STR# 0x92)**
(rodada 21, headless Ghidra: `try_new_reach` BV `0x69111c`,
`ComputeReachAnimIndex` BV `0x68ba50`). O índice é a faixa de altura
(< 0,4 chão 0; < 0,7 mesa de centro 1; < 0,9 mesa 2; senão balcão 3; os
`float` 0,4/0,7/0,9 lidos no `__literal4` do binário) + 4 se mais longe que
0,8 + 8 para pôr; a lista global (16 entradas
`a2o-<floor|tableCoffee|table|counter>-<get|put>`) é a última. Os objetos têm
as próprias: o semi-global do xadrez (`7FA1DF44`) tem
`a2o-chessBoard-get/put-<NE|NW|SE|SW>` (alcances **sentados**), a bandeja e o
copo do bar `a2o-barstool-seated-get/put`, o livro `a2o-table/counter-book-*`.
O opensims2 sempre tocava a genérica: no xadrez, "a2o-table-get" em pé com o
Sim contido na cadeira de jantar (o Sim em pé atravessando a cadeira,
`N001_Lot11`). Agora `reachListAnimation`: o objeto alcançado, a superfície e
os semi-globais deles, depois a genérica. **Inferido**: a ordem objeto →
superfície, a faixa pela classe de altura do SLOT (não pela altura em
metros), a distância do Sim ao objeto da superfície, e uma entrada "longe"
vazia caindo na "perto" (a lista do xadrez só tem as perto). Teste: caso
`chess` da bateria de assento. `OS2_DEBUG_REACH` mostra a escolha.

**Pôr tira as poses de carregar que a entrada "grasp" do próprio pôr nomeia**
(rodada 18, inferido: o `try_new_reach` passa os nomes da entrada ao
`AddToAnimList`, cuja operação remove ao pôr): além das que o pegar pôs,
todo overlay com esses nomes sai. A caneca servida na cafeteira vai para a
mão pelas árvores e o "a2o-coffeeMug-carry" vem do "Interaction - Walk and
Sip" (Animate Overlay, entrada sem "(carry)", tipo normal); o "CT - Reach -
Drop" põe com "a2o-coffeeMug-grasp,a2o-coffeeMug-carry" e nada mais parava
a pose: o braço segurava a caneca que estava na mesa até a carona (QA 15).
Teste em `test_reach`.

Operandos (disassembler `CatNewReachParam` + `SwizzleParams`; binário):

| bytes | campo |
|---|---|
| 1–2 | u16 (não impresso pelo editor) |
| 3, 4–5 | o objeto: (dono u8, dado u16) — "Pick up Object in …"/"Drop onto Object in …" |
| 6 | número do slot (de contenção) do objeto, no modo 1 |
| 7–8 | u16 (não impresso) |
| 9 | flags: 0x01 slot no temp 0; 0x02 espera a mão (handedness) no temp 3; 0x04 filtra a animação do objeto pela idade do Sim |
| 10 | modo: 0 pegar o objeto, 1 pôr no slot do objeto, 2 pôr no chão |
| 11–12 | animação de pegar ("grasp"): **índice a partir de 0** no STR# 0x81 (adulto); 0xFFFF = nenhuma |
| 13–14 | animação do objeto: **índice a partir de 0** no STR# 0x86 (objeto); 0xFFFF = nenhuma |

Correção (r4): `cAnimatable::FindSpecificName` soma 1 ao índice porque a
API de STR# do jogo conta a partir de 1 (como os `STR#` do Mac); o operando
é a entrada a partir de 0, como os dados mostram (copo de brinde: grasp 0 =
`a2o-toastingGlass-grasp`).

Execução (binário): só roda em Sims (senão erro 0x69 "Cant run Reach/Drop
on non Sim object"; sem pessoa 3D 0x42, sem animador 0x41). Na primeira
execução zera o "reach object ID" e o "reach surface object ID" do Sim
(`cTSPerson::SetReachObjectID`/`SetReachSurfaceObjectID`) e guarda no stack
frame um ID novo de pedido de animação (`0xA0000000 | mão | contador <<
21`; a mão vem do temp 3 com a flag 0x02); enquanto o pedido existe, a
primitiva devolve 2 (espera). Modo 0 (pegar): objeto que não existe → os
dois IDs = 0xFFFF e **falso**; objeto que já está nessa mão do Sim → IDs =
0xFFFF e **verdadeiro** sem animar; senão reach object = o objeto e reach
surface = onde ele estava. Modos 1/2 (pôr): o objeto é o que está na mão
(`GetContainedObject(mão)`); modo 1 põe no slot (byte 6, ou temp 0 com
0x01) do objeto alvo, que vira o reach surface (slot fora do intervalo:
erro 0x1B); modo 2 põe no chão. A animação vem das tabelas dos bytes 11–14
(tipo de animação errado na string: erro 0x73; sem animação: 0x71; sem o
evento 0: 0x6E). No fim os dois IDs voltam a 0xFFFF. Inferido: a troca de
dono acontece no evento 0 da animação (por isso o erro 0x6E) e as árvores
leem os dois IDs pelos dados de pessoa.

### 0x14 Run Functional Tree (`TryCallFunctionalTree`)

Operandos: função u16 0 (índice + 1 no STR# 0xC9 "functions"); flags u16 2:
0x01 muda o ícone, 0x02 passa os parâmetros da árvore atual, 0x04 roda só a
check tree. Binário.

Execução (binário): o alvo é o **stack object** (sem stack object: erro
0x15). O índice da primitiva não é o índice da OBJf: é traduzido por uma
tabela fixa —

| u16 0 | 0–9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17–23 | 24 | 25 | 26 | 27 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| entrada da OBJf | 0x12–0x1B | 0x0E | 0x1C | 0x10 | 0x11 | 0x1D | 0x1F | 0x20 | 0x24–0x2A | (falso) | 0x2C | 0x2E | 0x35 |

(rótulos do STR# 0xC9: 0 "prepare food" … 9 "stand", 10 "serving surface",
11 "clean", 12 "garden", 13 "wash hands", 14 "repair", 15 "dry clothes", 16
"wash clothes", 17 "message handler", 18 "pre route", 19 "post route", 20
"goal check", 21 "reaction Handler", 22 "along route callback", 23
"awareness", 24 "reset" (sem entrada: sempre falso), 25 "lookatTarget", 26
"Utility State Change", 27 "Global Awareness")

— e pega a action tree dessa entrada (com 0x04, a check tree). Árvore 0 →
falso. Senão `GosubObjectTree(stack object, parâmetros, árvore, mudar ícone)`
e devolve 3 (a árvore roda empilhada, na thread de quem chamou, com o stack
object como "me"; inferido pelo nome). Se o gosub falha → falso.

### 0x1A Relationship (`TryNewRelationship`, `NewRelationshipParam`)

Operandos (versão de nó 1; binário): variável u8 0 (rótulo: STR# 0x102
índice + 1), flags u8 1, de (dono 2, dado u16 3), para (dono 5, dado u16
6), valor (dono 8, dado u16 9). Flags (disassembler): 0x01 "Fail if too
small", 0x02 "Use Neighbor IDs", 0x04 **"Get value from"** o valor (ou seja,
grava a variável), sem 0x04 **"Put value into"** o valor (lê a variável).

Execução (binário):

- 0x02: de/para são neighbor IDs; vizinho que não existe → erro 0x1C. A
  tabela é a do vizinho "de".
- sem 0x02 e **com 0x08**: só o "de" é procurado como objeto (erro 0x17 se
  não existe) e a tabela é a do próprio objeto (`+0x1AC`); o "para" é usado
  cru, sem procurar objeto nenhum (confirma o achado §7 do
  `data-findings.md`: "para" é um neighbor ID). A flag não aparece no
  disassembler.
- sem 0x02 e sem 0x08: os dois são objetos (erro 0x17 se faltar um). **Se os
  dois são Sims, a relação é guardada pelos neighbor IDs deles** (a mesma
  tabela que 0x02 usa); senão, na tabela do objeto "de", com o ID de objeto
  do "para" como chave.
- Sem tabela → erro 0x68 "The relationship matrix requested could not be
  found".
- Se a relação com "para" tem ≤ `variável` entradas: com 0x01 → falso; sem
  0x01 ela cresce para `variável + 1` entradas (zeradas) e segue.
- Ler (sem 0x04): o destino tem de ser gravável (erro 0x13) e o valor tem de
  caber no intervalo do destino (erro 0x14); verdadeiro. Gravar (0x04):
  verdadeiro, ou erro 0x14 se a tabela recusa.

Versão de nó 0 (`NewRelationshipVersion0Param`, convertida no load): byte 1
era um modo (0 eu→stack object, 1 stack object→eu, 2 stack object→local,
3 local→stack object) e com o bit 0x02 os IDs viravam person data 31
(neighbor ID) em vez de object data 11 (ID do objeto). Binário.

**Contradiz o código**: a VM falha com 0x08 e não trata "dois Sims por ID de
objeto = mesma relação que por neighbor ID".

### 0x2A Create New Object Instance (`TryCreateObject`)

Operandos (binário): GUID u32 0; posição s8 4 (STR# 0xA7 índice + 1); flags
u8 5; local u8 6 (posições 8 e 9); u16 7 (não impresso); slot u8 9
(posições 4 e 10); flags u8 10.

Flags do byte 5 (disassembler + código): 0x01 não duplica (falso se já há
um objeto do mesmo tipo no tile), 0x02 passa IDs ao Main (parâmetro 0 = meu
ID, 1 = ID do stack object), 0x04 Sim a partir do **vizinho no stack
object**, 0x08 falso se o tile não está vazio, 0x10 passa o temp 0 ao Main
(parâmetro 0), **0x20 (sem texto no editor) vira o objeto novo para onde o
stack object olha** (`Orient`, vtable base `0x234` / BV `0x2a0`, quando as
direções diferem; rodada n48: lido no base 1.0 `0xb56320` e no BV `0xe34b2c`;
antes lido como "o nível do stack object", inferido), **0x40 GUID do token
temporário de inventário**,
0x80 GUID nos temps 0 (baixo) e 1 (alto). 0x02 junto com 0x10 → erro 0x36
"More than one passing value specified". Flags do byte 10: 0x01 "Moving in a
new Sim" (vai para a criação), 0x02 copia os materiais de design do objeto
no temp 5 (objeto inexistente → erro 0x0A).

Posições (byte 4; rótulos do STR# 0xA7 e código; binário): 0 "in front of
me" (meu tile + 1 na direção em que olho); 1 "on top of me" e 2 "in my hand"
(os dois: no meu slot 0); 3 "in front of stack object"; 4 "in user defined
slot of stack object" (slot do byte 9); 5 "underneath me" (meu tile); 6 "out
of world" (sem testes de posição); 7 "below object in stack param 0" (no tile
do objeto cujo ID está no parâmetro 0; sem parâmetros → erro 0x08); 8 "below
object in local" (local do byte 6; fora do intervalo → erro 0x08); 9 "next to
me in direction of local" (local + minha direção, só N/L/S/O, virado para
mim); 10 "in user defined slot of object in temp 0" (slot do byte 9).

Falso (sem erro): GUID sem definição; definição "morta" (tipo 0x11); posição
inválida ou tile ocupado (flags acima); a criação falha; o objeto não cabe
(`TSObjectCanSetPosition`/`CanPlaceInSlot`: o objeto criado é apagado; no VM
desde a rodada 15, §4g "Rodada 15 (planejador)").

**A ordem do `TryCreateObject`** (rodada n48, base 1.0 `0xb56320`, BV
`0xe34b2c`): o lugar é o **FTilePt do âncora** (`cTSObject +0x1c0`, vtable
`0x88`: x e y em ponto fixo 16.16, a posição exata, não o centro do tile;
`SetLocation` o grava no `SetPosition`), mais um tile na direção dele para 0 e
3 (as oito direções) e para 9 (`local` + a minha, só N/L/S/O); o nível é o do
âncora. Testes antes de criar: 0x01 (um objeto da mesma definição no tile,
vtable `0xd8`), 0x08 (algum objeto no tile), fora do lote (`WorldDB 0xc8`), a
sala (3, 4 e 10: a sala do lugar, definição 2, igual à do âncora, vtable
`0x1cc` base / `0x204` BV `GetRoomID`), e no 9 uma parede entre o lugar e mim
(`FUN_00b49108`: as paredes do tile do lugar viradas para a direção de volta,
bit 0; o laço do binário começa no segundo lado, então a parede do lado −Y
do tile nunca conta). Depois: **cria fora do mundo** (o gerente de objetos,
vtable `0x40`: o Init roda aí), o 0x20 vira o objeto, e **só então** testa
(`TSObjectCanSetPosition(objeto, lugar, 0, 0, flags 0)` com a direção do
próprio objeto, ou `TSObjectCanPlaceInSlot`) e põe (`SetPosition`, vtable
`0x2a4` / base `0x238`: tira e põe no mundo, a parede adjacente, a árvore
"placement" OBJf 9, a sala; ou `PlaceInSlot`); nada vira o objeto para o
âncora. Por fim os parâmetros do Main (0x02/0x10) e os materiais (byte 10
0x02). **No VM** (n48): assim, salvo as mãos de uma pessoa (slots 0 e 1, que
aqui não estão num SLOT: sem teste) e o 0x08, que não conta a mim (inferido).
Antes: o lugar no centro do tile, o objeto virado para o âncora (9: para mim),
a posição posta antes do Init, só a parte líder de um multi-tile posta, sem
teste de slot, sala nem parede.
Definição de template (tipo 0x10) → erro 0x90. No fim, **o stack object
passa a ser o objeto novo** e devolve verdadeiro.

### 0x20 Test Object Type (`TryTestObjectType`)

Operandos (binário): GUID u32 0, objeto (dono u8 6, dado u16 4), flags u8 7.

- GUID `0x4C7CAB2B` = "the GUID of the temporary inventory token"
  (disassembler): o GUID testado é o do token temporário.
- 0x04 "Return GUID selected in Temp0/1": não testa objeto; se o GUID tem
  definição, temp 1 = metade alta, temp 0 = metade baixa e verdadeiro; senão
  falso.
- 0x02 "incoming ID is a neighbor ID": o objeto é procurado como vizinho.
- 0x01 "Checking against original, not current GUID".
- Sem 0x04: verdadeiro se o GUID é o da definição do objeto (com 0x01, da
  definição original) ou, se não for, o da definição "mestre" dela (slot
  `0x6C`; inferido: objetos multi-tile e trocas de tipo).
- **Objeto que não existe → falso (não erro).** Contradiz o comentário em
  `src/vm/primitives.cpp` ("error if it does not exist").

### 0x10 Find Location For (`TryFindGoodLocation`)

Operandos (binário): modo u8 0 (STR# 0xEF); local u8 1; flags u8 2; byte 3 =
**máscara de direções proibidas**; objeto (dono 4, dado u16 5); referência
(dono 7, dado u16 8); routing slot u16 10; u16 12 (não usado aqui).

Flags (disassembler): 0x01 começa no objeto do local (byte 1), 0x02 **não**
prefere tiles vazios (o texto ", prefer empty" sai quando o bit está
desligado), 0x04 só tiles editáveis, 0x08 objetos explícitos (bytes 4–9),
0x10 em terreno plano, 0x20 com borda vazia, 0x40 começa na frente da
referência, 0x80 com linha de visão até o centro.

Byte 3 (binário): o código usa `~byte3` como máscara das direções
permitidas: **bit n ligado = direção n proibida** (bit 0 N, 1 NE, 2 L, 3 SE,
4 S, 5 SO, 6 O, 7 NO). `0xFE` = só N, `0xAA` = só N/L/S/O, `0xFB` = todas menos
L, 0 = todas. O §11 do `data-findings.md` tinha a leitura invertida.

Execução (binário): sem 0x08 o objeto é o stack object (sem: erro 0x15) e a
referência sou eu, ou o objeto do local (0x01; local fora do intervalo:
0x33). Com 0x08 os dois vêm dos operandos (faltando: 0x17). Modo 1: põe fora
do mundo, verdadeiro. Modos 5–8 (slot no parâmetro / local / literal /
global, u16 10; fora do intervalo: 0x08 e 0x1B): tenta cada posição do slot;
se o slot não fixa a direção, tenta as 8 direções a partir de uma aleatória,
pulando as proibidas. Modos 3/4 (ao longo do vetor da referência / lateral
= +90°; sem referência: 0x3D): busca a partir da direção da referência.
Modos 0 e 2: busca normal; **o modo 0 tenta primeiro no mesmo cômodo e, se
não achar, em qualquer lugar; o modo 2 só no mesmo cômodo**. Achou →
posiciona, verdadeiro; não achou → falso.

### 0x1F Set to Next (`TrySetToNext`, `SetToNextParam`)

Operandos (binário): GUID u32 0; tipo de busca = byte 4 & 0x7F (STR# 0xA4);
**byte 4 bit 7: alvo explícito** = (dono byte 5, dado **byte 7**) — sem ele o
alvo é o stack object; local u8 6 (tipos 9 e 0x22); flags u8 8: 0x01 "including
disabled objects", 0x02 "where" object data (u16 9, rótulo STR# 0x8D) ==
valor u16 11, 0x04 pula o teste do object data 107 (não impressa pelo
editor). A versão de nó 0 não tinha as flags nem o filtro (zerados no
load).

Execução (binário):

- O alvo tem de ser gravável (erro 0x13). A busca recomeça **depois** do
  valor atual do alvo (0 = do começo) e grava o achado no alvo; nada achado →
  falso.
- Filtro comum (`SetToNextIsObjectEligible`): objeto válido; sem a flag 0x01,
  pula objetos com o bit 0 do object data 56 ligado; **sem a flag 0x04 (não
  impressa pelo editor), pula objetos com object data 107 ≠ 0**; com 0x02,
  object data [u16 9] tem de ser igual a u16 11. Os rótulos do jogo base
  (STR# 0x8D) só vão até o object data 87 e chamam o 56 de "dry clothes
  value": a flag 0x01 ("including disabled objects", string ausente no exe
  base 1.0) e o campo 107 são **só BV**.
- Lista de objetos do lote: 0 objeto; 2 não-pessoa; 6 categoria (object data
  59) = parâmetro 0; 12 categoria = temp 0; 25 objeto no cômodo do temp 0; 27
  "Utility Using Object" (object data 76 ou 77 ≠ 0); 33 "Hauntable" (campo
  0x1C da definição); 37 categoria de desejo = temp 0 (os rótulos do jogo base
  vão até o tipo 37). Tipo fora da lista → erro 0x1E.
- 1 pessoa: lista de pessoas do lote, a partir do Sim cujo ID está no alvo.
  Os tipos 0x28–0x2D (só BV) filtram pelo dado de pessoa 186 (espécie?:
  0x29 < 0, 0x2A 1–2, 0x2B 1, 0x2C 2, 0x2D 3), e **no BV o tipo 1 só aceita
  dado 186 = 0** (o jogo base não tem esse campo: 170 rótulos de dados de
  pessoa).
- 3 parte de multi-tile: a próxima parte do grupo do objeto do alvo.
- 4 objeto do tipo (GUID u32 0) e 18 GUID nos temps 0/1: definição pelo GUID
  (sem definição → falso), instâncias depois da atual.
- 5 neighbor ID (e 0x2E–0x33, só BV): o próximo vizinho pelo ID, com o
  mesmo filtro do dado 186 (tipo 5: = 0, só BV); se o vizinho está no lote, o
  objeto dele também passa pelo filtro comum.

**No port (21/10; tipos 38 e 39 em 22/10).** Os tipos de espécie entraram: o dado de pessoa 186 (0 = o
humano; o jogo base não tem o campo, então os Sims dele leem 0) filtra o tipo 1
(as pessoas) e o tipo 5 (o ID de vizinho) pelo humano, e os tipos 40–45
(0x28–0x2D) e 46–51 (0x2E–0x33) pegam uma espécie cada (0 = o humano,
< 0, 1–2, 1, 2, 3 nas duas listas). Sem o filtro, o tipo 1 no Bon Voyage
achava os animais também. O tipo 33 (Hauntable, o campo 0x1C da definição)
também. O censo do AL (`s2pkg prim-usage <Res> 1F`, byte 4) mostra o uso: 40 ×126,
41 ×56, 46 ×9, 42 ×5, 47 ×3, 45 ×1 (os de espécie) e 38 ×36, 39 ×3, 31 ×9,
19 ×1, 20 ×1, 13 ×1, 34 ×1; **o 31 (cônjuge do temp 0) e o 34 (o grupo de
multi-tile vizinho) já estavam implementados**, e os **38/39 entraram em
22/10** (eram as 39 instruções que faltavam). O **38** percorre a **lista de
definições do app** (não a do objeto): o elemento é a definição OBJD, os
campos **75/76** (offsets **0xD6/0xD8**) são a **categoria e a subcategoria do
seletor** do AL (117 = as rádios, 121 = os badges/tokens, 153 = os acessórios,
2 = os balcões) e o **GUID** do achado vai para os **temps 2/3**, com o
**índice + 1** no alvo. O **39** é a busca do próximo **vizinho** com o
relacionamento **vitalício ≥ 0** com o vizinho do **temp 0** (a tabela dele;
sem vizinho, erro **0x77**; sem tabela, **0x68**), pulando o próprio e os que
não têm objeto no lote; o filtro comum vale (morte, `data 56 & 5`,
`data 107`, o "where") e o port o aplica no `eligible` desde 22/10, gateado
pelo layout de BV (no nó 0 o dado 56 é o "dry clothes value" e o 107 não
existe). Medido no AL: o `bhav-sweep` passa de 12.894.204
instruções, 18.622 true e 14.129 falhas para **12.915.916, 18.712 true e 13.783
falhas** (as espécies e o 33), e depois para **13.170.043, 19.087 true e 13.200
falhas** (os 38/39: as 70 buscas de tipo inválido somem); o jogo base fica
**igual** (7.234.312 / 11.107 / 2.461). `data-findings.md` §22 tem o assembly,
o censo dos 10.952 OBJDs do AL e a conferência da árvore
`0x7F067AA0:2003`.
- 7 vizinho do tipo: próximos vizinhos cuja definição tem o GUID u32 0.
- 8 objeto no mesmo tile: precisa de alvo ≠ 0 (senão falso); objetos do tile
  do alvo, no mesmo nível, depois dele. O iterador do tile (BV `0xe1e8a8`/
  `0xe1e484`) anda pela lista do tile (`First`/`NextValidObjectOnTile`) e
  **entra nos slots** de cada objeto (vtable `0x104` número de slots, `0x110`
  o conteúdo do slot i; ao acabar volta ao contêiner, `0x118`/`0x124`), em
  profundidade: um Sim sentado numa cadeira e um prato num balcão são
  achados; no fim recomeça do primeiro e para no próprio alvo. Cada achado
  passa pelo filtro comum e tem de estar no tile/nível do alvo (rodada 15:
  antes o VM pulava os contidos, e o "Don't Stand on Sims" do coelho social
  não via a Ophelia/Olive sentada e ficava no tile da cadeira; a "Chair -
  Animate - Get Out" falhava por "Sim on Tile" horas a fio, `N002_Lot2`).
- 9 objeto adjacente ao objeto do local (byte 6; fora do intervalo: erro
  0x08; objeto inexistente: 0x0A): **um objeto por lado**, o último da lista
  do tile (`nTSWorld::LastValidObjectOnTile`, BV `0x16cbd8`: de trás para a
  frente, o primeiro sem o teste `0x10000`), no mesmo cômodo do âncora, os
  lados na ordem **+X, +Y, −X, −Y**; a busca segue do lado do tile do alvo
  em relação ao âncora (dx ≥ 1 → +Y; dx < 0 → −Y; dx = 0 e dy ≥ 1 → −X;
  senão falso), então um alvo que não está num lado (no tile do âncora)
  termina a busca (rodada 15; antes o VM percorria todos os objetos dos
  quatro tiles por ID, e o "Set to Next - Good Dining Chair", que anda pelo
  tile com o tipo 8 entre um e outro, voltava a uma cadeira já vista e
  estourava o orçamento de instruções: `N002_Lot22`).
- 10 carreira: sempre falso. 11 Lot ID: o próximo lote.
- **13 a pessoa mais perto** (o único uso no base e no AL é "Reaction - Banter",
  `7F01EC29:20E9`, que a enumera num laço): a âncora é `GetObjectByID(temp 0)` —
  nula: erro 0x17 — e **tem de ser pessoa** (`QueryInterface` do IID
  `0x5A69D3D5`, senão erro 0x1C); as candidatas são as pessoas, da mais perto
  para a mais longe da âncora (a consulta espacial do módulo; aqui a ordem da
  lista do módulo desempata), cada uma passando pelo filtro comum (morrendo,
  dados 56/107 e o "where"). O alvo é o **cursor**: a próxima chamada continua
  na pessoa seguinte, 0 começa na mais perto; a distância vai para o **temp 4**
  (a mesma de `Get Distance To`: tiles inteiros, 10 por andar) e a direção 0–7
  para o **temp 5** (a de `Get Direction To`). Evidência: PC
  `0x9393b2..0x93960d` — a âncora, o filtro em `0x9394f0`, e as conversões
  `0x00e5c948` (distância) e `0x00c3e7b0` (ângulo) em `0x9395ec..0x93960d`
  gravando `+0x92`/`+0x94` = temps 4/5. Inferido: a âncora não é candidata (um
  Sim não papeia consigo) e o caminho do alvo 0 (o binário só grava os temps
  zerados) começa na mais perto, como o único usuário espera; o detalhe está no
  `re-pc-ghidra.md` §(e).
- 14/15 instância do "job object" pelo GUID nos dados de pessoa 89/90 do Sim
  no stack object / do vizinho (vizinho inexistente: erro 0x37).
- 16 veículo: a lista de veículos do lote (o GUID não é usado — confirma o
  que o `hood_bridge` já faz).
- 17 vizinho da família do temp 0 (dado de pessoa 61 = temp 0).
- 26 "Verify Stack Object", 29 "Verify Neighbor ID in Stack Object", 30
  "Verify Sim in Stack Object": só testam o stack object (o objeto existe e é
  válido / o vizinho existe / o objeto é um Sim); não mexem no alvo.
- 28, 35, 36: vizinhos depois do valor atual cuja relação com o vizinho do
  **temp 0** passa num teste (a tabela é a do vizinho do temp 0): 28 a
  variável 3 ≠ 0; 35 a variável 0 (diária) > **temp 1** (o rótulo diz "temp
  0 to me", o código usa temp 0 = de quem e temp 1 = limite); **36 a variável
  1 & temp 1 ≠ 0** (bits de relação, `docs/data-findings.md` §8). Vizinho do
  temp 0 inexistente ou sem tabela → falso.
- 38 e 39 (sem rótulo no jogo base; só BV): 38 percorre uma lista por índice
  comparando dois campos com os temps 0 e 1 (< 0 = qualquer) e devolve o GUID
  do achado nos temps 2 (baixo) e 3 (alto).

### 0x7A On Timer (`TryOnTimer`, `OnTimerParam`)

Operandos (binário): u16 0 = ticks até disparar ("Ticks till fire %d" no
editor; com a flag 0x08 vem do temp 1: "Getting ticks till fire from temp
1"); byte 2; u16 3 = árvore que o timer roda; flags u8 5;
três (dono 6, dado u16 7), (dono 9, dado u16 10), (dono 12, dado u16 13);
modo u8 15.

Execução (binário): pega o gerenciador de timers do lote (sem ele: erro
0x17). Ticks = u16 0, ou o temp 1 com a flag 0x08. Modo (byte 15) 0 (criar): argumentos =
os três (dono, dado) acima, ou, com a flag 0x01, os parâmetros da árvore
atual; chama o gerenciador com (u16 0, u16 3, flag 0x02, persistente?, n
argumentos) e **grava no temp 0 o ID do timer criado**. Modo 1: altera o
timer cujo ID está no temp 0 (ticks, árvore, flags 0x02 | 0x04→2; 0x04 =
"Reset Timer Ticks" pelo texto do editor). Modo 2: cancela o timer do temp
0 ("Start/Modify/Delete Timer" no editor). Sempre verdadeiro. Inferido:
0x02 repete. (Correção r4: a versão anterior trocava ticks e árvore.)

### 0x17 Play / Stop Sound Event (`TryPlaySound`)

Operandos (binário): som s16 0; flags byte 4; (dono 6, dado u16 7). O
binário de Mac não tem `CatPlaySoundParam` (o texto do editor não foi
achado).

Execução (binário): a fonte é o stack object com a flag 0x02 (e byte 4 ≠
0xFF), senão eu. O número escolhe a tabela de sons: |n| ≥ 20000 → tabela
**semi-global** (n − 20000), 10000–19999 → **global** (n − 10000), < 10000 →
privada. (Correção r4: o binário só mostra três slots da vtable do dono do
código da árvore — 0x38 para 20000+, 0x34 para 10000+, 0x3C para o resto —,
e a versão anterior chamou 0x38 de global; os dados decidem: "Pregnancy -
Make Pregnant", árvore global, toca 10002 = o som global
`sting_playin_make_preg`, e os telefones tocam 20001 sem tabela própria.) **n negativo: o índice vem da variável (dono 6, dado u16 7)** (a
tabela continua escolhida por |n|). O nome achado vai em minúsculas para o
sistema de áudio numa mensagem com o ID do objeto, a posição (para Sims, o
osso `root_rot` e os dados de pessoa 58, 65 e 88) e um de dois eventos
conforme a flag 0x40 (**rodada n41**: tocar / parar, ver §4j). Object data 34
(flags de som): bit 0x02, ou 0x10 com nome começando por "vox", não manda a
posição. **Sempre verdadeiro** (o fim da função é `li r3,1`).

### 0x11 Idle for Input (`cTSPerson::TryIdleForInput`)

Operandos (disassembler + `SwizzleParams`; binário): parâmetro u16 0 com os
ticks ("for ticks in" parâmetro n); "allow push" u16 2 (≠ 0 = ", allow
push"); byte 4 bit 0 = **"Handle Sub Queue Interactions"** (nesse modo os
outros campos não são impressos). Confirma o §5 do `data-findings.md`.

Execução (binário; `cTSPerson::ShouldInterrupt` = "há algo para fazer"):

- Modo normal. Parâmetro fora do intervalo → erro 0x08. Sem nada pendente:
  contador ≠ 0 → decrementa e devolve 2; contador = 0 → desliga a flag 6
  ("notified by idle for input", bit 0x40 do object data 8) e devolve
  verdadeiro. **Espera exatamente N ticks** (a VM, como a FreeSO, espera N + 1:
  ela só devolve verdadeiro quando o contador fica negativo).
- Com algo pendente: liga a flag 6. Se "allow push" ≠ 0 e a fila tem
  entradas: dentro de uma interação de prioridade alta (o frame diz 1) a
  interação da frente só entra se puder interromper; senão verdadeiro. A
  entrada da frente sai da fila, as sub-ações são limpas, a check tree roda
  (`cEdithObjectTestSim::TestInteraction`); passou e tem action tree →
  `GosubObjectTree` no objeto dela, ela vira a ação atual, o contador vai a
  0 e a primitiva devolve **3** (a action tree roda em cima desta árvore);
  não passou → `ActionSkipped` e devolve 2. Sem "allow push" ou fila vazia →
  verdadeiro.
- Modo "Handle Sub Queue Interactions" (byte 4 bit 0): se há sub-ações
  (`CountSubActions`), tira a primeira, testa e empilha a action tree dela
  como sub-ação atual (devolve 3); falhou → limpa as sub-ações e verdadeiro.
  Sem sub-ações → verdadeiro.

### 0x0D Push Interaction (`TryPushAction`, `PushActionParam`)

Operandos (disassembler `CatPushActionParam`; binário):

| bytes | campo |
|---|---|
| 0 | número da interação (TTA) |
| 1 | onde está o objeto: parâmetro n, ou local n com a flag 0x02 |
| 2 | prioridade (STR# 0xE0) |
| 3 | flags: 0x01 ícone do objeto no local (byte 4); 0x02 objeto num local; 0x04 "continue as current"; 0x08 "use name"; 0x10 número da interação numa variável (dono 5, dado u16 6); 0x20 "Force run check tree"; 0x40 liga à interação cujo ID está em (dono 8, dado u16 9); 0x80 devolve o ID da interação nova em (dono 11, dado u16 12) |
| 4 | local com o objeto do ícone (flag 0x01) |
| 5, 6–7 | (dono, dado) do número da interação (flag 0x10) |
| 8, 9–10 | (dono, dado) do ID da interação a que liga (flag 0x40) |
| 11, 12–13 | (dono, dado) onde gravar o ID da interação nova (flag 0x80) |
| 14 | flags: 0x01 passa os 4 primeiros parâmetros; 0x02 interação = resultado do último Find Best Interaction; 0x04 ícone pelo GUID do seletor nos temps 4/5; 0x08 índice do ícone no temp 6 |
| 15 | índice do ícone (sem 0x08 no byte 14) |

A interação vai para a fila do **stack object** ("onto the stack object's
queue"). Resolve o §12 do `data-findings.md` (os três pares e os bits).

Execução (binário):

- Começa gravando 0 no meu object data 52 ("primitive result"); as falhas
  "esperadas" gravam ali um código e devolvem falso: 1 prioridade herdada
  mas eu não tenho prioridade (dado de pessoa 33 = 0), 2 a check tree
  recusou, 3 fila do alvo cheia (mais de 6 entradas e não deu para trocar),
  4 sub-fila cheia (interação aninhada, mais de 4), 6 o alvo recusou a
  entrada. **Os limites 3 e 4 só valem sem "continue as current"** (byte 3
  bit 0x04): o binário põe a continuação na interação nova (vtable 0x88,
  `SetContinuation`) e só testa os tamanhos quando ela lê falso (0x84)
  (rodada 17). Com a continuação a entrada entra mesmo numa fila cheia.
  O "trocar" (vtable 0x1FC da pessoa, com a prioridade) não foi lido; o
  VM não troca.
- O alvo (stack object) tem de existir (erro 0x15) e ser um Sim (erro 0x1C).
- Objeto: parâmetro do byte 1 (fora do intervalo: 0x08) ou local (0x02;
  0x33); com o byte 14 bit 1 (último Find Best Interaction) o objeto é o dado
  de pessoa 53 do alvo ("FindBestAction result Interaction Object ID") e a
  interação o object data 20 dele ("tree table entry"). Objeto inexistente →
  0x17; sem definição → falso; sem TTAB → 0x1F; entrada inexistente → 0x16.
- Prioridade (byte 2): 0 herdada = meu dado de pessoa 33 (objeto que não é
  Sim: 1), 1 máxima = 100, 2 autônoma = 20, 3 do usuário = 50; outro valor →
  erro 0x2D.
- Interação aninhada sem o alvo estar numa interação que permite → erro 0x6F.
- Flag 0x80: grava o ID da interação nova (fora do intervalo do destino:
  0x14). Flag 0x40: liga a nova à interação de ID (dono 8, dado u16 9).
  Byte 14 bit 0: copia os 4 primeiros parâmetros da árvore atual (ou menos).
- Se eu sou Sim (e não é o caso do Find Best Interaction), a interação herda
  o bit "autônoma" (`+0x54` `0x400`, slot `0xBC` → `0xC0`) da minha interação
  atual (rodada n31, §4g; era "inferido: o dono/grupo").
- Verdadeiro quando a entrada vai para a fila (ou sub-fila) do alvo.

### 0x00 Sleep (`cTSObject::TryIdle`)

Parâmetro u16 0 com os ticks (fora do intervalo: erro 0x08). Se o valor é 0
→ verdadeiro; senão decrementa o parâmetro e devolve 2 (espera). Valor
negativo vira 0. Binário.

### 0x03 Find Best Interaction (`cTSPerson::TryFindBestAction`)

Operandos: u16 0 = **máscara de motivos** (bit = número do motivo:
"Interaction - Fulfill Hunger" passa 0x0080, "Fulfill Bladder" 0x0200; o
`TryFindBestAction` guarda o u16 como máscara e o passa à busca de objetos
por anúncio; ver `docs/data-findings.md` §16) — 0 = os N piores motivos,
escolhidos pelo valor (rótulos dos motivos no disassembler: 0 "Shopping", 1 "Scratch-Chew" (só
BV), 3 humor, 4 meta-humor, 5 energia, 6 conforto, 7 fome, 8 higiene, 9
bexiga, 11 alerta, 13 ambiente, 14 social, 15 diversão); flags byte 3: 0x01
depois da primeira passada escolhe os motivos restantes entre os piores,
0x02 só objetos no cômodo do temp 0, 0x04 inclui objetos fora do mundo,
0x08 só interações aninhadas, 0x10 só no objeto da interação atual, 0x20 usa
a posição do objeto do temp 1. Execução (binário): `FBA_Init`, depois até
200 passadas de `FBA_FindStaticInteractions` → `FBA_ScoreStaticInteractions`
→ `FBA_ScoreDynamicInteractions` → `FBA_CheckInteractions`, e
`FBA_DoInteraction` decide o retorno (essas funções têm nome no mapa e são o
ponto de partida para o motor de autonomia). As curvas de motivo vêm de
`cTSPerson::LoadMotiveEffects` (um `cTSMotiveCurveSet` por Sim, pelo campo
0x33 da definição ou pela idade; o dado de pessoa 84 bit 0x200 escolhe a
tabela 0x298).
A pontuação (`FBA_ScoreInteraction`, `GetInteractionScore`,
`GetAdScoreForHuman`, `CalcAttenuation`), a disponibilidade por idade
(`GetAvailabilityBitset`, flags u32 do TTAB) e as constantes da FCNS 2
estão no `docs/data-findings.md` §16 e em `src/vm/autonomy.*`.

#### O laço completo do Find Best Interaction (binário, rodada 7)

Funções do BV: `TryFindBestAction` `0xae36cc`, `FBA_Init` `0xae0cf0`,
`FBA_FindStaticInteractions` `0xad4130`, `FBA_ScoreStaticInteractions`
`0xae337c`, `FBA_ScoreDynamicInteractions` `0xae3258`, `FBA_ScoreInteraction`
`0xae2c4c`, `FBA_RunCheckTree` `0xae24e4`, `FBA_CheckInteractions`
`0xae2978`, `FBA_DoInteraction` `0xad6658`, constantes em
`cTSAutonomyConstantsClient::UpdateConstants` `0xac8f80` (FCNS 2 global; o
**padrão do binário** vale quando a chave falta).

```
máscara = u16 0 (0 → piores motivos)
usados = 0; repetir:
    se máscara de entrada = 0: máscara = os N piores motivos fora de "usados"
        (FUN_00e1de28; N = "random worst motive count": FCNS 8, padrão 3)
    usados |= máscara;  com flag 0x01 a máscara de entrada só vale na 1ª passada
    se máscara ≠ 0: FindStatic → ScoreStatic → ScoreDynamic → CheckInteractions
continua enquanto (checks < máx e máscara ≠ 0 e veio dos piores)
          ou (ainda não há candidato pontuado e máscara ≠ 0 e veio dos piores)
FBA_DoInteraction
```

- **O limite de 200** ("max check tree in fba count", FCNS 200, padrão 200) é
  o contador `+0x85c`. Ele soma 1 a **cada check tree rodada**
  (`FBA_RunCheckTree`), tanto na pontuação quanto na escolha. Por ele o laço
  para de abrir novos grupos de motivos (mas continua enquanto nada pontuou).
  Dentro de `CheckInteractions`, ele interrompe a varredura quando já passou
  de 200, não é a primeira passada e já há um escolhido.
- **ScoreStatic**: para cada candidato do grupo, pula entradas do próprio
  Sim sem "allow on self" (vtable `0x54` da entrada), aplica os modos
  0x08/0x10 do operando, testa a disponibilidade (idade, vtable `0x128`) e
  conta (`+0x860`). Se o **BHAV da check tree tem as flags 2 ou 4**
  (`FUN_00acdbdc(…, 6)`, cache por árvore), a check tree roda **antes** de
  pontuar (ela pode mudar os anúncios, donos 0x0C/0x1C/0x1D) e só pontua se
  passar. Assim gasta o limite. Senão pontua direto com os anúncios da tabela.
- **ScoreDynamic**: as interações dinâmicas (os anúncios que a check tree
  escreveu, `+0x894`/`+0x8a0`) são pontuadas com a entrada da tabela
  ajustada.
- **Pontuação** (`FBA_ScoreInteraction`):
  ```
  Δ  = GetInteractionScore(entrada) − GetCurrentScore()          (motivos, ver acima)
  d  = distância até o objeto (vtable 0x1f8, com "autonomy floor to floor distance cost", padrão 10)
  at = CalcAttenuation = 1 / (1 + a·d), a = atenuação da interação (vtable 0x30:
       baixa/média/alta da TTAB → FCNS 0,1/0,3/0,6; visitante 0,01/0,02/0,03;
       com "autonomy attenuation formula" ≠ 0: min(1, tuning / (a·d + 1)))
  s  = at·Δ
  s  = s·(1 + posse) · memória · relacionamento · clima · viés (vtable 0x734)
  descarta se s < "min autonomy score" (família, ou visitante se vtable 0x5e0) ou s < 1e-5
  ```
  Padrões do binário: mínimo família 0,2, visitante 0,05. A FCNS dá 0.
- **Escolha** (`CheckInteractions`): ordena por pontuação decrescente
  (`qsort`, 0x44 bytes por item). Se o Sim não é visitante e o primeiro não está
  marcado, sorteia r em [0, min(n, "random selection count")) (FCNS 1 → sempre
  0; padrão 10). Depois anda na lista: os itens cuja check tree ainda não
  rodou (`+0x3c` = 0) rodam agora, e o **r-ésimo que passa** vira o escolhido
  (`+0x890`).
- **Por que a comida perde** (leitura do binário, para conferir no nosso
  motor):
  1. o primeiro grupo é só dos **N piores motivos** (3 por padrão, 8 na
     FCNS). Se a fome não está entre eles, a geladeira nem é candidata até
     um grupo seguinte, e os grupos seguintes só abrem enquanto o limite de
     200 check trees não estourou (num lote cheio, as check trees de pré-anúncio do
     primeiro grupo gastam o orçamento);
  2. Δ é a diferença na **curva de utilidade**, achatada perto do topo: com
     fome alta (> ~0), o ganho é pequeno, e a geladeira anuncia fome 0 + 5..20
     (×0,001 no anúncio, então pouco);
  3. a atenuação: a geladeira longe ou noutro andar (+10 por andar) cai por
     1/(1 + a·d);
  4. uma check tree que falha (ex.: sem comida, geladeira quebrada) descarta
     a entrada mesmo com a melhor pontuação.
- **Decaimento diário**: nenhuma função C++ lida aplica os "Decay … per Day"
  (93–101). `cTSPerson::OnReset` (BV `0xad39f0`) só põe **padrões quando todos
  são 0**: 93 fome −400, 95 bexiga −200, 96 energia −200, 97 higiene −100,
  101 diversão −100, 98 −50, 99 social −100, 94 conforto −30, 100 −30, 106
  −30 (106 só BV). Como antes, a aplicação segue inferida no motor.
  `cTSPerson::DayPassed` (BV `0xad133c`) só decrementa o dado 200 (até −10,
  só BV), chama `cTSObject::DayPassed` (a depreciação, ver
  `docs/buy-build-mac.md` §2) e `UpdateMemories`.

#### Sociais esperando o outro Sim ("Wait - Person B State", dados, rodada 7)

Não há código C++ para isto. É SimAntics no grupo `0x7FD90EDB` (`objects.package`),
lido instrução a instrução:

- As duas metades de uma social compartilham um **objeto de controle** (o
  stack object das duas threads): atributo 0 = ID do Sim A, 1 = ID do Sim B,
  **6 = estado de A, 7 = estado de B**, 8 = flag de cancelamento, 14/15 =
  "A/B esperando". "Get in Temp 0 - Who Am I?" (`0x2036`) compara os
  atributos 0/1 com o meu object data 11 (A = BCON 8960, B = 8961).
  Nenhum dos dois → **Break Point** (é o erro que a VM vê quando o
  stack object não é o objeto de controle).
- "Test - Person State" (`0x203F`): lê o atributo 6 ou 7 do outro e compara
  com o parâmetro 2 (`==` se param 1 = BCON 9216, `>=` se 9217).
- "Wait - Person X State Test Y" (`0x203E`) é um **laço de espera**: testa o
  estado do outro; se ainda não chegou, liga o "esperando" (14/15), roda
  "Idle System - Start", e a cada volta chama o global **"Idle" (`0x118`)
  com 1 tick** (que cede a thread), confere a flag de cancelamento (atributo 8)
  e o dado de pessoa **110 "Interactions Linked Deleted"** (o outro largou a
  interação ligada → sai com falso), processa comandos do controle
  ("Command - Process My Command", `0x2015`) e roda "Run Tree by Name"
  (0x1C) nos ganchos de espera. Ao sair, desliga o "esperando" e chama "Idle
  System - Stop".
- "Wait - Person B State - ==" (`0x2010`) chama essa espera, ou a versão "+
  Route" (`0x2067`) quando "Social - Dynamic Walk Enabled?" e o param 2 = 1.

Ou seja, um **alvo ocupado** só "responde" quando a metade B da social
começa a rodar na fila dele (Push Interaction, com a prioridade de quem pediu)
e escreve o estado no atributo 7. Até lá, A gira 1 tick por volta. O motor só
precisa de: Idle/Sleep cedendo a thread, Push Interaction na fila do outro,
os atributos do objeto de controle e o dado de pessoa 110 ligado quando a
interação ligada é apagada (ver Push Interaction e a fila). Se a metade B nunca
começa e ninguém liga o 110, A espera para sempre. É o "as sociais esperam
pelo outro Sim" do TODO de autonomia.

### 0x0E Find Best Object for Function (`TryFindFunctionalObject`)

Função u16 0 (STR# 0xC9) → (object data com o "valor" da função, entrada da
OBJf), binário: 0 prep (31, 0x12), 1 cook (32, 0x13), 2 flat surface (33,
0x14), 3 dispose (36, 0x15), 6 wash dish (37, 0x18), 7 eating surface (38,
0x19), 8 sit (26 "Sitting Value", 0x1A), 10 serving surface (61, 0x0E), 11
clean (39 "dirty level", 0x1C), 12 garden (64, 0x10), 13 wash hands (65,
0x11), 14 repair (15 "Repair Level", 0x1D), 15 dry clothes (sem valor: 50,
0x1F), 23 awareness (sem valor, 0x2A), 25 lookatTarget (sem valor, 0x2C), 27
Global Awareness (0x35, só BV); **as outras funções (4 eat, 5 pick up, 9
stand, 16–22, 24, 26) dão falso direto**.

**O laço inteiro (rodada 11, BV `0xe35a94`, binário).** Antes do laço:

- referência = eu, ou com a flag 0x08 o objeto (dono byte 3, dado u16 4)
  (inexistente: erro 0x17); o cômodo da referência só é lido com 0x04/0x20;
- com 0x10, o serviço de linha de visão do mundo (`WorldDB→0x94→0xA0(5)`) e
  a posição da referência;
- se eu sou um Sim (interface `cITSPerson`, IID `0x5a69d3d5`): o conjunto
  dos **cômodos acessíveis** (`cTSPerson::GetAccessibleRooms(conjunto, meu
  cômodo)`, BV `0xacd218`: o roteador preenche, a partir do meu cômodo) —
  não na Global Awareness — e `UpdateAutonomyBiasCaching(0)`;
- a lista: **os objetos que anunciam a entrada da OBJf** (o módulo de
  objetos guarda uma lista por entrada, `+0x80(entrada)`), não todos.

Para cada objeto da lista, na ordem (um teste que falha pula o objeto):

1. `ValidateObject`; object data 56 bit 0 desligado e object data 107 = 0
   (ou, na dry clothes, o byte alto do 107 = 2) — os dois só existem no BV
   (no base o 56 é "dry clothes value");
2. com o conjunto de cômodos acessíveis: **o cômodo do objeto tem de estar
   nele**;
3. awareness/lookatTarget: só o lockout (object data 25 ≥ 1 pula, salvo com
   0x80). As outras: **`IsOccupied`** (object data 8 bit 5, 0x20 — a flag 6
   das expressões) pula; `AreInteractionsDisabled(!0x80)` pula: lockout
   (od 25) ≥ 1 sem a flag 0x80, **od 87 "Disable All Interactions" ≥ 1** ou
   **od 82 "Placement Flags 2" bit 2 (0x04)** — esses dois mesmo com 0x80;
   **`IsBurning`** (od 8 bit 9, 0x200) pula;
4. a entrada da OBJf do objeto (`GetFnTable→0x2C(entrada)`) tem de ter
   árvore de ação ou de teste;
5. meu od 66 "function score" := od do valor do objeto (ou 50 sem valor);
   **0 pula** (só quando há valor);
6. se a entrada tem check tree: `RunOneTickTree(comportamento do objeto,
   rodando = a referência, rodando-sobre = o objeto, árvore = check tree,
   tipo 4, parâmetros = nulos)` (BV `0xafe424` → `cTSTreeSim`
   `0xe1ab54`): a árvore roda até o fim num tick, com **o stack object = o
   objeto testado**, "eu" = a referência (eu, ou o objeto da flag 0x08) e
   **os parâmetros = os meus temps** (`cTSTreeSim::OnInit` guarda o
   ponteiro dos temps como parâmetros padrão; o `Gosub` copia os 4
   primeiros); falso → pula. Na awareness/Global Awareness de um Sim, meu
   od 66 := od 112 do objeto (só BV). **De novo: od 66 = 0 pula** (a árvore
   pode zerar ou mudar o valor);
7. filtro de lugar — **só a primeira flag ligada conta**, nesta ordem:
   0x01 só fora (o mundo diz que o tile não é "dentro"), 0x02 só dentro,
   0x04 mesmo cômodo da referência, 0x20 cômodo diferente do da referência,
   **0x40 cômodo = o meu temp 0** (`vtable +0x4C(0)` é o temp 0, BV
   `0xaff7e8`; não é "o meu cômodo");
8. pontuação = od 66 × 1 / (1 + d / 5) com **d = `CalcShortDistance`(ref,
   objeto, 10) = √(distância² no plano + (10 × diferença de andares)²)**
   (constantes 10,0 em `0x277a900` e 5,0 em `0x277a904`); num Sim, × (1 +
   `GetOwnerShipBias`) × `GetAutonomyBias(objeto, nenhuma interação)`;
9. melhor que o melhor até agora (começa em −FLT_MAX) e, com 0x10, a linha
   de visão da referência até o objeto livre → vira o melhor.

O melhor vira o stack object e a primitiva devolve verdadeiro; nenhum →
falso (o stack object não muda).

Os vieses (binário, BV `0xace2dc`/`0xac9ca8`): `GetOwnerShipBias` = 0 salvo
se o objeto tem dono (bit da flag 0x19 do seu "lot object") e conta > 1 para
a minha família: 0,1 × contagem. `GetAutonomyBias` = 1 salvo se o objeto
está no mundo e a opção global bit 2 de `0x277a90c` está ligada (padrão 1:
desligada): então v = Σ peso × queda das entradas da tabela de viés do Sim
(`UpdateCachedAutonomyBiasTable`, tirada do inventário; cada entrada:
ponto, andar, cômodo, peso, raio interno, raio externo, máscara; queda 1
dentro do raio interno, linear até 0 no externo) e o viés é (v + 1)^4 para
v ≥ 0, (1 / (1 − v))^4 para v < 0 (constantes 4 e 2 em `0x277a994…9ac`).
Com a tabela vazia dá 1: o VM pode usar 1 e 0.

Diferenças do VM atual (`src/vm/reach.cpp`, `findBestObjectForFunction`):
olha todos os objetos (o binário só os que anunciam a entrada — dá no
mesmo); **não testa ocupado (od 8 0x20), pegando fogo (od 8 0x200), od 87,
od 82 0x04 nem os cômodos acessíveis**; roda o check tree com "eu" = eu
mesmo com a flag 0x08; aplica os filtros de lugar todos juntos (o binário
só o primeiro); trata 0x40 como "meu cômodo" (é o temp 0); a distância é
a soma em vez da hipotenusa; o filtro de lugar vem depois do check tree nos
dois (igual).

### 0x21 Find 5 Worst Motives (`TryFind5WorstMotives`)

De quem u16 4 (0 eu, 1 o Sim do stack object, 2 outro); tipo u16 6 (> 2 →
erro 0x1E). Ordena os motivos {5 energia, 6 conforto, 7 fome, 8 higiene, 9
bexiga, 13 ambiente, 14 social, 15 diversão} pelo valor (crescente) e grava
os 5 primeiros números de motivo nos temps 0–4; verdadeiro. **O tipo (todos/
mentais/físicos) é validado mas não filtra nada** no BV. Não-Sim → erro 0x0A.
Binário.

### 0x1D Set Motive Change e o motor de motivos

`cTSPerson::TrySetMotiveDelta` (binário): byte 3 bit 0 limpa todos os
deltas; senão motivo = byte 2 (só 0, 1, 5, 6, 7, 8, 9, 14, 15; outro → erro
0x1A), delta por hora = (dono byte 0, dado u16 4), para em (dono byte 1,
dado u16 6), e guarda {motivo, delta / 60 / 30 por tick, limite} na lista do
Sim (**confirma 30 ticks por minuto de jogo**); byte 3 bit 1 zera o dado de
pessoa "Decay … per Day" do motivo (5→96, 6→94, 7→93, 8→97, 9→95, 14→99,
15→101; 0→100 e 1→106 no BV).

`cTSPerson::OnSimulatorTick` (binário), a cada tick: dado de pessoa 27
("tick counter") += 1; se o dado 29 ("Motives Static?") < 2, aplica cada
delta da lista ao motivo (float): delta ≥ 0 sobe até o limite, < 0 desce até
o limite (sem passar); a cada 60 ticks, com o dado 29 = 0, atualiza as
memórias; zera o dado 1 ("Money Amount over Head"). ~~O decaimento em si não
está no código~~ (corrigido na rodada n17, `docs/motives-mac.md` §3: a cada
60 ticks, antes das memórias, o `cTSMotives` vtable 0x10 soma
`dado / 24 / 30` dos "Decay … per Day", calcula o ambiente, prende os
motivos em ±100, copia o conjunto antigo, e o humor é refeito).

### 0x24 Dialog (`TryDialog`)

Na primeira execução marca o frame (estado 1) e abre o diálogo; **devolve 2
(espera) enquanto ele está aberto**. Resposta (binário): botão 1 →
verdadeiro (entrada de texto: guarda o texto); botão 2 → falso; três
escolhas: grava 0/1/2 no temp `(byte 7 >> 4) & 7` (> 7: erro). Os tipos 8–10
e 0x1C (notificações TNS) não bloqueiam e usam o objeto do stack object
(ausente: erro 0x17); 0x16/0x19 (painel do tutorial) **não bloqueiam**: temp 1 = 0
esconde o painel, senão o mostra e devolve verdadeiro (`docs/tutorial-mac.md` §5).
Textos: STR# 0x12D do arquivo privado do dono do código (0x01 o
semi-global dele, 0x40 o global; rodada n45 abaixo), índice u16 13 (ou temp
0 com 0x02), título pelo byte 6 (ou temp 3 com 0x10).

**Tempo esgotado (rodada 9, binário).** Os diálogos que bloqueiam não ficam
esperando para sempre. O `TryDialog` (BV `0xe2bb64`) põe o pedido na fila do
`cEdithObjectModule` (`EnqueueObjectDialog`, `0x527cf4`); o diálogo mostrado
é um `cTSObjectDialog` (tipos 0–4 do STR# 0xD9: 0 mensagem, 1 sim/não, 2
sim/não/cancelar, 3 entrada de texto, 4 tutorial). `cTSObjectDialog::SetParams`
(`0x1d2b20`) guarda o tipo (byte 5), o bit 0 do byte 7 e "não bloqueia" =
!(byte 7 & 0x80); o `SetupDialog` (`0x1d0da8`, chamado no `Show`) cria os
botões (ids 2, 3, 4 = botões 1, 2, 3 da primitiva) e, **salvo no tipo 4
(tutorial)**, chama `cTSWinPictureDialog::SetTimeout(último botão, ms)` com

    ms = "dialog TimeOut Default Milliseconds" (4000)
       + "dialog TimeOut Multipler Milliseconds" (40) × comprimento do texto

(dois `cTSFloatConstants`, padrões 4000 e 40 no código, existem iguais no
base 1.0). O "último botão" é o 1 no tipo 0 (e no 3: OK com o texto que
houver), o **2 no sim/não** (→ falso) e o **3 no sim/não/cancelar** (→ 2
no temp). O `cTSWinPictureDialog::Init` arma um `cRZCallbackTimer` de uma
vez com esse tempo (tempo **real**, não do jogo; inferido que não para com o
jogo pausado) e, ao estourar, o diálogo recebe o clique nesse botão
(`static_dialog_timeout_callback`, comando `0x287259F6`); a primitiva vê a
resposta como se o jogador tivesse clicado. Ex.: uma mensagem de 120
caracteres some em 8,8 s; um sim/não de 200 caracteres responde "não" em
12 s.

**A janela no app (rodada 13; refeita na rodada 14).** `app/dialog_window.*`
(os diálogos de escolha em `dialog_window_pickers.cpp`, os de roupas em
`dialog_window_clothes.cpp`)
desenha cada tipo pela classe do binário (tabela abaixo): o `cTSObjectDialog`
como o `cTSWinPictureDialog::DoLayout` (medidas, moldura `0xA9500615`,
botões sobre `0xA9500630`, fontes GenHeader/GenDialogBody/GenButton, o
ícone composto em `0xE9000005`–`7`; a caixa de fechar não existe no jogo
base, rodada n45), o nome pelos painéis do `0x2D91050A` e os telefones pelos
scripts `0x8C159243`/`44`/`42` com as listas pelas constantes `0xCCCC` (fonte
PhoneBooksListBox, linha 20–21 px, texto escolhido verde; as colunas e os
ícones de amor/paixão e amigo/inimigo da festa, o painel pintado com a sua
`fillcolor`: rodada n45). A primeira versão (rodada 13) usava o script `0x6CA51466`, uma
escolha inferida que o binário desmentiu. Sem cabeça: `--dialog-demo
0|0i|0c|1|2|3|5|6|7|14|11|16|16f|17|17f|20|21|23|23g|23t|23r|24[:título[:mensagem]]`, `--dialog-type`,
`--dialog-click`; `s2pkg pie` lista a classe, as linhas e as caixas de nome
dos diálogos especiais (`r14-telefone-*.jpg`, `r14-dialogo-*.jpg`; rodada 14 dialogs3: `r14-comprar-roupa.jpg`, `r14-experimentar.jpg`, `r14-mudar-aparencia.jpg`, `r14-espelho-cabelo.jpg`, `r14-telefone-ordem.jpg`, `r14-aspiracao-descricao.jpg`).

**Diálogos especiais (tipo ≥ 5) não têm tempo (rodada 11, binário).** O
`TryDialog` trata à parte os tipos 8–10/0x1C (notificações), 0x16/0x19
(tutorial, ver `docs/tutorial-mac.md`) e 0x39 (recurso pelo u16 13). Todos
os outros, inclusive 0–4 e os especiais, vão pela fila
(`EnqueueObjectDialog`: a fábrica de janelas cria a classe pelo número do
tipo, IID `0x0797ea13`) e a primitiva devolve 2 (espera) até o diálogo
mudar de estado (0/1 espera, 2 = botão 1 → verdadeiro, 3/4 = botões 2/3 →
falso, no tipo 2 grava 0/1 no temp). Com o byte 7 bit 0 vai para a fila e
devolve verdadeiro na hora. Antes de enfileirar, os tipos 5–7, 0x0B–0x0D,
0x0F, 0x21, 0x33, 0x34 e 0x46 exigem que o stack object, se houver, tenha
pelo menos um object array (`vtable +0x418` = tamanho da lista em `+0x188`;
senão erro 0x6C "Bad Object Array"). **Só o `cTSObjectDialog`
(`SetupDialog`) chama `cTSWinPictureDialog::SetTimeout`**: nenhuma das 37
classes `cTSWinProc*Dialog` com `SetParams` (`cTSWinProcBaseObjectDialog` e
derivadas: telefone, agenda, roupas, aspiração, oferta de emprego, nome do
objeto, preço, ...) arma timer, e as strings "dialog TimeOut ..." só são
lidas ali. Os especiais esperam o jogador para sempre; no jogo eles só
aparecem em interações que o jogador escolheu (inferido). Para o VM sem
cabeça: responder sozinho é escolha nossa (opção), não do jogo.

**Classe de cada tipo (rodada 14, binário).** O
`cTSUIDialogManager::GetObjectDialog` (BV `0x21c5a8`) é um `switch` do tipo
para um ID de classe (CLSID); a classe de cada ID é a que vem logo depois do
seu `GZCOM_<classe>CLSID` (o getter fica na frente do `QueryInterface` da
classe). Tipos do jogo base:

| tipo | classe | script (`ui.package`) | devolve |
|---|---|---|---|
| 0–4, 0x13 e os não listados | `cTSObjectDialog` (CLSID `0x3798E2DB`) | nenhum: `cTSWinPictureDialog`, montado em código | botão / temp |
| 5, 0x0F | `cTSWinProcPhoneBookServices` (`0xCC16C63A`) | `0x8C159243` | array 0 = [ID do objeto] |
| 6 | `cTSWinProcPhoneBookNeighbors` (`0x8C16CA13`) | `0x8C159244` | array 0 = [ID do vizinho] |
| 7, 0x46 | `cTSWinProcPhoneBookParty` (`0xEC16CA68`) | `0x8C159242` | array 0 = convidados |
| 0x0B, 0x0D, 0x2B, 0x2D | `cTSWinProcSelectItemsDialog` | | array 0 (≠ −1 = escolhido) |
| 0x0C, 0x2C | `cTSWinProcJewelryPurchaseDialog` | `0xACDC3000` (não está no `ui.package` do jogo base) | |
| 0x0E, 0x1B, 0x1F, 0x2A | `cTSWinProcFillDialog` | | temp 0 |
| 0x10, 0x2E | `cTSWinProcClothingPurchaseDialog` | `0x8C159251` (`0x40489c`) | o carrinho no guarda-roupa da família, o total sai do dinheiro |
| 0x11, 0x31 | `cTSWinProcClothingDialog` | `0x8C159250` (`Initialize`) | troca a roupa da categoria do temp 0 |
| 0x14 | `cTSWinProcClothingTryOnDialog` | `0x8C159255` | a roupa de provar (0x400) gravada |
| 0x15 | `cTSWinProcVanityDialog` | `0x8C159280` (`0x8C159286` com temp 0 = 1, expansão) | o visual gravado no personagem |
| 0x17 | `cTSWinProcNameObjectDialog` (`0x2D910709`) | `0x2D91050A` | nomes |
| 0x18, 0x22 | `cTSWinProcSetAspirationDialog` | `0xCC16A49E` / `0xCAFE0045` | dado de pessoa 46 |

Os outros tipos de 0x1A em diante são classes das expansões. O `TryDialog`
não enfileira o 0x12 ("Tutorial Tasks"): devolve verdadeiro sem janela.

**`cTSObjectDialog` (binário).** Não usa script: o `Show` cria um
`cTSWinObjectDialogWrapper` (um `cTSWinPictureDialog`) e o `SetupDialog`
(`0x1d0da8`) monta: título (`SetTitle`, fonte GenHeader) se houver;
imagem (o ícone, `SetImage`); **botão 1** (`AddButton`, id 2) — ou, com o
bit 7 do byte 7 ("continue"), **uma caixa de fechar** (`AddCloseBox(2)`,
imagem `0x11110002`) no lugar dele; botão 2 (id 3) nos tipos 1/2; botão 3
(id 4) no 2; o texto (`AddText`, GenDialogBody); no tipo 3 uma caixa de
texto vazia (`AddEditBox("", 999)`); o tempo (salvo no tipo 4); e as
teclas: **Return = botão 1, Esc = o último botão** (o 1 na mensagem e na
entrada de texto), **espaço = botão 1** salvo no sim/não e no
sim/não/cancelar. Botões sem string usam o UIText STR# 0x8E (1 OK, 2
Cancelar, 3 Sim, 4 Não). O `DoLayout` (`0x25b540`): largura mínima 128,
texto quebrado começando na largura do título ou dos botões e crescendo de
32 em 32 px (até 700) enquanto passa de 64 px de altura (o limite cresce
32 por vez até 500); texto em x = 26 (com imagem, à direita dela: x =
largura da imagem + 34), y = 18 (com título, altura do título + 26);
caixa de texto entre o texto e os botões (x 26 a largura − 26); botões
com a largura do texto + 16, no mínimo 82, os que diferem menos de 20 px
do mais largo igualados a ele, 12 px entre eles, centrados, 34 px de
altura, a 12 px do texto (no mínimo em y = 118 − altura); altura = y dos
botões + 34 + 14; a janela centrada na tela. A moldura é a imagem
`0xA9500615` desenhada em nove partes (cantos = um terço da imagem); a
imagem do ícone fica em (26, 20) (com título, altura do título + 28).

**Ícone (byte 7 bits 1–3, STR# 0xF4).** 0 automático: o objeto que a
expansão de tokens devolve, senão o stack object; 1 nenhum; 2 vizinho: o
vizinho cujo ID está no stack object; 3 indexado: pela definição do objeto
da árvore (inferido); 4 nomeado: a string do STR# 0x12D no byte 1, "gz
<id>"/"gzi <id>" uma imagem de UI, "guid <n>" um objeto (rodada n45: o
`SetParams` compara a primeira palavra com "gz", "gzi", "my", "guid", "rel"
e "job", strings BV `0x101ac98`–`0x101acb0`, `FUN_0007391c`; o que "my",
"rel" e "job" fazem e de onde vem a imagem não foram lidos; só três Dialog
do jogo base pedem o ícone nomeado, dois no "CT - Offer Job"). A imagem
(`FUN_001d206c`) é a miniatura 40×40 (objeto: a do catálogo; Sim: a
cabeça) centrada sobre o fundo `0xE9000005`, com a moldura `0xE9000006` por
cima e a máscara de alfa `0xE9000007` (grupo `0x499DB772`).

**Entrada de texto (tipo 3).** Com o botão 1 o `BtnPressed` guarda o texto
da caixa 999 e o `TryDialog` (estado 2, tipo 3) o entrega ao simulador
(`cTSObject+0x48`, vtable `0x68`). Nenhuma árvore do jogo base usa o tipo 3
e quem lê esse texto não foi achado (no The Sims 1 era o nome do próximo
Make New Character; o `TryMakeNewCharacter` do BV não lê). A VM guarda em
`VM::enteredText`. **Rodada 14 (engine-re):** o `+0x48` é o `cTSSimSystem`
(interface em `+0x1c`, vptr `0x27c50a0`) e a vtable `0x68` é
**`cTSSimSystem::SetLastUserTypedName`** (BV `0x588db4`, guarda a string); os
leitores são `GetLastUserTypedName` (`0x60` por referência, `0x64`, BV
`0x588d70`/`0x588dac`). Nenhuma chamada direta a eles; as virtuais por esses
slots não puderam ser separadas das de outras classes (busca ambígua), então
quem lê o nome continua não achado (inferido: o nome de um Sim novo, como o
"last user typed name" sugere). **Rodada 15:** o campo é o `+0xC4` do
`cTSSimSystem`; dos 164 métodos do `cTSSimSystem` só os dois getters, o
setter e os destrutores o tocam, e uma varredura do binário inteiro pelas
chamadas `lwz rX,0x48(rObj)` → `lwz rV,0(rX)` → `lwz r,0x60|0x64|0x68(rV)`
acha o setter só no `TryDialog` (`0xe2c9ac`) e nenhum getter num
`cTSObject`; os outros 22 casamentos são de classes cujo `+0x48` é outra
coisa. **Nada no BV lê o texto do tipo 3** (pelo menos não por esse
caminho): o app o deixa em `VM::enteredText` sem ligar a nada.

**Sim/não/cancelar.** O `TryDialog` grava no temp `(byte 7 >> 4) & 7`
**0 no "não" (estado 3) e 1 no "cancelar" (estado 4)**; o "sim" não grava
(STR# 0xDA diz o mesmo). A VM gravava 1/2.

**"Return" (byte 7 bit 0).** Enfileira e devolve verdadeiro na hora; a
janela fica até o jogador fechá-la (STR# 0xED 1/3: "return and block/continue
sim"). Sem o bit 7 o jogo continua pausado até ela fechar.

**Rodada n27: o `cTSObjectDialog` e os tokens lidos no binário (Ghidra MCP +
headless).**

*`SetParams`* (BV `0x1d2b20`): `+0x58` tipo (byte 5); `+0x70` = byte 7 bit 0
("return"); `+0x71` = **não** byte 7 bit 7; `+0x72` o stack object; `+0x74`
e `+0x75` zerados (o tutorial os liga: sem tempo e sem título). As strings
vêm do grupo privado do dono do código (`frame→0x50→0x28`: o
`cTSBehavior` do quadro, rodada n45 abaixo), do semi-global da definição
dele com o byte 8 bit 0, do global `0x7FD46CD0` com o bit `0x40`; mensagem u16 13 (temp 0 com
`0x02`), título byte 6 (temp 3 com `0x10`), botão 1 byte 3 (temp 1 com
`0x04`), botão 2 byte 4 (temp 2 com `0x08`), botão 3 byte 0 (temp 4 com
`0x20`). O índice de um temp vai como está (base 1, como o do operando):
só as notificações (tipos 8, 10, 0x1C, que o `TryDialog` lê ele mesmo) somam
1 ao temp 0. O app somava 1 em todos e não lia os botões dos temps; os
temps de botão aparecem nas cartas de chance da carreira ("Sub - Chance
Card - Dialog", tipo 2, byte 8 `0x0E`: mensagem e botões 1 e 2 dos temps 0–2)
e no "Sub - Chance Card - Results" (tipo 0, mensagem do temp 0). Botões sem
string: STR# 0x8E 1/3/4/2 (OK, Sim, Não, Cancelar) por tipo. Os cinco textos passam pelo `ParseUIString` (abaixo) com o objeto que
roda a árvore; o objeto que um token devolve vira o ícone automático.

*`SetupDialog`* (BV `0x1d0da8`): título, ícone, **botão 1**: com `+0x71`
(bit 7 desligado) `AddButton(texto, 2)` (interface vtable `0x14`, BV
`0x25cf94`); com o bit 7, uma **caixa de fechar** `AddCloseBox(2,
0x11110002)` (vtable `0x1c`, BV `0x25d444`; a `0x18`, BV `0x259588`, é a
mesma com a imagem padrão `0x11110002`). Botão 2 (id 3) nos tipos 1/2,
botão 3 (id 4) no 2, o texto, a caixa de texto no tipo 3, o tempo e as
teclas como acima. **A caixa de fechar não entra na fila dos botões**: o
`AddCloseBox` a guarda num campo só dela (`+0x108`) e a põe como filha da
janela; a fila `+0x110` (que o `FUN_0025b1fc` iguala e o `DoLayout` põe em
linha) tem só os `AddButton`. Nem o `DoLayout` nem o `SetArea` da janela
(BV `0x25aae4`/`0x25ab90`) mexem nela: fica onde o botão é criado, no canto
de cima à esquerda, do tamanho da imagem (inferido: o tamanho que o
`SetImage` do botão dá; a imagem `0x11110002` não está no `ui.package` do
jogo base).

*`FUN_0025b1fc`* (as larguras): largura = texto + 16 de cada botão da fila;
repete: o mais largo abaixo do limite anterior (`< prev`); menor que 82 vira
82 com piso 0, senão o piso é ele − 20; todo botão entre o piso e ele vira
ele; o limite passa a ser o valor **depois** de subir a 82. Sem botão menor
que o limite, para. A altura é a do botão mais alto. (O app punha a caixa de
fechar de 40 px na fila e a pulava ao igualar: o laço nunca acabava, o
travamento do `--dialog-demo 0c`.)

*`DoLayout`* (BV `0x25b540`), o que faltava: a altura do texto que conta
para os botões é **max(altura do ícone, altura do texto)** com ícone; sem
botões a altura deles é 0 (mínimo `0x76` − 0 para o y); e o texto **contorna
o ícone**: com ícone, as primeiras ⌊(altura do ícone + 4) / linha⌋ + 1
linhas ficam à direita dele e, se o texto passa de duas linhas a mais que
isso, o resto vai para uma segunda janela de texto (`+0x104`) em x = 21
(−5 de margem), da largura da janela − 57, logo abaixo das primeiras; a
altura do texto é então as duas partes somadas. A caixa de texto
(`AddEditBox`, BV `0x25ae00`) é um `GZWinTextEdit` com `vtable 0x84(0x40)`
(64 letras), fundo (204, 214, 255), texto (5, 6, 113).

*`ParseUIString`* (`cTSObject`, vtable `0x408`; base 1.0 `FUN_00b36ed0`, BV
`0xb0eeb4`). Chamado pelo `TryDialog` (BV `0xe2bf70`: objeto = quem roda a
árvore, segundo argumento 0 → o stack object do quadro) e pelo `SetParams`.
Cada token é um `cRZString` estático (base: `0x1a2e778` + `0x18`·k, na
ordem das strings `0x11370f8`…; BV: `0x2cc54f0` − 4·k). Para cada token,
**numa ordem fixa**, o texto é varrido do começo: `GetPropertyValueAsString`
(BV `0xb05f38`) acha o token **sem caixa e como prefixo** (`$Mes` acha
`$Me`), lê até dois `:n` e devolve o comprimento; se os argumentos servem o
trecho é trocado pelo valor, senão fica como está e a busca segue uma letra
adiante. "local n" é a variável local n da árvore (`n` < número de locais).
Os 35 tokens do **jogo base**, na ordem em que são trocados:

| ordem | token | valor |
|---|---|---|
| 1–8 | `$Token{Personal,Family}{Counted,Singular}Local{Name,Desc}:a:b` | inventário do vizinho do local a (pessoal) ou da família do local a, ficha no índice local b: nome/descrição do catálogo pelo GUID |
| 9 | `$FamilyAssets:a` | família de ID = local a: vtable `0xa4` (inferido: os fundos), número formatado |
| 10 | `$Family` | nome da família do vizinho cujo ID é o stack object (dado de pessoa 61) |
| 11 | `$LastName` | sobrenome do vizinho do stack object |
| 12 | `$NeighborLocal:a` | primeiro nome do vizinho de ID local a |
| 13 | `$Neighbor` | primeiro nome do vizinho do stack object (vira o ícone) |
| 14 | `$Me` | nome de quem roda a árvore (pelo seletor) |
| 15 | `$Object` | nome do stack object (seletor, o do mestre num multi-tile; vira o ícone) |
| 16 | `$Local:a` | o local a, número formatado |
| 17 | `$Money:a` | local a × local a+1 (só local a se o segundo é 0) |
| 18 | `$Cash:a` | local a + 10000 × local a+1 |
| 19 | `$TimeLocal:a` | hora local a (0–23) pelo formatador de hora do idioma, minuto 0; fora disso fica |
| 20 | `$Attrib:n` | atributo n (literal) do stack object, formatado |
| 21 | `$NameAttrib:n` | nome do objeto cujo ID está no atributo n do stack object |
| 22 | `$NameLocal:a` | nome do objeto de ID local a |
| 23 | `$FullNameLocal:a` | nome e sobrenome do vizinho de ID local a (sem ele, o do stack object) |
| 24 | `$FullName` | nome e sobrenome do vizinho do stack object |
| 25 | `$LotNameLocal:a` | nome do lote da família do vizinho de ID local a |
| 26 | `$CatalogLocal:a` | nome de catálogo (seletor vtable `0xc4`) do objeto de ID local a |
| 27 | `$DayLocal:a` | Live.package STR# 0x99 entrada local a + 1 (1 domingo … 7 sábado) |
| 28 | `$DayFlags:a` | bits 0–6 do local a: STR# 0x99 entradas 2–7 e 1 (segunda … sábado, domingo), separadas por ", " |
| 29 | `$JobTitle:a:b` | STR# 0x93 do grupo do objeto de ID local b, entrada local a + 1 |
| 30 | `$PrivateString:a:b` | STR# local a do arquivo do **stack object** (seletor `0x104`), entrada local b |
| 31 | `$BehaviorString:a:b` | STR# local a do grupo `0x7FE59FD0`, entrada local b |
| 32 | `$AspirationString:a:b` | STR# local a do Live.package (`0x7F96C284`), entrada local b |
| 33 | `$SemiGlobalString:a:b` | idem, do semi-global do stack object (seletor `0x110`) |
| 34 | `$GlobalString:a:b` | idem, do global `0x7FD46CD0` |
| 35 | `$TokenNameLocal:a` | ficha no índice local a do inventário pessoal de quem roda a árvore (erro 0x1C se não é pessoa) |

Os números passam pelo formatador do idioma (`FUN_0006e164`→`0x98(0)`→
`0x48`), com `%d` de reserva: grupos de três com "." em português e ","
em inglês (rodada n45 abaixo). O BV acrescenta `$LotNameLocalByLotId`,
`$SkillName`, `$WordPoints`, `$NeighborhoodLocal`, `$PrimaryNeighborhood`,
`$Want/FearDescriptionLocal`, `$Want/FearHelpTextLocal`, `$GroupName`,
`$Property{Private,SemiGlobal,Global}String` e `$GUIDCatalogNameLocal`;
`$Subject`, `$Constant` e os de `$JobTitle`… `$ObjectType` do outro bloco
são de outras funções. Nos textos do jogo base (STR# de `objects.package` e
`Text/`): `$Me` 4.111 vezes, `$Local` 968, `$Object` 372, `$Neighbor` 336,
`$NeighborLocal` 194, `$NameLocal` 135, `$PrivateString` 101, `$JobTitle`
46, `$AspirationString` 32, `$TimeLocal` 11, `$CatalogLocal` 8, `$FullName`
8, `$BehaviorString` 5, `$Money` 4, `$FamilyAssets` 3, `$Family` 3,
`$GlobalString` 2, `$DayFlags`, `$Cash` e `$FullNameLocal` 1 cada; `$Mes`
aparece 2 vezes.

**Nomes (tipo 0x17, `cTSWinProcNameObjectDialog`).** `SetParams`
(`0x370a50`): os vizinhos nos **temps 0–3** (o 0 obrigatório); cada caixa
começa com o primeiro nome do Sim (selecionado); dado de pessoa 65 = 1
(menina) escolhe o ícone `0x1A`/`0x1B` (menino `0x2A`/`0x2B`); um só Sim
com dado 58 "Person Age" > 1 é um renomear. `onAttach` (`0x371cfc`): painel
`0xCDC2CAA4` (renomear), `0xCDC2CAA5` (um bebê), `0xCDC2CAA6` (gêmeos:
temp 1), `0xCDC2CAA7` (servo); `0xCDC2CAA8`–`B0` são de bichos (expansão).
Caixas `1`–`4`, OK `0xA000`, cancelar `0xA001`. O OK fica desligado com uma
caixa vazia (`ValidateUIStringRequired` do texto aparado); `OnOk`
(`ProceedOkButton`) põe o texto aparado de cada caixa como primeiro nome e
devolve verdadeiro; cancelar, falso.

**Telefone (tipos 5/0x0F, 6, 7).** `SetParams` (`0xa7af30`): título byte 6,
mensagem u16 13, botões bytes 3/4 (STR# 0x12D); as linhas vêm do **array 0
do stack object**: objetos nos serviços (`AddAvailableServices`: nome de
catálogo e miniatura), vizinhos nos outros (`CreateNeighborCallList`: nome,
lote da família, diária + vitalícia do relacionamento de quem liga). Os
serviços e o convite começam com a linha 0 escolhida; o texto de cima é
"Chamar %s" (UIText STR# 0xAB #16, ou a mensagem). `OnOk` limpa o array 0 e
grava o ID escolhido (0 se nenhum); a festa grava os convidados (no máximo
temp 5, lista da direita, botões `0x2000` incluir e `0x1000` tirar).
Títulos padrão: STR# 0xAB #9 "Serviços", #10 "Vizinhos". Com o stack object
sem object array, erro 0x6C antes de abrir. **Rodada 14 (dialogs3):** o livro de vizinhos
(`cTSWinProcPhoneBookNeighbors`) nasce no modo 5 (construtor): **slides**
(`0x9000`, as cabeças do script `0x8C159245` em `0x1006`, 7 × 4 de 52 px,
com as marcas amigo/inimigo e paixão/amor das janelas 3–6 e o overlay
apertado no escolhido; setas `0x1004`/5) **ordenados por nome**; o
`PreShow` escolhe o primeiro dessa ordem (`SetSelectedSim(0)`; a VM ordena
as linhas). `onBtnClickedID`: `0x1000` por nome (modo 2/5), `0x2000` por
relacionamento (1/4), `0x3000` slides, `0x4000` texto (`0x8000`); `0xB000`
química é de expansão. A festa ordena a lista da esquerda (`0x3000`/`0x4000`)
e a de convidados (`0x7000`/`0x8000`) com os predicados de lista
(`ListBoxSortPredicate_Name`/`_Relationship`). `CompareByName`: sobrenome,
depois nome (sem caixa, inferido); `CompareByRelationship`: quem tem
paixão ou amor primeiro, depois a maior diária + vitalícia, depois o nome.
**Rodada 15:** o `PreShow` da festa (BV `0x341688`) põe as linhas e chama `SetNeighborListBoxSort(1)` e `SetInviteListBoxSort(1)` (BV `0x3410c0`/`0x34120c`; 1 nome, 2 relacionamento, 3 química, que acendem `0x3000`/`0x4000`/`0xE000` e `0x7000`/`0x8000`/`0xF000`): as duas listas começam **por nome**, e a linha 0 da esquerda escolhida. O nome do lote
(`SetLotName`) é o do lote onde mora a família do vizinho (`VM::LotRecord::
name`, do nome do lote do bairro). A miniatura do serviço: o
`cTSUIThumbnailService` renderiza o objeto ou, sem modelo, uma imagem do
próprio arquivo (`GetEmbeddedThumbnail`); os "Phone Service - ..." não têm
modelo e o grupo deles (`0x7FF65712`) tem os IMG 1 (silhueta) e 2
(ferramentas): o app usa o IMG 1 do grupo do objeto (qual instância o jogo
lê não foi fechado, inferido).

**Guarda-roupa (tipo 0x11, `cTSWinProcClothingDialog`).** `SetParams`
(`0x330088`): a pessoa é o stack object (sem ele, o Sim da família
selecionado); o temp 0 é um literal do Change Outfit traduzido para o bit
da categoria (0 → 0x001, 1 nu → 0x080, 2 banho → 0x008, 3 ginástica →
0x200, 4 formal → 0x020, 5 pijama → 0x010, 6 provar → 0x400, 8 gestante →
0x100, 9/10 → 0x002/0x004, 11 íntima → 0x040, 13 → 0x1000); copia as peças
da categoria para a roupa de provar (0x400). Abas parte de cima `0xECC70000`
(peça 4), de baixo `0xECC70001` (0x10), conjunto `0xECC70002` (8); começa no
conjunto se o Sim veste um, senão em cima. `ApplyClothingChoice`: um
conjunto tira cima e baixo e vice-versa. `OnOk` copia a roupa de provar de
volta para a categoria e grava (`SimSystem` vtable 0x3C); devolve
verdadeiro. **A lista é o guarda-roupa da família** (rodada 14, dialogs3):
o `SetParams` percorre os itens BINX do bin da família (`cTSFamily`, dado
de pessoa 61) e fica com os da idade, gênero e categoria do Sim. No app:
essas peças (as do catálogo do CAS que estão no bin). **Rodada 15:** o bin
de uma família nova é feito como o `cTSFamily::AddMemberClothing` (BV
`0x47ec68`, vtable `0x24` da família: o AGED do personagem →
`AddClothingFromOutfitData`, BV `0x47e5ac`) o enche: para cada categoria de
roupa menos `0xC86` (0x002, 0x004, nu 0x080, provar 0x400, 0x800), as
partes 4 (cima), 0x10 (baixo) e 8 (conjunto) da roupa do Sim viram itens
BINX (`AddClothing` guarda uma de cada; a parte 0x20, acessório, vai pelo
serviço de bins e fica de fora no app). **Quem chama o `AddMemberClothing`
(rodada 15, binário):** o `cTSFamilyManager::AddToFamily(vizinho, família,
família antiga)` (BV `0x4fddcc`, vtable `0x44` do gerente; único chamador
achado). Se o vizinho ainda não é da família (`TestMember`, vtable `0x78`):
`AddMember` (`0x1c`), dado de pessoa **61 := número da família** e **67
:= 0** (no vizinho e na pessoa do lote, se há), vtable `0x5c(1)` da família,
`AddMemberClothing(ID)` (que volta sem fazer nada se o número da família,
`+0x2c`, não está em 1..`0x7fd9`: a família dos NPCs `0x7FFF` nunca tem bin;
rodada 17, o app também não o enche) e, se as duas famílias são reais (0 < número <
`0x7fda`), **copia do bin da família antiga** (`AddClothing`, vtable `0x28`)
as peças cujas máscaras batem com as do Sim (três máscaras lidas de um
recurso `0xAC598EAC` do personagem; inferido: idade, gênero e categoria;
sem membros na antiga, todas); no fim posta a mensagem `0x94` com o número
da família. Quem chama o `AddToFamily`: a **Generic Sims Call 4** ("add to
family", `cTSObject::TryGenericSimCall` BV `0xe36834`: tira da família
antiga com `RemoveFromFamily`, vtable `0x48`, e põe na nova: nascimento,
adoção, casamento), a **junção de famílias** do mudar-se no bairro
(`cTSUserToolNeighborhoodMoveIn::DoMessage` BV `0x3b7940`, caso 2: cada
membro da família que entra sai dela e vai para a do lote) e o **lote do
catálogo com família** (`cTSLotCatalog::MoveFamilyDataFromCatalogToPlacedLot`
BV `0x4a91d0`). O CAS (`ProcessSaveSimRequest`/`ProcessCreateSimRequest`/
`ProcessCreateBabyRequest`) não chama o `AddToFamily` direto (não achado
como chega a família do CAS ao bin; inferido: pelo mesmo `AddToFamily` no
commit da família). O app o faz ao gravar a família do CAS
(`writeNewFamily`, `NewSim::clothing` = `cas::familyBinClothing`;
`s2pkg cas-make --hood` também) e, para uma família ainda sem bin no lote,
com os Sims dela no lote (`familyBin` no app, guardado ao salvar; o momento
é inferido); um bin vazio ainda mostra o catálogo (inferido).
`r15-planejar-roupa-familia-nova.jpg` com a miniatura do
`CASThumbnails.package` no lugar da vista 3D do Sim (inferido), e o OK grava
o personagem com a roupa nova (`writeCharacter`, como o Sim que cresce) e o
veste de novo se ele usa essa categoria.

**Arara de roupas (tipo 0x10, `cTSWinProcClothingPurchaseDialog`, script
`0x8C159251`; rodada 14, dialogs3).** `SetParams` (`0x404ac8`): o Sim é o
stack object (as árvores "Interaction - Buy - Clothing Rack" e "DEBUG -
Interaction - Buy" do `ShoppingRackGlobals` põem o Sim lá); o temp 0 é o
tipo de roupa pelo mesmo `switch` do guarda-roupa, mas **6 vira 0x001 e é
tratado como 0** (separados); só 0, 9 e 10 (teste `0x601`) oferecem partes
de cima e de baixo (abre em cima), os outros só conjuntos (abre em
conjuntos); o **temp 1 é o preço por peça** (`cTSShoppingCart` slot 0x20 →
`+0x28`; o total, slot 0x48, é `quantidade × preço`). O byte 9 = `'.'` é a
loja própria das expansões (lê locais da árvore; não existe no jogo base).
A janela: abas cima/baixo/conjunto (`0xECC70000`–2), idade
(`0x200`–`0x204`) e gênero (`0x400`/`0x401`) que filtram a grade, grade
`0x1006` (cima 3 × 2 do `0x8C159252`, baixo/conjunto 4 × 2 do
`0x8C159253`/`54`, as constantes do script) com setas `0x1004`/5, "pôr no
carrinho" `0xECCDF663`, a aba do carrinho `0x8CCD0001` (grade 3 × 2,
"tirar" `0x6CCDF7D7`), "%s Itens no Carrinho" (`0xCCCDF42D`) e "Total: %s"
(`0xACCDF819`); na célula, a janela 7 (tique) marca o que está no carrinho e
a 8 (armário) o que a família já tem (inferido pelas imagens). `OnOk` →
`SaveCartStandard` (BV `0x406bfc`; **lido na rodada 15**): as partes de
cima e de baixo do carrinho (os conjuntos inteiros, `outfit` & 8, ficam de
fora) dão as idades pedidas por gênero (1 mulher, 2 homem); para cada um dos
7 bits de idade, o `cTSFamily::GetNumCompleteOutfits` (BV `0x47f258`: os
itens da idade, gênero, espécie 1 e categoria — ou sem categoria —, sem os
de conteúdo próprio com `allowCustomContent` falso; cima × baixo +
conjuntos) do bin com essas peças tem de passar o do bin sozinho em cada
gênero pedido; se alguma falha pergunta (sim/não do gerente de diálogos,
vtable `0x130` estilo 3, UIText STR# 0xCA #7; `0x5301814C` = sim): sim
compra, não fecha a arara sem comprar (o `OnOk` devolve verdadeiro dos dois
jeitos). O UIText do jogo base não tem o STR# 0xCA (é de expansão): o app
usa um texto seu (inferido). `r15-arara-pergunta.jpg`. Então
`cTSShoppingCart::CompletePurchase` (slot 0x6C) põe cada item no bin da
família (`cTSFamily::AddClothing`) e tira o total do dinheiro (vtable 0xB0,
despesa 6). Na VM o OK põe as chaves GZPS do carrinho
(`Dialog::cart`) no `VM::Family::wardrobe` (uma vez) e tira `itens × temp 1`;
o `hood_save` grava cada item novo como um BINX (`binidx` 1, `objectidx` 2,
o resto 0, como os do jogo) com o 3IDR da mesma instância (vazio, o bin
`0x6C4F359D` da família, o GZPS); instância: uma depois do maior BINX/3IDR
do pacote (inferido). A grade mostra o catálogo do CAS da idade/gênero/categoria (quem enche a
lista `+0x1a8` da grade não foi achado; inferido); o OK só liga com algo no
carrinho e dinheiro para pagar (inferido).

**Provar roupa (tipo 0x14, `cTSWinProcClothingTryOnDialog`, script
`0x8C159255`).** `SetParams` (`0x3bed44`): o Sim é o stack object; o temp 0
pelo mesmo `switch` (6 → 0x001, mas sem separados); copia as peças da
categoria para a roupa de provar (0x400). A janela tem as abas e a grade da
arara (sem carrinho nem idade/gênero; o fundo sem imagem pintado com a
`fillcolor`, inferido); escolher um conjunto tira cima e baixo e
vice-versa. `OnOk` grava o personagem (`SimSystem` vtable 0x3C) com a roupa
de provar; a árvore troca a roupa depois. No app o OK grava a categoria
0x400 com o escolhido.

**Penteadeira / espelho (tipo 0x15, `cTSWinProcVanityDialog`, script
`0x8C159280`).** `SetParams` (`0x2effa4`): o Sim é o stack object; temp 0 =
1 abre a cirurgia plástica (`0x8C159286`, das expansões). As abas
(`HandleTabSelect`): penteado `0x4CEDF125` → painel `0x2CEDF326` (cores
`0x8800`–`0x8803`, grade 3 × 4 de `0x8C159283`), maquiagem `0x4CEDF126` →
`0x2CEDF327` (blush `0x6CA52068`, delineador `0xAD474C78`, sombra
`0x6CA52069`, batom `0x6CA52067`, 4 × 1 de `0xCB980E93`), óculos
`0x4CEDF128` e maquiagem personalizada `0x4CEDF127` → `0x2CEDF328` (3 × 5),
sobrancelhas `0x4CEDF130`, fios de barba `0x4CEDF131` e barbas
`0x4CEDF129` → `0xAD4F1B1C` (3 × 4, com a fileira de cores
`0x2D4F258C`). `OnOk` (`0x2f2448`): o penteado em todas as categorias (menos
nu, provar e 0x800), grava o personagem (CAS vtable 0x20) e veste o Sim de
novo no que ele usa (dados de pessoa 8, 119, 120). No app as escolhas são as
do nosso CAS (`cas_mode_panels.cpp`: sobreposições por subtipo e bin, óculos e
barbas de malha), o OK grava o personagem e veste de novo; a vista 3D do Sim
é a cabeça (inferido). **Rodada 15:** o `HandleTabSelect` (BV `0x2f50b0`)
mostra a fileira de cores da barba `0x2D4F258C` (`FUN_0093fed8(painel,
0x2D4F258C, 1)`) nas abas sobrancelhas, fios de barba e barbas; no
`onBtnClickedID` (BV `0x2f651c`) um id com `0x8800` dentro dessa fileira lê o
GUID da legenda do botão (o tom de cabelo "0000000N-…") e chama
`cTSUISimEditor::SetHairColor` (vtable `0x128` do editor; o único setter de
`cTSLID` de cor além do tom de pele) e atualiza os overlays: sobrancelhas e
barbas seguem a cor do cabelo, então a fileira muda a cor do cabelo também
(no app: a mesma escolha das cores do cabelo). "Personalizado" `0x8804` do
componente de cabelo (`cTSUICASComponentHair::HandleTabHairColor`, BV
`0x3e1e54`) liga a flag `+0x268` que o `UpdateFilter` passa ao filtro como
"custom": a grade mostra os cabelos de tom fora dos regulares
(`nTSCASCore::IsRegularHairTone`: GUID 1..5; no jogo base, os 81 de tom 6,
os tingidos); uma cor desliga. `r15-espelho-barba-cor.jpg`,
`r15-espelho-personalizado.jpg`.

**Joias (tipo 0x0C, `cTSWinProcJewelryPurchaseDialog`, script
`0xACDC3000`).** O script não está no `ui.package` do jogo base e nenhum
objeto do jogo base abre o tipo 12 (a árvore "Interaction - Jewelry Rack -
Browse" do `ShoppingRackGlobals` não tem objeto); fica uma caixa OK/Cancelar.

**Aspiração (tipo 0x18, `cTSWinProcSetAspirationDialog`, script
`0xCC16A49E`; 0x22 usa `0xCAFE0045`).** O vizinho é o ID no stack object;
os botões `0x0CDC000n` valem `1 << (n − 1)` (romance 1, família 2, riqueza
4, popularidade 0x10, conhecimento 0x20) e gravam o dado de pessoa 46 do
vizinho; `Validate` exige uma escolhida (≠ 0, 0x40, 0x100) e `OnOk` grava no
Sim do lote. O texto "%s não é mais criança" é o `0x2D9B8E78` (menina) ou
`0x2D9B8E7E` (menino). **O slide da descrição** (`RefreshInfoPanel`, `0x3cf954`): escolhida
uma aspiração, o slide `0x2000` sai para a direita (o `onTick` move o
`0x0DA39A45` até a largura do pai menos a sua; no app de uma vez,
inferido) com o nome (`0x4D9B8E8F`) e a descrição (`0xCDA394CA`) do STR#
0x91 do Live.package (um-baseado: romance 13/6, família 9/1, riqueza 11/3,
popularidade 10/2, conhecimento 12/4, 0x40 14/7, 0x80 40/42), a descrição
logo abaixo do nome e do tamanho do texto. **Rodada 15:** o catálogo
"Aspirações Altas" (`0xCDA395BB`) é um `cTSWinProcWantsCatalog` (criado no
`onAttach`, BV `0x3d14f8`); o `RefreshInfoPanel` o põe logo abaixo da
descrição, chama `SetAspiration` → `LoadPowerWantsByAspiration` (BV
`0xaa5b2c`: o simulador de desejos, vtable `0x64` = `GetLifetimeGoals`,
mapa da aspiração preenchido por `AddLifetimeGoal` com o `<lifetimeGoals>`
do `WantTuning.package`) e estica o slide até o fim do catálogo mais a
margem que ele tinha no script (`+0x48`, 18 px). A grade `0x1006` tem 5 × 2
células do script `0x49065095` (`CreateThumbnail`; `onAttach` do catálogo:
10 itens), e o `SetWant` (BV `0xaa6748`) liga a janela 1 (fundo de desejo)
ou 2 (de medo), a 3 (estouro de desejo "Power") pelo nível, a 4 com o ícone
primário sob a máscara de alfa da própria imagem (`ApplyAlphaMask`) e a 5
com o ícone secundário. No app: `DialogWindow::lifetimeWants` (os nomes do
XML viram bits como os botões do diálogo — Romance 1, Family 2, Wealth 4,
Reputation 0x10, Knowledge 0x20, Grow Up 0x40 —, inferido) e
`r15-aspiracao-desejos.jpg`; o espaçamento das células (margens 1 e 3 do
script) é inferido.

**Comida (tipo 0x0E, `cTSWinProcFillDialog`, script `0x2CB242F8`).**
`SetParams` (`0x294ca4`) lê 4 locais a partir do byte 11: unidades que tem,
custo de 50 unidades, entrega, máximo (os tipos 0x1B/0x1F/0x2A são de
expansões). `CalculateCostOfUnits(n) = (n × custo + 49) / 50 + entrega`; a
barra não passa do dinheiro da família; OK só com algo comprado e grava
`valor − unidades` no temp 0.

**Prateleiras (tipos 0x0B/0x0D, `cTSWinProcSelectItemsDialog`, script
`0x2CB242F6`).** Os objetos do array 0 do stack object em uma fileira de
miniaturas; temp 0 = 1 deixa escolher vários, temp 1 = 1 só o que a família
pode pagar (`UpdateFunds`; temps 2/3, 100 por padrão, não seguidos); `OnOk`
grava −1 nos itens não escolhidos. **Rodada n45:** cada item é uma célula do
script `0x2CB242F7` (`CreateThumbnail` BV `0x38ddd8`, 44 × 44: fundo
`0xE9081001` na janela 1, a miniatura 40 × 40 na 2, um botão de alternar
`0xE9081000` por cima na 3), um `cTSWinProcObjectThumbnail`: `SetSelected`
(`0x38b28c`) aperta o botão (o quadro 2, a moldura verde); o `UpdateFunds`
(`0x38beb4`) habilita cada célula não escolhida a menos que o diálogo
obedeça o preço (`+0x171`, temp 1) e o item custe mais que os fundos (menos
o já escolhido com vários, `+0x170`); `SetEnabled` (`0x38b394`) mostra o
quadro 1 do fundo (azul) numa habilitada e o 0 (vermelho) com o botão
desligado numa desabilitada (`SetBackgroundImage` `0x38c554`: largura ÷ 4 ×
n). O lugar das células na grade (centradas no app) não foi lido.

**Notificações (tipos 8–10, 0x1C)**: o byte 10 é o tempo de vida
(`cTSUINotificationService::SetNotificationTimeOut`) e o byte 9 a prioridade
(`SetNotificationPriority`). A fila (`cTSUINotificationQueue`) conta com um
`cRZTimer` em **segundos** (`SetUnits(5)`, unidade base ms ÷ 1000) desde que
a notificação apareceu; `cTSWinNotification::IsDone`: some quando tempo ≠ 0
e decorrido ≥ tempo. 0 = fica até ser fechada (ou até o tipo 9 "Stop Text
Notification").

**Rodada n45: o formatador do idioma, as strings, a caixa de fechar e as
listas dos telefones (binário, Ghidra MCP; BV, com o base 1.0 onde dito).**

*O formatador de números e de hora.* O `FUN_0006e164` do base 1.0 é o
`RZ::FS()`; o `0x98(0)` dele é o `cRZLanguageManager::GetLanguageUtility`
(BV `0x12b5a8`): o `cRZLanguageUtility` embutido em `+0x44`, o do idioma
corrente (`+0x3c`). A vtable do utilitário (BV `0x27a5da0`): `0x24`
`MakeTimeString` (`0x137700`), `0x38` `GetThousandSeparator` (`0x136dd4`),
`0x48` `MakeNumberString` (`0x138448`, não é função no Ghidra: lido pela
listagem), `0x58` `CompareStrings` (`0x1352b4`), `0x6c`
`AreFirstAndLastNamesReversed` (`0x12cffc`), `0x70` `MakeFullName`
(`0x137c04`). O idioma (`+8`) usa os números dos STR# (0x23 = 35 é "pt",
`GetISO639LanguageCodeFromLanguageID` BV `0x12b708`).
- `MakeNumberString(long long v)`: `"%lld"` de |v|; com mais de 3 dígitos,
  grupos de 3 contados da direita, separados pelo de milhar; `"-"` na
  frente se v < 0. Separador: `","` nos idiomas 1, 2, 0xC, 0xF, 0x11–0x14,
  0x1C e nos não listados; `"."` em 4–8, 0xE e **0x23 (português do
  Brasil)**; o espaço sem quebra `"\xC2\xA0"` (o `+0x18` do utilitário, BV
  `0x135af4`) em 3, 9, 10, 0xB, 0xD, 0x10, 0x1A. O base 1.0 tem o mesmo
  laço (`0xa3e80`…, `"%lld"`, vtable `0x38`). O `ParseUIString` cai em
  `"%d"` só sem o utilitário. Ex.: `$$Money` de 12 × 100 dá "$1.200" em
  português e "$1,200" em inglês.
- `MakeTimeString(h, m, s, texto)`: h mod 24, m e s mod 60; em inglês dos
  EUA (1), no idioma 0x2A ou com a propriedade `force12HrTime` (só no BV),
  `"%u:%.2u %s"` com "am"/"pm" (0 → 12, 13–23 → 1–11; sem minuto
  `"%u %s"`); senão, com minuto e sem segundo: 2–5, 0x10, 0x1D
  `"%.2u:%.2u"`; 8, 0x24, 0x27, 0x28 `"%.2u.%.2u"`; 0xB, 0x1C, 0x1A
  `"%u.%.2u"`; os outros, **português do Brasil inclusive**, `"%u:%.2u"`.
  O `$TimeLocal` pede (hora, 0, −1): "17:00" em português, "5:00 pm" em
  inglês.

*De qual arquivo saem as strings.* O `+0x20` do quadro (vtable `0x50`, BV
`0xe17660`) é o `cTSBehavior` do quadro, o do objeto cujo código roda (o
dono do código: uma árvore semi-global que o objeto chama roda com o
`cTSBehavior` dele). A vtable do `cTSBehavior` (BV `0x27c0d70`): `0x20`
`GetGlobalGroupId` (`+0xc`), `0x24` `GetSemiGlobalGroupId` (`+0x14`),
`0x28` `GetPrivateGroupId` (`+0x1c`), `0x94` `GetOwningSelectorGUID`
(`+0x20`). O `cTSObjectDialog::SetParams` (BV `0x1d2b20`) lê as strings do
**grupo privado do dono do código** (`0x28`); com o byte 8 bit 0 do
semi-global da definição do dono (`0x38(GUID)` → `0x118`; sem ele, nenhuma
string); senão com o bit `0x40` do global. O bit 0 vence o `0x40`. O
`TryDialog` (BV `0xe2bb64`; notificações e painel do tutorial) faz o mesmo
com o arquivo da definição (`0x48` → `0x3c`) no lugar do `0x28`. **O grupo
da árvore não entra.** Conferido nos dados: a carta de chance ("Sub - Chance
Card - Dialog", árvore `0x203B` do `JobDataGlobals` `0x7F8F4EB6`, tipo 2,
byte 8 `0x0E`) roda no objeto de emprego e lê o STR# 0x12D **dele** (ex.:
grupo `0x7FBDC3E8`, 711 kB de cartas), não o do semi-global, que tem as
ofertas, as promoções e a carona; as árvores do semi-global que querem as
próprias strings marcam o bit 0 (`0x2004`, `0x2006` a oferta, `0x2021`,
`0x2027`, `0x2032`). O "Age Transition" do Controller - Age (privada, rodada
no Sim pelo Run Tree by Name) roda com o dono do código = o controlador,
então lê as do controlador.

*A caixa de fechar.* O `AddCloseBox` (BV `0x25d444`) só cria o botão se acha
a imagem (`FUN_000c8944` → vtable `0x1c`): `0x11110002` não está no
`ui.package` do jogo base, então não há botão nenhum. E o
`cGZWinBtn::SetBuffer` (BV `0x8c87d0`) chama o `SetStyle` (`0x8cb3bc`),
que mede o quadro da tira (largura ÷ 4, altura) e chama o `on_layout`
(`0x8c9c78`) com a largura e a altura que a janela já tem: **não dá tamanho
à janela** (fica 0 × 0 no canto de cima à esquerda, sem desenho nem
clique). O diálogo "continuar" fecha pelo Return, pelo Esc ou pelo tempo.

*A ordem dos telefones.* `SortNeighborCallListByName` (BV `0xa7c664`,
vtable `0xb8` do `cTSWinProcPhoneBookDialog`) ordena o vetor `+0x78` das
entradas com `CompareStrings(a.0x1c, b.0x1c, false)` do utilitário: o
**nome completo** (`GetFullName`, `+0x24`, que o `CreateNeighborCallList`
BV `0xa7bbb0` faz com o `MakeFullName` do utilitário: "nome sobrenome", só
um quando o outro falta; invertido em japonês, chinês e coreano), sem caixa.
`_CompareStringA` (BV `0x3b410`) → `FUN_0003d7dc`: `tolower` de cada byte
**com sinal** (`extsb`): os bytes ≥ 0x80 (letras acentuadas em UTF-8; a
conversão de codificação é UTF-8 → UTF-8, `FUN_000a2340` = 8) vêm antes de
qualquer letra ASCII, e "Ana" vem depois de "Anaí". A festa ordena as duas
listas com os predicados de lista (`ListBoxSortPredicate_Name` BV
`0x34089c` → `CompareByName` `0xa7a118`: sobrenome, depois nome, o mesmo
`FUN_0003d7dc`; `_Relationship` `0x340940` → `CompareByRelationship`
`0xa7a204`: amor ou paixão primeiro, a maior diária + vitalícia, depois
`CompareByName`); o `cTSWinListBox` (`FUN_0158d2a4`) usa o resultado como
comparação (< 0 antes).

*As linhas das listas* (`cTSWinListBox`, `cWinListBoxRow`). A festa
(`onAttach` BV `0x34264c`) cria quatro colunas: 3, largura − 33, 15 e 15
(`AddColumn` BV `0x194f08`, alinhadas à esquerda); `AddEntry` (BV
`0x340b48`): coluna 1 o nome completo, coluna 2 o ícone de amor
(`kListBoxIconLove`) ou, sem ele, o de paixão (`Crush`), coluna 3 o de
amigo (`Friend`; `PetFriend` num bicho, expansão) ou, sem amizade, o de
inimigo (`Enemy`). Os ícones são tiras de quatro e o
`cWinListBoxRow::GZPaint` (BV `0x194830`) desenha o quadro 1 na linha
escolhida, 2 na realçada, 3 na desligada e 0 nas outras, com a cor de texto
do mesmo índice (`kListBoxTextColorNormal`, `Selected`, `Highlited`,
`Disabled`); texto e ícone à esquerda da coluna e centrados na altura
((linha − altura) >> 1). O texto que não cabe na coluna é cortado com "..."
ao entrar (`FUN_001976a8` → `FUN_0019450c`: a fonte, vtable `0xcc`; a
string `0x5377be31` do gerenciador de janelas no lugar dos três pontos, se
houver). **Inferido:** os livros de vizinhos e de serviços com as colunas
da festa (o código deles não foi achado; os serviços sem ícones).

*O painel sem imagem.* `cGZWinBMP::GZPaint` (BV `0x8c7320`): sem a flag 2
de `+0x114` (`transparentbkg`; `InterpretPropertiesGZWinBMP` BV `0x95f068`,
atributo `0xb01`; o construtor começa com `0x12`, transparente) pinta a
área da janela com a `fillcolor` opaca (`GetFillColorARGB` | `0xff000000`)
e depois a imagem. O painel (21, 38)–(470, 430) dos telefones é um
`GZWinBMP` sem imagem com `transparentbkg=no` e `fillcolor` (109, 165, 182):
pintado (confirmado). **Falta:** o fundo das janelas com imagem e
`transparentbkg=no` não é pintado no app (no retrato do telefone a moldura
"overlay" tem `fillcolor` preta e cobriria a cabeça; não foi visto se o
diálogo muda essas janelas).

*A pausa no começo de um episódio do tutorial.* `cTSTutorialManager::
PlayEpisode` (BV `0x217740`) manda a mensagem `0x2bea2063` com
"vizinhança;lote", a mesma de entrar num lote pelo bairro
(`ConfirmAndMoveInFamily`, `SwapLot`, o clique no lote): o lote carrega
pelo caminho de sempre e o `ApplySavedUIState` (estados `0x0BEA28C2` /
`0x2C1805B0`, o do `EnterLotBegin`) aplica a pausa salva, sem ramo de
tutorial: **os lotes 1 e 4 do Tutorial abrem pausados**. Com o jogo
pausado o `cTSSimulator::TickAllObjects` (BV `0x498544`; `IsPaused`, vtable
`0x5c` BV `0x498a0c` = `+0x46`) só roda os objetos com o `SimFlag` 2 do
módulo de objetos (bit `0x20000` da palavra de cada ID;
`cEdithObjectModule::GetSimFlag`/`SetSimFlag` BV `0x524fb0`/`0x525008`,
vtable `0x12c`/`0x128`); o `AddObject` zera as flags e o
`MakeNewOutOfWorldObject` liga a 1 (roda); **quem liga a 2 não foi achado**
(no app: só os controladores do tutorial rodam pausados, inferido).

*No app (rodada n45).* `vm::languageNumber`/`languageTime` (`$Local`,
`$Money`, `$Cash`, `$Attrib`, `$FamilyAssets`, `$TimeLocal` pelo idioma
preferido); `scopeGroup` (o grupo privado do dono do código, o semi-global
dele com o bit 0, o global com o `0x40`; antes, o grupo da árvore primeiro e
o bit 0 lido como privado); no jogo base 89 Dialog que não achavam strings
passam a achar (as cópias privadas do "Set Result" com o bit 0 leem o
semi-global), 108 leem outra tabela e 12 perdem as suas (diálogos especiais
em árvores semi-globais cujo objeto não tem STR# 0x12D, como no binário);
`s2pkg bhav <Res> 7FBDC3E8 203B` mostra a carta de chance com as strings do
objeto de emprego. Os telefones: a ordem por nome completo (vizinhos) e por
sobrenome (festa) com o byte com sinal, as colunas da festa e o quadro 1 do
ícone na linha escolhida (`DialogWindow::frame`); as prateleiras com as
células do `0x2CB242F7`. Os lotes 1 e 4 do Tutorial ficam pausados. Medido:
`s2pkg autonomy` de N001_Lot68 e N001_Lot11, sementes 1–3, 12.000 ticks:
saídas idênticas às do main.

### 0x2E Snap (`TrySnap`)

**Quem se move sou eu**, em relação ao stack object (sem stack object: erro
0x15; eu inválido: 0x17). Modo u16 2 (STR# 0xCF): 0 slot do número no
parâmetro u16 0 (fora do intervalo: 0x08), 1 contido no stack object, 2 na
frente dele, 3 slot literal u16 0, 4 slot global; slot inexistente → 0x1B.
Falha → falso e grava no meu object data 52 ("primitive result") 1 (pediu a
um Sim no caminho para sair, flag 0x02), 0x0E, ou o código do erro de
posicionamento + 2. Com 0x10 só testa. Binário.

### 0x6F Change Light (`nTSSG::cObject::TryChangeLight`)

Objeto (dono byte 2, dado u16 3; inexistente: 0x17). Intensidade = byte 7
(ou temp 0 com o byte 1 bit 1) em %, **> 100 vira 50**; com o byte 1 bit 0 o
temp 1 é lido (duração). Luz byte 8 = 0xFF: todas as luzes do modelo; senão
o nome é a entrada do STR# 0x8E do stack object (sem stack object: 0x0A;
nome vazio: 0x4D). Sem modelo: 0x4C. Instantâneo (o fade é do renderizador),
verdadeiro. Binário.

### 0x79 Change Outfit (`nTSSG::cObject::TryChangeOutfit`)

Sim (dono byte 9, dado u16 10; inexistente: 0x32; não pessoa: 0x1C). Fonte
(byte 0): 0x01 a definição do stack object (stack object ausente = sem
fonte), 0x02 GUID u32 4, 0x40 GUID nos temps 0/1, nenhuma = as roupas do
próprio Sim; categoria = byte 8 (ou a variável (dono 1, dado u16 2) com
0x04); 0x20 zera os dados de pessoa 119 e 120; troca (0x10 reconstrói a
atual); 0x08 grava. Binário.

### 0x69/0x6A/0x6B Animate Object/Sim/Overlay (`try_animate_*`)

**O byte 3 não é flag: é a velocidade em 1/32** (0x20 = 1,0; com a flag 0x02
do byte 2, o temp 2). Isso explica o "0x20 em 96% dos usos" do código atual.
Animate Sim (binário): na primeira execução resolve o nome (lista byte 6;
0x80 = global da idade; senão a lista privada e, se não achar, as outras),
nome vazio → 0x43, tipo errado → 0x73; guarda o pedido no frame e **devolve
2 até a animação acabar, depois 1** (rodada n43, `anim-mac.md` §13.3: até o
fim da *ação* do pedido, quando ele começa a sair — `T` antes do fim, ou
no fim sem blend de saída —; o pedido segue tocando e saindo enquanto a
árvore segue); árvore de animação que não existe →
verdadeiro na hora ("missing animation tree"). Byte 13 / 100 é um peso de
blend (0 ou fora de 0–1 = 1,0). Animate Object/Overlay devolvem 1 logo depois
de começar (não bloqueiam); repetições no temp 1 com 0x20 (0 ou < −1 → erro
0x70). Sem animador: 0x41; sem pessoa (Sim): 0x42.

**Animate Object com temp 1 = −1 (fix-plumbob).** O −1 (o único valor < 1
que o erro 0x70 deixa passar) é "até parar" **em laço**: o "PlumbBob -
Start" (`0x7F4C04B9:0x1007`) toca `o-plumbob-spin` assim, e o jogo salva o
pedido com loops `0xFFFFFFFF` (o PlumbBob do `N001_Lot11`; o do
`N001_Lot68` também `o-plumbob-poses-default-0`). Antes a VM segurava o
último quadro depois de uma volta (o diamante parava). Agora
`Overlay::forever`: o tempo da VM dá a volta (`played` mod duração), o app
toca em laço, e o save grava `0xFFFFFFFF`. 49 usos do jogo base têm a flag
0x20 no Animate Object (alarmes, velas, pia, luzes). Um número N > 0 ainda
toca uma vez e segura (inferido).

**Generic Sims Call 25 (selecionar Sim, fix-plumbob).** `TryGenericSimCall`
(`0xe36834`, caso 0x19): temp 0 = 0 seleciona ninguém; senão o objeto tem
que ser pessoa (erro 0x1C) e vai para `SetSelectedPerson` (vtable +0xA0;
`cEdithObjectModule::SetSelectedPerson` `0x52a55c` troca o bit 2 do Sim
antigo e do novo e manda a mensagem 0x93 à UI). Os roteiros da história
usam isso ("Script - Pleasantville - Goth" → "Simulation - Select/Activate
Sim" `0x7F63E3F2:0x2031` seleciona a Cassandra no tick 27 do `N001_Lot11`,
e um roteiro do `N001_Lot68` seleciona o Daniel). O app agora segue o
global 3 quando a VM o muda (`followVmSelection`): pie, painel, câmera e o
PlumbBob, cujo "Main - Update My Owner" (`0x100F`) põe o PlumbBob no slot 0
do "Head Slots Holder" do Sim do global 3 (Snap Into #13). Antes o app
ficava com o Sim do `--sim-id`, o PlumbBob do jogo seguia o global 3 e o
app desenhava um segundo PlumbBob.

**Rodada 13: as regras de postura inferidas na rodada 11, conferidas** (BV:
`try_animate_sim` `0x69d634`, `try_animate_stop` `0x6a0554`,
`AddChoreoAnimRequest` `0x696644`, `CreateTransitionBlend` `0x68b4c4`,
`AbortAnimRequest` `0x68bac0`, `GlobalRootRotHeading` `0x68c320`,
`cObject::ApplyAnimationRotation`/`ApplyAnimationOffsets` `0x656b10`/`0x654b28`):

- **"Corpo inteiro substitui overlay de corpo inteiro": não existe no
  jogo** (rodada 13, anim-choreo, decifrado). O Animate Sim cria um pedido de
  tipo 2 (`tARType`: 1 objeto, 2 Sim, 3 overlay, 5 rota) e nada em
  `try_animate_sim` nem em `AddChoreoAnimRequest` mexe nos outros pedidos.
  A troca explícita é só a do `CreateTransitionBlend` (um pedido novo de
  tipo 5 faz o anterior de tipo 5 sair por blend). O que aparece é decidido
  **osso a osso** pela mistura dos canais (abaixo). A regra inferida da
  rodada 11 saiu do `animateSim`.
- **Mistura dos canais** (`nGZSceneDBAnim`): cada pedido é um conjunto de
  canais, um por osso que a animação mexe, e cada osso tem um
  `cChannelUberOutlet` que junta os canais "base" que o alimentam
  (`AddBaseChannel(canal, prioridade, tipo)`, BV `0xee6ed4`: a lista fica
  ordenada da prioridade maior para a menor). O misturador
  (`PrepareForBaseChannelBlender`, BV `0xee659c`, e o `ChannelData` do
  quatérnio, BV `0xd5e1c4`) faz, por osso: dos **níveis 8 a 0**, soma os
  pesos dos canais do nível (peso = função de blend do pedido × peso do
  pedido); se a soma ≥ 1, o nível leva o que sobrou inteiro (cada canal
  `peso × resto / soma`) e os níveis de baixo ficam com 0; se < 1, cada
  canal leva `peso × resto` e o resto vira `resto × (1 − soma)`. No fim, se
  o total ficou entre 0 e 1, tudo é normalizado (um canal sozinho entrando
  em fade já tem o osso inteiro); osso sem peso nenhum não é escrito
  (fica como estava). A combinação é uma média corrida por peso (slerp
  com `peso / acumulado`); os canais "delta" (aditivos) entram depois,
  multiplicando (ver "Canais delta" abaixo). **O nível de cada pedido** vem da primitiva
  (`AddChoreoAnimRequest` dá a ele também o nome de canal para depuração,
  as strings `sil/sim/...`):
  | primitiva | nível | nomes |
  |---|---|---|
  | Animate Sim (byte 12 = p) | `2p` (p < 0 → 0, p > 3 → 6) | `sil` 0–1, `sim` 2–3, `sih` 4–5, `siu` 6+ |
  | Animate Overlay (byte 11 = p) | `2p + 1` (p < 0 → 1; p ≥ 4 → 0) | `osl` 0, `ovl` 1–2, `ovm` 3–4, `ovh` 5–6, `ovu` 7+ |
  | Animate Object | 2 | `obj` |
  | `AddChoreoAnimRequest` tipo 5 | 2, 3 ou 5 | `rsm`, `rom`, `roh` |
  | locomoção (andar, começo, parada) | 0 | — |
  | Reach/Put: o grasp / a pose de carregar | 3 / 5 | `rom` / `roh` |
  Nos dados: o Animate Sim usa p = 1 em 2.984 de 4.010 (nível 2), 2 em 392,
  0 em 329, 3 em 304; os idles do idle system e da cadeira usam overlay de
  prioridade 0 ou −1 (nível 1), e as poses de carregar bebê ("Pickup -
  Poses", "CT - Set Carry", "Feed - Baby - Loop") prioridade 2–3 (níveis
  5–7). Logo o get-out da cadeira (nível 2) passa por cima do idle sentado
  (nível 1) sem regra nenhuma, e a pose de carregar o bebê fica por cima
  do Animate Sim. No código: `vm::animlevel`, `Overlay::level`,
  `AnimationState::level/weight`, e o `AnimPlayer` mistura assim
  (`Options::level`, `test_anim` "channel levels"). **Rodada 15
  (anim3), conferido**: a locomoção não passa pelo `AddChoreoAnimRequest`;
  o `cLocomotable::CreateLocoRequest` (BV `0x6b2158`) chama o
  `CreateChoreoLocoRequest` (BV `0x9492b0`) com a prioridade (o 18º
  argumento, `0x5c(r1)`, que vai ao `SetPriority`, vtable `0x44` do
  `cChoreoRequest` pela base virtual) = `r29` = **0**: o andar é nível 0
  (era 3 inferido). Os nomes `rsm`/`rom`/`roh` são do
  `try_new_reach` (BV `0x69111c`), que monta até cinco pedidos com os
  níveis 3, 2, 3, 3 e **5**, o último só quando há o nome da pose de
  carregar (a segunda entrada da lista, `a2o-newspaper-carry`): o grasp
  fica no 3 e a pose de carregar no 5 (era 5 inferido para os dois; o
  mapa das fases 0–3 para os nomes não fechou por inteiro). No código:
  `animlevel::Walk = 0`, `Grasp = 3`, `Carry = 5`.
- **Canais delta (aditivos)** (rodada 14, front "anim-additive", decifrado
  no BV): o que faz um canal ser delta é o **bit 15 das flags do canal**
  (`cSharedChannelCubicKeysConstProxy4::IsBaseDataReturned`, BV `0xee4680`:
  `byte[0x12] >> 7` da palavra big-endian; aceso = base, apagado = delta; na
  mesma palavra a duração são os bits 0–14 e o tipo os bits 22–24). O
  `cActiveAnimSkeletal::SetupChannel` (BV `0xd5b8d4`) pergunta isso (vtable
  `0x6c` do canal) e põe o canal no outlet do osso com
  `AddBaseChannel(canal, prioridade, tipo)` (BV `0xee6ed4`) ou
  `AddDeltaChannel(canal, prioridade)` (BV `0xee6dc8`: uma segunda lista em
  `+0xb8`, ordenada da prioridade maior para a menor, sem níveis). Nos dados
  (`s2pkg anim-sweep`): 1.033 canais de rotação e 689 de translação delta,
  **todos** em alvos de tipo 6 (`cAnimTarget::AnimType`; os ciclos de andar
  são 5, quase todo o resto 1), e esses alvos não têm canal base nenhum:
  106 animações — as posturas `walk/run-altK-*Blend` (idosos, grávida,
  deprimido, feliz, bravo), as mudanças de pé da conversa
  `*-soc-talk-shift*-SWAT`, as quicadas no sofá `*-sofa-playful-*bouncee*`,
  a cadeira assombrada, `e-idle-bentSpine[-FK]` (a coluna curva do idoso),
  `*-spine-bend-*` das cadeiras, `a2o-book-carry-blend`,
  `a-react-nod/shake-expBlend`, `p-run-blend`. Muitas têm a flag de corpo
  inteiro (bit 0) mesmo assim.
  **A mistura** (`cChannelUberOutletQ::ChannelData`, BV `0xd5e1c4`): depois
  da média dos canais base, se o peso base total > 0 (ou o outlet tem
  `ApplyDeltaDataToIK`, abaixo) e há deltas, cada delta com peso `w`
  (vtable `0x20` do canal no tempo, > ε) vira `q^w` (meio ângulo
  `a = acos(q.w)`, `(sin(a·w)/sin(a)·xyz, cos(a·w))`; nada com `q.w` fora de
  (−1, 1); sem trocar de hemisfério) e acumula `acc = acc · q^w` na ordem da
  lista; no fim **`base · acc`** (produto de Hamilton, `FUN_00dc71f8`), pela
  "delta base order" do outlet = 1 (o construtor, `FUN_00ee7004`, põe
  `+0xc4 = 1`; com 0 seria `acc · base`; quem chamaria
  `SetDeltaBaseOrder(false)` não foi achado). Translação
  (`cChannelUberOutletXYZ`, BV `0xd6b0fc`): a média base normalizada e, só
  se houve base, `+ w · t` de cada delta. O `SetupChannel` liga
  `SetApplyDeltaDataToIK(true)` (`+0xc7`) nos outlets de rotação de `head`,
  `neck`, `spine0`, `spine1` e `spine2` (hashes `0x42CD1793`, `0x1A9F50FD`,
  `0xE446CAC7`, `0xE087D770`, `0xEDC4F1A9`): neles o delta vale mesmo sem
  canal base (o look-at mexe neles). No código: `Animation::Channel::isDelta`,
  `sampleDeltas`, `deltaPower` e o fim do `AnimPlayer::pose` (depois da
  mistura por níveis); `samplePose` e `keyedBones` ignoram os deltas (um alvo
  só de deltas não toma osso nenhum, nem com a flag de corpo inteiro, e as
  cadeias de IK dele não resolvem nada); o Animate Sim de um clipe só de
  deltas toca por cima do corpo, como um parcial. Inferido: o peso do delta
  como fade × peso do pedido (o `0x20` do `cChannelUber` não foi lido) e o
  espelho de um delta (pelo espelho da pose "repouso + delta"). **Deltas
  nos nós de IK (rodada 15)**: o `cActiveAnimIK` lê o alvo e o polo pelos
  outlets dos nós (`SetTranslationChannelOutlet`/`SetRotationChannelOutlet`),
  que já levam base × delta; no `AnimPlayer` os nós de alvo e polo que uma
  camada lê para o IK recebem os deltas que o outlet tomou (as quicadas no
  sofá, `*_handcontrol1`/`*_ikctr` delta, mexem nas mãos e pés do sentado).
  Testes: `test_anim` "testDeltaChannels", "testDeltaIk".
- **Orientação de junta do osso** (rodada 20, ramo `r20-arms`; regra tirada
  dos dados, inferida, não lida no binário): o `cBoneDataExtension` v4 de um
  nó do CRES traz uma rotação (`cBoneDataExtension::JointOrientation`). No
  `auskel` e no `puskel` só as clavículas (−14° em X, uma torção ao longo do
  osso) e a base dos polegares (45°) têm uma; no `cuskel` as clavículas
  (−10°), no `buskel` o pescoço. A pose de repouso do CRES é `orientação ×
  chave` (clavícula: `qx(−14°) · qz(∓90°)`, e as animações chaveiam só
  `z = ∓90`, `x ≈ 0`), então a chave de rotação de um canal-base vira
  `local = orientação × Euler XYZ` (`samplePose`). Sem isso os braços de
  todas as poses ficavam 14° para trás (no idle em pé a mão 8 cm atrás do
  ombro, atrás da coxa; "mãos nas costas" no balcão, playtests 16/30.png);
  com ela a mão fica ao lado da coxa, 6 cm à frente do ombro, como no jogo.
  `OS2_DEBUG_POSES`, 15000 ticks: N001_Lot68 1318 → 632 quadros com a mão
  atrás das costas (sobram golpes do tapa, danças, pegar do chão e da mesa de
  centro), N002_Lot4 95 → 17. `s2pkg skeleton` mostra as orientações. Teste
  `test_anim` "testIdleArmsHangForward". `docs/screenshots/r20-arms-antes-depois.jpg`.
- **Onde fica o quadro da animação**: na fila de coreografia. A posição e a
  direção "globais" do objeto animado são as do osso raiz da fila
  (`GlobalRootTransPosition`, `GlobalRootRotHeading`: o vetor +Y do osso
  raiz girado, `atan2`), lidas no `snap_to_current_location`/`PostSnap` para
  deixar o objeto onde a animação o largou. Os objetos ainda têm um
  deslocamento e uma rotação de animação nos **dados de objeto 108–110**
  (x, y, z × uma constante) e **111** (ângulo × outra constante), aplicados
  por `cObject::ApplyAnimationOffsets`/`ApplyAnimationRotation`.
- **O giro pelo `heading` do cabeçalho do ANIM: o jogo aplica** (rodada 13,
  decifrado). `cActiveAnimSkeletal::SetupChannel` (BV `0xd5b8d4`) soma o
  `HeadingOffset` do pedido (`cChoreoRequest` vtable 0xd4) e o do recurso
  (`cAnimResource::HeadingOffset`, o campo do cabeçalho) e gira por esse
  ângulo o canal de rotação do osso marcado com um eixo de heading
  (`cBoneDataExtension::HeadingOffsetAxis`: X, Y ou Z). É o que o
  `AnimPlayer::Options::turn` faz (os filhos do osso do topo girados em Z).
  O heading **do pedido** vem do Animate Sim: com o bit 0x04 do byte 8 (sem
  texto no editor; 7 usos nos dados) é `−temp 4 × k` (`FUN_008b6860`),
  senão 0; o Animate Overlay faz o mesmo com o bit 0x02 do byte 15
  (`try_animate_overlay`, BV `0x69ea54`). **k = π/4** (rodada 15): o
  global `0x2c4f038` não tem escrita achada, mas as funções vizinhas que o
  usam convertem as 8 direções do SimAntics: `FUN_008b61fc(d) = (2 − d)·k`
  e `FUN_008b68a4` volta de radianos `mod 8`; o temp 4 conta passos de
  45° (horários). Feito: `animlevel::headingOfDirections`,
  `AnimationState::headingOffset`, `Overlay::headingOffset`, somados ao
  `turn` do recurso (também nos overlays).
- **Cadeira de jantar: assento, levantar e o quadro das animações** (rodada
  17, ramo `r17-chair`; o que é do app é inferido). O assento
  `slot_chairDining_0` fica sob o `slot_chairdining_trans`, que a cadeira
  empurrada deixa 0,373–0,375 para a mesa (pose salva, valor fixo do
  `o2a-chairDining-getIn-SE`); o "leaveOut" move e gira esse osso
  (−0,087; 0,048; −15,6°). O Sim sentado é desenhado no osso como o ator da
  cadeira o posa no instante dos Sims (`SimActor::bonePlacement`,
  `VmObjectView::slotPlacement(sim, now)`; antes usava os ossos do quadro
  anterior, ou a pose de repouso enquanto o ator não tinha sido posado). O
  levantar ("Chair - Animate - Get Out", `0x7F0B61F3 0x2045`) faz Snap ao
  osso de canto `route_chairdining_2_se` (temp 0) e "Snap - Away" vira o
  Sim (direção 1 sentado → 7 → 3); o `a2o-chairDining-getOut-SE` tem
  heading −180° e começa sentado em (−0,362; 0,350) do seu quadro, que é
  exatamente o assento (1 cm no teste). Dois defeitos do app faziam o Sim
  aparecer sentado no ar ao lado da cadeira, meio dentro da quina da mesa
  (playtest, `N001_Lot68`): o corpo girava até a direção nova a 7 rad/s
  (≈0,34 s, mais com poucos quadros) levando o quadro da animação junto, e
  a animação começava com fade a partir da pose antiga no lugar novo.
  Agora uma animação do Animate Sim toma a direção do Sim na hora
  (`SimMotion::turn`) e, se o Sim acabou de ser movido ou girado por Snap,
  começa sem fade (`LotSimulation::sampleAll`).
- **Vários alvos de IK por cadeia, pesados pelo canal de contato** (rodada
  17, ramo `r17-chair2`; a mistura é inferida). Uma cadeia de IK do ANIM
  lista vários alvos (`IkTarget`), cada um com o nome de um canal de
  contato (atributo 3, o 6º CRC: `r_handcontrol0_a` para o alvo próprio
  no espaço do modelo, `r_handcontrol1_a` para o da coxa, tipo 1,
  `r_handcontrol1_b`/`_c` para ossos do objeto de IK, tipo 2). O
  `a2o-chairDining-getOut-SE` passa a mão do alvo próprio para o
  `*_seatgrab`/`*_pulltarget` da cadeira e volta, pelos canais 0→1→0. O
  opensims2 usava só o primeiro alvo: a mão ia para a chave parada do
  alvo próprio enquanto a animação queria a cadeira (braço comprido e
  reto, ou esticado para trás na mesa/cadeira; QA do usuário). Agora
  `applyIk` mistura os alvos pelo peso do contato (posição linear, rotação
  da mão por nlerp); um alvo tipo 2 sem objeto de IK fica de fora, e sem
  nenhum em contato vale o primeiro (o tipo 2 sem objeto, no espaço do
  modelo, como antes). Os objetos animados pela VM são posados antes dos
  Sims a cada quadro (os ossos da cadeira para o IK do mesmo instante).
- **Cadeira de jantar deixada para fora ("leaveOut"): o Sim no assento**
  (rodada 18, ramo `r18-chairs`, QA do usuário "as cadeiras estão
  estranhas"; o que é do app é inferido). As cadeiras **não** estão tortas
  ao carregar: as quatro Value1 do `N001_Lot68` (643/645/648/651) e as
  Colonial do `N002_Lot3` têm `slot_chairdining_trans` 0,373–0,374 para a
  mesa e rotação zero salvos (`OS2_LOT_OBJECTS=Chair OS2_LOT_NODES=1 s2pkg
  lot`), e são desenhadas assim. O par "leaveOut"
  (`o2a-chairDining-getIn-leaveOut-SE` / `getOut-fromOut-SE`, que as árvores
  escolhem quando o Sim senta com uma caneca, um prato ou o jornal — o caso
  mais comum na autonomia) move o próprio `slot_chairdining_trans`
  (−0,087; 0,048, girado −15,64°, espelhado quando "flipped"): a cadeira fica
  puxada e torta **enquanto o Sim está sentado** (fiel ao jogo) e o
  `getOut-fromOut` a devolve a 0,375 sem rotação (conferido no ator e no
  teste). O defeito era do Sim: o `a2o-chairDining-getIn-rightHand-
  leaveOut-SE` (heading 0 no cabeçalho) é feito a partir de onde o Sim
  está, o osso de canto `route_chairdining_2_sw` (17,105; 25,0, virado 45°),
  e acaba sentado 0,67 dali, exatamente no assento puxado; o Sim só entra no
  slot da cadeira (container) depois, e aí o app o desenha no osso do
  assento. A última pose do get-in, mantida por baixo das idles da cadeira,
  ia junto e o Sim aparecia sentado 0,67 ao lado da cadeira, entre duas,
  com a cadeira dele vazia e torta (`14.png`, `r17-chair2-sentar.jpg`). O
  `getIn-SE` normal é feito no quadro do assento (começa no canto, termina
  em (0,006; 0,010)), por isso não mostrava o defeito. Agora, quando o Sim
  entra ou sai de um assento, o que já toca fica onde estava no mundo
  (`AnimPlayer::reframe`, chamado por `LotSimulation` na troca entre o
  lugar da VM e o osso do assento); o que começa depois toca no quadro
  novo (rodada 21: `AnimPlayer::stayInWorld`, regra 3 do módulo de assento;
  `OS2_NO_STAY_IN_WORLD=1` desliga). Teste
  `testDiningChairCycle` (com `OS2_DATA`): cadeiras do lote carregam
  empurradas; ciclos getIn/getOut e leaveOut/fromOut terminam empurrados; o
  get-in leaveOut do canto acaba no assento puxado e fica lá depois do
  reframe. `r18-chairs-lot68-antes-depois.jpg`,
  `r18-chairs-lot68-levantar.jpg`, `r18-chairs-n002-lot3-antes-depois.jpg`.
- **Camadas mantidas de objeto juntadas** (rodada 19, ramo `r19-chairlayers`;
  **rodada 21: substituído** — quando todas acabam, a pose vira a base;
  regra do app, **inferida**: a VM fica igual). A VM nunca tira os overlays
  de uma cadeira de jantar: cada sentar e levantar põe duas camadas de uma vez
  só (nível e prioridade 1 + id do overlay, `lot_vm_setup.mm onObjectOverlay`), que
  ficavam no ator para sempre (N001_Lot11, 3 dias: até 8 camadas numa cadeira,
  129 nos atores de objeto). Num ator que mantém a pose (`setHoldPose`), quando
  toda camada já tocou uma vez e está parada no último quadro há 1 s (sem
  loop, sem fade-out, sem canais delta, IK ou velocidade), o `AnimPlayer` as
  junta numa camada só: a pose que davam e, por osso, o peso que tomavam
  (1 − o que sobrou), no nível mais alto delas; o que toca depois por cima
  mistura com ela como misturava com elas (o blend-in de 0,1 s inclusive). A
  junção também é feita no `play()`, para objetos que ninguém desenha. Supõe
  que camadas novas vêm em nível acima (os ids crescem); uma camada mantida
  coberta não volta mais quando a de cima é tirada (antes voltava). Testes
  `testHeldLayersCollapse` (200 ciclos, mesma pose que sem juntar) e
  `testDiningChairLayersBounded` (com `OS2_DATA`: 60 ciclos da Value1, normal e
  leaveOut, puxada/torta sentada e empurrada depois, ≤ 3 camadas);
  `OS2_DEBUG_LAYERS=1` conta as camadas a cada hora de jogo nas rodadas sem
  tela, `OS2_NO_LAYER_MERGE=1` desliga a junção. N001_Lot11 (3 dias, 60
  overlays de cadeira de jantar): no máximo 1 camada por ator (49 em 64
  atores) contra até 8 (129) sem juntar; N001_Lot68: 1 contra 6.
- **Onde o Sim é desenhado: uma regra** (rodada 19, ramo `r19-eat`, playtests
  do usuário 15–19.png; o que é do app é inferido). A raiz do Sim é a posição
  e a direção da VM (interpoladas entre os dois últimos ticks) ou, contido num
  slot, o osso do slot no quadro do modelo do contêiner; a animação toca com o
  movimento de raiz dela e o heading do cabeçalho. Quando essa raiz pula (entra
  ou sai de um assento, um Snap, o fim de uma escada que põe o Sim no topo),
  o que já toca e toca uma vez fica onde estava no mundo
  (`AnimPlayer::reframe`, `onlyOnce` fora dos assentos) e a animação seguinte
  faz o blend a partir dali; o corte do blend depois de um Snap da rodada 17
  saiu (a mesma regra cobre o levantar da cadeira). A VM pulando mais que um
  passo num tick (`SimMotion::kMaxStep` = 1,5 tile; um visitante correndo faz
  0,7) ou de andar não é interpolada: o corpo vai direto. Defeitos achados com
  o detector (`OS2_DEBUG_JUMPS=1`, `app/jump_detector`) e as checagens de pose
  (`OS2_DEBUG_POSES=1`), sem tela:
  - **Slot de parte de objeto multi-tile**: os ossos dos slots estão no modelo,
    desenhado no tile líder. O prato posto no slot 1 da parte "1,0" da mesa de
    jantar (`slot_table_eat_1_N`) era posto a partir da posição da parte e
    ficava um tile fora da mesa, no ar (15.png). Agora o quadro é o do líder
    (`VmObjectView::modelFrame`), também para o Sim no assento de uma parte
    (sofás). `r19-eat-prato-mesa-antes-depois.jpg`.
  - **Fim da escada reta**: a VM põe o Sim no topo num tick; o app o desenhava
    a meio caminho e depois com o último quadro da subida (raiz 4,4 tiles
    escada acima) aplicado a partir do topo: 1,5–3,4 tiles fora por 3 quadros,
    no topo e embaixo (o Daniel "teleportando", `N001_Lot68`).
    `r19-eat-escada-salto-antes-depois.jpg`.
  - **Animação de objeto no Sim**: a VM põe o `o2a-book-carry` (alvo "book",
    chaves em `root_trans`/`root_rot`) como overlay no leitor; tocado no
    esqueleto da criança, o corpo afundava no chão um tile ao lado e ia e
    voltava ao andar (`N001_Lot11`). O jogo liga um alvo ao esqueleto de mesmo
    nome: um alvo de objeto não liga osso nenhum de um esqueleto de Sim.
  - Sem tela, os objetos agora são posados e postos a cada passo como num
    quadro ao vivo (o assento do carro ficava onde o último quadro o deixou).
  Conferido sem defeito (sem tela, `N001_Lot68`): poltrona Art Nouveau, banco
  de bar (o Sim fica no banco; o do outro lado do balcão é o banco certo),
  comer à mesa (prato na mesa à frente), café na cadeira "leaveOut", sair da
  piscina pela escada. Teste `drawn_places` (com `OS2_DATA`): escada, prato,
  criança lendo; `test_jump_detector`. Censo de 15000 ticks em `N001_Lot68`,
  `N001_Lot11`, `N002_Lot10`, `N001_Lot36`: 0 saltos sem razão da VM, 0 Snaps
  repetidos.
  - **Sims deslizando sem andar** (rodada 20, ramo `r20-slide`, relato do
    jogador em Estranhópolis; do app, não do binário). Detector
    `OS2_DEBUG_GLIDE=1` (`app/glide_detector`): a pélvis desenhada anda mais
    de 0,4 tile/s por 1 s enquanto os pés, medidos da pélvis no quadro do
    corpo, mexem menos de 0,06 (`OS2_GLIDE_LEGS` muda o limite); cada
    episódio sai com rota, clipe da locomoção, Animate Sim, overlays da VM,
    camadas do `AnimPlayer`, interação e árvore. Causa: o `LotSimulation`
    punha o começo de cada overlay na fila do ator (`SimActor::cue`, tocada
    no quadro seguinte) mas tirava os que a VM tirou **na hora**
    (`player.stop`). Com vários ticks num quadro (velocidade 3 ou 4) um
    overlay que entrou e saiu nesse meio tempo era parado antes de começar e
    começava depois, em laço, para sempre: o idle "breatheLoop" de nível 1
    por cima do andar (nível 0), as pernas paradas com a VM sem overlay
    nenhum. Agora todas as paradas vão pela mesma fila (`cueStop`). Sem
    tela, 8 h de jogo: velocidade 3, 370 episódios → 1 (5 lotes); velocidade
    4, 151 → 1 (4 lotes); velocidade 1, 1 → 1. Os que sobram: o beijo
    (`a2a-soc-kissPeck`, o corpo se inclina com os pés parados: certo) e, no
    começo do `N001_Lot11`, 0,3 s da criança com o breathe da VM ainda ligado
    na rota retomada do save. Fantasmas (`*-walk-ghost`) flutuam de propósito.
    Teste `glides` (com `OS2_DATA`), `test_glide_detector`.
- **Módulo único de assento** (rodada 21, ramo `r21-seat`; `app/seat_module`,
  substitui os remendos das rodadas 11–19; o que é do app é inferido salvo
  onde o binário é citado). Uma regra para ficar em pé, andar, sentar, ficar
  sentado, trocar de assento e levantar (e deitar na cama/banheira, vaso,
  escada, piscina):
  1. **Raiz**: onde a VM põe o Sim (posição e direção, interpoladas entre
     ticks ao andar, direto depois de um Snap); **contido** no slot de um
     objeto (dados 2/3), preso ao slot: o osso do slot no quadro do modelo do
     contêiner (líder do multi-tile), como o ator do contêiner está posado
     **naquele instante** (`VmObjectView::slotPlacement`, a mesma conta dos
     objetos dentro de objetos: `slotFrame`).
  2. **Corpo**: as animações a partir dessa raiz, com o movimento de raiz, o
     heading do cabeçalho e o do pedido, misturadas por níveis (§4); nenhum
     outro ajuste do lugar do Sim.
  3. **Pulo da raiz** (contido/solto, Snap, fim da escada): o que toca uma vez
     (o último quadro mantido ou ainda tocando) e o que está saindo por fade
     fica onde estava no mundo (`AnimPlayer::stayInWorld`, o `PostSnap` do
     jogo que põe o objeto onde a animação o deixou); camada em loop ainda
     tocando segue a raiz nova. Substitui o `reframe` (e o `onlyOnce`) das
     rodadas 18/19, agora a mesma regra para assento e salto.
  4. **Sentado** sem nada tocando no corpo: segura a pose (não volta ao idle
     em pé entre as idles da cadeira).
  5. **IK no mesmo quadro**: objetos animados posados antes dos Sims; um Sim
     que alcança ossos de outro Sim (beijo, abraço) é posado de novo depois
     dos dois.
  Mudanças de semântica que vieram junto:
  - **Pedido que acaba expira**: camada tocada uma vez, ao terminar, vai para
    o nível −1 (abaixo de tudo): os ossos guardam o último valor só onde
    nenhum outro canal escreve, como o `cChannelUberOutlet` (osso sem canal
    não é escrito); um pedido novo da mesma prioridade ainda faz crossfade
    dela. Antes o get-in mantido (nível 2) cobria as idles da cadeira
    (nível 1), que não apareciam nunca; e o "a2o-table-get" em pé ficava por
    cima da idle do xadrez depois do alcance. As fases da locomoção não
    expiram (`Options::expires`; a seguinte toma o lugar).
  - **Objetos**: quando todas as camadas acabaram, a pose do momento vira a
    pose-base e as camadas saem (ossos mantêm o valor); substitui a junção em
    camada mantida do r19-chairlayers (`OS2_NO_LAYER_MERGE` saiu).
  - **IK pela parte do nível**: cada cadeia pesa pela parte que a camada tem
    do osso da ponta da cadeia (mão, pé), inteira; as camadas resolvem em
    ordem de nível, cada uma a partir do corpo como já está, e as soluções de
    um osso entram numa média corrida a partir da parte que a mistura deixa a
    ele. Antes o IK de uma idle de nível 1 (`a2o-chairDining-idle-breathe`)
    forçava as mãos por cima do braço que come (nível 2): mãos a 0,17–0,31 do
    alvo.
  - **Ossos do assento estático como alvo de IK**: o assento que contém o
    Sim, desenhado sem ator (namoradeira), dá os ossos do próprio modelo aos
    alvos de tipo 2 (a mão na braçadeira; antes ficava de fora e caía no
    alvo no espaço do modelo, 0,51 longe). Outros objetos estáticos continuam
    de fora (inferido; não conferidos).
  - **Alvos de IK girados uma vez só** (rodada 21, ramo `r21-childseat`):
    os nós de alvo e de polo (`*_foot_ikctr`, `*_foot_ikpole`, `*_hand_ikpole`,
    filhos diretos do nó do topo do esqueleto) entram no `applyIk` como a
    animação os grava, e o `applyIk` os gira pelo quadro da camada (heading
    do cabeçalho + do pedido + `stayInWorld`). Antes vinham da pose já girada
    e eram girados de novo: as `c2o-chairDining-SitFwd-*` da criança são
    feitas de frente para 180° (heading do cabeçalho π, `root_rot` com 180°
    em Z) e seguram as pernas só por IK do pé; giradas duas vezes (360°), os
    pés iam para trás do assento (criança "ajoelhada" na cadeira com as
    canelas atravessando o encosto, playtest 32.png). As idles sentadas do
    adulto têm heading 0 e não mostravam. Checagem nova `legs` na bateria
    (pé sentado mais de 0,1 atrás da pélvis no quadro do slot).
  Mantidos (do binário): espelho do Animate Sim/Object/Overlay (flip),
  mistura por níveis, os vários alvos por canal de contato, o giro na hora
  do Animate Sim (`SimMotion::turn`). Bateria `tests/seat_battery.sh`
  (`OS2_DEBUG_SEATS`, `app/seat_check`, `OS2_THEN_AT`): pélvis sentada sobre o
  slot, mãos ao alcance dos alvos de IK, nada atravessando mesa/cadeira em
  repouso, sem carregar com a mão vazia, sem pulo > 1 tile. Fotos
  `docs/screenshots/r21-seat-*.jpg`.
- **Alvos de IK de tipo 2: o objeto de IK do pedido** (rodada 15, front
  anim3, decifrado). Os alvos de IK (`cIKTarget::TargetType`) têm 3 tipos
  nos dados (`s2pkg anim-sweep`): 0 sem osso de quadro (20.859, o alvo no
  espaço do modelo), 1 um osso do próprio esqueleto (4.742) e **2 um osso
  de outro objeto** (2.118: `a2b-babybottle-feeder-*` com a mão direita no
  `head_grip` do bebê e a esquerda no `bottomtarget_surface` dele, as
  poses de carregar `a2b-carry-belly`, os abraços do sofá/banheira nos
  ombros do outro, a barra do supino). O `AddChoreoAnimRequest` (BV
  `0x696644`) recebe o ID do **objeto de IK** (o `short*` do 6º
  argumento): se o recurso pede alvo (vtable `0x140(2)`), acha o objeto,
  pega o nó de cena dele e passa ao `ValidateIKTargetBones` e ao
  `CreateChoreoAnimRequest` (erros 0x62 sem ID, 0x17/99 sem objeto/nó,
  100/101 ossos que faltam); o `InitIKAndLookAtTargetsInChoreoRequests`
  (BV `0x699100`) refaz isso para os pedidos vivos. Quem dá o ID: o
  Animate Sim, o dono do byte 9 / u16 10 (o mesmo "IK object" que já ia
  como parâmetro 1 da árvore de eventos); o Animate Overlay, o **temp 6**
  com o bit 0x08 do byte 15 (`try_animate_overlay`), senão nenhum. O
  opensims2 resolvia o osso de tipo 2 no próprio esqueleto: o adulto que
  dava mamadeira levava a garrafa ao **próprio** `head_grip` (o rosto) e
  a mão esquerda ia ao espaço do modelo, deixando o bebê baixo — era a
  causa do "inclina e o bebê some no peito" da rodada 12. No código:
  `applyIk(..., IkObjectBone)`, `AnimPlayer::Options::ikObjectBone`,
  `LotSimulation::ikObjectBones` (o ator do objeto de IK, Sim ou objeto
  animado, no espaço do modelo de quem anima; o carregador é posado de
  novo depois que o carregado é posto no osso, para as mãos alcançarem o
  bebê da mesma imagem), `Overlay::ikObject`. Sem objeto (ou sem o osso)
  o alvo fica no espaço do modelo (inferido); camada espelhada usa o osso
  gêmeo refletido. Testes: `test_anim` "arm IK: a type-2 goal",
  `test_animation_vm` "IK object in temp 6". Captura:
  `docs/screenshots/r15-mamadeira-ik.jpg` (o alienígena de N002_Lot10 dá
  mamadeira: antes sem objeto de IK, depois com).
- **Tempo de blend: T = 233,333 ms** (decifrado). O global `0x2c0ddd0` é
  escrito uma vez, pelo inicializador estático do `TSSGAnimatable.cpp`
  (BV `0x20b4ef0`, no `__StaticInit`; o mesmo valor vai para `0x2c0de34`),
  com o literal `233.33333` (7 quadros a 30/s). Unidade: ticks de
  animação (ms), como as durações com que o `CreateTransitionBlend` o
  compara. O Animate Stop usa **0** sem flag (os pedidos saem na hora,
  `cChoreoRequest::Abort` com status 3, vtable 0x228 da fila), **T** com a
  flag 0x02 ("blended Out") e **0,3 × T** com 0x20 ("short blended Out";
  `FLOAT_0274b670` = 0,3), pelo `AbortAnimRequest` (`FUN_00a47bf0`: o
  pedido ganha uma função de blend que desce a 0 nesse tempo e a regra de
  expiração 4). O mesmo T é o blend de entrada/saída do
  `CreateTransitionBlend` (com as flags "No Blend in/out" desligadas) e o
  que o Animate Sim usa para tirar o próprio pedido interrompido. No
  código: `vm::kBlendTicks`; o overlay começa a sair (evento 9997) T antes
  do fim, entra em T, e o Animate Stop passa ao app o fade de cada overlay
  tirado (`AnimationState::stoppedFades`).
- **Outros detalhes do Animate Stop** (`try_animate_stop`, BV `0x6a0554`):
  a prioridade do byte 8, se > 0, só para os pedidos cujo nível com o bit
  0 apagado é `2p − 2` (o Animate Sim de prioridade p − 1 e o overlay de
  mesma prioridade); só para os pedidos tocados espelhados como o operando
  diz (bit 0 do byte 2, ou o temp 3 com 0x08), os dois com 0x40; pedidos
  dos tipos 7 e 9 e os já expirados nunca param. Modos 4–11 comparam o
  `tOEType` (4→3 carregar … 8→2 normal, 9→7, 10→8, 11→9), como o código já
  fazia. Feito em `animateStop`.
- **Modo 0 para todos os pedidos do nome** (rodada 18): o `try_animate_stop`
  junta numa lista todo pedido de overlay cujo nome bate (ID `0x6000000x`
  com o bit de espelho, não expirado, tipo ≠ 7/9) e aborta todos, não só o
  primeiro. Como o Animate Overlay aceita dois do mesmo nome
  (`AnimRequestByNameAndID` duas vezes, só pedidos ativos: tocando, sem
  parar, em loop ou antes do fim), a get-in com caneca começa dois
  "a2o-coffee-DN-carry"; parar só o primeiro deixava um depois de levantar
  (o braço preso na coxa sentada enquanto o Sim andava; QA 15). Teste em
  `test_animation_vm`.
- **O evento −1 no Animate Stop**: o `try_animate_stop` não chama árvore
  de evento nenhuma; o pedido parado na hora é abortado com status 3
  (`tCRStatus`) e quem avisa é o tratador de eventos do pedido
  (`cAnimEventCB` → `cCustomEventHandler2`, chave −1 → −1). Que o status 3
  vire a chave −1 continua inferido (bate com as árvores de idle); o
  pedido que sai por blend mantém o status e o evento dele no fim do blend
  não foi lido.

## 4b. Pendências do round 5 (binário)


**Veículos (rodada 12, VM).** O carro dirige pelo "Event Tree - Drive"
(CarGlobals `0x7F4EA230`, `0x2008`): no 9999 de cada volta de
`o-…-drive` decide parar ("Function - Should Stop?" pela distância até o
portal de parada) ou tocar a volta de novo com Animate Object. No
opensims2 isso travava de dois jeitos: (1) o limite "duas de um nome" do
Animate Overlay valia também para o Animate Object, e a terceira volta não
começava (nunca chegava o 9999, "Wait for Vehicle Stop" esperava para
sempre: a van da empregada e o ônibus escolar paravam no meio-fio o dia
todo); agora a mesma animação do Animate Object substitui a que toca. (2)
o motor move o objeto pela raiz da animação (`cAnimatable`); a VM não
movia, e a distância nunca diminuía: agora, no fim de cada volta, o objeto
(todas as partes do grupo) anda para a frente (direção do objeto) o
quanto o `root_trans` da animação anda (Y do modelo; ~1,95 tile em
`o-vehicleMinivan-drive`; inferido o momento e a direção). Prova:
`S2PKG_VEHICLES=1 s2pkg autonomy N001_Lot68 --ticks 24000`: a empregada
chega às 9:07, desce em (35,5; 16,5), trabalha e a van vai embora antes
das 10:30; o ônibus escolar chega às 8:00, espera ~25 minutos, buzina e vai
embora. Os adolescentes não sobem porque "Get My Riders" só chama quem
tem humor (motivo 3) > 0, e os Sims do lote 68 estão com humor muito
negativo (fome: frente da comida). A textura
`vehicleschoolbus-groundshadow-alpha_txtr` que o TXMT da sombra do ônibus
pede não existe nos dados do jogo base (só o nome no material).

**Rodada 18 (r18-life): quanto o veículo anda.** Os `root_trans` das
animações de veículo não começam em 0: `o-vehicleFire-stop` vai de −8,99 a
0, `-start` de −9 a 0, `-drive` de −3 a −0,18, e só o `-driveOff` termina à
frente (+21,9); a van (`o-vehicleMinivan-*`) termina em 1,95, que é onde o
esqueleto dela descansa a raiz (`vehicleminivan`: `root_trans` em
(1; 1,9474)). Ou seja: as voltas de dirigir, partir e parar trazem a raiz de
trás até o lugar de repouso (o veículo já fez o Snap para o osso
`slot_travel_drive` à frente) e não devem mover o objeto; mover pelo
percurso da volta (fim − começo, rodada 12) fazia o caminhão de bombeiros
parar 9 tiles depois do portal "Car - Service Stop": no `N001_Lot11` a porta
(osso `route_vehiclefire_3_w`, 6,8 à frente da origem) caía fora do lote, o
Snap do "Interaction - Get Out" falhava (2, fora do lote) a cada 200 ticks
e o bombeiro nunca descia; o segundo saía mas não voltava ao caminhão
("Get In": sem metas). Agora o objeto anda **onde a raiz termina menos onde
o esqueleto a descansa** (`AnimationLibrary::rootTravel(nome, VM::rootRestY)`;
menos de meio tile conta como 0). Inferido dos dados, não lido no
`cAnimatable`. Com isso, no `N001_Lot11` com fogo no fogão (`s2pkg pie
N001_Lot11 Mortimer 118 --create "0x24C95F99@17,20,2!"`; o `!` final
inicia o Main do Fire depois de posto): pânico, o fogo se espalha, o
alarme de fumaça chama o bombeiro, o caminhão para no portal, o bombeiro
desce, apaga os três Fires, se despede ("Tenho que ir..."), sobe e vai
embora. A van da empregada (Lot68), a carona e o ônibus escolar (Lot11)
continuam chegando e indo embora.

### Conversão da versão 0 do Animate Overlay (rodada 13)

`AnimateOverlayParam::UpgradeVersion` (BV `0xac4af8`): na versão 0 (os
BHAVs do jogo base, 1/3 dos Animate Overlay, entre eles "CT - Set Carry" e
"Pickup - Poses") os bytes 10–11 e 12–13 são dois u16; a conversão põe no
byte 10 o byte baixo do primeiro, no **byte 11 (a prioridade) o byte baixo
do segundo** e zera 12–13. Sem isso a pose de carregar o bebê (versão 0,
`FF FF 02 00` em 10–13) ficava com prioridade −1 (nível 1) em vez de 2
(nível 5). Feito em `animateOverlay`.

### Conversão da versão 0 (BHAVs do base) de Look At e Run Tree by Name

O parâmetro guarda os 16 bytes de operando em `+4..+0x13` e a versão em
`+0x14` (0 = como veio do arquivo do base; 1 = convertido). A conversão roda
uma vez, ao ler (`XPrimParam::Read` → `UpgradeVersion`).

- **Look At** (`LookAtParam::UpgradeVersion`, BV `0xac4990`): a versão 0 tem
  um u16 no operando 4 (`LookAtVersion0Param::SwizzleParams`: u16 em 2, 4,
  6, 9 e 12; a versão 1 só em 2, 6, 9 e 12). Conversão:
  `op[4] = op[5] = (int16)u16_v0 / 2` (divisão com truncamento para zero) e,
  se o byte 0 tem o bit **0x08**, liga o bit **0x02 do byte 15**
  (`FUN_019a9768`). O resto fica igual.
- **Run Tree by Name** (`RunPrivateTreeParam::Read`, BV `0xac4608`): na
  versão 0, o byte 12 vira só o seu bit 0 (`op[12] = op[12] & 1`; os outros
  bits são apagados). A troca de bytes (`SwizzleParams`) tem duas formas,
  conforme um teste global (`FUN_016606e4`): u16 nos operandos 1, 4, 7, 10
  ou 8, 10, 12, 14.

### Remove Object Instance (0x12): os 7 GUIDs e o "cleanup all"

`cTSObject::TryKillObject` (BV `0xe40d68`): **não remove** e devolve
**falso** se o GUID do objeto é um destes (conjunto estático):

| GUID | objeto |
|---|---|
| `0xCC7B08F8` | Portal - Car - Service Start |
| `0x6C7B0905` | Portal - Car - Service Stop |
| `0xCC203477` | Portal - Car - Start |
| `0x865A6812` | Portal - Car - Stop |
| `0x81E6BEF9` | Portal - Pedestrian |
| `0xD20C0AEE` | Trash Can - Outside |
| `0x39CCF441` | MailBox |

(os pontos de chegada/saída do lote e os dois objetos fixos da calçada).
Removendo: `cEdithObjectModule::AddToKillQueue(id, cleanup)` com
`cleanup = !(byte 2 & 0x02)`. Ele marca o objeto (e as partes de um
multi-tile) para morrer, roda a **entrada 3 do OBJD ("cleanup")** dos
objetos contidos que não são Sims, e grava a flag de objeto **0x1000 = !cleanup**
(com o bit 0x02 ligado o objeto morre sem rodar a própria limpeza). Volta
**2** (espera; a thread morre junto) quando o objeto removido sou eu e o
byte 2 não tem 0x01; senão **1**. Objeto inexistente: falso.

### Push Interaction (0x0D): interação aninhada e o erro 0x6F

`cTSObject::TryPushAction` (BV `0xe33530`):

- O byte 3 bit **0x08** liga a flag 0x40 da interação nova (`+0x54`).
- Aninhada = a entrada do TTAB tem o bit **0x8000** dos flags (`cTSTreeTableEntry
  +0x30` bit 15, copiado em `cTSInteraction +0x2E`). Uma interação
  aninhada exige que o alvo tenha **interação atual** e que a entrada do
  TTAB dela tenha o bit **0x4000** (bit 14, `+0x2C`: "permite aninhar");
  senão **erro 0x6F**.
- Aninhada vai para a sub-fila (cheia com mais de 4 → resultado 4); não
  aninhada para a fila (mais de 6 e não deu para trocar → resultado 3).
- Depois vai para `cTSPerson::AddAction` (BV `0xadbbdc`). Uma entrada
  **"Run Immediately"** (flag 0x04 do TTAB, `cTSTreeTableEntry +0x30` bit 2,
  getter BV `0x49d7e4`; por exemplo "*Push Look At" do "Controller - Look
  At", empurrado nos Sims perto de um social a cada poucos segundos) **não
  entra na fila**: roda a check tree (`TestInteraction`) e, se passa e não
  há outra imediata rodando (guarda global `DAT_02cc0210`), roda a árvore de
  ação inteira na hora com `cTSObject::RunOneTickTree` (o Sim, o objeto da
  interação, os parâmetros), com os dados de pessoa 33 (prioridade) e 166
  (autônoma) trocados pelos da interação e devolvidos depois. Check que falha
  ou outra rodando: AddAction devolve 0 e o Push Interaction dá falso com
  resultado 6. Inferido: temps novos na árvore; um erro nela conta como
  recusa (rodada 18; antes as "*Push Look At" se empilhavam na fila do
  Daniel dançando, QA 15). `vm::runImmediately`. O clique do jogador
  passa pelo mesmo `AddAction` (rodada 19, `vm::addAction`): a entrada
  imediata escolhida no pie roda na hora e não aparece na fila.
  O teste da flag vem antes da escolha entre fila e sub-fila: uma aninhada
  (0x8000) imediata também roda na hora. Fora isso, o `AddAction` anda pela
  fila de espera (a interação que roda não está nela: `TryIdleForInput` a
  tira da frente) e, **a partir da primeira de prioridade menor que a nova,
  pula todas** (`ActionSkipped` BV `0xaca0cc`: saem da fila, os vínculos
  desfeitos, e o objeto recebe o evento 4, que não fazemos); a nova vai para
  o fim. Antes inseríamos depois da última de prioridade maior ou igual e
  mantínhamos as menores (como a FreeSO): pânico de fogo ou um clique do
  jogador não limpavam o que estava esperando. "Continue as current" vai para
  a frente (`push_front`). (Rodada 19; os "*Get Sick" 6–10 e o "*Stop Morning
  Sickness" do controlador de doenças são imediatos, 0x0C05/0x0414; os
  "*Get Sick" 1–5 têm 0x08 "Allow Consecutive" e esperam na fila.)

### O menu de pie (`cEdithObjectTestSim`, rodada 19)

`cEdithObjectTestSim::Init(pessoa, objeto, autônomo, todos os menus)` (BV
`0x4c5cd0`): o byte `+0x10` é "autônomo" e escolhe entre
`AppendInteractionsForMenu` (`0x4c9764`) e `AppendInteractionsForAuto`
(`0x4ca54c`); `+0x11` vem de `cTSSimSystem::GetAllMenusOn` (cheat: tudo
passa e nada se esconde). `TestInteraction` (`0x4c6214`) zera os dados
0x32 ("hide"), 6, 0x65 e 0x68 **do Sim**, roda a check tree com os
parâmetros 0 = autônomo, 1 = interação de "join" (`Init(pessoa, pessoa)`
`0x4c815c`, índice de join da entrada que o outro roda), 2 = o Sim clicado
(join), e depois copia o 0x32 para "escondida". `RunMenuCheckTree`
(`0x4c93e4`) põe a interação no menu só se a árvore **não** acrescentou
textos, passou e não está escondida; cada texto do Add/Change the Action
String entra no menu na hora (`sMakeNewMenuItem` `0x4c8f28`, lista global
`DAT_027797ec`), com resultado 1 e não escondido, **seja qual for o fim da
árvore**, e também quando vem de uma árvore rodada por nome dentro dela.
Clicando num Sim, `AppendInteractionsForMenu` também oferece o join da
interação atual de cada um (o "Relaxar" da cama tem join 4: "Juntar-se a"/
"Pedir para Juntar-se").

**Socials da cama** (rodada 19): as entradas 5–17 da cama de casal
(Acariciar, Beijar, Oba-Oba, Tentar Fazer um Bebê, A e B) devolvem falso
para a autonomia e verdadeiro **escondido** (global 0x109 "Hide Menu")
para o jogador: nunca aparecem sozinhas. O jogador as escolhe **clicando no
parceiro deitado**: a entrada 183 da pessoa, "*Object Menu Subqueue"
(flags 0x8E04, imediata; só para o jogador, parâmetro 0 = 0), pega o objeto
que o Sim clicado usa (dado de pessoa 59) e roda nele por nome "CT - Object
Menu Subqueue" (STR# 0x12F #59 da pessoa), que acrescenta Acariciar /
Beijar / Dar um Amasso / Oba-Oba / Primeiro Beijo / Tentar Fazer um Bebê
conforme o relacionamento, com a opção no parâmetro 0; escolhida, a ação
roda "CT - Object Menu Subqueue Command", que empurra o par A/B nos dois
Sims. A autonomia usa o "*Autonomous subqueue" da cama (só com parâmetro
0 = 1), que chama a mesma árvore. Nosso pie não rodava check trees de nome
com '*' (só a flag 0x80 é de depuração no jogo: o '*' sem a flag não
aparece sozinho, mas os textos que a árvore acrescenta sim) e perdia os
textos de árvores rodadas por nome (thread separada).

### `SetReachObjectID` (`cTSPerson +0x8EA`) e `+0x8EC`

Não são dados de pessoa: são campos do `cTSPerson` (**reach object** e
**reach surface object**), escritos pelo Reach/Put (§4) e lidos só pela
animação: `cTSPerson::GetReachObjectSelectorGUID`/`GetReachSurfaceSelectorGUID`
e `cEventModifierBase::GetReachSelectorGUID` (BV `0xbf0d8c`) pegam a
definição desse objeto para os eventos da animação de pegar/pôr (prender
o objeto na mão no evento de "grasp", soltar na superfície). O SimAntics
não lê esses campos; não vão para o PERS.

### Animate Sim: como o jogo acha o nome da animação

`try_animate_sim` (BV `0x69d634`): índice = u16 do operando 0 (ou temp, com
o bit 0x04 do byte 2); a **tabela = byte 6**:

- `0x80`: `FindGlobalName` (BV `0x6901e0`): a tabela de animações globais
  da idade (abaixo), entrada = índice; se a animação não existe, tenta de
  novo com a tabela "de reserva" (`retry`).
- outro valor: a tabela é o **STR# com esse número**, procurado
  (`FindSpecificName`, BV `0x690700`) **no objeto da árvore que roda**
  (correção da rodada 16: o seletor vem da árvore do frame, não do stack
  object; §4g "Rodada 16 (autonomia e erros)") (ou, com o bit 0x10 do byte 8, no objeto que o
  Sim está usando) e, se a entrada estiver vazia, **no semi-global desse
  objeto**; o nome final é o
  **prefixo + a entrada**, e só vale se a animação existe. Se falhar, tenta
  com a tabela de reserva, e por fim `FUN_0068bcb0` troca a letra da idade
  do prefixo pelas alternativas (`d`, `f`, `w`, `l`, `k`).
- Só depois disso (BV `0x69da50..`): o nome é classificado pelo tipo
  (vtable `0x380`; inferido: pelo prefixo `a2o`/`o2a`/`a2a`/...); tipo não
  reconhecido → erro **0x73** ("Invalid animation type specified in
  animation string"); nome vazio → erro **0x43**.
- **Animação que não existe não é erro**: a busca acima falha em silêncio e
  fica com o último nome montado; o pedido de animação (vtable `0x2E0`)
  volta nulo, o Sim loga "missing animation tree" e a primitiva **devolve
  verdadeiro na hora**, sem duração (o mesmo no base 1.0, que tem a mesma
  string). Só um nome sem prefixo de tipo válido vira 0x73.

A tabela e o prefixo vêm de `DetermineAppropriateAnimTable` (BV `0x68f7c8`),
pela espécie (dado de pessoa 186, só BV; 0 = humano no base) e a idade
(**dado 58 "Person Age"**):

| idade (dado 58) | `0x81` (global) | prefixo `0x83` |
|---|---|---|
| ≥ 51 | `0x8B` idoso (reserva: `0x81`) | `e2` |
| 19–50 | `0x81` adulto | `a2` |
| 16–18 | `0x8A` adolescente (reserva: `0x81`) | `t2` |
| 3–15 | `0x82` criança | `c2` |
| 2 | `0x89` bebê que anda | `p2` |
| < 2 | `0x91` bebê | `b2` |

`0x83` é a tabela de **animações entre dois Sims**: o prefixo é
`<minha idade>2<idade do outro>` (`GetAnimPrefixStart`/`End`: o outro é o
Sim da interação; se não for um Sim → erro 0x5A), por exemplo `a2a` +
a entrada. Na reserva, a idade do Sim vira `a` (adulto).

**O "nome vazio" do "Interaction - Standard Reaction - SG"/"Wave" na nossa
VM** vem de ler a entrada no lugar errado: a árvore é de um semi-global e
a tabela (byte 6 ≠ 0x80) tem de ser procurada no objeto do stack object e
depois no semi-global dele, com o prefixo de idade; para a tabela global
(0x80/0x81) a tabela certa depende da idade (um adolescente/idoso usa
0x8A/0x8B e cai para 0x81 se não achar).

## 4c. Rodada 7 (VM): relógio, meia-noite, objetos fora do mundo, Make New Character

### Relógio e meia-noite (`cTSSimulator::SimulateOneTick`, BV `0x49a814`)

A cada tick que avança o relógio (segundo `+0x30` += 2):

```
segundo > 59 → segundo −= 60, minuto (+0x2e) += 1, ObjectModule.MinuteChanged
               mensagem 0x88 (minuto)
minuto > 59  → minuto −= 60, hora (+0x24) += 1, ObjectModule.HourChanged, flag "hora nova" (+0xdc)
               mensagem 0xD08962D0 (hora)
hora ≥ 24    → hora −= 24, dia (+0x5c) += 1, dia do mês (+0x26) += 1,
               dia da semana (+0x68) = dia mod 7, ObjectModule.DayChanged,
               um gerente diário (vtable 0x24 do sistema), histórico de 5 dias de
               tokens de inventário deslocado (o de hoje zera), negócios (só BV)
               dia do mês > 30 → 1, mês (+0x32) += 1 (> 12 → 1, ano +0x34 += 1)
               mensagem 0x89 (dia)
hora nova    → ComputeTimeOfDay (vtable 0xc0) e SetTimeOfDay (0xc4): a luz do dia
depois       → ObjectModule.PostSim (vtable 0x64) com "tick sem avanço"
```

`cEdithObjectModule` (vtable `0x68/0x6c/0x70`):
- **MinuteChanged** (BV `0x5250b8`): para cada Sim da lista de minuto,
  `PersonalTimeMinutePassed` (só decrementa o dado de pessoa 189 até 0, BV).
- **HourChanged** (BV `0x525144`): `PersonalTimeHourPassed` de cada Sim
  (dado 172, BV).
- **DayChanged** (BV `0x525a60`): `DayPassed` de **todos os objetos**:
  `cTSObject::DayPassed` (BV `0xb07a4c`) = `UpdateAge` + depreciação
  (`docs/buy-build-mac.md` §2) e contador `+0x140` −1 por dia;
  `cTSPerson::DayPassed` (BV `0xad133c`) = dado 200 −1 (até −10, BV) +
  o do objeto + `UpdateMemories`.

Nada no C++ da meia-noite toca motivos, empregos ou carona: **é SimAntics**.
As árvores leem o relógio (globais de hora e dia) e os controladores rodam
no Main. **Carona e ônibus** (dados): o semi-global de carreira do grupo
`0x7FBE051B` tem "CT - Get Carpool Time" (`0x200D`), "CT - Is Neighbor at Work"
(`0x200E`), "Function - Create Car" (`0x200A`), "Function - Is Job Car
Here?" (`0x201B`), "Sub - Return to World" (`0x201D`) e "Sub - Process"
(`0x201F`). O grupo `0x7F8F4EB6` tem "Sub - Carpool Warnings" (`0x2027`) e outro
"Function - Create Car" (`0x200B`). O carro é um objeto criado pela árvore
(Create Object) que chega pela rua (portais de carro, `docs/primitives-mac.md`
§5 "Portal - Car - …"). O Sim sai do lote pelas mesmas árvores. O motor só
precisa do relógio, de Create Object, das rotas e dos portais de carro.

### Objetos fora do mundo (`cEdithObjectModule::MakeNewOutOfWorldObject`, BV `0x533fb8`)

É o mesmo caminho para Create Object com "fora do mundo", para a compra no
modo compra (vtable `0x40` do módulo, ver `docs/buy-build-mac.md`) e para
objetos de inventário:

```
seletor → ConstructObject (vtable 0x3c) → AddObject (0x38) dá o ID
od 19 "lead tile object ID" := o próprio ID
OnInit(param)            ← a árvore init (entrada 0), com o parâmetro pedido
OnPostMultiTileJoin, JustBorn
se não "sem reset":  Reset(1,1,1,0) e **od 41 "current value" := OBJD campo 18 (preço)**
flag 0x80 ligada (vtable 0x68), sim flag do módulo (0x128)
multi-tile: cria cada parte do grupo (o mestre no índice 0), liga as partes ao líder,
            init em cada uma; uma parte que falha mata o grupo (KillOutOfWorldObject)
```

O objeto **não tem posição** (`IsInWorld` falso). Os erros de VM 0x34, 0x7B
e 0x96 (apêndice A) são os testes de "fora do mundo". Ele só entra no mundo
com `SetPosition`/colocação. `KillOutOfWorldObject` (BV `0x532d08`) tira do
mundo o que estiver dentro (vtable `0x2ac`), roda o cleanup e chama
`RemoveObject`, recursivo nos objetos contidos e nas partes. Room: um objeto
fora do mundo não tem tile. O "room −1" dos saves é a posição (−1, −1) do XOBJ
(`docs/save-mac.md`). O que o od 29 "room" vale nesse caso não foi lido
(aberto).

### Make New Character (0x13, `TryMakeNewCharacter`, BV `0x65cb14`)

**Assíncrona**: pede ao CAS (`nTSCASCore::CASSystem`) a geração e guarda os
IDs dos pedidos numa lista do elemento da pilha:

- **bebê** (byte 9 bit 0x01): pais pelos dois operandos (ID de objeto, ou
  neighbor ID com 0x02/0x04). O número de bebês vem do temp 0 do controle (1–10,
  senão o erro **0x76**). Os pais precisam ter sexo diferente ou o flag 6.
  Faz um pedido `CASSystem 0x18(pai, mãe, roupa)` por bebê, posta a mensagem
  `0xAD90F890` (nascimento; o byte 13 bit 0x01 a cala) e devolve **2**.
- **aleatório** (sem 0x01): idade = local do byte 5 (≤ 0 → sorteio de 1 <<
  rand(6)), sexo = local do byte 6 (≤ 0 → 50%), cor da pele = local do
  byte 18 (< 4). Com 0x40 vem o GUID de um "bin" de personagens (com 1 =
  "EP2 Downtownie Bin", só BV), e um bin com vários sorteia um. O pedido é
  `CASSystem 0x14(idade, sexo, pele, local do byte 4, GUID da roupa, 7, bin)`.
  Devolve 1 se o pedido foi aceito (−1 → falso).
- **De novo no tick seguinte** (o elemento marca "começado", vtable `0x48`):
  percorre os pedidos (`CASSystem 0x30(id, &pronto)`). Algum pendente → **2
  (espera)**, todos prontos → 1 (verdadeiro).
- Erros: 0x15 (sem stack object), 0x6C (sem pilha), 0x7A/0x94 (GUID de
  roupa inválido / de pet), 0x08 (local fora), 0x9E, 0x37 (pai/mãe
  inválidos).

O Sim pronto entra pelo `cTSNeighborManager::AddNewCharacter` (BV
`0x5386dc`) e vira um objeto fora do mundo como acima. **Para nós**: sem CAS,
basta criar o personagem na hora a partir de um modelo e devolver 1. O
resultado "2" só importa para quem espera o nascimento.

## 4d. Rodada 8 (VM): carona, trabalho e volta

### Slots nos ossos do modelo (`cTSRoute::GetPossibleSlotsFromModelRoutingSlot`, BV `0x475b98`)

Um slot de rota com a flag `0x20000` no u32 depois das proximidades
(`Slots::Slot::unknown10`) fica nos **ossos** do modelo: o primeiro u32 extra
do SLOT do TS2 é o índice no STR# 0x90 ("Model - Bones"), cuja entrada pode
listar vários ossos separados por vírgula (o "Multislot" do sedã). O binário
faz um objetivo por osso (contagem pela vtable `0x140` do objeto, cada ponto
pela `0x148`) e dá a cada um a direção **do objeto + a do osso**:
`objDir + (8 − k) mod 8` (k da vtable `0x14c`, convertido por
`FUN_008b6334`), seja qual for o campo de direção do slot. O `TrySnap` faz o
mesmo para um slot com `0x20000` (sem ela usa o campo do slot, e só se o slot
fixa a direção; senão fico com a minha). Aqui (`vm/slot_bones.cpp`): k = o
eixo +Y do osso na pose de repouso girado em Z, em oitavos de volta no
sentido anti-horário (inferido da conversão). Assim o carro da carona, que
anda com Snap no seu próprio slot 1 ("slot_travel_drive", 2 tiles à frente),
mantém o rumo; antes virava para o centro (direção "para o objeto") e ia e
voltava no lugar. Um objeto multi-tile que faz Snap leva as outras partes
junto (inferido).

### Snap: o código no object data 52

`TrySnap` (BV `0xe43624`): se falhar, od 52 := 0; o primeiro erro da lista
de colocação é 0x0C ("Can't intersect other objects") e havia uma pessoa
com a flag 0x02 → pede para ela sair, od 52 = 1; senão od 52 = **código do
erro + 2**: 0x0E para objeto no caminho, **4 para "Location out of bounds"**
(código 2). O "Drive - Drive Leave" do carro testa od 52 = 4 depois do Snap
de teste no slot 0 ("slot_travel_start", 8 tiles à frente) para saber que
chegou ao fim da rua; com 0x0E o carro ficava parado na borda.

### Find Location For fora do mundo e ao longo de um vetor

Modo 1 (fora do mundo): o binário chama a vtable `0x2ac` do objeto ("tirar
do mundo"), que também o tira do contêiner. O "Sub - Send Passengers Out of
World" do carro repete enquanto houver alguém nos slots: sem isso era um laço
infinito. A sala (od 29) de um objeto fora do mundo é **−1** (todos os
objetos fora do mundo dos lotes salvos têm −1; vale também para Create New
Object Instance com "fora do mundo"); o "Get Out TEST" do carro, que a "Sub -
Return to World" empurra, testa sala < 0. Modos 3/4 (ao longo do vetor /
lateral): `TryFindGoodLocation` entrega a busca à vtable `0x2d8` do objeto
com os parâmetros (não lida); aqui a máscara de direções proibidas vale
**relativa à direção da referência** (inferido): o carro sai do portal "Car -
Start" (só N, `0xFE`) virado para a rua como o portal, e a poça nova (`0xAA`)
fica num lado que o laço "girar até ficar reto" da árvore alcança. Com a
leitura absoluta o carro saía virado para o oeste, atravessado na rua.

### Set to Next 18 (GUID nos temps) e a escola particular

BV (`TrySetToNext` `0xe47288`): os tipos 4 e 0x12 percorrem a lista de
**instâncias da definição** do GUID; com valor atual ≠ 0 procuram o atual
nessa lista e seguem do próximo; se o atual não está na lista, **não acham**.
Jogo base 1.0 (`cTSObject_TrySetToNext` `0xb577b0`): o tipo 0x12 é outro
caso, que anda na lista geral de objetos do lote a partir do objeto atual
(comparando o GUID da definição, campo `0xca`); o tipo 4 é como no BV. O
"Sub - Is Stack Obj my Student?" dos JobData de escola faz Set to Next 18 a
partir do **aluno** (stack object) procurando o JobData da escola dele (dados
de pessoa 107/108). No BV não acha nunca; no 1.0 só acharia se o JobData
viesse depois do aluno na lista do lote, e os lotes salvos estão em ordem de
ID (o JobData particular do N001_Lot11 é o 70, o Alexander o 142). Depois
disso a árvore só aceita a escola pública (Test Object Type `0xB58867D0`)
para quem tem 107/108 = 0.

**Rodada 18 (r18-life): corrigido.** A conclusão acima ("o jogo também não
manda o Alexander") estava errada: no jogo os alunos de escola particular
vão. Lido de novo no 1.0 (`0xb577b0`): o tipo 0x12 acha o objeto atual na
lista do lote (lista ligada, o objeto no `+8` de cada nó: ordem de inserção,
não de ID), segue do próximo e não dá a volta. Várias árvores da Maxis
começam essa busca **a partir do Sim** ("Sub - Is Stack Obj my Student?",
"Sub - Set to Sim's School", "Sub - Send to School", "Work - Do I Have a
Job?", "Neighbor - Is home?"), procurando o JobData, que tem ID menor: só
funciona se os Sims vêm antes dos outros objetos nessa lista. **Inferido**:
a VM anda o tipo 18 pelas instâncias da definição (como o BV, na ordem de
criação), mas um Sim que não é dessa definição conta como o começo (no BV,
"atual fora da lista" é falso; no 1.0, com os Sims primeiro na lista do
lote, a busca a partir do aluno acha o JobData). Com isso o Alexander (`N001_Lot11`) põe o uniforme, anda até o
ônibus às 08:07, "Na Escola" das 08:26 às 15:02, nota 12 → 13, nos dois
dias de `s2pkg autonomy --ticks 86400`; a Jill (`N002_Lot10`), que também
não ia, agora vai. Captura `docs/screenshots/r18-life-escola-particular.jpg`.


### O dia de trabalho (só SimAntics)

Nada disso precisa de C++ além do que está acima: às 07:00 o "Sub - Process"
do JobData avisa, às 08:00 cria o carro (motorista NPC no slot), o carro
anda pela rua de Snap em Snap até parar perto do portal "Car - Stop", buzina,
o Sim recebe "Ir Trabalhar", anda até o carro, entra ("o-policeCar-getIn"),
o "Interaction - At Work" atualiza o desempenho (dado de pessoa 63) e põe o
Sim fora do mundo quando o carro sai do lote. O Sim fica esperando no "At
Work" (Wait For Notify), fora do mundo, com a hora passando no lote. Na hora
do fim do turno o JobData chama "Sub - Return to World": cria o carro, empurra
"Get Out" (sub-fila) e o carro traz o Sim de volta à calçada; o "Sub - Get
Paid" põe o salário nos fundos da família (globais de orçamento → FAMI). Não
há "trabalho fora do lote" em C++: o que acontece enquanto o Sim está fora
é a espera da árvore.

## 4e. Rodada 8: o Sim no trabalho e as contas (árvores dos dados + binário)

Não há código C++ para isso no binário: o trabalho e as contas são árvores.
Nomes pela lista de BHAVs (`objects.package`); donos de dados como na FreeSO
(`0x0E` meus motivos, `0x12` meus person data, `0x19` local, `0x1A`
constante de ajuste).

**Sair do mundo**: `Sub - Send out of World` (BHAV `0x2011` do grupo
`0x7F8F4EB6`, o semi-global das carreiras) é um **Find Location For no modo
1** (STR# `0xEF` #1 "out of world"): o Sim sai do mundo como os objetos de
§4c (sem tile; testes de "fora do mundo" do apêndice A). O carro tem
`Sub - Send Passengers Out of World` (grupo `0x7F4EA230`).

**Enquanto está fora** roda `Interaction - At Work` (`0x200E`, 54 instruções),
um laço por hora de jogo (`Function - End Hour?` `0x2002`, teste da hora de
saída; não lido em detalhe): `Sub - Work Idle` (`0x2039`), motivos do
trabalho (`Sub - Start Motive Deltas` `0x2015`, `Check Motives` `0x2023`),
carta de sorte (`Sub - Chance Card` `0x2003`), doença (`0x2026`), aspiração
de poder (`0x2038`), `TNS - Bad Mood` (`0x2012`) e **`Sub - Update Job
Performance`** (`0x205A`): `local0 = meus motivos[3] / 2` (dono `0x0E`, dado
3; inferido: humor), **person data 63 "job performance" += local0**, preso em
−100..100. No fim do turno: **`Sub - Get Paid`** (`0x2004`: salário do nível
pela `Budget - Make Transaction` global `0x0294`, PTO `0x201F`, promoção
`Sub - Level Change` `0x203F` quando `Function - Qualify for Level?` `0x2005`
passa, demissão `CT - Lose Job` `0x2025` com desempenho muito baixo) e
**`Sub - Return to World`** (`0x2016`: cria o carro, `Function - Create Car`
`0x200B`, empurra a interação de chegada no Sim, tenta trazer amigo
`0x202C`; falhando, `Sub - Return to World - Failsafe` `0x2063`). Depois:
recálculo dos desejos (`Wants - Refresh - Recalc`, global `0x03A5`).

**Contas** (objeto `MailBox`, grupo `0x7FE4B434`, e `Bills`, grupo
`0x7FBD8997`; rodada 9, árvores lidas por inteiro):

- **Quando.** O `Functional - Main` da caixa dorme 200 ticks (global
  `0x118`) e roda `process` (`0x1002`) enquanto há casa no lote (global 10
  ≠ 0). `process`: se **hora (global 0) = BCON 0x1000[2] = 9** e **minuto
  (global 5) > 50**: se o **dia da semana (global 34 = Days Running % 7)** é
  BCON 0x1001[1] = **1** ou [3] = **3**, roda `Bill due?` (`0x102E`, que
  procura a caixa `0x39CCF441`, chama `Appraise lot value` e sempre devolve
  verdadeiro) e então `Create Mailman` (`0x1004`: NPC carteiro pelos globais
  `0x2EC`/`0x2E4`), que traz as contas. No dia [6] = **6** nada. Nos outros
  dias, só se existe um objeto agendador `0xD840F488` com atributo 7 = 3
  (o que `CT - Reschedule Bills` `0x1017` cria, com atributo 1 = dias de
  atraso + 1). Ou seja: **contas duas vezes por semana, nos dias 1 e 3,
  entre 9:51 e 10:00**, mais as reagendadas.
- **Quanto.** `Create Bill` (`0x1006`) cria o objeto `Bills` e grava no
  atributo 0 o valor de `Appraise lot value` (`0x1033`):
  **4 × global 46 (valor do lote / 1000) + 6 × global 48 (valor dos objetos
  / 1000)** (BCON 0x1000[5] = 4 e [6] = 6 da caixa); valor 0 → a conta é
  apagada. Os globais vêm do `cTSSimulator`: `SetLotValue` (`0x498b80`) grava
  `+0xA4` e os globais 46/47 (÷1000, resto); `SetLotObjectsValue`/
  `SetFamilyObjectsValue` gravam `+0xA8`/`+0xAC` e `UpdateTotalObjectsValue`
  põe (`+0xA8` + `+0xAC`) ÷ 1000 e o resto nos globais 48/49. São os
  campos do SIMI (`docs/save-mac.md` §5) e os globais também vão salvos;
  quem chama esses `Set*` é o `cTSLot::ComputeAndStoreLotData` (rodada 10,
  `docs/neighborhood-mac.md` §11.1) com o `cEdithObjectModule::ComputeStats`
  (BV `0x52c638`), na virada do dia e quando o lote muda. O opensims2 refaz os
  globais 46–49 e o SIMI +0xA4/+0xA8/+0xAC (`vm::computeLotData`) ao
  carregar, na meia-noite (depois da depreciação), ao comprar, vender e
  apagar. Pelo decompilado, o `ComputeStats` soma o dado 41 de objeto líder no
  mundo (fora tipo 7 e o bit 0x400 do campo 40): `IsDeletedByEvict` (modo
  compra, campo 0x45 = 0, sem o bit 0x10 do dado 43) → família; senão objeto
  do modo construção, ou tipo 0xB/0xC (+ corrimão, não feito) → lote.
  `N001_Lot11`, 3 dias (`s2pkg autonomy --ticks 130000`): conta de 466 no dia
  1 às 10:18 (globais 46 = 4, 48 = 75) e de 400 no dia 3 (48 = 64, depois de
  duas depreciações).
- **A constante `0x81`** (dono `0x1A`: tabela 1 = BCON privado 0x1001,
  entrada 1) do `CT - Calculate Bill` (`0x1017` das contas, Σ valor atual
  / 10 dos objetos sem o bit 14 do dado 40) vale **10**, mas essa árvore é
  **código morto**: o `Function - Init` das contas só a chama na instrução
  7, que nenhuma outra alcança. O Init grava no atributo 1 o BCON 0x1001[0]
  = 4 (prazo em dias, inferido) e as cores (`CT - Set Bill Colour`)
  seguem o atraso.

## 4f. Rodada 11: comida e fogo (árvores dos dados + binário)

Donos como em §4e; flags das expressões contam de 1 (flag 6 = bit 5 =
0x20). "stk.slot[n]" é o dono 0x10 (objeto no slot de contenção n do stack
object); "stk.semi" o dono 5 (semi-atributos do stack object, `+0x84` do
objeto no binário).

### A cadeia da refeição e os balões de falha

`Food - Eat - _Core` (`0x2009` do semi-global da comida, grupo
`0x7F3C9675`): `Food - Eat - Prepare` (`0x2008`) falso → **`Food Chain -
Failure - Prepare`** (`0x205B`); verdadeiro → `Wait For Notify` → `Food -
Eat - Cook` (`0x200C`) falso → **`Food Chain - Failure - Cook`** (`0x205C`).
Então **"Failure - Cook" é a falha do passo de cozinhar** (fogão, micro-
ondas, forno elétrico), não do de preparar; os dois só mostram balões
(`Food Chain - Failure Balloon` `0x2059`, pelo tipo da comida nos semi-
atributos 5 e os BCONs `0x2181…0x218A`).

Os dois passos fazem o mesmo antes do Find Best Object (stack object := 0):
**temp 0 := tipo da comida** (preparo: semi-atributo 4 da comida; cozinha:
semi-atributo 0x22 & temp 2 dos tipos do `Function Test - Cook - Fridge`)
e **temp 1 := ID da comida** (a bandeja), depois FBO função 0 (preparo) ou
1 (cozinha) sem flags e `Run Functional Tree` da mesma função no achado. Se
a comida já tem um objeto preferido (semi 15 no preparo; semi 0x1E com
`Validate Preferred Cook Obj` na cozinha), vai direto nele. Como o check
tree recebe **os meus temps como parâmetros** (§4 FBO), param 0 = tipo,
param 1 = ID da comida.

**`Function - Prep - TEST`** (`0x200D` do semi-global dos balcões,
`0x7F12A804`; o processador de alimentos tem o seu) devolve verdadeiro só
com: atributo 0 do balcão ≠ BCON `0x2202` e não é peça de canto (`Test -
Is Corner?`: od 0 "graphic" ≠ `0x2382`); param 1 > 0 (param 1 < 0 ou = 0:
modo consulta — grava os tipos que sabe preparar no temp 2 com o operador
0x12 e devolve **falso**, por isso o `Function Test - Prep` vê falso de um
balcão livre); `In Use?` falso (semi-atributo 3 ≠ 1); param 0 igual a um
dos 10 tipos (BCONs `0x2081…0x208C`); sem eletrodoméstico embutido (`Slot
Test - In Counter?`: atributo 0 = `0x2200` e algo no slot 4); e os slots
livres pelo `Slot Test - Standard?` (slot 0 e 1, e 2–4 se o atributo 0 =
`0x2201`): **qualquer objeto nos slots de cima do balcão o descarta**, a
menos que seja a própria comida no slot 1 ou uma bandeja de ingredientes
(`Get in Temp 0 - Ingredients Tray`, que aceita a bandeja da comida). Daí o
laço do soak: cada falha deixa a bandeja num balcão, e esse balcão sai das
próximas buscas (além de bloquear o chão, quando cai lá).

**`Function - Cook - TEST`** (`0x2001` do semi-global dos fogões
`0x7F033984`) devolve falso se `Is Power On?` falha (**global 37 "Utility
Available Flags" flag 2** = luz cortada, posta por `Utility - Turn Off
Power` e `Power Failure` das contas), em modo consulta (param 1 ≤ 0),
`In Use?` (semi-atributo 3 do fogão = 1) ou com fogo no cômodo (`Test -
Fire in Room?`: algum Fire no mesmo od 29); senão confere o tipo (param 0)
contra os bits de cozimento (BCONs `0x2082…0x2088`) e o tipo do aparelho
(semi-atributo 1 = `0x2100…0x2102`), e exige, conforme o tipo, o slot 0
do aparelho livre e, se há algo no slot 1 ou 2 (panela, assadeira), que o
slot 0 **desse** objeto seja a própria comida (param 1); verdadeiro devolve
os tipos no temp 2. **Uma panela ou bandeja esquecida no fogão com outra coisa dentro
descarta o fogão.**

Para depurar no VM: além dessas árvores, o binário tira do FBO (§4) objetos
ocupados (od 8 bit 5), pegando fogo (od 8 bit 9), com od 87 ou od 82 bit
2, e fora dos cômodos acessíveis do Sim; o VM faz todos desde as rodadas 11
e 12 (§4g).

### Fogo (`Fire`, grupo `0x7FD0A790`; só SimAntics)

O motor só guarda flags: **pegando fogo** = od 8 bit 9 (0x200, flag 10;
`cTSObject::IsBurning`), **à prova de fogo** = od 8 bit 11 (0x800,
`IsFireproof`), **pode queimar** = od 40 bit 5 (`CanBurn`, flag 6 do od 40
nas árvores). Tudo o mais é árvore:

- **Início** (`Main - Setup` `0x1004`): o Fire nasce no tile do objeto (od
  11), acorda o cômodo, soma a inflamabilidade (od 84 dos objetos em volta)
  no atributo 3 → atributo 1 = 0–3 (< 40, < 70, …); chama =
  BCON `0x1003`[1] (80) + (200 − 80) / 4 × atributo 1 + aleatório 0–7 − 7
  (73–170); marca o objeto (od 8 flag 10) e desliga as interações dele;
  `CT - Fire - Start` no objeto (`stk.od[19]`) e, se ainda não havia fogo no
  cômodo, no `Controller - NPC` (só grava o dia: `stk.attr[12] := global 1`);
  câmera; empurra **Pânico** em todos os Sims que não estão ocupados;
  `Notify - Fire Alarm` acorda (Notify out of Idle) os **alarmes de fumaça
  do mesmo cômodo** (od 29).
- **Laço** (`Function - Main` `0x1000`): a cada `Idle(3)` o atributo 4 +=
  3; `Burn - _Simulate` a cada 20 acumulados: idade (atributo 0) += 1,
  contador de propagação (atributo 5) −= 1 e **chama (atributo 2) −= 1**
  (`Burn - Decrease Flame`; ≤ 0 apaga; com a flag 0x502 apaga já em ≤ 35):
  ~1 ponto a cada 20 ticks, **uma chama de 170 dura ~3.400 ticks ≈ 1,9 h
  de jogo**. Com chama > 10, atributo 5 < 0 e idade > 45: 1 chance em 8
  (`Random` com o BCON 0x1003[6] = 8) de **propagar** (`Burn - Spread`:
  novo Fire num vizinho inflamável), atributo 5 := 10. Idade > 75 sem a
  flag 0x505 → `Burn - Destroy` (o objeto queima: troca por cinzas, mata o
  conteúdo dos slots que pode). A cada 30 voltas empurra Pânico de novo e
  seca poças.
- **Apagar** (`Interaction - Extinguish - TEST` `0x1012`): nunca criança;
  bombeiro (dado de pessoa 155 = o tipo NPC do BCON `0x5910`) sempre;
  outros: falso para visitantes (dado de pessoa 32 ≠ `0x4200`) e **falso
  quando param 0 = 1 (autônomo; confirmado no binário na rodada n32,
  abaixo)**; com a flag 0x502 do fogo
  também falso. Ou seja, **os Sims da casa só apagam por ordem do
  jogador**; sozinhos só entram em pânico.
- **Bombeiros**: o `Alarm - Smoke` (`0x7F97EA32`), acordado pelo fogo do
  seu cômodo, roda `Sub - Call Fire Fighter` (agenda o NPC 0x5910 se não há
  um no lote); fora isso, o telefone. Sem alarme no cômodo do fogo, nenhum
  bombeiro vem.

Então o "Fogo!" de 7–8 h do soak (`N001_Lot49`) é o esperado de um lote
**sem alarme no cômodo** e sem jogador: cada Fire dura ~2 h e se propaga
enquanto houver vizinhos inflamáveis. A conferir no VM: alarme no cômodo,
`Notify out of Idle` acordando o alarme, e o agendamento do NPC.

### Rodada 14 (front "engine-re"): pânico, fogo do fogão e quem pega fogo

Lido nas árvores (`tools/datahunt/bhavs.py`; nas listagens de assinatura ≥
0x8007 o alvo `0xFFFD` é verdadeiro e `0xFFFE` falso). Constantes `dono
0x1A`: dado >> 7 escolhe a tabela (< 64 privada `0x1000 + t`, < 128
semi-global `0x2000 + t − 64`, senão global `0x100 + t − 128`), os 7 bits de
baixo a entrada.

**`Interaction - Panic` (`0x1013` do Fire) tira o Sim de perto do fogo.**
Roda no Sim com o Fire como stack object:

1. Fora do lote (od 29 < 0) devolve verdadeiro. `Memory - Add`, `Want
   Satisfy - Fire`. **Pegando fogo** (od 8 flag 10) → `Panic - On Fire!`.
2. **Criança de colo** (`Age - Am I a Toddler?`): só vira para o fogo e
   toca a reação; não foge.
3. Os outros: guarda o od 17 (estilo de andar) num local e põe **od 17 := 1
   (corre)**; com o dado de pessoa 32 igual à constante global `0x104`[0]
   (0x4200; inferido: morador) faz `Go To Routing Slot` do **slot global 9**
   (`SLOT` global `0x64`: alvo, 2–5 tiles do fogo, ótimo 2, flags `0x55` =
   as quatro diagonais/retas pares, resolução 1); senão o **slot 1 do
   próprio Fire** (3–6 tiles, ótimo 3, flags `0xff`, resolução 0,5). Os dois
   com a flag 0x08 (permite outra altura).
4. A rota falhou: `Get Distance To` o fogo; a < 10 tiles ainda vira para ele
   e entra em pânico ali, a ≥ 10 não vira. Depois devolve o od 17, zera os
   dados de pessoa 27/28, empurra `Push - Save Baby/Toddler` (bebês e
   crianças de colo do cômodo) e entra no laço: uma de quatro animações de
   pânico ao acaso (`Random 8`), `Divide Tick Counts`, o dado de pessoa 28
   somando o tempo, até passar de **`Tuning - Panic`[0] = 15**; `Wait For
   Notify` entre as voltas; se pegou fogo no meio → `Panic - On Fire!`.
5. O Fire empurra o pânico de novo (`Push - Panic - All Sims Not
   Interacting`, `0x102c`) no começo e **a cada 30 voltas do laço** (`Idle
   3`), para todos os Sims do lote que não estão numa interação (com os
   filtros de idade/NPC do `0x102c`); com `Hurry - Start`.

Ou seja: **sim, o pânico afasta o Sim** (correndo, 2–6 tiles) antes de
entrar em pânico parado. TODO do opensims2: conferir num soak que o `Go To
Routing Slot` global 9 / slot 1 do Fire leva os Sims para longe (o `SLOT`
global com flags de direção e resolução) e que o od 17 volta.

**Quem pega fogo** (`Function - Main` `0x1000` do Fire):

- **Sims no tile do Fire** (`Main - Set on Fire - Sims on Tile`, `0x1051`):
  a cada volta do laço (a cada `Idle 3`), toda pessoa no tile do fogo ganha
  od 8 flag 10 (pegando fogo), sem sorteio.
- **Um Sim vizinho** (`Main - Set on Fire - Sim on Adjacent Tile`,
  `0x1040`): só uma vez, no começo do Fire: `random(100) == 1` (o `Tuning -
  Burn`[7] = 100), isto é **1%**, um Sim ao acaso da lista de vizinhos.
- O Fire liga a sua flag 2 e desliga a 5 do od 8: `CanIntersectPeople` falso
  (o teste por passo não deixa entrar no tile do fogo; §6.1 do
  `routing-mac.md`).
- **`Panic - On Fire!`** (`0x102a`): câmera, efeito de fogo no corpo pela
  idade, motivos (`Tuning - On Fire!`: −200/−90 por motivo), contador local
  = aleatório entre **5 e 10** (`Tuning - On Fire!`[16..17]) voltas de
  animação; acabou o contador ainda pegando fogo → zera os motivos 5–9, 14,
  15 e **`Kill Sim`** (morte por fogo). Alguém apagar o Sim (a flag 10
  limpa) para o laço. Bebês/crianças de colo têm ramo próprio.

**Fogo do fogão: a chance por cozinhada** (semi-global da comida
`0x7F3C9675`):

- `Get in Temp 0 - Skill (Hi/Med/Lo)` (`0x2057`): a habilidade de cozinhar
  do Sim em 0–100 (`Sub - Get Skill Based Value`), **≥ 80 alta, ≥ 40 média,
  senão baixa** (`SG - Tuning` `0x2008`[6] = 80, [7] = 40).
- `Test - Food Failure?` (`0x2045`): falha com **5% (alta), 10% (média),
  30% (baixa)** (`SG - Tuning`[10..12] = 5, 10, 30): `random(100) ≥ 100 −
  chance`.
- `Main - Cook - Loop` (`0x2032`): falhou → o ponto de queima é
  `tempo · (100 − (1 + random(50) + 50))/100` do cozimento (`SG -
  Tuning`[9] = 50); ao chegar nele, `Sub - Set Burnt` e **`random(100) < 20`
  (`SG - Tuning`[15] = 20) → `Sub - Set Fire`** (cria um Fire no aparelho).
  Então por cozinhada: **1% com habilidade alta, 2% média, 6% baixa** (a
  habilidade 0 de quem nunca cozinhou é a baixa).
- Comida pronta esquecida (`Function - Main` `0x2000`): sem ninguém
  interagindo com ela, queima em **10 min** (`Tuning - Cook Failure Time`) e
  **pega fogo com certeza em 30 min** (`Tuning - Cook Fire Time`), contados
  de minuto em minuto (`Idle - Minutes 1`).
- `Function - Cook - TEST` não deixa cozinhar com fogo no cômodo.

**O motor e o fogo** (binário): `cTSObject::IsBurning` (od 8 bit 9, `+0xaa`),
`CanBurn` (od 40 bit 5, `+0xea`) e `IsFireproof` (od 8 bit 11) só são
chamados pela vtable (slots `0x214`/`0x218`/`0x21c` do `cTSPerson`/
`cTSObject`); nenhuma chamada direta. O que se sabe: o FBO tira objetos
pegando fogo (§4); o teste por passo trata o Fire como objeto que não deixa
pessoas entrarem. Nada mais no motor reage ao fogo (sem desvio de rota, sem
alarme, sem bombeiro no código C++): pânico, propagação, mortes, alarme e
bombeiros são todos árvores. (Busca por chamadas virtuais nesses slots:
ambígua, outras classes usam os mesmos deslocamentos; não fechada.)

Hipótese para as mortes do soak (a conferir pela causa da morte): um Fire
dura ~2 h e empurra pânico a cada 30 voltas a todos que não estão em
interação; Sims em pânico repetido por horas não comem nem dormem.

**Conferido nos soaks (rodada 14, sobrevivência 4; `S2PKG_FIRES=1` no
`s2pkg autonomy` imprime cada Fire com o tile e onde cada Sim está ao
trocar de interação, "burning" com a flag 0x200 do od 8).** Em 20 rodadas
de 3 dias (10 lotes que perdiam gente × sementes 1 e 2): 39 mortes, **26 de
fogo** e 13 de fome. Todas as de fogo: o Sim estava pegando fogo e **no
tile de um Fire** (uma a 1 tile, na borda), depois de "Fogo!" — ninguém
pegou fogo fora do tile. Os motivos dessas mortes variam (o Daniel do
`N001_Lot68` tinha todos acima de −60): é o fogo, não os motivos do pânico
repetido. O pânico **afasta**: o "Interaction - Panic" vai ao slot global 9
(SLOT 0x64: direções 0x55, distâncias 2/2/5) do Fire e para a 2 tiles dele
(conferido no trace). Como se morre então: o fogo se espalha (10 a 32 Fires
vivos nos lotes sem detector, horas a fio) e o pânico seguinte é empurrado
para **outro** Fire (o "Push - Panic" escolhe o Fire pelo Set to Next): o
objetivo a 2 tiles dele pode cair no tile de um terceiro Fire, porque o Fire
tem a flag 0x04 do od 8 ("allow object and person intersection") e não
bloqueia nem rota nem objetivo (o `EvalTileForGoal`, BV `0x475074`, testa
ocupação com o `TSObjectCanSetPositionAndOrientation` do Sim, que essas
flags liberam: o mesmo no jogo). Chance por cozinhada: 11 começos de
incêndio para ~170 "Cozinhar" (~6,5%, perto dos 6% de quem não tem
habilidade, mais a comida esquecida: há dos dois casos), não por tick. As
mortes de fome são outra coisa: refeições que falham (a rota ao fridge
interrompida por reações empurradas, "Cheira Mal", "Ficar com Nojo", baratas
"Pisar"), e eram muitas mais com a pegada nova antes de o teste por passo e
o Snap usarem a pegada também (`docs/routing-mac.md` §2).

### Rodada 15 (front "engine-re"): como o fogo se espalha e quando apaga

Lido nas árvores do Fire (`0x7FD0A790`) com os BCONs resolvidos (dono
`0x1A`: `Tuning - Burn` = BCON `0x1003`, `State Flags` = `0x100a`,
`Tuning - Extinguish` = `0x1001`); operadores do Expression como na FreeSO
(8 testa flag, 9 liga, 10 desliga, 11 `++<`, 12 `%=`, 14 `>=`).

**O relógio do Fire.** `Function - Main` (`0x1000`) roda `Main - Setup` e
entra no laço: `Idle 3`, atributo 4 += 3, chama (atributo 2) = 0 → fim;
senão `Main - Set on Fire - Sims on Tile` e `Burn - _Simulate`; a cada 30
voltas `Push - Panic - All Sims Not Interacting` e `Main - Evaporate
Puddle`. `Burn - _Simulate` (`0x1002`) converte o atributo 4 em unidades de
**20 ticks** (temp 0 = atr 4 / 20, atr 4 %= 20; 0 unidades → só o efeito)
e, por unidade: idade (atr 0) += n, contador de propagação (atr 5) −= n,
`Burn - Decrease Flame(n)` (chama −= n). Depois, **com chama > 10, atr 5
< 0 e idade > 45** (`0x1003`[3]): `random(8)` (`0x1003`[6]) = 0 →
`Burn - Spread` e só então **atr 5 := 10** (`0x1003`[5]); o sorteio que
não dá 0 não mexe no atr 5 e **se repete na unidade seguinte** (corrigido
na rodada n32: o nó 15 vai direto ao teste de idade; antes estava "tentou
ou não"). Ou seja, 1/8 a cada 20 ticks enquanto valem as condições, e 10
unidades de pausa depois de cada `Burn - Spread`. Idade > 75
(`0x1003`[4]) sem a flag de estado 5 → `Burn - Destroy`.

**Chama inicial** (`Main - Setup`, `0x1004`): soma o od 84 dos objetos do
tile (Set to Next 8) no atr 3 → atr 1 = 0 (soma 0), 1 (< 40), 2 (< 70) ou 3;
chama = 80 + (200 − 80) / 4 · atr 1 + `random(7 · 2)` − 7, isto é
**80 + 30 · atr 1 + random(14) − 7 = 73 a 176** (o "aleatório 0–7" e o
máximo 170 do §4f estavam errados: o `Random Number` usa o temp 0 = 14
como faixa). Em lote **não
residencial** (`Lot - Is Residential?` falso) o Fire se apaga sozinho na
hora (`Remove Object Instance` de si mesmo). O Setup também chama a
**Generic Sims Call 10** (Comprar/Construir desligados) e o fim do fogo a
**11** (religados).

**Para onde se espalha** (`Burn - Spread`, `0x1003`, uma direção por
tentativa): o atr 6 do Fire guarda, em bits, as direções já tentadas (flag
d + 1); −1 = esgotado, nunca mais se espalha. Escolhe a direção 0–7 cujo
vizinho tem a maior inflamabilidade (`adjObjList - Get Flammability -
Highest`); se nenhuma > 0, uma reta ao acaso (`random(4) · 2`). Se essa
direção já foi tentada, anda +2 (mod 8) até 4 vezes; sem nenhuma livre,
atr 6 := −1. `Burn - Spread - Skip?` (`0x1049`) pula a direção com
**parede** (`Test - Wall in Direction?`), com um Sim mais novo que o
parâmetro de idade, com objeto de categoria 69 ou 80 (`G - Category`) ou
com entradas da lista de vizinhos sem nenhuma das flags 1/2/4/5 (`adjObjList
- Flags`); pulada ou não, a direção fica marcada. Não pulada → `Burn -
Spread - Create Fire` (`0x1001`): vira o Fire para a direção, **Create New
Object Instance** do Fire (`0x24C95F99`) "in front of stack object" com a
flag **0x01 (não cria se já há um Fire no tile)**; o motor ainda testa o
lugar (`TSObjectCanSetPosition`); o novo Fire ganha a flag de estado 4 e
atr 8 = a direção de onde veio; objetos do tile com od 40 flag 6 ("Burns")
passam o fogo às partes do multi-tile (`Burn - Spread - MT`). Ou seja,
**cada Fire acende no máximo os 8 vizinhos, um por tentativa**, e o
incêndio cresce como uma mancha enquanto houver tiles sem Fire; **não há
limite de número de Fires** nas árvores, e no binário não achei nenhum
limite de objetos (nenhuma string "too many objects"; o `TryCreateObject`
só falha por lugar/tile ocupado, §0x2A).

**Quando apaga.** A chama só desce: −1 por unidade de 20 ticks sozinha
(~3.400 ticks para 170). Com a flag de estado 2 (alguém apagando) e chama ≤
35, apaga de vez (`Burn - Decrease Flame`, `0x100e`). Chama < 1 → 0, para
efeitos e sons, `Sub - Extinguish` (limpa a flag 10 "pegando fogo" dos
objetos do tile e zera a chama dos outros Fires no mesmo tile), `Sub - Pass
Control to Another Fire`, religa as interações; o Fire espera até 20 ×
`Idle 30` enquanto houver fogo no cômodo e se remove.
**Apagando** (`Extinguish - Simulate`, `0x100d`, a cada volta da
animação): `Divide Tick Counts(30)` → n; chama −= **4 · n** (`0x1001`[4])
para um Sim e **200 · n** (`0x1001`[5] · 2) para o bombeiro (NPC tipo 12):
um Sim leva ~18–43 unidades de 30 ticks por Fire (com o corte em 35, a
metade disso), o bombeiro apaga na primeira volta. `Extinguish - _Core`
segue para o próximo Fire (`Extinguish - Next Fire`) até não sobrar.

**O motor no fogo** (o que o VM precisa ter certo): Create New Object
Instance com a flag 0x01 e a posição 3 (§0x2A), `TSObjectCanSetPosition` do
Fire no tile novo, Generic Sims Call 10/11, `Divide Tick Counts`, a
contagem de paredes do `Test - Wall in Direction?` e o Set to Next 8
(objetos do tile). Nada mais: sem limite de objetos, sem código C++ de
propagação.

### Rodada n32: quem chama os bombeiros, e o incêndio do `N001_Lot68`

Pergunta (fix-noite): no `N001_Lot68` o fogão pega fogo, ninguém chama os
bombeiros e dois Sims morrem; é o jogo? **É.** Lido nas árvores
(`tools/datahunt`, todos os BHAVs do `objects.package` e dos globais) e no
binário (Ghidra MCP, só leitura).

**Quem agenda o bombeiro.** Só duas árvores chamam `Schedule - NPC - Once`
(`0x2E4`) com o tipo do bombeiro (constante `0x5910` = **12**; nenhuma chama
o `0x2E5` com ele):

- **`Sub - Call Fire Fighter`** (`0x1005` do `Alarm - Smoke`, grupo
  `0x7F97EA32`). O Main do alarme roda `Process` a cada **60 ticks** (`Idle
  60`) e na hora em que o Fire o acorda (`Notify - Fire Alarm` do `Main -
  Setup` do Fire: atributo 0 := 1 e Notify out of Idle nos alarmes **do mesmo
  od 29**). `Process`: com fogo no seu cômodo (`Test - Fire in Room?`: algum
  Fire com o mesmo od 29) toca, liga os efeitos, acorda quem está no cômodo e
  chama o `Sub - Call Fire Fighter`: se o NPC já está agendado (`Schedule -
  NPC - Check`) ou no lote (`NPC - Instance of on Lot?`) não faz nada; senão
  agenda uma vez e põe o atributo 14 do `Controller - NPC` em 1 ("houve
  fogo": o bombeiro só o lê em `Fire - Was there a fire?`/`Fire - Clear Fire
  History`, para a bronca do trote). Alarme em outro cômodo não vê o fogo.
- **`Emergency - Fire`** (`0x1004` do objeto de chamada `Phone Emergency -
  Fire`, grupo `0x7F0884BA`, semi-global `PhoneCallGlobals`), pelo
  `CT - Handle Call` da chamada. `Fire - Can Call?` é sempre verdadeiro (sem
  fogo o bombeiro vem e dá bronca: `Fire - If Fires, Set Attrib in NPC
  Controller` põe o atributo 14 em 0). Chega-se a ele só pelo telefone: a
  entrada 0 "Call" do `PhoneGlobals` (`0x7FB208FA`; `Interaction - Call -
  TEST` `0x2008` → `Call - Build Menu` `0x2025`) põe as categorias como
  action strings (STR# `0x12E`: "Call.../Service", "Call.../Emergency",
  "Call.../Work", "Call.../Delivery", "Call.../Transportation"; constantes
  9601–9612, Emergência = 9602), e o `Interaction - Call` (`0x2030`) abre o
  `Call - Display Selection Dialog` (`0x2023`, "Emergency Phonebook") onde
  **o jogador** escolhe a chamada. Nenhuma árvore do pânico, do Fire, do
  `Controller - NPC` ou da pessoa empurra uma chamada.

Ou seja, **sem alarme no cômodo do fogo só o jogador chama os bombeiros**.
63 dos 69 lotes não têm alarme; têm: `N001_Lot11`, `N001_Lot49`,
`N002_Lot10`, `N002_Lot2`, `N002_Lot22`, `N002_Lot3`.

**Os moradores não apagam sozinhos (confirmado no binário; era inferido).**
`Interaction - Extinguish - TEST` (`0x1012`) devolve falso quando o
parâmetro 0 = 1, depois de aceitar o bombeiro (dado de pessoa 155 = 12). O
parâmetro 0 é o "autônomo" do `cEdithObjectTestSim`: `FBA_FindStaticInteractions`
(BV `0xad4130`) e `FBA_RunCheckTree` (BV `0xae24e4`) o montam com
`FUN_004c5cb0(teste, pessoa + 0x2e0, 1, 0)` (`+0x10` := 1, `+0x11` := 0), e
`cEdithObjectTestSim::TestInteraction` (BV `0x4c6214`) passa ao check tree
os parâmetros {`+0x10` ≠ 0, 1 se a vtable `0x2c` da interação (o campo
`+0x8`) devolve 1, o argumento `long`}, os "autônomo, join, Sim clicado" do
menu de pie (rodada 19, acima); no menu de pie `+0x10` = 0 (o construtor
`FUN_004c7024` o zera e só as duas FBA chamam o `FUN_004c5cb0`). A entrada "Extinguish"
do TTAB (autonomia 50, anúncios 0–100 em nove motivos) serve ao bombeiro.

**O pânico prende a casa perto do fogo.** `Push - Panic - All Sims Not
Interacting` (`0x102c`, no Setup e a cada 30 voltas do laço de cada Fire)
só empurra o "Fogo!" em quem está **no cômodo do Fire** (ou fora, se o
cômodo do Fire é externo), não é bombeiro, tem idade, está no lote, sem o
bit 0 do od 34 e do dado de pessoa 68, e cujo objeto de interação (dados de
pessoa 80, 79 e 103) ainda não é um Fire. Quem está em outro cômodo o vê
pela `Function - Awareness` do Fire, que a `Reaction Awareness` da pessoa
(`0x20dc`, semi-global `0x7F01EC29`) roda quando anda (`Function - Along
Route`) ou está à toa (`Function - Main - Do Idle Actions`). O `Interaction
- Panic` (`0x1013`) leva o Sim ao slot global 9 do **seu** Fire (2–5 tiles)
e fica no laço até o dado de pessoa 28 passar de `Tuning - Panic`[0] = 15
(`Divide Tick Counts(20)` por volta); no fim, se o `Wait For Notify` é
interrompido, empurra outro pânico em si mesmo (`Push - Panic`: o primeiro
Fire por ID com chama ≥ 1 e od 25 = 0). Então **a casa entra em pânico sem
parar enquanto há fogo no cômodo**, a 2 tiles do Fire do seu pânico, não
dos outros.

**Andar pelo fogo.** O `Function - Init` do Fire liga a flag 3 do od 8
(`0x04`, "allow object and person intersection") e o `Main - Setup` desliga
a 5 (`0x10`) e liga a 2 (`0x02`): `CanIntersectPeople` (vtable `0x1d8`, od 8
& `0x14`, `routing-mac.md`) fica verdadeiro, e rotas e passos atravessam os
tiles de Fire. `Main - Set on Fire - Sims on Tile` (`0x1051`) põe fogo em
toda pessoa no tile a cada `Idle 3`. É assim que se morre: o pânico novo
anda até o slot 9 de um Fire e cruza o tile de outro, ou o fogo se espalha
para o tile onde o Sim está parado em pânico (`Burn - Spread - Skip?` só
pula Sims mais novos que a constante global `0x4801`).

**Quanto tempo se vive pegando fogo.** `Panic - On Fire!` (`0x102a`): põe um
Fire no tile do Sim se não há (Create New Object Instance), `Set Motive
Deltas`, e **5 a 10 voltas** (`Get in Temp X - Random in Range` com `Tuning
- On Fire!`) de `a-burn-loopWrithe` (6,93 s) ou
`a2o-react-panicFire-loopFanButt` (5,74 s) por cara ou coroa; ainda pegando
fogo no fim → motivos 5–9, 14 e 15 a 0 e `Kill Sim`. Se a flag 10 some (o
Fire do tile apaga: `Sub - Extinguish` limpa a flag dos objetos do tile), o
Sim sobrevive. São **29 a 69 minutos de jogo** (5,74–6,93 s × 5–10 voltas, a
30 ticks por segundo de animação e 30 ticks por minuto de jogo).

**O fogo do `N001_Lot68` nesta árvore** (semente padrão, 12 h do horário
salvo; o "17:29, Angela cozinhando" do fix-noite era de outra linha do
tempo): às 11:49 o Daniel põe uma comida congelada no forno ("Cook - _Type -
Bake" com o Free Will ligado: põe e sai). Às 12:04 a comida fica pronta e o
`Push - Get Food` (`0x2034`) **não chama o cozinheiro porque alguém interage
com ela** (`Test - Anyone Interacting w/ Me?` `0x2036`: o dado de pessoa 80
de alguém = a comida): a Mary-Sue tinha escolhido "Comer" nela e andava para
o forno (`Route - To Oven`); a rota falhou (códigos 7 e 14: o Daniel estava
parado no lugar do forno, "Preocupar-se") e ela desistiu às 12:05. Ninguém
mais veio: queimou aos 10 min (`Sub - Set Burnt`, 12:14) e pegou fogo
(`Sub - Set Fire`, 12:43). Pânico de todos, 35 Fires em 6 h do fogão até a
sala, Lilith (pega fogo 14:54, morre 15:50: 56 min) e Daniel (pega fogo
14:49, o Fire do tile apaga 15:18, pega de novo 15:32, morre 16:25: 53 min);
a Angela pegou fogo 11 min e o Fire dela apagou. Tudo isso são as árvores;
a rota que falhou no forno é das rotas (`TODO.md`).

**Por que o fogo dura horas.** Com o sorteio a cada unidade (correção do
§ da rodada 15 acima), um Fire tenta se espalhar, em média (Monte Carlo da
árvore: chama inicial, idade > 45, chama > 10, 1/8 por unidade, pausa de 10),
**1,5 vez no chão vazio** (atr 1 = 0, chama 73–86), 3,1 / 4,8 / 6,5 com atr 1
= 1 / 2 / 3. Cada tentativa vai a uma direção nova (atr 6) e falha em
parede, em Fire já aceso, em objeto sólido que não queima (`Skip?`) ou onde
o Fire não cabe; tile vazio é sempre alvo. Mais de uma tentativa por Fire
mesmo no chão nu: **o incêndio cresce por todo piso aberto** e reacende os
tiles já queimados (nada marca o chão queimado) até, por acaso, todos os
Fires de uma frente apagarem antes de passar adiante. O VM roda essas
árvores; a medida abaixo é isso.

**Medida (rodada n32; nada no comportamento mudou nesta rodada, então
antes = depois).** `s2pkg autonomy`, 3 dias de jogo (129.600 ticks) do
horário salvo, `OS2_SEED` 1–6, `S2PKG_FIRES=1` (que agora diz quem pega
fogo, quem para de queimar e os NPCs que chegam, com o tipo: 12 =
bombeiro); causas das mortes pelo chamador do `Kill Sim`:

| Lote (alarme) | Sementes com fogo | Fires (pico vivos, duração) | Bombeiro | Mortes no fogo | Fome depois do fogo | Fome sem fogo |
|---|---|---|---|---|---|---|
| `N001_Lot68` (não) | 4, 6 | 168 (29, 21,1 h); 65 (20, 12,3 h) | nunca | 3 (uma visita) + 1 | 1 + 3 | 2 (semente 5) |
| `N003_Lot52` (não) | 2, 4 | 41 (16, 8,7 h); 39 (14, 7,7 h) | nunca | 2 + 2 | 2 + 2 | 1 + 1 (sementes 5, 6) |
| `N001_Lot11` (sim, na cozinha) | 6 | 1 (1, 0,3 h) | chegou e apagou | 0 | 0 | 1 + 1 (sementes 1, 2: Cassandra) |

Nos lotes sem alarme, **um fogo mata a família inteira** (os que não
queimam passam fome em pânico, com os motivos derrubados pelos `Set Motive
Deltas` do `Panic - On Fire!` de quem pegou fogo e sobreviveu, ou depois,
na casa queimada: a Mary-Sue do `N001_Lot68` semente 6, 29 h depois): é o
jogo.
Quem pega fogo morre em 43–56 min de jogo (Hermia 55, Tybalt 43, Lilith 56,
Daniel 53), dentro dos 29–69 das árvores. As mortes de fome sem fogo são
outra frente (`TODO.md`: Angela 4 h em "Receber Beijo" no `N001_Lot68`
semente 5).

## 4g. Rodada 12: Sims vivos e destravados (binário + árvores)

### Erros reiniciam o objeto (`cTSObject::HandleError`, BV `0xb05510`)

Fora do depurador, um erro de SimAntics não deixa o Main morto: o
`cTSObject::Error` liga a flag `0x100` de `+0x214` e o `HandleError`, sem
depurador (`+0x44` = 0 e o módulo sem modo debug), chama a vtable `0x534`
(`UnLinkAllInteractions`), avisa o gerente de objetos (`+0x1b4`, vtable
200) e chama a vtable `0x320` com (nenhum, falso): o **`Cleanup(cITSObject*,
bool)`** (BV `0xb09838`; o nome do `symxfer` confere com a descrição do
editor da entrada 3: "chamada quando o objeto vai ser apagado ou quando a
interação de uma pessoa com ele foi abortada"). Com "nenhum" ele limpa o
próprio objeto (rodada 13):
- **pessoa** (`cTSPerson::OnCleanup`, BV `0xadcbcc`, vtable `0x530`, que
  devolve verdadeiro e encerra): limpa a pilha e as interações, tira do
  mundo e põe de volta num lugar livre perto (`0x2d8`), zera o dado de
  pessoa 34, reinicia a árvore no Main e roda a **árvore de cleanup** (OBJf
  entrada 3);
- **objeto com árvore de cleanup** (vtable `0x170` com 3 ≠ 0): roda-a
  (`RunTree(3)`, vtable `0x3b4`) e reinicia a árvore no Main; os dados ficam;
- **objeto sem ela**: tira do mundo, `cTSObject::Reset(1, verdadeiro, falso,
  falso)` (BV `0xb0ab9c`) e volta ao lugar (ou ao slot do contêiner).
  O `Reset` copia os 113 dados de objeto (`+0x9a`), zera todos e devolve
  só 1 direção, 2/3 contêiner e slot (do lugar lido antes), 11 ID, 19 líder,
  21/24/45/46/47 data de nascimento, 29 cômodo, 41 valor atual, 55, 67
  contagem de slots, 79 semi-atributos, 80/81 e 105–107 ligações; põe 71
  (render flags) = 0, 57 e 72–75 (alfa e RGB) = 255, 23 (rotation notches)
  = 2, 58 (versão) e 78 (look at score) = rótulos 49 e 13 do OBJD, e com
  "Use Default Placement flags" (rótulo 12) 4/5/13/42 = rótulos 6/3/5/4
  (alturas, paredes vizinhas, colocação na parede, colocação); 7 vem da
  virtual `0x578`, **`cTSObject::GetDefaultNicenessMultiplier`** (BV
  `0xb00b64`, confirmado na rodada 14): o **campo 45 do OBJD**
  ("NicenessMultiplier" do catálogo), gravado como inteiro (`sth` em
  `+0xa8`). Os atributos só são zerados com o
  terceiro argumento (aqui falso); os 8 temps do objeto (`+0x8a`) e
  `+0x1a0` são zerados. Para um objeto que não é parte de multi-tile
  (vtable `0x19c`) roda o **Init** (`RunTree(0)`), depois a árvore volta ao
  Main.
O jogo grava o `ObjectError` e segue. A VM (`VM::resetAfterError`) faz o
mesmo desde a rodada 14 (sobrevivência 4): o dado 7 é o campo 45 do OBJD, e
a pessoa sai do slot e volta a um lugar livre perto (`VM::placeNearFree`:
onde está se a caixa dela não toca a pegada de nenhum objeto nem outro Sim
a menos de 0,5, senão o centro do tile livre mais perto, no mesmo cômodo
primeiro, até 10 tiles; inferido até a rodada 15, ver abaixo), o dado de
pessoa 34 zerado e a árvore de cleanup rodada; os
dados além do 87 não existem na VM. `S2PKG_NO_RESET=1` mantém o
Main morto para depurar. O orçamento de instruções (a "Too many
iterations" 0x44d do jogo: 100000 nós por tick, rodada 16) também reinicia.

**O lugar livre do reset (rodada 15, binário).** O `cTSPerson::OnCleanup`
(BV `0xadcbcc`) pega a posição da pessoa (vtable `0x2dc`); fora do lote
(`IsOnLot`), o **centro do mundo** (largura/2, altura/2). Chama a vtable
`0x2d8` = **`FUN_00af87c8`** (busca genérica de lugar para um objeto) com
os parâmetros: começar nessa posição e nível, **mesmo cômodo** (`+0x14`),
sem direção preferida (`+0x1a` = −1), sem as outras restrições (`+0x15`
tiles com objeto que não deixa cruzar, `+0x16` o gerente de rotas, `+0x17`
tiles travados, `+0x18` tirar o bit `0x100` do od 42, todos 0); sem achar,
chama de novo **sem o mesmo cômodo**; sem achar de novo, fica onde está.
A busca: uma **espiral quadrada** a partir do tile da pessoa (preso a [1,
lado − 2] do mundo), no máximo **8 × o lado maior do mundo** passos; cada
tile precisa estar no lote (`IsOnLot`), passar o teste de partição do tile
(`PartitionManager` vtable `0xbc`), estar no cômodo do começo (`0x13c` do
`PartitionManager`; só na primeira volta) e passar o
**`TSObjectCanSetPosition`** da pessoa no **centro do tile** (x + 0,5, y +
0,5). O primeiro que passa vira o lugar (vtable `0x2a4`, sem mudar a
direção). O `VM::placeNearFree` faz assim desde a rodada 15 (sobrevivência
5); inferidos: a ordem da espiral (+X, +Y, −X, −Y, os lados crescendo a
cada duas voltas) e o teste de partição como "tem chão".

### Set to Next: objetos que vão morrer não contam (`SetToNextIsObjectEligible`)

`ValidateObject` (BV `0x1be5460`) recusa objetos com a flag interna
`0x10000` (posta por `AddToKillQueue`/`ProcessObjectForKillQueue`) ou
`0x20000`; o Set to Next e o Find Best Object for Function pulam esses
objetos. Sem isso o "Reaper - Scan Lot For Dead People" achava de novo o Sim
que tinha virado urna no mesmo tick e o Main do Ceifador morria no "Wants
Satisfy - Death of Sim". O filtro 0x02 (dado de objeto u16 9 == u16 11) é
aplicado só em nós de versão 1 (rodada 13: na versão 0 o `SetToNextParam` zera esses bytes no load; lidos como filtro, o "New Baby" do túmulo de depuração não achava nada e ninguém cuidava do bebê em `N002_Lot10`); o 56 bit 0 e o 107 ficam de fora (só BV).

**Tipo 3 (parte de multi-tile)**: o binário exige que o alvo seja parte de
multi-tile (vtable `0x19c`) e anda no grupo (`+0x1c` próxima, `+0x18`
primeira): um grupo que ficou com uma só parte devolve o próprio alvo. O
"Burn - Spread - MT" laça no Set to Next 3 até voltar ao início (falso volta
ao mesmo nó): com o "não há outra parte" a VM estourava o orçamento num
objeto meio queimado (`N001_Lot36`, 51 vezes em 3 dias). A VM toma "é
multi-tile" pelo campo 10 da definição ("Multi-Tile master id"), o mesmo que
a árvore testa.

### Notificação de texto sem stack object (`TryDialog`)

Nos tipos de notificação, o objeto do texto é o objeto **dono da árvore**
do frame (vtable `0x0C` do `cITSTreeStackElem`; o log do `TryKillObject`
chama-o "the tree owning object"), o stack object só se ele não existir; o
erro 0x17 só sem os dois. O stack object só dá o ícone. (A Circe de
`N002_Lot3` morria numa ligação cujo interlocutor já tinha saído.)

### Force Recalc of Wants do Ceifador (neighbor 0)

O "Wants - Refresh All - Verify" chama a Generic Sims Call 39 (op 14) com o
dado de pessoa 31 de cada Sim do lote; o binário dá erro 0x37 se o vizinho
não existe. O Ceifador é criado por Create New Object Instance (sem a flag
0x04) e ficava com o vizinho 0. O bairro tem um vizinho sem arquivo de
personagem para cada NPC único (N002: #1 terapeuta, #34 carrinho, #35
**Ceifador** GUID `0x84EC24A8`, #225 dançarina, #361 polinizador). A VM
liga o Sim criado ao vizinho de mesmo GUID, ou cria um com o próximo ID
(`VM::neighborForGuid`; **inferido**: no binário isso fica atrás de
chamadas virtuais não seguidas). Com isso e o filtro acima, o Ceifador
recolhe os mortos.

### Find Best Object for Function: cômodos acessíveis

`cTSPerson::GetAccessibleRooms` (BV `0xacd218`) pede o conjunto ao roteador
(`this+0xf8 → 0xd4`, functor com a pessoa e o cômodo); a versão antiga
(`GetAccessibleRoomsOld`, `0xad4abc`) é uma busca em largura pelos cômodos
ligados por portais (portas, escadas; com a flag 0x04 do dado 22 testa se a
pessoa pode passar). A VM monta, uma vez por planejador, os grupos de células
livres do mapa estático (paredes, chão, aberturas de portas) ligados pelos
portais, e o conjunto é o dos cômodos do grupo do Sim
(`PathPlanner::accessibleRooms`; **inferido**: objetos, Sims e travas de
portal ficam de fora). Um Sim só olha objetos desses cômodos. Achado com
isso: **portas diagonais** ("Door - 1 Tile - Diagonal") são uma parte só,
na parede diagonal do tile, e o carregador só abria portas com outra parte
atrás: os quartos de cima de `N001_Lot68` e o quarto/banheiro de
`N003_Lot52` estavam fechados.

### Slots de osso de objetos de várias partes

O modelo de um objeto multi-tile é o da parte líder: os ossos dos slots de
rota ficam a partir do ponto e da direção do líder, qualquer que seja a
parte do slot (**inferido**). A escada reta (`Stairs - Straight`) tem os
ossos "route_staircasestraight_*_top" no "Hi 0,0 Pad", 3,7 tiles acima do
líder "Lo 0,4": tomados do próprio pad caíam 3,7 tiles além do topo, o Snap
do "Portal - Do" falhava e ninguém subia (`N001_Lot11`: o "Dormir" falhava
sempre e os Sims dormiam no chão).

### Pedir para sair do objetivo: tipo 1

O `DoBlockedGoalStillAvailable` pede com **tipo 1** (1 a 3 tiles, direções
±45°/±90°) quando a rota tem objeto de destino e o bloqueador está a menos
de 1,5 tile dele (`docs/routing-mac.md` §8.3); a VM só usava o tipo 0 (0,3
tile). O cozinheiro no cooktop saía 0,3 tile e ficava no slot do forno do
mesmo fogão; a outra Sim desistia de tirar a comida do forno (`N002_Lot3`)
e ela pegava fogo 40 minutos depois. Direções tomadas em relação à de quem
pede (inferido).

### Idle for Input dentro de uma interação

Com "allow push", dentro de uma interação a entrada da frente roda **por
cima** da atual quando pode interrompê-la (prioridade maior); quando volta,
a interrompida continua (`TryIdleForInput`, §4). A VM só empurrava fora de
interações. A criança que a assistente social leva fica em "Chorar por Filho
Perdido" (`Interaction - Cancel` do carro, prioridade 50, um Wait For Notify
sem fim com allow push) e só sai pelo "Entrar" (100) da assistente.

### Comida no forno: o que a protege de "Limpar"

Ninguém marca a comida assando como em uso (nem od 8 flag 6 nem o semi 3,
que é "refeição em grupo"). A proteção é a árvore do prato (`Interaction -
Clean Up - TEST` do semi-global `0x7F8834C8`): autônomo, uma refeição
individual intacta (semi 2 == semi 1 da comida) só pode ser limpa se o "CT -
Stats" (nome 42 da tabela global de árvores nomeadas) disser que está
estragada (semi 17 flag 2). Com as datas de nascimento da rodada 11, a
comida nova não está estragada e ninguém mais tira a comida do forno: não
se repetiu nesta rodada.

### Cozinheiros que saem do forno

Com o "Free Will" (global 30, `cTSSimSystem::SetFreeWill`: o booleano das
opções, ligado por padrão) o "Cook - _Type - Bake" põe a comida e sai. A
comida tem dois relógios (BCONs do semi-global `0x7F3C9675`):
- **queima durante o cozimento** (`Main - Cook - Loop`): o "Test - Food
  Failure?" sorteia a falha pela habilidade do cozinheiro (semi 23 = dado de
  pessoa 10 no início, pelo "Sub - Get Skill Based Value"; abaixo de 40 é
  "baixa", 30% de falha; as faixas média/alta usam as constantes seguintes,
  não lidas)
  e, falhando, num ponto do cozimento a comida queima e **20% das vezes pega
  fogo** — esteja o cozinheiro lá ou não;
- **esquecida depois de pronta** (`Function - Main`): "Tuning - Cook Failure
  Time" (0x2011, 10 min) até queimar e "Tuning - Cook Fire Time" (0x2012, 30
  min) até pegar fogo, se ninguém interage com ela.
O "Push - Get Food" chama o cozinheiro com prioridade de usuário (50) assim
que fica pronta. Ele só vem se a interação dele tem prioridade menor que 50
(`ShouldInterrupt`); comendo outra coisa empurrada a 50 ele termina primeiro
(25+ min num pedaço de pizza, 30% de mordida a cada volta). Nada no motor
atrasava além disso, salvo os dois bugs de rota acima (escada e slot do
forno ocupado).

### Change Material sem grupo de malha (`TryChangeMaterial`, BV `0xe2f128`; rodada 13)

O material (STR# 0x88) e o grupo de malha (STR# 0x87) são procurados como
strings preguiçosas (grupo, tabela << 16 | índice + 1, língua): o material
vem do grupo global (`0x7FD46CD0`) com a flag `0x02`, do semi-global do
objeto com `0x04` (sem semi-global: sem nome), senão da tabela do próprio
objeto e depois da do grupo do código; o grupo de malha igual com `0x40`
(global) e `0x80` (semi-global). Material vazio é o erro **0x46**
("Missing Material / Mesh Group Name"); o nome do grupo de malha é
procurado e **nunca lido** (o registrador só volta ao destrutor): faltar a
entrada não é erro. Os arbustos ("Shrub - Creeping Phlox", "Hydrangea")
pedem o grupo 0 de um STR# 0x87 que o grupo deles não tem a cada hora no
"Function - Main" do semi-global `0x7F1A9ECC`; a VM dava erro e os
reiniciava. Agora o material vale para o modelo todo quando o grupo não tem
nome (inferido; no BV o nome nunca é usado). O byte 13 com `0x02`, `0x04`
ou `0x20` pula o material (snapshot, "Moving Texture"; não feito).

### Quem chama os bombeiros (rodada 13)

Ninguém da casa liga sozinho. O "Ligar..." do telefone (`7FB208FA`, TTAB 2,
entrada 0, ação `0x2030`) não tem **nenhum anúncio de motivo** (ads = 0),
então o Find Best Interaction nunca o escolhe; o "Emergency - Fire" do
objeto de chamada `7F0884BA` só roda pelo menu. O "Interaction - Panic" do
fogo (`0x1013`) só anima, empurra "Salvar Bebê" e grava a memória; não chama
ninguém. O motor não tem código de bombeiro (nenhum símbolo no BV). O
único caminho autônomo é o alarme de fumaça **no cômodo do fogo** (`Alarm -
Smoke` → `Sub - Call Fire Fighter`, §4f). Sem alarme no cômodo, o fogo só
termina sozinho (a chama de cada Fire dura ~2 h) ou pelo jogador.

### O carro da assistente social (rodada 13)

O carro (`7FA5FB07`, semi-global `CarGlobals` `7F4EA230`) anda e vai embora
desde a rodada 12 (laço do Animate Object). A criança levada fica em "Sub -
Create Orphan( Kill Kid )" (um Sleep de 250 ticks que volta a si mesmo) até
o "Sub - Remove Vehicle" do carro chamar o "Sub - Send Passengers Out of
World": para esse tipo de veículo (semi-atributo 5 igual a uma constante do BCON, não conferida) os
passageiros são **removidos** com ele. A VM já fazia isso; o "parado em
Levando a criança" do soak era o soak, que não via um Sim sumir (agora o
`s2pkg autonomy` imprime "is gone (removed from the lot)").

### Rodada 15 (sobrevivência 5): sociais de três, laços de habilidade e o fogo no VM

**Interações ligadas** (`TryPushAction`, flag 0x40, BV `0xe33f..`): a
interação nova pega o link da ligada e a ligada passa a apontar para a nova
(`new.SetLinkedGUID(linked.GetLinkedGUID())`, `linked.SetLinkedGUID(new.
AssignUniqueGUID())`; vtables `0x108` `AssignUniqueGUID`, `0x110`
`GetUniqueGUID`, `0x114`/`0x118` `Get`/`SetLinkedGUID`, `0x11c`
`FindInteraction`): uma corrente que sai da primeira. O `UnLinkInteractions`
(BV `0x4c6a54`) segue a corrente: a que roda ganha o dado de pessoa 110, a
que espera sai da fila. Na "Social - Baby - Talk To" (A, depois B e o
"Person C - Baby Holder" ligados a A) fica A → C → B; o VM ligava A e B um
ao outro e o C nunca sabia que A tinha saído: a Brandi (`N001_Lot36`)
segurava o bebê 11 h no laço do C (que só sai com o estado de A ≥ 12 ou
com o Wait For Notify verdadeiro, isto é, o 110).

**Laços de habilidade** (piano, cavalete, livros: `Skill - Exit Skill
Object?` → `Exit Object - Motive Failing?`, global `0x24F`): saem pelo
Wait For Notify (usuário, prioridade maior, 110) ou com fome ≤ −50,
energia ≤ −80, bexiga ≤ −75, diversão ≤ −80, conforto ≤ −70 (adulto) ou
higiene ≤ o parâmetro (0). Nada sobre a diversão cheia: um Sim toca piano
até a fome chegar a −50. Conferido no soak: o "Tocar" de 7,7 h do Alexander
(`N001_Lot11`) termina com a fome em −51 (≈ 5,5 pontos por hora). O "Ler"
do jornal (`Fun Object (Sim X) - Exit?`, `0x1DE`) sai com diversão ≥ 60,
fome ≤ −80, bexiga ≤ −85 ou energia ≤ −80: os 6,6 h da Ophelia vão de −90 a
60 de diversão. É o que as árvores pedem.

**O fogo no VM** contra a leitura acima ("como o fogo se espalha"): o
`Idle 3` do laço dura 3 ticks (o Sleep do binário conta o parâmetro até 0 e
segue no mesmo tick), a unidade de 20 ticks vira uma a cada 21 (7 voltas),
a chama inicial 73–176 dura 1.530–3.700 ticks e o Fire espera até 20 ×
`Idle 30` antes de sumir; nos soaks a vida (do nascer ao sumir) vai de 37 a
4.281 ticks. Num `N002_Lot22` semeado: a primeira tentativa de propagação
de cada Fire depois de ≥ 1.117 ticks (≥ 900 pedidos), intervalo mínimo de
219 entre tentativas (≥ 200), no máximo 5 "Create Fire" por Fire (≤ 8),
nenhum Fire num tile que já tem um (flag 0x01 do Create New Object
Instance). O Set to Next 8 (os objetos do tile, com o conteúdo dos slots)
e o 9 (um objeto por lado) da lista de vizinhos agora são os do binário.
O `TSObjectCanSetPosition` do Fire novo: feito na rodada 15 (§4g "Rodada 15
(planejador)").

### Rodada 15 (planejador): criar objeto, a escolha do Find Best Interaction e o limite de nós

**Create New Object Instance testa o lugar** (`TryCreateObject`, BV
`0xe34b2c`): cria o objeto (o Init roda aí, e é ele que liga as flags do od
8 que deixam pessoas/objetos atravessar), e para uma posição no mundo chama
`TSObjectCanSetPosition(obj, lugar, 0, 0, …)` (BV `0x8ae264` →
`TSObjectCanSetPositionAndOrientation` → `TSObjectCanSetPositionImpl` (deck e
porta, toldo) → **`TestPlaceInWorld`** (BV `0x8ad754`): `TestPlacementCommon`,
`TestPlacementWithRespectToWalls` (não para pessoa), 
`TestPlacementWithRespectToObjects` e o teste de altura do od 4 (erro 0x19));
num slot, `TSObjectCanPlaceInSlot`. Não cabe → o objeto é apagado (vtable
`0x44` do gerente) e o resultado é **falso**. `TestPlacementWithRespectToObject`
(BV `0x8ac158`), por par de pegadas que se cruzam (o objeto e cada um nos
tiles em volta): pessoa no caminho, salvo `CanIntersectPeople` do objeto (od
8 & `0x14`); objeto no caminho salvo `CanIntersectObjects` de um dos dois (od
8 & `0x06`), camadas diferentes (`FootprintBlocksCeiling` = od 42 bit
`0x1000`, `FootprintBlocksFloor` = o bit apagado) ou a árvore "allow
intersection" (entrada 5) de um dos dois; portas têm testes de círculo e de
pegada próprios. **No VM** (rodada 15; rodada n48: o objeto só é posto depois
do Init e do teste, como no binário, e o teste é o do base inteiro, com a
árvore 5, o teste exclusivo do od 63 e as portas, `docs/buy-build-mac.md`
§7.26 e o "A ordem do `TryCreateObject`" acima): com as pegadas de
`routingFootprint` (`checkPlacement`).
Efeito: Fires que se espalham para tiles sem piso, cinzas em cima de portas,
`_Coordinate`/emissores em cima de cadeiras deixam de ser criados. Os fogos
mudam de tamanho de semente para semente (N002_Lot22, 3 sementes: 125/0/0
Fires contra 71/162/73 sem o teste); `OS2_NO_CREATE_PLACEMENT=1` desliga.

**A escolha do Find Best Interaction** (`FBA_CheckInteractions`, BV
`0xae2978`) ordena com o **`qsort` da libc** (itens de 0x44 bytes) e o
comparador `0xac8f08`: `b − a > 1e−7 → 1`, `< −1e−7 → −1`, senão 0. O `qsort`
do Mac OS X (o do FreeBSD, Bentley–McIlroy) **não é estável**: empates saem na
ordem que a partição deixa. O VM ordenava com `stable_sort` (ordem do TTAB).
O monte de lixo da lata chutada (`Trash - Trash Pile`, grupo `0x7FD0658A`)
tem "Jogar Fora" (entrada 0: ação `0x2007` "Interaction - Dispose" das
`TrashGlobals`, guarda o global `0x109` **"Hide Menu"**, que só põe o dado
de objeto 50 = 1 e devolve verdadeiro) e "Limpar" (entrada 1) com **os mesmos
anúncios** (motivo 13 +40, motivo 15 +10, personalidade 11). O Dispose quer o
monte na mão (`Interaction - Dispose - NEW` → `Object in Hand - Get ID` falso
→ falso), e o FBA do binário não olha o "esconder" (o `FBA_RunCheckTree`, BV
`0xae24e4`, recusa só pelo dado de objeto 6 que a check tree deixa, bit
`0x200` da interação; o dado 50 só esconde do menu). Com a ordem estável o
Dustin (`N001_Lot36`, semente 1) escolheu o Dispose 20 vezes seguidas; com o
`qsort` do Mac, 2 vezes e depois "Limpar". A ordem em que os
candidatos entram foi lida na rodada 16 (§4g "Rodada 16 (autonomia e erros)"). A memória de falhas da autonomia
(`GetAutonomyBias`) continua desligada como no binário.

**Limite de nós por turno** (`cTSTreeSim::DoNodeAction`, BV `0xe1b3xx`): o
contador `+0xc` sobe a cada nó; passou de `DAT_0277ea94` = **100000** (300000
com uma flag de depuração, bit 0 do valor `0x33` do objeto) → erro `0x44D`
"Too many iterations". O VM já tinha 100000 como padrão, mas o `s2pkg
autonomy` usava 20000: o "Controller - Relationship Decay" do `N003_Lot192`
normaliza todo Sim da casa contra todo vizinho num tick (22.798 nós no tick
77371, semente 2) e era reiniciado. O `s2pkg autonomy` usa agora 100000
(`S2PKG_BUDGET=n` muda).

### Rodada 16 (autonomia e erros): a ordem dos candidatos, as listas de animação e as tabelas de árvores

**A ordem dos candidatos do Find Best Interaction** (`FBA_FindStaticInteractions`,
BV `0xad4130`). O módulo de objetos guarda **13 × 16 tabelas hash** de objetos
com anúncios (`GetObjectsWithAdsForMotiveSet`, `0x5268c4`: `this + 8 + grupo ×
0x140 + motivo × 0x14`; o grupo é a idade/espécie da pessoa, vtable `0x5b4`),
preenchidas por `AddOrUpdateObjectInObjectsByMotiveAds` (`0x531098`): para
cada entrada do TTAB que anuncia o motivo para o grupo (vtable `0xe0` da
entrada), o índice da entrada vai para a lista do objeto naquela tabela,
em ordem crescente. O objeto só entra se `CanChooseAutonomously` (`0xafea58`:
tipo ≠ 7, é o próprio líder de interação, `GetInteractionLeader`, e tem
alguma entrada). Na busca: **motivo a motivo da máscara, do bit 0 ao 15**;
dentro do motivo, os objetos na ordem da tabela hash; e para cada objeto
`cEdithObjectTestSim::AppendInteractionsForAuto` (`0x4ca54c`) **anda na lista
de entradas de trás para frente** (a última primeiro). Um par entrada/objeto
já pontuado não é pontuado de novo (`SetLastCheckTreeCheck`, `0x49f9a4`: por
entrada, um vetor indexado pelo ID do objeto com o "tick" e o ID de quem
procura). Os candidatos dinâmicos (strings de ação das check trees) são
pontuados em `FBA_ScoreDynamicInteractions`, **depois de todos os
estáticos**, e entram depois deles no vetor do `qsort`.

A tabela hash (`hashtable<tobjectWithAdsInfo, ObjectAdsRefHash>` do SGI STL)
tem como chave o **ponteiro do `cITSObject`**: o balde é o ponteiro módulo o
número de baldes (primos 53, 97, 193…; `insert_unique_noresize` `0x16d6948`
põe na frente do balde; `resize` `0x16d6264`). Essa ordem depende dos
endereços do heap e não dá para reproduzir; **o VM usa a ordem dos IDs**
dentro de cada motivo (inferido como substituto). O resto é o do binário:
motivo a motivo, entradas de trás para frente, dinâmicos depois. Efeito: com
o `qsort` (inserção abaixo de 7 itens) duas entradas de anúncios iguais do
mesmo objeto empatam e **a de índice maior ganha** ("Limpar", entrada 1, antes
de "Jogar Fora", entrada 0, no monte de lixo).

**Listas de animação: de que objeto** (`try_animate_sim` `0x69d634`,
`try_animate_overlay` `0x69ea54`). Para uma lista ≠ 0x80, o seletor vem do
elemento da pilha: `cTSTreeStackElem` vtable `0x50` devolve o campo `+0x20`,
que é **a árvore** (o `Write` `0xe185dc` grava o `+0x0A` = ID do stack object,
o `+0x0C` = nó, `+0x18/+0x1A` = rodando como/sobre, e o `+0x20` pelo
`cITSBehaviorFinder`); a árvore → o seu `cITSBehavior` (vtable `0x94`) →
`cTSObjectFolder::GetSelectorByBehavior` (`0x4e56b8`). Cada tipo de objeto tem
o seu `cTSBehavior` (com os arquivos privado, semi-global e global), então é o
**tipo do dono do código** (ou o grupo de uma árvore privada rodada para outro
objeto, como uma árvore de evento). O `FindSpecificName` lê a entrada da
lista desse seletor e, **só se ela for vazia**, a do semi-global. O VM lia
primeiro a lista do stack object: o "Event - Sit Carry - Trans Stand - Start"
de um copo (grupo `0x7F31DF4D`), com o Sim num banco de bar, pegava a entrada
9 do banco, o rótulo " ~Get Out", em vez de "a2o-trans-drinkCarry-SlotIKpose".
**As linhas "~" não são puladas**: `cTSAnimTable::GetEntry` (`0x594c6c`) é o
índice + 1 na tabela de strings, sem filtro; `MakeLower` também não as trata.
As buscas por "~Spine Overlays", "~Snooze Animations" e "~Get Out" vinham
todas da lista errada e sumiram (8 lotes semeados), junto com os
"a-spine-bend-chair*" e "a-deadPose" que faltavam.

**Overlay da lista global** (0x80): `try_animate_overlay` chama o
`FindGlobalName` (vtable `0x3c8`) **no seu próprio `cAnimatable`**, então a
tabela é a da idade de quem roda a primitiva; as outras listas vão pelo
`DetermineAppropriateAnimTable` com a pessoa alvo. O "CT - Route Failure
Feedback - Animation" (global `0x3A8`, nó 56) toca a entrada 90 no bebê no
colo (temp 0x0E): a lista adulta tem "b2a-routeFail-carry-baby" ali; a do
bebê (0x91) tem ".". O VM usava a idade do bebê e cada falha de rota com o
bebê no colo reiniciava o Main do adulto ("Animation name is empty string";
`N002_Lot4`, `N001_Lot36`).

**Tabela de árvores de partes de multi-tile**: `cTSObject::GetTreeTab`
(`0xb017b4`) → `cTSObjectSelector::GetTreeTable` (`0x51edf0`) com o ID de
`GetEffectiveTreeTableID` (`0x51d0e4`): **campo 7 = −1 → o do seletor mestre**
(`GetMasterSelector`), para qualquer parte, com ou sem grupo de interação. O
VM só dava a tabela do mestre a líderes de grupo e às partes 0,0 sem grupo: o
"Change - Common - Subqueue" do trocador põe o stack object no tile líder (a
parte 0,0, grupo de interação 1) e empurra dele a entrada 4 para o bebê
("Tree table not found (object 204)", `N002_Lot4`); o "Party Break-Up - Try
Push Turn Off Stereos" do policial falhava igual (`N003_Lot192`). Com isso
cada assento de sofá e cada parte do berço oferece as interações (o jogo
também abre o menu em qualquer assento); na autonomia cada objeto entra uma
vez pelo líder de interação.

**Entrada "." não é erro**: depois de achar o nome, `try_animate_sim` e
`try_animate_overlay` passam-no pelo vtable `0x380` (`0x68d934`: separa o
"(papel)", tipo 1 sem ele, e apara) e só então testam o comprimento: **só o
nome vazio é o 0x43**. Um "." é um nome sem animação: nada toca e a
primitiva volta verdadeiro ("missing animation tree"). O VM dava 0x43 também
para ".": o bebê no chão (o "Interaction - Drop - Sim" do carregar põe o bebê
no chão; há "a2b-soc-baby-putDown-onFloor-N") a quem pediam para sair do
caminho tocava a entrada 84 da lista do bebê, ".", e tinha o Main
reiniciado.

**Threads de objetos removidos**: o VM matava a thread de um objeto
removido "na próxima vez que rodasse", mas se o ID já tinha ido para um
objeto novo ela seguia como ele (o controlador do ônibus escolar do
`N003_Lot192` seguia no ônibus criado com o mesmo ID: "tree 0x1008 not
found" a cada tick). Agora cada objeto tem um número de série e a thread
guarda o de quem a começou (do VM, não do binário, que não reusa IDs assim).

**Limite de nós**: todos os comandos do `s2pkg` que rodam o VM usam os 100000
nós por turno do binário (`cli/s2pkg/budget.hpp`; antes 20000 fora do
`autonomy`).

### Rodada 17 (laços): fila cheia, a lista de objetos e as visitas

- **Push Interaction numa fila cheia** (`TryPushAction`, BV `0xe33530`): os
  limites da fila (mais de 6 → resultado 3) e da sub-fila (mais de 4 → 4)
  só valem quando a interação nova **não** é "continue as current" (byte 3
  bit 0x04; vtable 0x88/0x84 da interação, `SetContinuation`/
  `GetContinuation`). O "Pickup - Do Carry Sim" empurra o carregar para
  quem carrega (flags 0xA7) e para quem é carregado (0x67) assim. Com o
  limite para todos, a fila da criança cheia (no berço, com seis "*Push
  Look At" esperando atrás do "No Berço") fazia o push falhar; o "Do Carry
  Sim" volta falso depois de criar o objeto de carregar e de empurrar a
  entrada para o adulto, e o "Interaction - Give Toddler a Bath" volta ao
  "Get Sim Push Interaction" sem esperar (nós 1 → 5 → 4 → 3 → 6 → 7 → 2 →
  1): um objeto "Social - Baby/Toddler - Carry" por volta, a fila do adulto
  cheia depois de 6 voltas, o limite de nós e, com IDs passando de 32767,
  o crash do soak (N001_Lot36, sementes 3 e da rodada 16). Um objeto novo
  sem ID livre agora é erro do VM (a thread é reiniciada), não um ID que
  dá a volta.
- **A lista de objetos do Set to Next** (`TrySetToNext`, BV `0xe47288`): os
  tipos que andam pelos objetos (0 objeto, 1 pessoa, 2 não pessoa, 4/18
  GUID, 6/12 categoria, 16 veículo, 25 sala, 27, 37) usam a lista do
  `cEdithObjectModule` (vtable 0x7C do módulo): o objeto depois do atual
  nessa lista; um atual que não está nela (ou 0) começa da frente. **Não é
  a ordem dos IDs**: o ObjM do lote grava a lista (`write_pre_object`, os
  pares ID/save type nessa ordem) e no N001_Lot68 a parte (1,0) do som (42)
  vem antes do líder (14). O VM guarda a ordem (`Object::listOrder`,
  `VM::objectList`): o carregador põe cada objeto no lugar do ObjM, o
  `lot-save` grava nessa ordem, e um objeto feito em jogo vai para a
  **frente** (inferido: as listas salvas começam pelos IDs mais novos,
  260, 254, 250, 206...). O "Function - Goal Check" do fantasma
  (`0x7F38B8A7:0x205D`, nós 28 → 34 → 35 → 24 → 26 → 28) vai da parte ao
  líder (object data 19) e segue com Set to Next 6 (categoria 10): por ID
  achava a parte de novo e estourava o limite de nós.
- **Definições de todos os vizinhos** de novo em toda busca por GUID
  (`CodeLibrary::definitionByGuid` cai nos arquivos de personagem do
  bairro uma vez por GUID): Create/Test Object Type acham as visitas e os
  que passam a pé vêm aos lotes.
- **Bebidas do bar** ("Tomar um Drinque", `0x7F711E0D`): o "Interaction -
  Have a Drink" acha o copo da bandeja pelos slots ("Have a Drink - Pick
  Glass on Tray": 1, 0, 2, 3) e, se não é o copo que o Find Best
  Interaction escolheu, empurra "Have a Drink" para esse copo (Push
  Interaction 0x0F) e volta falso ("Find Tumbler to Pick Up"). Cada falha
  contada é seguida do drinque de verdade: os dados do jogo. Os copos da
  mesma bandeja empatam nos anúncios e a ordem do empate é a da tabela
  hash do jogo (não reproduzível; ver rodada 16).
- **`hood-save` e as famílias**: grava os membros mudados pela Generic
  Sims Call 4/6 (sai do FAMI antigo, entra no novo), o FAMI e o STR# de uma
  família feita em jogo (`MakeNewFamily`; o nome é o sobrenome do membro,
  inferido) e tira o FAMI de uma família removida (`RemoveFamily`,
  `VM::removedFamilies`).

### Rodada 18 (visitas e efeitos): tipo de pessoa, limpeza, eventos de overlay

- **Visitantes e a TTAB** (`cEdithObjectTestSim::TestInteraction`, BV
  `0x4c6214`): além do bit de idade, uma entrada só vale para um Sim com
  `IsVisitor` (`cTSPerson::IsVisitor`, `cITSPerson` vtable 0x78, thunk BV
  `0x19ace78`: **dado de pessoa 32 "Person Type" ≠ 0**) se tem a flag
  **0x0001** da TTAB ("allow visitors", `TestFlags(1)` = vtable 0x2c,
  `(flags & 1) == 1`). Vale no menu e na autonomia
  (`autonomy.cpp availableForAge`). Na geladeira, "Verificar Mantimentos" e
  "Fazer Uma Boquinha" não têm a flag; comer e cozinhar têm (as check trees
  decidem o resto).
- **Quem é visitante**: o tratador do evento "objeto criado" (BV
  `0x4700c8`, evento 0x8F) põe **Person Type 1** numa pessoa (OBJD tipo 2)
  criada com 0; as árvores que fazem um membro da família voltam a 0 ("Make
  Baby - Common", "Sim - Create New Sim - Generic", "Make New Character",
  "Make My Family"), as Inits de NPC põem 2 e o
  `cTSNeighborhood::UpdateInstanceVisitorTypes` (BV `0x4d7e94`) zera os
  membros da família. O VM faz isso no Create New Object Instance, antes da
  Init (inferido: o evento vem com a criação). Antes os que passam a pé
  chegavam com o 0 do próprio lote, eram "moradores": cozinhavam, usavam a
  geladeira e tudo mais e não iam embora (Dina Caliente 14 h no N001_Lot49,
  um jantar congelado que pegou fogo); agora passam ~0,5 h e as visitas no
  fim dos 69 lotes caíram de ~90 para ~10.
- **Objeto removido** (`cTSObject::Cleanup(objeto)`, BV `0xb09838`, chamado
  em cada pessoa pelo `cEdithObjectModule::CleanupPeople`): primeiro o tree
  sim desenrola os quadros do código do objeto (`kill.cpp unwind`); se não
  achou nenhum, o `cTSPerson::OnCleanup(objeto)` (BV `0xadcbcc`) **reinicia
  a pessoa** (o mesmo reset de depois de um erro, sem o erro:
  `VM::resetObject`) quando o objeto é o dela no dado de pessoa 80
  (interação atual) ou 157 (sub-fila) ou está nos registros de objetos dela
  (`+0x60c`, não modelados); e o roteador (`cTSRoutingObjectAggregate::
  OnCleanup` BV `0x5727c8`, `DoObjectDeletedWhileWalking` `0x56d880`)
  reinicia quem anda até ele. Antes: "stack object N does not exist" em
  quem reagia a um Sim que morreu e foi levado (N001_Lot16).
- **"CT - Route Blocked Animation"**: parâmetro 0 = pausas, 1 = bloqueador
  (`TriggerBlockedAnimation`, BV `0x579bc0`). Trocados, o Verify do
  parâmetro 1 falhava, a árvore voltava na hora e o Sim bloqueado tentava
  de novo no mesmo tick: o "Dispose" → "Route To Can" → "Dispose Trash In
  Hand" do lixo recorria até estourar a pilha (N001_Lot68).
- **Árvore de eventos de overlay** (Animate Overlay, Animate Object): roda
  **no objeto animado** (o tratador `nTSSG::cCustomEventHandler2`, BV
  `0x780ad4`, chama vtable 0x3ac do objeto do `cAnimatable` com o stack
  object do pedido; o pedido não guarda "eu": MovedObjectID 0 em todos os
  saves do jogo). O "Animate - Events - Get" do berço põe a criança (o
  objeto do overlay) nos braços de quem a tira no 9998; rodando como o
  adulto, nada ia para os braços e a criança ficava no berço ("No Berço" 16
  a 32 h no N001_Lot36).
- **Comparação do round trip**: o código da árvore de eventos de um overlay
  vai pelo GUID (é o que o pedido guarda); dois objetos do mesmo GUID são o
  mesmo código (N003_Lot192).
- **Dados do jogo, não bugs**: os incêndios que sobraram são moradores
  cozinhando (nenhuma visita nas amostras dos soaks); "Pisar" falha com 7
  (destino ocupado: o `EvalTileForGoal` recusa o tile da barata com outro Sim
  parado nele, ex. Loki depois de pisar, N002_Lot3).

### Rodada n31: visitas em laço de "Leave", sociais presos e o "autônomo" da fila

Lido nas árvores (`tools/datahunt`) e no binário (Ghidra MCP, só leitura).
Medida de partida (`s2pkg autonomy`, main `8bfebd80`, `S2PKG_VISITORS=1`):
`N003_Lot192` 12 h (21.600 ticks, semente padrão): Juliette e Tybalt começam
"Leave" 135 e 123 vezes das 23:33 em diante (cada um termina em 30–80 ticks
e recomeça) e não saem; Hérmia, Mercúcio e Romeu saem 23:51–00:32. 3 dias
(129.600 ticks): `N001_Lot68` semente 5, Daniel em "Beijar no Rosto" e
Angela em "Receber Beijo" 28,1 h (14:08 → 18:17 do dia seguinte), os dois
morrem de fome (18:38 e 19:20); `N001_Lot11` semente 1, Cassandra em
"Contar Piada" 25,4 h (Mortimer em "Ouvir Piada" 18,5 h), ela morre às 22:40
do 3º dia; `N003_Lot52` semente 5, Tybalt preso das 07:39 e morto às 01:23;
semente 6, Juliette morre às 03:11.

**1. O laço do "Leave" é rota.** O "Leave" global (`0x12F`) é um laço: acha
o "Controller - Visitor" (Set to Next por GUID; cria se não há) e, se o Sim
não roda a interação dele ("Sim - Running Interaction?" `0x31D`), empurra
em si mesmo a entrada 0 "Leave" (prioridade máxima, objeto no local 0) e
espera. "Interaction - Leave" (`0x1010` do controlador, grupo `0x7F92C86E`)
tenta "Leave - By Vehicle?" (`0x1015`) e "Leave - By Ped Portal" (`0x1018`:
Go To Routing Slot até o portal de pedestre, dois slots); falhando, vai a um
slot global (nó 10) e **acaba verdadeira**, e o "Leave" global a empurra de
novo. Sem caminho até o portal o jogo também giraria assim. Juliette e
Tybalt estão (desde o lote salvo) no deque do andar 2 (x 21–27, y 24–28),
cujas únicas descidas são as escadas modulares de 6 degraus (x 26/27 e
12/13, y 28 → 25, "Stair Modular - Balustrade - No Railings", borda de baixo
em y = 27,5). Com as escadas como portais de slot (`9cf74b11`,
`routing-mac.md` §8.6) as pontas de baixo das duas faixas ficam em
y = 27,71 (`OS2_OBJECT_PORTALS=1`: 568 slot 0 (27,67; 25,81; 2) → 600 slot 1
(27,67; 27,71; 1)), no tile da peça "End" (600, em (27,5; 27,5; 1), que
bloqueia) e das "Bridge" 593/598: o planejador não sai da ponta e o deque
não tem caminho para o andar 1 (`S2PKG_ROUTE_TRY` de qualquer ponto do deque
até o portal ou até (27,5; 28,5; 1): falha 2; `Leave - By Ped Portal`
falha com 2 nos dois slots). Com `OS2_STAIR_ONE_PORTAL=1` (o portal antigo,
patamar a patamar) os dois saem às 23:57 e 00:02 com um "Leave" cada (na
v0.2.0, 23:50 e 00:16). **Frente de rotas** (a rodada n30 mexe nelas; nada
mudou aqui): a pegada da "End"/"Bridge" sobre a ponta da faixa (ou a ponta
fora dela) como o binário a tem não foi lida. **Rodada n31b**
(`routing-mac.md` §9.4): a pegada da "End" acaba em 27,625; quem fechava a
ponta era o tile debaixo dos degraus, que o binário não fecha. Feito: os
dois saem às 23:58 e 00:10.

**2. Os sociais presos são o laço do bloqueio da locomoção.** Nos dois
casos o lado A está em "Route - Dynamic w/Lead in" (Go To Routing Slot até
B, com o goal check) e o B em "Wait - Person X State Test Y" (o estado de A,
atributo 6 do objeto de controle, fica em 2; B espera ≥ 4). A não anda: a
cada tick o passo dele entra num objeto (Daniel em (16,97; 19,93; 2), dentro
do tile do balcão 151 "Counter - Quaint" em (16,5; 19,5); Cassandra no
objeto 691). O VM vê o bloqueio e roda o goal check (o `DoBlocked` forçado,
`r.check = 3` em `routing_walk.cpp`); a árvore volta verdadeira e o
`advance` recomeça do alto: testa o passo de novo com o Sim parado (a
locomoção foi morta no bloqueio), esse passo não entra no balcão, o
bloqueio é esquecido (`blocker = 0`, `blockCheckDone = false`) e o andar
recomeça; no tick seguinte o passo entra no balcão de novo. O contador de
tentativas nunca sobe e a rota nunca falha. No binário,
`DoBlockedWaitingForGoalCheckTree` (BV `0x56da50`) vai do fim da árvore
**direto ao estado `0x1A`** (`DoBlockedGoalStillAvailable`, BV `0x57a8d4`)
com o bloqueador que o `DoBlocked` (BV `0x5762a4`) guardou, sem testar o
passo de novo; com um objeto é "blocked by object", a rota falha, o social
acaba e o B sai da espera. **Frente de rotas** (`routing_walk.cpp`; nada
mudou aqui). **Feito na rodada n31b** (`routing-mac.md` §9.4), com o limite
de um goal check por tick do agregado. O Daniel entrou no tile do balcão pelo lead-in do beijo
anterior, que o deixou em (16,99; 19,93), 0,01 dentro de x < 17.

**3. `N003_Lot52`.** Semente 5: o "Chutar" do Tybalt na urna 174 anda até o
centro dela (47,50; 41,50; 1) e de lá toda rota falha com 2 (101 vezes) até
ele morrer de fome: rota (um objetivo no centro de uma pegada). Semente 6:
Juliette com os motivos baixos leva um susto ("Assustar") e morre 15 min
depois: o "Sub - Be Scared - Die of Fright?" das urnas (do jogo, como a
Ofélia da rodada 22).

**4. O "autônomo" de uma interação (binário).** O `cTSInteraction` guarda
dois bits no `+0x54` (que o lote salva):
- **`0x400`, autônoma** (vtable `0xbc`/`0xc0`, BV `0x4c58b4`/`0x4c5894`):
  `SetCurrentAction` (BV `0xadc6d8`) a copia para o **dado de pessoa 166**;
  o `AddAction` de uma entrada imediata (BV `0xadbbdc`) põe o 166 e o bit da
  ação atual nos dela enquanto a árvore roda. Ligam: `try_gosub_action_found`
  (BV `0xade17c`) e o `TryPushAction` (BV `0xe33530`) com o byte 14 bit 0x02
  (a entrada do último Find Best Interaction); um push **feito por um Sim**
  sem o 0x02 herda o 0x400 da interação atual dele (`0xbc` → `0xc0`); fora
  isso 0 (`cTSInteraction::Init`, BV `0x4c7dec`, zera o `+0x54`). O VM dava
  166 = "prioridade 20" (inferido).
- **`0x800`, testar como autônoma** (vtable `0xc4`/`0xc8`, BV `0x4c5888`/
  `0x4c5868`): o `TryIdleForInput` (BV `0xade664`) monta o
  `cEdithObjectTestSim` com ele (a chamada em `0xadeb6c`), então a check tree
  de uma entrada da fila recebe **parâmetro 0 = 1** quando ela veio do Find
  Best Interaction. Só o gosub e o push com 0x02 o ligam. O teste do próprio
  `TryPushAction` usa o byte 14 bit 0x02 como "autônomo"; o modo "Handle Sub
  Queue Interactions" testa com 0. O VM passava 0 nos dois.
- Nos 69 lotes, das 100 interações salvas só 4 têm entrada; as quatro são de
  prioridade 20 e têm o `0x400` (`+0x54` = `0x400`, `0x480`, `0xC80`,
  `0xCA0`; as duas com `0x800` são "What's This?" e "Ask Tybalt to Join").

**5. Entrada recusada: `ActionSkipped` e `ClearSubActions` (binário).** O
`TryIdleForInput` tira a entrada da frente da fila, chama `ClearSubActions`
e, se o objeto dela existe, a testa (`TestInteraction` com a check tree
forçada); passou e tem action tree → `GosubObjectTree`, `SetCurrentAction`,
contador 0, devolve 3. Senão `ActionSkipped` (BV `0xaca0cc`: o
`UnLinkInteractions` da entrada, vtable `0x10c` = BV `0x4c6a54`, e o evento
4 no objeto dela) e **devolve 2**: espera um tick e a próxima entrada fica
para o tick seguinte. Objeto sumido: a entrada sai sem `ActionSkipped` e
devolve 2. O VM tirava a recusada **sem desfazer o vínculo** (o Sim do outro
lado de um social não recebia o dado 110 e sua "Wait - Person X State" não
acabava) e tentava a próxima no mesmo tick. `ClearSubActions` (BV `0xadc4f4`)
faz o `UnLinkInteractions` de cada sub-ação esperando antes de limpar a
sub-fila e zera a sub-ação atual e os dados 26, 75–78 e 157; o VM limpava
sem desfazer vínculos. No modo da sub-fila, a recusada leva `ClearSubActions`
e `ActionSkipped`.

**6. O que mais o `TestInteraction` (BV `0x4c6214`) testa** (lido; ver o
código abaixo para o que entrou): sai sem resultado se a entrada da interação
é < 0; zera "passou" (`0xa0`); pessoa, objeto ou entrada nulos → recusada;
zera os dados de objeto 50, 6, 101 e 104 **do Sim** (e no fim os copia para a
interação: 50 "escondida", 6, 101, 104); recusa se o bitset de idade da
pessoa (`0xd0`) não cruza o da entrada (`0x48`), se é visitante (`0x78`) e a
entrada não tem a flag 0x0001 ("allow visitors", `TestFlags(1)`), se o objeto
diz `0x214` ou `0x2e8(autônomo)` (não lidos), se um flag do sim system
(`0x23`) está ligado e a entrada tem `0x20000` (o flags2 bit 1); entrada de
depuração (TTAB `0x80`, vtable `0x9c`) é recusada fora do modo de depuração
(o `TryPushAction` passa o último argumento verdadeiro: lá ela é sempre
recusada). Passou tudo: sem check tree ou sem "forçar" → passou; senão roda a
check tree com {autônomo, join, argumento, 0}. O `TryPushAction` sempre chama
o `TestInteraction` (forçando a check tree só com o byte 3 bit 0x20) e falha
com resultado 2 quando ele recusa.

**7. A volta da interação.** Na nova execução do Idle for Input depois que a
interação que ele rodou volta (o `0x48` do elemento da pilha, o mesmo
"re-run" que o `0x4c(1)` liga), o binário só começa a próxima entrada se ela
é "continue as current" (`0x84`, `+0x54` bit `0x02`) e a que acabou (o
`+0x5d8` da pessoa) **não** tem o bit `0x4000` (vtable `0x8c`; o
`TryPushAction` o liga com o byte 14 bit `0x10` pela `0x90`, BV `0x4c51ac`;
nenhuma árvore do jogo base usa esse bit: só os bits 0x01, 0x02, 0x04 e 0x08
do byte 14 aparecem nos dados). O VM pedia "não cancelada" (inferido).

**O que mudou no código** (um commit por parte, `ctest -j2` verde, 57 testes):

1. Os bits `0x400`/`0x800` (`QueuedInteraction::autonomous`/`testAutonomous`,
   `vm.hpp`): o Gosub Found Action e o Push Interaction com o byte 14 bit 0x02
   ligam os dois; o push de um Sim sem ele herda o `0x400` da interação atual
   dele; o dado de pessoa 166 é o `0x400` da interação atual (`syncQueueData`)
   e o da imediata enquanto ela roda (`runImmediately`); o teste do Idle for
   Input passa o `0x800` como parâmetro 0, o teste forçado do Push o byte 14
   bit 0x02; salvos e lidos no `+0x54`. Teste: `test_queue` (uma entrada cuja
   check tree só aceita a autonomia roda quando o Find Best Interaction a
   empurrou e é recusada quando um objeto a empurrou; o 166 segue o bit).
2. `ActionSkipped` e `ClearSubActions`: a entrada recusada sai com os vínculos
   desfeitos e o Idle for Input espera um tick (`PushOutcome::Skipped` →
   `Exit::Yield`); `clearSubActions(vm, sim)` desfaz os vínculos da sub-fila;
   a sub-ação nova desfaz os da que ela substitui. Teste: a recusada ligada à
   interação de outro Sim lhe dá o dado 110, a próxima roda no tick seguinte.
3. Os testes do `TestInteraction` antes da check tree (`entryAvailable`:
   depuração, idade, visitante) no Push Interaction (sempre, resultado 2, depois
   do erro 0x6F e antes dos limites da fila), no Idle for Input, no modo da
   sub-fila e na imediata. Num dia de seis lotes isso recusou 8 pushes (sete no
   `N003_Lot192`, um banho no `N001_Lot36`); nenhum empurra entrada de
   depuração. Teste: depuração nunca entra, visitante só com "allow visitors".
4. A volta da interação pelo bit `0x4000` (`noContinuation`), salvo no
   `+0x54`. Teste: a continuação roda na volta antes do Main seguir, e depois
   dele com o bit.

**Medidas** (`s2pkg autonomy`, `S2PKG_VISITORS=1 S2PKG_FIRES=1`, 3 dias; antes =
main `309a28c3`, já com as rotas da rodada n30; depois = estes commits por
cima). Nada disso mexe nas causas (rotas), e as medidas mostram isso:

| | antes | depois |
|---|---|---|
| `N003_Lot192` 12 h: "Leave" começados por Juliette / Tybalt | 122 / 120, não saem | 117 / 112, não saem (com `OS2_STAIR_ONE_PORTAL=1`: saem 23:52 / 00:13) |
| `N001_Lot11` sementes 1–6: mortes sem fogo; social ≥ 10 h | 1; "Contar Piada" 25 h+ (s3) | 0; nenhum |
| `N001_Lot68` sementes 1–6: mortes sem fogo / no fogo; social ≥ 10 h | 3 / 4; "Estapear" 61 h+ (s2), "Tapa na Mão" 19 h (s5) | 0 / 4; "Estapear" 61 h+ (s2) |
| `N003_Lot52` sementes 1–12: mortes sem fogo / no fogo (sementes com fogo); social ≥ 10 h | 5 / 17 (5); 3 (21–22 h) | 6 / 20 (6); 4 (18–23 h) |

Os fogos (a comida esquecida no forno, cozinhar com pouca habilidade) mudam
de semente quando a ordem dos eventos muda. Os sociais presos continuam o
laço do bloqueio da locomoção (item 2; conferido depois da n30 no
`N001_Lot68` semente 2: "step into 639" a cada tick e o goal check a cada
tick, sem contar tentativa). O cercado das urnas do `N003_Lot52` (x 44–55,
y 39–45) prende Sims à noite: as rotas de lá falham com 2 (o planejador sem
os Sims, `S2PKG_ROUTE_TRY`, acha caminho; a causa dinâmica não foi lida).
Antes da n30 (main `8bfebd80`) era o mesmo quadro: Lot192 135/123, os
sociais presos de 23–44 h no Lot68 (s2, s5) e no Lot11 (s1, s2).

## 4h. Rodada 15 (front "vm-confirm"): o que era inferido na VM

Tudo lido no binário (Ghidra headless, um clone com os nomes do
`symxfer`); BV salvo indicação, e o base 1.0 conferido onde dito.

### Donos de dados (`cTSObject::InterpValue`, BV `0xb15f2c`; base 1.0 `0xb2d890`)

Um `switch` só, dono -1..0x33 (fora disso: **erro 0x09**). A referência
devolvida (`cTSSint16Ref`: ponteiro, contagem, índice, mín./máx.) serve
para ler e escrever; o `cTSPerson::InterpValue` (BV `0xad56ec`) trata
antes os donos 0x0C, 0x12, 0x1C, 0x1D e 0x1E.

- **Temps: 8 por objeto** (`cTSObject +0x8A`, 8 × i16, zerados no `Reset`
  e no construtor). Dono 8 com índice ≥ 8 → **erro 0x02** ("Temporary
  reference out of range"); 0x0B, 0x0D, 0x1E, 0x1F, 0x30, 0x31 também
  leem só os temps 0–7. Base e BV iguais. Nenhum BHAV do jogo usa temp ≥ 8
  (`objects.package`, todas as Expressions). O opensims2 tinha 20.
- **Atributos**: `+0x7C`/`+0x80` (contagem). O construtor (`0xb0c7a8`) usa
  o campo **0x3A** do OBJD ("num attributes", byte 0xB4) **mas no mínimo
  8**; índice fora → **erro 0x06**. Não crescem sob demanda. O
  `cTSObject::Read` (`0xb14cb0`) troca a contagem pela salva.
- **Semi-atributos**: `+0x84`/`+0x88`, contagem = nº de entradas do STR#
  0x100 do semi-global (ao ler um estado salvo, cresce até a contagem
  salva, nunca diminui; o object data 79 "SemiAttribute Count" é ela).
  Fora → erro 0x06.
- **Arrays de objeto**: tantos quanto o campo **0x3B** do OBJD (no máximo
  256), vazios; o número do array fora → **erro 0x6C** ("Bad Object
  Array"); elemento fora (iterador ou temp 0, donos 0x2B–0x2E) → **erro
  0x6D**. O `Read` mantém a contagem da definição (arrays salvos a mais
  são lidos e descartados).
- Object data: 0x71 (113) no BV, **0x58 (88) no base**; globais 0x3A no BV,
  **0x36 (54) no base**; dados de pessoa 0xCC no BV, **0xAA (170) no base**.
  Fora: erros 0x07 / 0x3F / 0x21. O port usa os **do AL** (18/10): as árvores
  dos EPs leem além dos do base **e** além dos do BV — com 88/54/170 o
  `bhav-sweep` do AL acusava 460 globais, 1.897 de person data (1.882 + os 15
  de vizinho) e 264 de object data fora do intervalo; subindo os tamanhos até
  a varredura zerar, os **maiores acessos** das árvores do AL são object data
  **113**, global **59** e person data **230** → arrays de **114 / 60 / 231**
  (`kObjectDataCount` / `kGlobalCount` / `kPersonDataCount`), e o sweep passa a
  rodar 46.880 árvores / 13.054.928 instruções (+14,7%) sem nenhum deles. O
  `OpenTS2` dimensiona object data e globais em 114 e 60 também; o limite
  compilado do AL é ≥ o medido (aqui não há binário do AL para ler).
- **Job data (0x21)**: sempre **erro 0x3A** ("Job data not found"), base e
  BV: o jogo não tem esse dono.
- **Check tree ad (0x0C, 0x1C, 0x1D)**: só pessoa (senão erro 0x1C),
  índice < 16 (senão 0x1A); os valores vêm da tabela de anúncios do
  objeto da pessoa para a idade dela (`+0x48` → vtable 0xFC, 0xD4/0xD8/
  0xDC com `GetProxySimAge`); a personalidade (0x1C) é limitada a 0..22
  (escrever fora: erro 0x14).
- **Temps do stack object (0x0D)**: os do objeto (`+0x8A` do stack
  object), 0–7. Confirmado o que o TODO dizia.
- **Slots (0x10, 0x14, 0x30, 0x31)**: `GetObjectSlot(i)` (vtable 0x10C,
  `0xb02204`) dá o slot se 0 ≤ i < `NumObjectSlots` (os slots de contenção
  do SLOT, que o object data 67 guarda: `InitSlotsFromDefinition`
  `0xb05a28` grava `NumObjectSlots` no `+0x120` = object data 67 e
  `NumObjectTargetSlots` no `+0x108` = object data 55), senão **erro 0x1B**
  ("Slot number out of range"); slot vazio = 0 (o slot existe e não tem
  objeto). Antes (FreeSO) passar do último dava 0.
- **Constantes (0x1A)**: `cEdithLanguage` vtable 0x2C (`0x4b2704`): bits
  13–14 = escopo (0 privado 0x1000, 1 semi-global 0x2000, 2 global 0x100,
  3 → erro 0x2E), tabela = bits 7–12 | (bit 15 → 0x40), entrada = bits
  0–6. **Constante [temp] (0x2F)**: vtable 0x34 (`0x4b2764`): escopo nos
  bits 13–15, tabela = bits 3–12, **temp = bits 0–2** (confirmado). A
  tabela é procurada pelo `cTSBehavior` do elemento da pilha
  (`GetConstants`, `0x489ed4`: grupo pela faixa da instância), o do **dono
  do código**, também em código semi-global/global (confirmado). Tabela ou
  entrada que falta → erro 0x2E ("Invalid constant."). Nenhuma Expression
  do base usa o bit 15 ou o escopo 3.
- **Função do stack object (0x23)**: a tabela de funções do objeto
  (`GetFnTable`, vtable 0x168) → vtable 0x20 (entrada): sem stack object,
  erro 0x0A.
- Sala [temp 0] (0x17), valor 3 = `nTSWorld::RoomArea` (`0x1671bc`):
  quartos de tile (máscara 0xF = 4, senão os bits) / 4 — confirmado.

### Expression (`TryExpression`, BV `0xe4bb64`)

Operador = byte 5 (> 21 → erro 0x01); o byte 4 **não é lido**
(confirmado). O 21 ("Assign 32bit Value") copia o elemento e o seguinte das
duas referências (sem o seguinte: erro 0x78) — o opensims2 já fazia.

### Array Operation (`TryArrayOperation`, BV `0xe2a158`)

Operação u8 no byte 1; byte 2 ≠ 0 = arrays do stack object (sem ele: erro
0x08); array u16 no byte 3 (fora: 0x6C); valor (dono byte 5, u16 6) e
índice (dono byte 8, u16 9). 0 limpa; 1 tamanho no valor (referência que
não se escreve: 0x13; fora do mín./máx.: 0x14); 2 redimensiona (novos = 0);
3 preenche; 4/5 insere o valor no começo/fim; 6 insere **antes do índice**
(índice fora dos elementos: 0x6D); 7/8 tira o primeiro/último (vazio:
0x6D); 9 tira no índice (fora: 0x6D); 10 procura o valor a partir do índice
(índice fora ou ausente: **falso**; achado: posição no índice); 11 troca os
elementos no valor e no índice (fora: 0x6D); 12 ordena decrescente; **13
ordena crescente**; outra: 0x01. **Inserir e tirar não escrevem nada de
volta** (o opensims2 escrevia o índice no 2º operando e o valor tirado no
1º, o que estragava temps das árvores).

### Generic Sims Call (`TryGenericSimCall`, BV `0xe36834`)

- Chamada sem vizinho que precisa dele (4, 6, 21–23, …): **erro 0x37**
  (o fim do `switch`).
- **4 "add to family"**: vizinho no stack object; família = temp 0 (0 = a
  do lote). Família que o bairro não tem é feita por
  `cTSFamilyManager::MakeNewFamily` (`0x4ffd28`): abaixo de 0x7FD9 com o
  **menor número livre a partir de 1** (não "maior + 1"), a partir de
  0x7FD9 com o número dado. Temp 1 ≠ 0 marca a família nova (vtable 0xE8,
  não entendido) e ela recebe o sobrenome do Sim. Na primeira passada o
  estado da primitiva guarda a família; se o Sim era o último membro de
  uma família com casa, o jogo manda a mensagem `0xec1aac03` (mudar a
  família do lote) e espera (devolve 2) até a casa sair; temp 0 := os
  fundos da família antiga nesse caso, senão 0. Depois:
  `RemoveFromFamily` (`0x4fe800`; vizinho que a família não lista →
  falso) e `AddToFamily` (`0x4fddcc`): `AddMember`, dado de pessoa 61 :=
  família e 67 := 0 no vizinho e na pessoa do lote, `AddMemberClothing`,
  e entre duas famílias reais (0 < n < 0x7FDA) as peças do bin antigo que
  servem (três máscaras do recurso 0xAC598EAC do personagem contra as da
  peça; sem ninguém na antiga, todas); a família antiga vazia e real é
  apagada (`RemoveFamily`). **Não há limite de 8 Sims** (nem no
  `AddMember`). No opensims2: `vm::addToFamily`, com o bin pelos ganchos
  `VM::memberClothing`/`clothingFits` que o app liga ao catálogo do CAS
  (idade e gênero como as máscaras, a terceira inferida "passa"); a espera
  pela mudança feita na rodada 16 (abaixo).
- 6 "remove from family": sem família (61 = 0) ou família que o bairro não
  tem → falso; senão `RemoveFromFamily` e verdadeiro.
- 13 aborta as interações associadas ao stack object no gerenciador de
  objetos (todas as filas; confirmado); 18 velocidade: > 1 passa o número
  como está, 1 despausa, ≤ 0 pausa (sem erro de faixa; espera se o estado
  do jogo é 10).
- 21/22/23 (árvore genealógica): `cTSFamilyRelationshipArray`
  `AddRelationship(temp 0, temp 1, 4)` / `(…, 2)` /
  `RemoveRelationship(temp 0, temp 1)` (`0x5442e8`, `0x544028`). Adicionar
  **limpa antes os laços entre os dois**; tipo 4 (filho): o pai vira pai 0
  ou 1 do filho (com dois, nada) e o filho vira **irmão** (tipo 3) dos
  outros filhos desse pai; tipo 2 (cônjuge): cada um perde o cônjuge
  anterior. Remover tira todos os laços entre os dois (confirmado). **Os
  irmãos são laços próprios** (tipo 3, também lidos do bairro), não "um
  pai em comum".
- **As chamadas 54–84 (0x36–0x54) do AL.** A tabela do editor vai até 53 (a
  do jogo base, 54 chamadas), mas o conteúdo do AL chama **até 0x54**: o
  censo do `prim-usage` no AL dá 71 valores no byte 0 e **~232 instruções
  acima de 53** (0x4E ×57, 0x3E ×34, 0x36 ×18, 0x51 ×11, 0x48 ×11,
  0x38/0x37 ×10, 0x43 ×9, 0x4F/0x47/0x45/0x35 ×7, …, 0x54 ×2). A que dói é
  a **60**: o `Function - Main - Set Skin` do semi-global do Sim
  (`0x7F01EC29:0x22A4`, instrução 2) chama 60, e com o erro 0x2A do port o
  **Main de todo Sim do lote reiniciava** — e cada reinício reabria o pedido de
  look_at, que estourava o limite por turno: no `autonomy N001_Lot11 --ticks
  24000`, com a instalação inteira (como o app), **5.914 resets** do Mortimer, o
  Main em `Idle #0` e o dos outros dois parado no "Motive Failure - Pass Out -
  Loop". O PC não tem rótulo para 54–84 (os rótulos ficam no Mac, que para na
  tabela do base), então o port os trata como as chamadas de motor do base:
  **registra em `VM::engineRequests` e devolve verdadeiro** (o limite do erro
  0x2A sobe para 0x54). Medido no mesmo lote: **15 resets** do Mortimer (9 e 62
  dos outros, por outras falhas do AL: orçamento de instruções, break point,
  atributo), o Main em "Routing - Goto Head of Bed" e as buscas em
  **17/94/25** (antes 58/109/38, com os Sims travados). A alternativa (devolver
  **falso** sem registrar) foi medida e é muito pior: o mesmo lote não terminou
  24.000 ticks em 30 min, com 32 "too many look_at requests". No `bhav-sweep` do
  AL as **3.982** falhas "Invalid global sim call" somem: 14.025.434 / 19.927
  true / 10.335 falhas → **14.093.824 / 23.864 true / 6.385 falhas**.

### Set to Next (`TrySetToNext`, BV `0xe47288`)

- 21 pai: os pais 0 e 1 do vizinho do temp 0 (sem ele: erro 0x77); 22
  irmão e 23 filho: as listas de tipo 3 e de filhos, na ordem da lista
  (aqui por ID: inferido igual); o atual fora da lista recomeça; sem
  achar, alvo 0.
- 28 (relação 3 ≠ 0) e 35: a tabela de relacionamentos **do vizinho do
  temp 0** com cada vizinho (direção temp 0 → vizinho; o opensims2 já lia
  assim, o TODO dizia o contrário).
- **24 (objeto visível)**: `cTSPerson::GetNextObjectInView` (`0xacfa00`;
  não Sim: erro 0x1C). Distância r = temp 0, no máximo 16. Um cone de
  tiles à frente: linhas 0 a r − 1 (a 0 é o tile do Sim), cada uma com
  2·linha + 1 tiles para cada lado (no máximo r), dentro de um círculo de
  raio r + 1; virado para uma diagonal, a grade gira 45° (r·√2 + 1 linhas
  em passos de meio tile diagonal, 0,7071 × 0,7071). Nesses tiles: objetos
  no mundo (fora de slots), não eu, não do tipo 5, **na minha sala**, sem a
  flag 0x10000 e com o teste de linha do mundo livre (vtable 0x4C da
  `WorldDB`). Lista por ID, guardada enquanto se itera; um atual que não
  está nela termina a busca (0, falso). Era o "cone de 90°" inferido.
- **19/20 (o próximo/anterior do grupo multi-tile)**: a âncora é
  `GetObjectByID(temp 0)` (nula: alvo 0, falso; o 19 também recusa a âncora
  morrendo, o 20 não); a chave é o grupo da âncora (`+0xe4`, negativa para o
  objeto fora de grupo: falso sem tocar no alvo) e a lista vem do módulo de
  objetos (`+0x14c`) — um grupo multi-tile (o dado de objeto 19). Com alvo 0 o
  19 pega o primeiro objeto válido da lista e o 20 o último; com alvo != 0 o
  alvo tem de estar na lista (fora: verdadeiro, alvo intacto), é zerado e a
  busca continua no seguinte (19) ou em **i−2** (20: `0x939f04`/`0x939f12`,
  pula o objeto imediatamente anterior — um off-by-one do jogo). Lista nula ou
  vazia: falso sem tocar no alvo. Os únicos usuários no base e no AL são as
  árvores `0x7FFD4F2D:2006` ("Set to Next") e `:2007` ("Set to Prev"), do grupo
  da lareira; o RE do PC está no `re-pc-ghidra.md` §(e).

### Manage Inventory (`TryInventoryAction`, BV `0xe26a40`)

O BV converte os operandos da versão 0 do base
(`ManageInventoryParam::UpgradeVersion`, `0xac3f00`): byte 0 bits 0–1 =
tipo (global, lote, família, vizinho; o BV tem um 5º, 4), **0x04 =
contados** (vira 0x08), bits 3–5 (viram 4–6), 0x80 = busca de trás para
frente (nenhum BHAV do base). Byte 9: nos singulares o BV o põe em **0x0C**;
nos contados é a **categoria**. Inventário inexistente → erro 0x57. Índices
a partir de 1 (o operando − 1).

**Flags de busca (rodada 16)**: `FUN_01676420` dá o valor = bits 4–5 do
byte 0 convertido (= **bits 3–4 do byte 0 do base**) | (byte 9 & 3) << 3;
`FUN_016764b4` dá a máscara = (byte 9 >> 2) & 0x1F. Com o 0x0C dos
singulares: máscara 0x03, valor = bits 3–4 do base. As flags do token
(`cTSInventoryToken +0xC`, `CatManageInventoryMatchFlags`): 0x01 visível,
0x02 memória, **0x04 marcado para apagar**, 0x08 compras. Um token casa
(`MatchesGuidAndFlags` `0x16644d0`, `FindNextSingularTokenOfGUID`
`0x494038`) quando o GUID é o pedido, não tem 0x04, `flags & máscara ==
valor & máscara` e o "instance ID" (`+0x1C`, dados de instância do BV) é 0.
No base: 0x03 (vizinho, valor 0: tokens sem visível e sem memória), 0x1B
(visível + memória, as memórias), 0x13 (só memória, os marcadores).

**GUID 0 = o do stack object** (sem ele: erro 0x15) nas singulares 0, 2 e 3
e nas contadas 0, 2, 4, 6 e 7 — agora feito, junto com as flags.

Singulares (`cTSInventory`, vtable no `0x27c0f98`):
- 0 `AddSingularToken(guid, valor, 0)`: o token novo recebe o **valor como
  flags**; falso a partir de 0x7FFF tokens.
- 1 `RemoveSingularTokenAtIndex`: falso fora do intervalo.
- 2 `RemoveAllSingularTokenOfGUID(guid, valor, máscara, 0)`: **falso se
  nada saiu**.
- 3 `FindNextSingularTokenOfGUID(guid, início, valor, máscara, de trás)`: o
  início é o operando 1 (o índice achado antes; 0 = do começo); de trás:
  a partir do anterior a ele (0 = do fim); índice achado (+1) no operando 1
  (que não se escreve: erro 0x13); falso se nenhum.
- 4/5 empilha/desempilha propriedade do token no índice (falso fora; 5 vazio
  falso).
- 6 lê o token no índice para o token temporário (índice **a partir de 0**
  no campo 3, como o jogo guarda em `+0x1A6` e o save grava).
- 7/8 propriedade (a partir de 1) do token temporário; falso fora.
- 9 grava o token temporário onde foi lido; com o temporário lido de
  contados, roda a **9 contada** (o tipo vem do temporário).
- 10 contagem; 11 cópia do temporário (com as flags dele); 12–19 só BV (a 12
  busca qualquer GUID, a 13 conta com flags, 14/15 propriedades por índice,
  16–19 objetos de tokens). Outras: falso.

Contados (o token guarda **contagem e categoria nas propriedades 0 e 1**,
`AddCountedToken(guid, categoria, flags, n)` `0x496ac4`; o opensims2 agora
também):
- 0 soma n à contagem do GUID+categoria (novo: flags = bits 3–5 do byte 0 do
  base, propriedades [n, categoria]; máximo 0x7FFF); falso se n ≤ 0.
- 1 o mesmo para o token no índice (verdadeiro se existe).
- 2 `SubtractCountedToken(guid, categoria, n)`: some com 0; falso se não há
  ou n ≤ 0. 3 o mesmo pelo índice.
- 4 `RemoveCountedToken(guid, categoria)`; 5 tira no índice (índice < 1:
  **erro 0x74**).
- 6 procura GUID+categoria: contagem no operando 2, índice no 1.
- 7 lê GUID+categoria para o temporário; 8 lê no índice.
- 9 `FindNextCountedTokenOfCategory(categoria, operando 1 − 1)`: índice no
  operando 1, GUID nos operandos 2 e 2+1 (sem os dois: 0x78).
- 10 contagem; 11 copia o token no índice para o inventário do tipo byte 6 &
  7 (ID no operando 2) somando a contagem lá, se byte 6 & 0x08 (vtable 0xC4,
  `0x497eb4`). Outras: falso.

Soak semeado (N001_Lot68, N002_Lot10, N001_Lot16, N001_Lot11, sementes 1–8,
43200 ticks) com só esta mudança: interações verdadeiras 2926 → 2920,
falsas 676 → 635, mortes 9 → 9 (antes, o GUID 0 sem as flags derrubava o
N001_Lot68 de 137 para 45).

### Rodada 16: chamadas, limites, atributos de tipo, Look At e Generic Sims Call 4

- **Argumentos de chamada** (`cTSTreeSim::DoNodeAction` `0xe1b18c`, `Gosub`
  `0xe1a428`, `cTSTreeStackElem::SetParams` `0xe1759c`): a árvore chamada
  recebe tantos argumentos quanto declara, **até 8**: literais = os u16 nos
  bytes 0, 2, …, 14 (os operandos são o próprio vetor: o 5º ao 8º vêm dos
  bytes 8–15); bytes 0–7 todos 0xFF = os temps (lista nula: o Gosub usa os
  temps do tree sim); byte 12 bit 0 = pares (dono, dado) em 0, 3, 6, 9, só
  para árvores de 1–8 argumentos (de 4 a 8 recebem 4; o resto 0;
  `DAT_0273b4f8` = zeros); byte 12 bit 1 ("meus parâmetros") não existe na
  versão 0. Árvore que não existe: erro 0x11; árvore sem nós: o Gosub
  recusa e o `DoNodeAction` lança **1000 "Stack overflow"** (era nosso
  "tree has no instructions"); pilha de no máximo **1000** quadros
  (`DAT_0277ea90`, 0x452), não 128; **100000 nós por tick** por tree sim
  (`DAT_0277ea94`; 300000 com uma flag 0x33 do simulador, não entendida;
  erro 0x44D).
- **Run Tree by Name** (`TryCallNamedTree` `0xb06244`): sem 0x10/0x20 os
  parâmetros são uma lista nula, ou seja, **os temps do tree sim onde a
  árvore roda**: no modo 1 (no stack object), os temps dele. 'Call "Look
  At" For All' do controle de conversa põe temps 0–2 do Sim (alvo, duração,
  velocidade) e roda "CT - Look At" nele; com os temps de quem chamava a
  velocidade era 0. 0x20 copia até 4 parâmetros meus; 0x10 três pares (o 4º
  = 0).
- **Atributos de tipo** (donos 0x24/0x25): `cTSObject::GetSelector` →
  `cTSObjectSelector::CountTypeAttributes`/`GetTypeAttributes`
  (`0x51e5e8`/`0x51e6c8`): no gerenciador de atributos de tipo pela chave =
  campo 0xCD (= campo 29 do editor, o **proxy GUID**) ou, sem ele, 0xCA (14,
  o GUID); o gerenciador tem uma entrada para cada objeto de catálogo com o
  campo 90 ≠ 0, com tantos atributos (`PopulateTypeAttributesFromSelectors`
  `0x4e4b0c`). Os campos ≥ 0xC9 são os 10 campos de 32 bits do OBJD
  (`cEdithObjectDefinition::GetFieldAddress` `0x58d0c0`: 0xC9 versão, 0xCA
  GUID, 0xCD proxy GUID, …). Índice fora: erro 0x40 (sem texto na tabela do
  jogo). Nenhuma Expression do base usa 0x24/0x25.
- **Look At** (`nTSSG::cAnimatable::try_look_at` `0x6a19f8`): só Sim (0x42),
  no máximo 25 pedidos por Sim por turno (0x95); 0x80 câmera, 0x01 STOP,
  senão o alvo (inexistente: **erro 0x17**; sem modelo: 0x4B); velocidades
  byte 4 × 2 (temp 1 com 0x08) e byte 5 × 2 (temp 1/2 com bits 0x02/0x01 do
  byte 15) em °/s precisam ser **> 0** (erro 0x51, também no STOP; o valor
  do temp vai como inteiro sem sinal, então só 0 falha); STOP para os
  olhares e é verdadeiro; sem 0x20 um alvo em **outro cômodo** é falso; sem
  0x40 o jogo testa se a cabeça alcança o ponto (um teste no esqueleto, por
  espécie; não feito); slot byte 8 do tipo byte 14 (0 = 3/4 da altura do
  alvo, 1 alvo, 2 rota, 3 contenção; fora: 0x1B); duração s16 6 ou temp 0
  (0x10), −1 = um valor padrão (tunable); a árvore de evento é o u16 9. O
  pedido roda sozinho: a primitiva é verdadeira na hora.
- **Generic Sims Call 4**: o jogo de fato espera (`TryGenericSimCall`
  `0xe36834`): na 1ª passada guarda a família no estado da primitiva
  (`cITSTreeStackElem` vtable 0x4C); se o Sim é o último membro de uma
  família com casa e o lote existe, manda `0xEC1AAC03` (o
  `cTSGameStateController::EvictFamily`: devolve o preço do lote, salvo tipos
  2/3; lote da família 0; flag 8 no lote) e devolve 2; nas passadas
  seguintes espera a família ficar sem casa e então temp 0 := os fundos.
  Feito (`VM::evictFamily`, ligado pelo `hood_bridge`; o `hood-save` grava o
  lote 0 da família e a flag 8 do lote).
- **Vizinhos sem definição**: o jogo tem a definição de todo vizinho (os
  arquivos de personagem). O opensims2 só carregava as dos Sims do lote:
  dono 0x26 ("neighbor N has no definition") e Test Object Type falhavam e
  as visitas não vinham. Agora um GUID sem OBJD é procurado uma vez entre
  os arquivos de personagem do bairro (`vm::useCharacterFiles`). Rodada 17: de novo em toda busca por GUID (o laço do banho no N001_Lot36 era a fila cheia do Push Interaction; ver §4g "Rodada 17").

### Motivos do PERS

Os dois conjuntos de 16 f32 são `GetMotiveRef` (vtable 0x5CC, os
**atuais**, que o motor simula) e `GetOldMotiveRef` (0x5D0, os do passo
anterior) do motor de motivos da pessoa (`+0x83C`). O opensims2 usa o
primeiro: confirmado.

### Primitivas

Todas as primitivas com handler no base (§2) estão registradas na VM;
o `bhav-sweep` não acha nenhuma faltando.

## 4i. Rodada n04: o que o motor mantém (sala, load, roomChanged, wallAdjacency)

Lido no BV com o Ghidra MCP (só leitura). Os slots de vtable foram
resolvidos com `tools/symxfer/vtables.py` (classe `9cTSObject`).

### A sala do objeto (object data 29, `cTSObject +0xD4`)

O object data começa em `cTSObject +0x9A` (logo depois dos 8 temps em
`+0x8A`): a sala (od 29) é o `+0xD4` (`GetRoomID`, BV `0xaf56a8`: `lha
r3,0xd4(r3)`), as wall adjacency flags (od 5) o `+0xA4`, a direção (od 1) o
`+0x9C`.

- **`SetLocation(FTilePt, int, long)`** (BV `0xaf5ba8`, vtable `0x304`):
  `BeginEditing`; guarda o ponto; `SetLevel`; a sala é a do **gerenciador de
  partições, definição 2** no ponto (`WorldDB +0x94` → vtable `0x13c`
  (local, **2**, 1)), isto é o mapa do 3ARY `0x09` (paredes que bloqueiam
  colocação; `docs/buy-build-mac.md` §7.3); um segundo número (a "sala de
  envscore", `+0x288`) vem de outro serviço (vtable `0x24`, não lido);
  `SetRoom(sala, envscore, false)`; depois cada objeto nos slots recebe o
  mesmo `SetLocation`; `FinishEditing`.
- **`SetRoom(short, long, bool)`** (BV `0xaf48c8`, vtable `0x98`): se a sala
  ou o envscore mudou, grava os dois; com o bool, repassa a cada objeto
  contido (`NumObjectSlots`/`GetContainedObject`, vtables `0x104`/`0x110`).
  Não chama nenhuma árvore.
- **`BeginEditing`/`FinishEditing`** (BV `0xaf54ec`/`0xaf55bc`): um contador
  em `+0x78`, propagado aos contidos. Quando volta a 0,
  **`OnFinishEditing`** (BV `0xb01a44`) chama **`OnRoomChanged`** se a sala
  (`+0xD4`) difere da última avisada (`+0x1CC`), e `OnEnvScoreRoomChanged`
  se o envscore mudou (`+0x28C` ≠ `+0x288`). Toda mudança de lugar
  (`SetPosition`, `SetLocation`, `PostLoad`) passa por aqui.
- O construtor (`FUN_00b0c7a8`) põe **−1** na última sala avisada
  (`+0x1CC`), no envscore (`+0x288`, `+0x28C`) e no cache de salas do portal
  (`+0x2B4`/`+0x2B6`). Nada disso é salvo: todo objeto novo ou carregado é
  avisado na primeira vez que ganha um lugar.

### `OnRoomChanged` (BV `0xb0093c`, vtable `0x334`): a entrada 7 do OBJf

1. Se a sala não mudou desde o último aviso, só continua para uma porta
   (`GetType` = 8) diagonal (`PortalDiagonalWallPlacement`, flags de parede
   `& 0x50`); senão volta.
2. Guarda a sala em `+0x1CC`.
3. Objeto que **não é portal** (`IsPortal`, vtable `0x374`) e tem a entrada
   7 (`GetTreeID(7)`): `RunTree(7, eu, {sala, −1, 0, 0})` (o stack object é o
   próprio objeto, `+0x1BC` = o ID; `RunTree` BV `0xafe524` roda a árvore da
   tabela de funções até o fim, na hora).
4. **Portal**: `PortalOnRoomChanged` (BV `0xe1eeec`): com a entrada 7 e
   `PortalGetRooms` (BV `0xe22498`) verdadeiro, `RunTree(7, eu, {sala A,
   sala B, 0, 0})`. `PortalGetRooms` só é verdadeiro quando o par mudou desde
   a última vez (cache `+0x2B4`/`+0x2B6`). `PortalCalculateRooms` (BV
   `0xe221e4`) anda pelos pares de slots do portal, acha a sala (definição 2)
   no ponto de cada slot (`PortalGetLocationOfSlot`) e devolve a primeira
   sala achada e a primeira diferente dela (se todas são iguais, A = B; menos
   de dois pontos com sala = falso).
5. `AggregateOnRoomChanged` (agregados; nada da VM).

Nos dados do jogo base a entrada 7 está em 140 OBJf: portas e portões (o
"Function - Room Changed - SG" das portas, semi-global `0x7FD0DEBA`, só faz
**atributo 3 := param 0, atributo 4 := param 1**: as salas dos dois lados),
o barquinho da caixa de brinquedos e o objeto de teste. Os Sims não têm.

### `UpdateObjectsInRoom` (BV `0x52e170`): quando o mapa de salas muda

O `cEdithObjectModule::DoMessage` (BV `0x534dec`) recebe a `0x2B4D18F6` (o
`FlushRoomChangedNotifications` da definição 2, uma por sala mudada) e chama
`UpdateObjectsInRoom(sala)`: para cada tile da sala (`FloorTilesInRoom`,
definição 2) e cada objeto no tile, `SetRoom(sala, envscore, true)` e
`OnRoomChanged`. As mensagens de parede (`0x0B4CF313`, `0x2B4CF319`) vão
para `UpdateObjectsAdjacentToWall` (abaixo).

### `PostLoad`: a entrada 2 (load) roda em todo objeto carregado

`cEdithObjectModule::PostLoad(versão)` (BV `0x52d590`) chama o
`cTSObject::PostLoad(versão, nullptr)` (BV `0xb0b1e0`, vtable `0x318`) de
**todo** objeto do lote, os contêineres antes dos contidos. Esse
`PostLoad`: `BeginEditing`; `SetLocation` no lugar salvo (a sala é
recalculada); acertos de versão do arquivo; **`RunTree(2, 0, params)`** com
os quatro parâmetros zerados quando o chamador não passa nenhum; para
objetos que o pedem (`+0x180` bit 2 e outros), entrada `0x2B`,
`InitSlotsFromDefinition` e `UpdateWallAdjacency(true)`; `FinishEditing`
(o aviso de sala da primeira vez). **Não há teste de versão do objeto**: o
opensims2 rodava o load só quando o od 58 diferia da versão do OBJD (tirado
da descrição do editor; inferido e errado). Nos dados, a entrada 2 está em
476 OBJf: os Sims (o "Function - Load" das pessoas, semi-global
`0x2034`), o lixo e alguns outros.

### `UpdateWallAdjacency(bool)` (BV `0xaf7104`, vtable `0x2f4`): od 5 e a entrada 6

Com o bool, ou quando o objeto tem a entrada 6
(`RequiresWallAdjacencyUpdate`, BV `0xaf5050`): olha as 6 paredes do tile
do objeto (`FUN_001645b0`: segmentos 1, 2, 4, 8 = as arestas, 0x10/0x20 = as
diagonais; vtable `0xb0` do gerenciador de partições) girados pela direção
(`WallSegmentRotate(segmentos, (−od1 / 2) & 3)`, BV `0x1563540`: um quarto
de volta leva 1→2→4→8→1 e troca as diagonais) e monta o od 5 a partir do
byte alto dele: segmento 1 → `0x01`, 2 → `0x04`, 4 → `0x02`, 8 → `0x08`,
0x10 → `0x40`, 0x20 → `0x80`; e, no tile do andar de cima, 1 → `0x10`, 4 →
`0x20`. Se o od 5 mudou, `RunTree(6, 0, {})`. Chamado no `SetPosition` (BV
`0xafd048`, com `false`), no `PostLoad` e pelo
`cEdithObjectModule::UpdateObjectsAdjacentToWall` (BV `0x52e468`, os dois
tiles de cada lado da parede que mudou). A entrada 6 está em 158 OBJf
(bancadas, cadeiras, fogões, geladeiras, lareiras, banheiras, escadas).

**Que aresta é cada segmento** (dos dados): com os segmentos do mundo 1, 2,
4, 8 = as paredes para −Y, −X, +Y, +X, o od 5 calculado bate com o salvo em
**319 das 321 bancadas** de N001–N003 (`S2PKG_WALLADJ=Counter s2pkg
autonomy <lote> --ticks 0`). No sentido do objeto: `0x01` = parede do lado
anti-horário da frente (lado 3 do `sideStep`), `0x04` = na frente, `0x02` =
do lado horário, `0x08` = atrás. Qual diagonal é `0x40` ficou inferido (não
há diagonal ao lado de bancada nos lotes). Geladeiras, cadeiras, lareiras e
escadas salvas têm od 5 = 0 mesmo encostadas na parede: o jogo só recalcula
quando o objeto é posto ou a parede muda, e esses lotes vieram assim; por
isso o opensims2 não recalcula ao carregar (o `PostLoad` só o faz com o bit
2 de `+0x180`, não lido).

**No opensims2** (rodada n04): `vm::onRoomChanged`/`finishMove`/
`notifyRoomChanges` (`vm/rooms.*`: o aviso ao mover, ao pôr e no fim de cada
tick; a porta portal é a parte líder do tipo 8 e as salas são a do tile
dela e a do tile atrás, como os atributos salvos de `N001_Lot11` mostram:
parte 0,0 = (frente, trás), parte 0,1 = (sala, −1)); o modo Construção
chama `UpdateObjectsInRoom` e `UpdateObjectsAdjacentToWall`
(`build_edit.cpp`); `vm::updateWallAdjacency` (`placement.*`); a entrada 2
em todo objeto carregado (`lot_resume.cpp`; `VM::runLoadEntries` desliga só
nas conferências de salvar do `s2pkg`).

**Soak (antes → depois, `tools/soak.sh` sem o app).** 8 horas de jogo
(14.400 ticks) em `N001_Lot11`/`Lot68`/`Lot49`: interações autônomas que
terminam verdadeiro 88 → 94, falso 1 → 0, "done with" 93 → 105; Sims presos
3 → 3 (os mesmos: piano e xadrez no `Lot11` por 6–7 h, e a escola no
`Lot49`, que é normal); nenhum erro, reset ou diferença de salvar. Os 62
lotes de N001–N003 por 4 horas: nenhum crash, travamento, erro de Main,
preso ou diferença de salvar, antes e depois (os lotes divergem cedo, porque
o load roda agora: o controlador de doenças, por exemplo, liga a flag de
"acabou de carregar").

### `cTSPerson::SetCurrentAction` (BV `0xadc6d8`)

Chamado pelo Idle for Input (próxima entrada da fila) e pelo Gosub Found
Action (`try_gosub_action_found`, BV `0xade17c`, que depois chama
`UpdateObjectOwnership`). Grava os dados de pessoa da ação: 21, 164 (flag do
objeto da interação), 80, 33, 102/103 (cópias de 21/80), 166; com ação, **39
:= 0**; **73** (o recorte da pegada) := 0 e marca a pegada suja; **71 := 0**;
**110 := 0** ("Interactions Linked Deleted", que o global "Wait For
Notify" lê; quem o liga é o `cTSInteraction::UnLinkInteractions`, BV
`0x4c6a54`, no outro Sim de uma interação ligada: `vm/links.cpp`); `ClearSubActions`; `SetInteractionData`. No opensims2,
`vm::setCurrentAction` (`queue.cpp`), menos o 164.

## 4j. Rodadas n41 e n46 (VM, resto): o que foi lido

Ghidra MCP (só leitura), programas `sims2_base_1.0.ppc` (o alvo; sem
símbolos: as funções foram achadas pelas do BV, que têm nome) e
`sims2_bv.ppc`.

### Dado de pessoa 39 "route status" no jogo base

O roteador do base 1.0 (`cTSRoutingObjectAggregate`) mantém o 39 em três
lugares; nenhum outro código o grava, fora o `SetCurrentAction` e o
`SetCurrentSubAction` (abaixo). Varredura do executável base por `li r4,
0x27` antes de um `SetPersonData`: só esses cinco.

- **`ProcessGoto`** (base `0xc59afc`; BV `0x57b500`), quando a primitiva de
  rota começa (a flag "começada" do elemento da pilha de árvores, vtable
  `0x48`, desligada; ela é ligada aqui): depois dos testes do alvo (sem ele,
  erro `0x17`), da rota corrente (dentro da pré-rota: erro `0x80`; dentro da
  "along route": `0x81`) e do `ShouldInterrupt` (`FUN_00c54b60`: dado 62 := 3
  e falso), cria a rota nova e, **com a pilha de rotas vazia** (`+0x20` = 0),
  grava **39 := 1** (`0xc5a778`) antes do `PushRoute` (vtable `0x68`). Uma
  rota começada dentro de outra (o goal check, o feedback de falha) não grava.
- **`InitRoute`** (base `0xc574d4`; BV `0x5737cc`), chamado só pelo
  `DoInitial` (vtable `0xc8`), isto é, a cada planejamento: o primeiro, o de
  depois da pré-rota e todos os re-planejamentos (bloqueio, pausa, "CT -
  Route Blocked", quem saiu da frente). Sem rota, sem alvo (`GRS_State +8`)
  ou sem pessoa sai com o código 1 **sem gravar**. Achado o caminho
  (`route->0x20(alvo)`, o `FindPath`, que gera os objetivos):
  `UpdatePersonDataFields(1, true)` → **39 := 1** (`0xc57cb4`), mesmo quando
  o Sim já está no objetivo (resultado 1, sem andar). Sem caminho (códigos 2,
  6 ou o do último candidato, 15 com bloqueador): `UpdatePersonDataFields(0,
  true)` → **39 := 0** (`0xc58130`).
- **`PopRoute`** (base `0xc55310`; BV `0x570a00`): sobra rota embaixo →
  `UpdatePersonDataFields(0, false)` (`0xc554fc`: o 39 fica); era a última →
  `UpdatePersonDataFields(0, true)` → **39 := 0** (`0xc55514`). Chamam: o
  `DoPostFailedOrSucceeded` (o fim da instrução de rota, sucesso ou falha,
  depois do feedback de falha e da pós-rota) e o `ClearRouteStack` (BV
  `0x56ef74`: `PopRoute` até esvaziar; com a pilha vazia não grava nada), que
  o `cTSRoutingObjectAggregate::OnCleanup` (BV `0x5727c8`) chama no reset da
  pessoa (`Cleanup(nada)`, o reset de depois de um erro) e quando o objeto
  apagado é o alvo de uma rota da pilha.
- Na pessoa, `SetCurrentAction` (base `0xb83f54`, a gravação em `0xb841d8`)
  e `SetCurrentSubAction` (base `0xb849ec`, em `0xb84ac0`): com ação,
  **39 := 0** (vtable `0x640` da pessoa). O VM já fazia o do
  `SetCurrentAction`; o da sub-ação não. O `SetCurrentSubAction` grava
  ainda: 157 := o objeto da sub-ação (0 sem ele); 73 := 0 (se ≠ 0, e marca
  a pegada suja, vtable `0x2e8`), 71 := 0, 110 := 0 e 109 := vtable `0xec`
  da sub-ação; `+0x558` := `+0x540`. O VM agora zera 39, 73, 71 e 110 na
  sub-ação nova; o 109 continua o da ação (`syncQueueData`; `TODO.md`).

Então, no jogo base, o 39 é 1 do começo da rota (antes mesmo da pré-rota)
até o fim dela, inclusive na pré-rota, na pós-rota e no feedback de falha
de uma rota que falhou andando; é 0 quando o planejamento falha (o feedback
de falha de uma rota sem caminho roda com 0) e fora das rotas. No BV o 1 sai
do `UpdatePersonDataFields` (BV `0x570674`, que grava `0xb3` no lugar) e o
`PopRoute` grava o 0 ele mesmo; a rodada n33b leu só esse lado ("sumiu").

`UpdatePersonDataFields(valor, grava39)` (base `FUN_00c64a70`) grava ainda,
da rota corrente (vtable `0xb0`; o `PopRoute` o chama antes de tirar a rota
da pilha, então na última os campos ficam os dela): 40 "route goal" (o ID
do objeto do objetivo, ou 0), 37 "route slot" (`WorkOutSlotIndex`,
`FUN_00c54cc0`), 55 "route start slot" (rota `0x5c`) e 38 "route multi slot
index" (o `+0x14` do objetivo atual, ou −1 sem objetivo); sem rota
corrente, 40 = 0 e 37, 55, 38 = −1. O VM grava 37/38/40 pelo `noteRouteSlot`
só no Go To Routing Slot, no começo (inferido; fica, a rodada é só do 39) e
não grava o 55.

Nenhuma árvore do jogo grava o 39 (varredura das BHAV de `TSData/Res`: 31
leituras, `==`, `>`, `!=` e teste de bit). Leem: os "Shoo" (global `0x2FA`
"Shoo - Specific Sim", `0x2FC` "Shoo - Sims from tile Object X is on",
"Dartboard - Shoo Sims": só enxotam quem tem 0), "Look At - Check Out",
"Function - Awareness - Private", "CT - Visible People List Changed",
"Interaction - Talk At", "Interaction - Work The Room", "Test - Sim
Routing?", "Fire - Another Fighter Routing to Fire?", "Sub - Wait for Cop &
Burglar done route", "Stand - Try Scoot", "TV - Clear View of Screen?",
"Sub - Reaction Sim is Routing?" e mais alguns.

**No código** (`persondata::RouteStatus`): o `walkStep` grava 1 quando a
instrução de rota começa sem rota na pilha (`routeStackDepth` = 1, o
`ProcessGoto`); o `planRoute` (que já era o `InitRoute`) grava 1 com
caminho e 0 sem; o `walkStep` grava 0 também sem objetivos (o `FindPath`
falha); o fim da instrução (`walk`, o `DoPostFailedOrSucceeded`) grava 0 se
a rota era a única da pilha; o `VM::resetObject` grava 0 (o
`ClearRouteStack`: o 39 só é 1 com rota na pilha) e a sub-ação nova também
(`queue.cpp`). Antes o 39 ficava 0 sempre, e os "Shoo" enxotavam também
quem passava andando pelo tile. Teste: `test_routing` (`testRouteStatus`).

Medido no N001_Lot68 (`s2pkg autonomy --ticks 129600`, `S2PKG_FIRES=1`,
sementes 0–12; incêndio = um fogo que aparece sem outro aceso): **antes 2
incêndios (sementes 7 e 9) e 13 mortes ("Falecer"), depois 2 (sementes 0 e
7) e 8**; "Afastar-se" 9 → 7. Os incêndios dos dois lados são do sorteio de
quem cozinha ou do jantar congelado no forno (o Daniel), não de quem foi
enxotado; fora dos incêndios morrem 5 antes (sementes 0, 1, 6, 12) e 1
depois (semente 4). Cada semente é caótica; a mudança não aumenta nem
incêndios nem mortes. N001_Lot11, sementes 1–6: nenhum incêndio nem morte
antes e depois, "Afastar-se" 2 → 0, os mesmos 6 resets por erro.

### Play / Stop Sound Event (`cTSObject::TryPlaySound`, BV `0xe41844`)

Lido de novo. Do operando o binário só usa o número do som (s16 0), o byte
de flags (4) e, com número negativo, o dono e o dado (6, u16 7): **o volume
(byte 5), a taxa (u16 2) e o bit 0 das flags não são lidos**. A fonte é o
stack object com o bit `0x02` e flags ≠ `0xFF`, senão eu. A tabela sai de
|n| pelo dono do código da árvore (vtable `0x50` do elemento da pilha):
≥ 20000 → `0x38` (semi-global) com n − 20000, 10000–19999 → `0x34` (global)
com n − 10000, o resto → `0x3C` (privada); com n negativo o índice é **o
valor** de (dono, dado) dentro da tabela escolhida por |n| (não somado). O
nome achado vai em minúsculas numa mensagem com o ID do objeto e a
posição; o tipo da mensagem é `0x2BC78968` com o bit `0x40` e `0xCBC78960`
sem ele. O `cTSAudioEventHandler::DoEvent` (BV `0x6108a4`) toca o evento
com `0xCBC78960` e **para** o evento com `0x2BC78968`: o `0x40` é "parar"
(confirmado; era inferido dos dados). Mais do `DoEvent`: um evento tocado
cujo nome começa por `stop_` para o evento do resto do nome (nenhum nome das
tabelas de som do jogo base começa assim); nomes `vox*` vão no canal 1,
`cine*` no 6; `foot*` só tocam com as duas flags do tratador ligadas (`+0x38`,
`+0x39`, a última pela mensagem `0x2CBF1A2B`); evento de objeto que não é
mais válido é recusado. **Laço**: o `TryPlaySound` não diz se o som repete;
isso é da trilha (o único uso do bit 0 no jogo base, o "Process" do
detector de fumaça, `0x7F97EA32` `0x1004`, toca `alarm_smoke` com `0x01` e
para com `0x41`; a trilha dele não tem a chave `0xff595ffd` que o
`SoundSystem::loops` toma por laço). O VM segue tocando em laço com o bit 0
ou nome `_lp` (inferido, para o alarme não calar depois de 3 s).

### Os registros de objetos da pessoa (`cTSPerson +0x60c`, `cObjectRecord`)

Lido na rodada n41 (em parte) e na n46 (inteiro), no BV e no base 1.0
(Ghidra MCP, só leitura). No BV é um `std::vector<cObjectRecord>` em
`+0x60c`/`+0x610`; no base 1.0 um vetor próprio com o começo em `+0x53c` e
a contagem em `+0x540`. Cada registro tem 12 bytes: vtable (`Write`), o
objeto (`+4`, ponteiro com contagem de referência), a **profundidade da
pilha de árvores** (s16 `+8`) e a flag "trocar ícone" (u8 `+10`). Salvo no
PERS (`save-mac.md` §3) por `cObjectRecord::Write` (BV `0xad5cd8`): o ID do
objeto (o gerenciador de objetos, vtable `0xf8`), a profundidade num u32 e
a flag num u8; `Read` (BV `0xad27fc`) lê o mesmo.

**Quem cria.** `cTSPerson::OnGosubObjectTree(objeto, flag)` (BV `0xad2318`,
vtable `0x55c` do `cTSObject`; base 1.0 `0xb878f0`, vtable `0x4a0`); o
`cTSObject::OnGosubObjectTree` (BV `0xaff3b0`) não faz nada, então só as
pessoas têm registros. Ele monta o registro com o objeto, a profundidade da
pilha **depois** do quadro novo (vtable `0x5c` do tree sim; base `0x58`) e
a flag; com a flag, a sub-ação corrente (`+0x5dc`; senão a ação, `+0x5d4`)
recebe o objeto como alvo (`SetTargetObject`, vtable `0x104` da interação:
`+0x10`, o ícone da fila) e zera o ícone por GUID (`0x120(0, 0)`: `+0x38`/
`+0x3c`); põe o registro no fim e soma 1 no **object data 62** do objeto.
Quem o chama: as duas `cTSObject::GosubObjectTree` (BV `0xb033b8`, vtable
`0x3cc`, e `0xb03270`, `0x3d0`; base 1.0 as chamadas em `0xb2ce20` e `0xb2cfd0`), depois de empurrar a
árvore do objeto na pilha (vtable `0x30` do tree sim). No base 1.0 só quando
o empurrão deu certo (no BV a `0x3cc` o chama mesmo sem quadro novo). E elas
são chamadas por: o Idle for Input (a ação e a sub-ação), o Gosub Found
Action, o Run Tree by Name no modo 2 (e 3), o Run Functional Tree, as árvores
do roteador (`TriggerPreRouteTree`, `TriggerPostRouteTree`,
`TriggerPortalTree`, `TriggerPauseTree`, `TriggerAlongRouteTree`,
`TriggerGoalCheckTree`, `TriggerRouteBlockedTree`, `TriggerFailureAnimation`,
`TriggerAskToMoveAnimation`, `TriggerWaitAnimation`,
`TriggerBlockedAnimation`), o Reach e as Animate Sim/Overlay/Stop. A flag é
0 em todos, menos no **Run Functional Tree**, que passa o bit 0x01 das flags
do operando ("trocar ícone", `TryCallFunctionalTree` BV `0xe253a4`). O
objeto é o dono do código empurrado (o da interação; o próprio Sim nas
árvores do roteador que são dele, como a pré-rota e o "CT - Route Blocked").

**Quem tira.** `cTSPerson::StackJustPopped` (BV `0xadef18`, vtable `0x6e8`;
base 1.0 `0xb83b04`), a cada quadro tirado da pilha da pessoa (também os
das árvores de um tick, que rodam na mesma pilha: `cTSTreeSim::
RunOneTickTree` BV `0xe1ab54` empurra um marcador e a árvore em cima da
pilha corrente): sem registros, nada. Com o último registro: se a pilha
ficou com a profundidade dele − 1 (o quadro dele saiu), od 62 do objeto −=
1, **cada** registro com a flag (do primeiro ao último, inclusive o que
sai) dá o seu objeto como alvo da sub-ação corrente (senão da ação), e o
registro sai; se a pilha está mais rasa que ele, só od 62 −= 1 e sai.
Depois: com sub-ação e o número de registros igual ao `+0x644` a sub-ação
acabou (157 := 0, `+0x644` := 0, 102 := 21, 103 := 80); sem registros, ou
com o número igual ao `+0x646` − 1, a ação acabou (abaixo).

**Fim da ação e as interrompidas.** O Idle for Input empurra a árvore da
ação e só então chama `SetCurrentAction` (BV `0xadec48`/`0xadec8c`; base
1.0 `0xb7fbac`/`0xb7fc20`), que guarda o número de registros no `+0x646`
(base `+0x55a`): a ação acaba quando o registro dela sai. A sub-ação é o
contrário: `SetCurrentSubAction` antes do empurrão (BV `0xade834` →
`0xade854`), o `+0x644` (base `+0x558`) é o número antes do registro dela.
No fim da ação o base 1.0 faz uma interação vazia, desfaz os vínculos da que
acabou (`UnLinkInteractions`, vtable `0xe8` no base), `SetCurrentAction(vazia)`,
`+0x55a` := 1 e zera 21, 102, 103, 80. **No base 1.0 não há pilha de ações
interrompidas**: quando uma entrada roda por cima de outra (o Idle for Input
dentro de uma interação), o base desfaz os vínculos da corrente
(`0xb7fc08`) e põe a nova no lugar; quando a nova acaba, a pessoa fica com a
interação vazia (21/80/33 = 0) enquanto a árvore da interrompida segue
rodando. O BV guarda a interrompida numa `deque` (`+0x5e0`, BV `0xadec80`)
e a devolve como corrente no fim da que interrompeu (a não ser que a
primeira da fila seja "continuar"). O `try_gosub_action_found` (BV
`0xade17c`) também põe a nova no lugar da corrente, sem guardá-la nem
desfazer os vínculos. **No VM** (n46) como no base: a entrada que roda por
cima da corrente (`attemptPush`) toma o lugar dela na fila, com os vínculos
dela desfeitos, e o Gosub Found Action também (sem desfazer); a árvore da
interrompida segue sem ser a ação de ninguém (dados 21/80/33 a 0) até
voltar (era como o BV: a interrompida voltava a ser a corrente; nas 14
sementes do `N001_Lot68` e do `N001_Lot11` a saída é a mesma).

**No reset.** `cTSPerson::OnCleanup(nada)` (BV `0xadcbcc`), o
`Cleanup(nobody)` de uma pessoa (o reset depois de um erro): guarda pela
flag `0x800` do objeto; para cada registro, do último ao primeiro: tira
quadros da pilha até a profundidade dele − 1 (cada um com
`StackJustPopped`, que tira o registro e o od 62) e, **se a pessoa não está
na fila de morte** (flag `0x1000`, que o `AddToKillQueue` põe):
`cEdithObjectModule::CleanupPeople(objeto)` (BV `0x524ea4`: cada outra
pessoa recebe `Cleanup(objeto)`) e `objeto->Cleanup(nada)` (vtable `0x320`,
BV `0xb09838`: o reset do objeto, com a guarda `0x40000`). Um registro cujo
quadro já não está na pilha sai à mão (od 62 −= 1). Depois vem o resto do
reset (RemoveFromWorld/lugar livre, dados de pessoa 0 e 59 := 0, **object
data 34** "Hidden Flags" := 0 (no base 1.0 o mesmo, `0xb85f74`–`0xb85fc0`;
o VM zerava o dado de pessoa 34, corrigido na n46), o tree sim volta ao Main, a entrada 3 do OBJf, a ação
vazia, a fila e a sub-fila apagadas). Ou seja: **o reset de um Sim reseta os
objetos que ele usava e as pessoas que os usavam** (um parceiro de social,
quem estava no mesmo objeto).
`OnCleanup(objeto)` (o objeto apagado ou resetado): faz o reset inteiro
acima se o objeto está num registro, é o 157 ou o 80 (com a cascata só se o
**objeto** não está na fila de morte); senão tira da fila e da sub-fila as
entradas cujo objeto ou alvo é ele e tira o alvo da ação corrente e da
última (`SetTargetObject(0)`).

**Quem lê.** `cTSPerson::HasObjectReference(id)` (BV `0xacce70`, vtable
`0x6a4`): o ID está num registro. É o **Test Sim Interacting With** (0x25,
`TryTestInteractingWith` BV `0xaca1f0`: o stack object nos meus registros);
o VM procurava na fila (inferido). Os registros são o **contador de uso**
(od 62): 87 Expressions do jogo base leem o od 62 de si mesmas e 39 o do
stack object (os "Function - Main" esperam ninguém usar o objeto, os carros
de NPC só saem com 0, "Function - In Use?").

**No VM** (rodada n46, `vm/records.cpp`): `Person::objectRecords`; o
`VM::call` é o `GosubObjectTree` (os chamadores do VM são os do binário: o
Idle for Input, o Gosub Found Action, o Run Tree by Name no modo 2, o Run
Functional Tree com a flag 0x01, as árvores do roteador) e faz o registro;
o `VM::branch` chama o `StackJustPopped` em cada quadro tirado; o od 62 é o
dado guardado (era calculado ao ler, contando quem tinha o objeto na fila
ou código dele na pilha, como a FreeSO: inferido). As árvores de um tick, de
check e de entradas do OBJf rodam numa thread à parte aqui: o registro
guarda a thread que o fez e os que ela deixar saem com ela. Lidos e gravados
no PERS (e `+0x644`/`+0x646`); um Sim sem a pilha salva retomada perde os
seus (od 62 − 1). O reset (`VM::resetObject`) faz o `OnCleanup`: os
registros saem e, sem a pessoa na fila de morte, cada objeto deles é
resetado (a árvore de limpeza, entrada 3) com as outras pessoas que o usam
(`cleanupPeople`); as entradas da fila e da sub-fila saem por
`ActionSkipped` e as que rodavam têm os vínculos desfeitos; o dado 33 vai a
0 (a ação vazia). O objeto apagado (`flushKillQueue`) reseta quem o tem num
registro, sem cascata (ele está na fila de morte). O Test Sim Interacting
With lê os registros. Nos lotes salvos de N001–N003 o od 62 bate com os
registros em todos os objetos (um registro só, no `N001_Lot11`). O filtro
do Find Best Interaction que pula objetos em uso (`useCount`, como a FreeSO)
segue inferido: o `FBA_FindStaticInteractions` do BV não lê o od 62 (pula
objetos fora do mundo, na fila de morte, com `AreInteractionsDisabled(1)` ou
o bit 0 do od 56); o VM soma o od 62 das partes do objeto de várias partes
(com o od 62 de cada parte só, a banheira de duas partes era escolhida pela
parte livre enquanto outro Sim a usava, e o banho falhava na rota: 12 → 52
interações autônomas falsas em 14 sementes do `N001_Lot68`).
**Inferido:** o adiamento do `Cleanup` de um objeto no meio de uma árvore
de um tick (`+0x290`/`+0x3c`) não é feito; os quadros do Reach e das
Animate Sim/Overlay que o binário empurra por `GosubObjectTree` não têm
correspondente aqui (o VM roda as árvores de evento em um tick).

### Os dados de pessoa 109 e 164 (`SetCurrentAction`, `SetCurrentSubAction`, `SetInteractionData`)

BV `0xadc6d8`/`0xadae58`/`0xada7b0`; base 1.0 `0xb83f54` e seguintes.

- **109 "ci: Current Interaction GUID"** := o GUID da interação (o `+0x40`,
  `GetGUID`: vtable `0x110` no BV, `0xec` no base). `SetInteractionData`
  grava o da ação corrente (e 45/72/79 da primeira da fila); chamam-no o
  `AddAction`, o `RemoveAction`, o `OnCleanup` e o `SetCurrentAction`. O
  `SetCurrentSubAction` grava o da **sub-ação**, e nada o volta ao da ação
  quando a sub-ação acaba (fica até o próximo `SetInteractionData`). O
  `SetCurrentAction` dá um GUID novo à ação que não tem (`AssignUniqueGUID`,
  vtable `0x108`/`0xe4`, e o `+0x42`). Leem o 109 dois Push Interaction
  (vínculo, flag 0x40): "Interaction - Read to" e "Test Interaction Cancel".
- **164 "Current Interaction Joinable"** := 1 se a entrada do TTAB da ação
  tem a flag **0x02** ("joinable": `cTSTreeTableEntry` vtable `0x38` no BV,
  `0x30` no base, bit 1 das flags `+0x30`/`+0x88`), senão 0 (ação vazia: 0).
  Lê: "Joinable - Are Sim X and Y in same joinable Obj?".

**No VM** (n46): o 109 deixou de ser o da ação a cada `syncQueueData`: é
gravado pelo `setInteractionData` (o `AddAction`, o `RemoveAction`, o
`SetCurrentAction`, o fim da ação e o reset) e pela sub-ação nova, que
fica no 109 até o próximo; quando a ação acaba sem outra, a interação vazia
ganha um GUID (o global 36) e o 109 é ele. O 164 é a flag 0x02 da ação
corrente (`syncQueueData`; antes ficava 0). **Inferido:** o GUID da
interação vazia não é salvo, e um Sim que entra no lote fica com 109 = 0
até a primeira ação. Nas 14 sementes do `N001_Lot68` e do `N001_Lot11` a
saída do `s2pkg autonomy` é a mesma de antes.

### `UpdatePersonDataFields` (base 1.0 `FUN_00c64a70`, BV `0x570674`): 37, 38, 40, 55

Da rota corrente do roteador (vtable `0xb0` do agregado; o BV pula a rota
interna, vtable `0xcc`): **40** := o objeto alvo (vtable `0x108`) ou 0; **37**
:= `WorkOutSlotIndex(slot, alvo)` (BV `0x56dfe4`, base `FUN_00c54cc0`: o índice
do slot de rota entre os slots do SLOT do alvo, procurado do fim; não achado,
entre os slots globais, + 1000; nada: −1); **55** := a rota `0x5c` (BV `0x68`,
`+0x126`), que o `Construct` (base `FUN_00c452dc`) põe em −1 e só o `FindPath`
(base `FUN_00c475d0`) troca: o índice do ponto de partida escolhido quando o
Sim sai de um contêiner pelos slots dele (`FUN_00c49c58`), −1 ao partir do
lugar onde está; **38** := o `+0x14` do objetivo corrente (vtable `0xf0`/`0xf4`)
ou −1; sem rota corrente, 40 = 0 e 37/55/38 = −1. Com o bool, 39 := o valor.
Chamam: o `InitRoute` a cada planejamento (com caminho e sem, com a rota
ainda na pilha) e o `PopRoute` (antes de tirar a rota: na última os campos
ficam os dela). Leem: 37 os "Goal Check" e "Force - Stand - In Place"; 38 as
camas, sofás, banheiras etc. (43 leituras); **40** o "Test - Sim Routing to
Inside?", o "Fire - Another Fighter Routing to Fire?", o "Route - Is Person
going to Ped Portal?" (o de outro Sim, por isso vale para toda rota, não só
o Go To Routing Slot); 55 o "Function - Stand" e o "Stand - Try Snap".
Os Sims salvos do `N003_Lot52` mostram os campos de rotas do jogo: 37 = 1033
e 1004 (slots globais 33 e 4), 38 = 4 e 6, 40 = o alvo, 55 = −1.

**No VM** (n46): `updatePersonDataFields` (`routing_plan.cpp`) a cada
planejamento (`planRoute`, com caminho 39 := 1, sem 39 := 0 e 38 := −1) e
quando o planejador não acha objetivos, para toda rota (o Go To Relative
Position também: 40 é o alvo dele); 37 é o slot do Go To Routing Slot, −1
nas outras. Era: 37/38/40 só no começo do Go To Routing Slot, 55 nunca.
**Rodada n49** (`docs/routing-mac.md` §9.5): o 37 de um slot global é o
índice + 1000, como o `WorkOutSlotIndex`; quem o lê de volta é a árvore
social "Turn toward incoming Sim's target slot" (semi-global `0x7FD90EDB`
`0x2019`: o 37 do Sim que chega == 1033 → vira para a direção absoluta do
ponto pelo 38, global `0x2DF`; senão vira para o Sim) e o `resumeRoutes` do
VM (tira os 1000). O laço do 13 que a n46 viu com o + 1000 (`N001_Lot11`
semente 2) não aparece nas sementes 1–12 de 3 dias com o resto da rodada
n49 (as trajetórias mudaram; a causa não foi achada); o 13 a cada tick do
Mortimer perto do alpendre aparece com e sem o + 1000 (semente 8) e acaba
com a interação. `OS2_SLOT_PLAIN=1` volta ao índice só, para comparar. **Inferido:** 55 é sempre −1 (o planejador do
VM parte de onde o Sim está; o do jogo parte dos slots do contêiner quando o
Sim sentado planeja antes da pré-rota), o 38 de um objetivo sem índice é 0,
e o `PopRoute` não grava de novo (os valores são os do último planejamento,
os mesmos que ele gravaria).

### As sobras da fila (BV)

- **`ActionSkipped`** (BV `0xaca0cc`): `UnLinkInteractions(pessoa)` da
  entrada e, com o objeto, `RunTree(4, pessoa, nada)` no objeto: é a
  **entrada 4 do OBJf, "queue skipped"** (não um evento), com o stack object
  = a pessoa. 13 OBJf do jogo base a têm ("Function - Queue Skipped" de dois
  grupos e três árvores semi-globais).
- **`TestInteraction`** (BV `0x4c6214`), os testes do objeto: vtable `0x214`
  é `IsBurning` (od 8 bit `0x200`) e `0x2e8(autônomo)` é
  `AreInteractionsDisabled` (BV `0xb01634`: autônomo e od 25 "lockout" ≥ 1,
  ou od 87 ≥ 1, ou od 82 bit `0x04`); qualquer um recusa. O "flag do sim
  system 0x23" é o **global 35 "Parental Control"** (`TS::Globals()->
  GetSimulator()->GetGlobal(35)`, simulador vtable `0x3c` = `+0x24 + 2i`):
  ligado, recusa a entrada com a flag `0x20000` (bit 1 do flags2, vtable
  `0x84` da entrada).
- **A fila cheia** (`TryPushAction` BV `0xe33de0`): a vtable `0x1FC` da
  pessoa é `AnyLowerPriorityInteractions(prioridade)` (BV `0xad65b0`): há uma
  entrada **esperando** de prioridade menor que a nova. Não aninhada: mais de
  6 esperando (vtable `0x1f4`, sem a corrente) e nenhuma menor → resultado 3;
  com uma menor o push entra (e o `AddAction` pula as menores). Aninhada:
  mais de 4 na sub-fila → 4.

**No VM** (n46): o `ActionSkipped` roda a entrada 4 (`actionSkipped`, também
no `DeleteTopAction` do reset); o `entryAvailable` (o `TestInteraction` antes
da check tree) recusa o objeto pegando fogo, com as interações desligadas e,
com o controle dos pais, a entrada `0x20000`; a fila cheia conta só as que
esperam e dá lugar quando há uma menor (era: contava as que rodavam e não
dava lugar). Nas 14 sementes do `N001_Lot68` e do `N001_Lot11` a saída do
`s2pkg autonomy` é a mesma. **Inferido:** o menu de pie não aplica esses
testes do objeto (só a idade, como antes).

### Medidas da rodada n46

`s2pkg autonomy`, main `41956eed` → o fim da rodada. 14 sementes × 12.000
ticks: `N001_Lot68` interações autônomas que terminam verdadeiro 640 → 631,
falso 12 → 17, falhas de rota 416 → 366 (fora do 13: 94 → 68);
`N001_Lot11` 442 → 441, 1 → 1, 550 → 456 (112 → 89); nenhum reset nem morte
dos dois lados. Três dias (129.600 ticks, `S2PKG_FIRES=1`): `N001_Lot68`
sementes 0–12, mortes 10 → 20, objetos de fogo 196 → 395, resets por erro
17 → 9; mais 40 sementes (13–52), sementes com fogo 13 → 11, mortes 49 → 49,
objetos de fogo 409 → 725 (os fogos crescem mais: o "Sub - Can Destroy?" do
fogo, grupo `0x7FD0A790`, lê o od 62 do objeto, agora o da parte, e o
"Function - Main" o do próprio fogo); `N001_Lot11` sementes 1–6, mortes 0 →
0, falhas de rota 669 → 1.377 (fora do 13: 317 → 196; o 13 é o laço da
aproximação do Mortimer em "Route - Dynamic w/Lead in" nas sementes 4 e 5,
o mesmo da rodada n31), resets 15 → 14, interações ≥ 6 h 44 → 56.

### `cTSGlobals` e `cTSDataTables`

- **`cTSGlobals`** (BV vtable `0x27a20d0`; 58 métodos) não guarda os globais
  da SimAntics: é o registro de serviços do jogo (`GetLot`, `GetSimulator`,
  `GetWantSimulator`, `GetMemorySimulator`, `GetNeighborhood`,
  `FamilyManager`, `GetRouteManager`, `ObjectManager`, `TypeAttributes`,
  `TestingCheatsEnabled`, `TextDebuggingEnabled`…; os nomes pelo
  `baserom.sym` do projeto de decompilação, na ordem dos getters de
  `0xaa210` em diante: `+0x2c` lote, `+0x20` simulador, `+0x24` desejos,
  `+0x28` memórias, `+0x30` bairro…). Nada a portar. Os globais (STR# 0x81)
  são do `cTSSimulator` (`+0x24`, s16): `GetGlobal` (vtable `0x3c`), o
  endereço (`0x44`, que o `InterpValue` usa para ler e gravar, índice > 57:
  erro) e `SetGlobal` (`0x40`, BV `0x49caf0`), que **ignora** os globais 19
  ("Mode") e 55 e grava o 53 ("Instant Write Once Flags") também na
  configuração `edithConfigFlags`. Quem chama o `SetGlobal` no motor não foi
  levantado (`TODO.md`).
- **`cTSDataTables`** (39 métodos: tabelas hash nomeadas de
  `cGenericData` com `GetBool`/`GetFloat32`/`GetSint32`/`GetUint32` por
  nome, um recurso salvo) **não existe no base 1.0** (nem a RTTI nem as
  strings `NeighborDataTable`/`ObjectDataTable`, que só o BV tem): é de uma
  expansão. Nada a portar para o jogo base.

## 5. Outras primitivas: operandos pelo disassembler

Resumo do que o disassembler do editor (`cEdithLanguage::Cat*`, mesmo texto
que as strings do `Sims2.exe`, mas agora com o byte de cada campo) e o
`SwizzleParams` dizem. Binário, salvo indicação.

- **0x02 Expression**: esquerda (dono byte 6, dado u16 0), operador byte 5
  (STR# 0x88), direita (dono byte 7, dado u16 2).
- **0x08 Random Number**: destino (dado u16 0, **dono u16 2**), limite (dado
  u16 4, **dono u16 6**): "dest := random from 0 to < limite". Execução:
  **limite 0 grava 0 no destino e devolve verdadeiro** (o erro 0x04 "Random
  number range evaluated to zero" não é lançado aqui; a VM hoje lança);
  destino só leitura → 0x13; resultado fora do intervalo do destino → 0x14;
  um destino float (motivo) também é aceito. Sempre verdadeiro.
- **0x0B Get Distance To**: destino = temp (u16 0); flags byte 6: 0x02 em
  centésimos de tile; distância de mim (ou do objeto do operando) ao stack
  object. **Rodada n49** (`cTSObject::TryDistanceTo` BV `0xe25a28`): a
  distância é o `CalcShortDistance` (vtable `0x1f8`, BV `0xaf581c`) com 10,0
  (BV `0x277a900`): √(distância no plano² + (10 × andares entre eles)²),
  cortada para inteiro (ou × 100 e cortada). O VM somava 20 por andar (da
  FreeSO), também em centésimos: um Sim no andar de cima a 1,6 tile ficava a
  "180" e o "Goal Check - Continue?" (≤ 300) da aproximação social o achava
  perto (o 13 a cada tick do Mortimer no alpendre do `N001_Lot11`).
- **0x0C Get Direction To**: destino (dado u16 0, dono u16 2); flags byte 8:
  0x02 em graus.
- **0x0E Find Best Object for Function**: função u16 0 (STR# 0xC9); flags
  byte 2: 0x01 só fora, 0x02 só dentro, 0x04 só no cômodo da referência,
  0x20 fora do cômodo da referência, 0x40 no cômodo do temp 0 (só a primeira
  dessas cinco conta), 0x08 relativo ao objeto (dono 3, dado u16 4), 0x10
  com linha de visão, 0x80 ignorando "lockout" (§4).
- **0x0F Break Point**: (dado u16 0, dono u16 2) "if != 0"; byte 4 bit 0 =
  ignorar no jogo final.
- **0x12 Remove Object Instance**: alvo u16 0 (STR# 0x9C: 0 eu, 1 stack
  object, 2 "target - UNUSED" = ID 0; > 2 → erro 0x09); byte 2: 0x01 volta na
  hora, sem 0x02 "cleanup all" (passado à remoção). Objetos cujo GUID está
  numa lista fixa de 7 GUIDs não são removidos (§4b).
- **0x16 Turn Body Towards**: tipo u16 0 (STR# 0xD8 dos globais).
- **0x1B Go To Relative Position**: lugar s8 2 (STR# 0x82, índice + 3),
  direção s8 3 (STR# 0x83, índice + 3), flags byte 6: **0x02 sem árvores de
  falha, 0x04 permite outro nível**. Contradiz o `routing.cpp`, que usa os
  bits do The Sims 1 (0x01 altitude, 0x02 sem falha).
- **0x1C Run Tree by Name**: flags u16 2 (0x01/0x02 de onde vem o nome,
  0x04 e 0x08 impressos como true/false, 0x10 passa três (dono, dado) em 6,
  9, 12, 0x20 passa os parâmetros da árvore atual); modo byte 5 (STR# 0xDE).

  **O modo 3 (21/10, pelo exe do PC).** Os rótulos do editor (STR# 0xDE do
  grupo 0x7FE59FD0, "how to call named tree") são três no **jogo base** — "run in
  my stack" (0), "run in stack object's stack" (1) e "push onto my stack" (2); o
  **modo 3 é do Bon Voyage** (o AL o usa em 648 árvores) e o erro do jogo para um
  modo fora da lista é o **0x2C** ("Bad parameter to call named tree."). O handler
  da classe base do PC (`FUN_008D3510` @ `0x8D3510`, o caso 3) diz o que ele faz:
  o **GUID nos temps 0/1** (a palavra baixa no temp 0) é procurado no mundo, o
  objeto achado dá a **própria árvore** (o vfunc +0x48 dele) e ela roda na hora,
  como o modo 0, com os mesmos argumentos; um GUID que nenhum objeto tem é o
  erro **0x7A** ("You sent me a crappy GUID, please fix it."). No AL o modo 3
  vem logo depois de um **0x7E** (Run Script) — que é justamente quem põe o GUID
  nos temps.
- **0x1D Set Motive Change**: motivo byte 2 ("motive" n), += (dono byte 0,
  dado u16 4) por hora, até (dono byte 1, dado u16 6); byte 3: 0x01 limpa
  todos, 0x02 zera a "Motive Decay" dos dados de pessoa.
- **0x1E Gosub Found Action**: u16 0 (STR# 0x1FE).
- **0x21 Find 5 Worst Motives**: u16 4 (STR# 0x99) e u16 6 (STR# 0xA5),
  resultado nos temps 0–4.
- **0x22 UI Effect** (o que cada modo faz: `docs/tutorial-mac.md` §2): modo s8 0 (0 aperta controle, 1 desliga, 2 liga, 3
  efeito, 4 árvore de evento, 5 reseta todos, 6 desliga todos, 7 reseta
  árvores, 8 árvore de evento em nó TNS, 9 mostra, 10 esconde), controle u16
  3, janela u16 1, flags byte 5, efeito u16 6, árvore u16 8 com o lugar s8 10
  (0 privada, 1 semi-global, 2 global), (dono 13, dado u16 11).
- **0x23 Camera Control**: byte 4: 0x01 mostra o stack object, 0x08 centra,
  0x20 timeout no temp 0 (senão u16 0), sem 0x40 "slow down"; zoom byte 3
  (1 longe, 2 médio, 3 perto).
- **0x24 Dialog**: tipo s8 5 (0 mensagem, 1 escolha, 2 três escolhas, 3
  texto, 4 tutorial, 5–0x46 diálogos especiais: telefone, revistas, roupas,
  aspirações, ...), mensagem u16 13 (STR# 0x12D), temp do texto = (byte 7 >>
  4) & 7, ícone (byte 7 >> 1) & 7 com o índice no byte 1, flags byte 8
  (0x01/0x02/0x40 de onde vem o texto: privado/semi-global/global, 0x04–0x20
  índices de botões e título no temp 0), prioridade byte 9 + 1, timeout byte
  10, tipo de notificação s8 12.
- **0x2D Go To Routing Slot**: slot u16 0; de onde u16 2 (0 parâmetro, 1
  literal, 2 global, 3 "???"); flags byte 4: 0x01 sem árvores de falha, 0x02
  slot no temp 0 (com u16 2 = 1), **0x04 ignora a área do objeto de
  destino, 0x08 permite outro nível**. Resolve o "0x02–0x10 not understood"
  do `routing.cpp`.
- **0x2E Snap**: modo u16 2 (STR# 0xCF); slot u16 0 (modos 0, 3 e 4); flags
  u16 4: 0x02 pede para a pessoa sair, 0x08 slot no temp 0, 0x10 só testa
  ("TEST ONLY"); u16 8 = 1 "from slot in temp 1".
- **0x30 Stop ALL Sounds**: u16 0: 0 os meus, ≠ 0 os do stack object.
- **0x32 Add/Change the Action String**: modo s8 9 (0 texto, ≠ 0 ícone);
  flags u16 2 (0x04/0x08 tabela global/semi-global, 0x10 índice no temp 0,
  0x100/0x200 desligada propagando/não, 0x1000 para a sub-fila, 0x20/0x40/
  0x80 no modo ícone); texto u16 14; ícone: objeto (dono 11, dado u16 12),
  GUID u32 5, índice byte 10.

  **No port (21/10).** O índice do texto é o **u8 em 4** na versão de nó 0 e o
  **u16 em 14** na versão de nó 1: o censo (`s2pkg prim-usage <Res> 32`) do jogo
  base dá 1 ×173, 2 ×135, 3 ×118… no byte 4 e 0 no byte 14, e o do Bon Voyage
  dá 0 ×2938 no byte 4 e o índice no byte 14 (1 ×475, 2 ×290, 3 ×173…). O port
  lia sempre o byte 4, então no BV as árvores que montam uma action string não
  achavam a string: o sweep do AL passa de 364 falhas "Add/Change the Action
  String: no STR#" para **47**, e no total de **12.915.916 → 12.917.222
  instruções, 18.712 → 19.007 true e 13.783 → 13.485 falhas**.
- **0x69/0x6B Animate Object/Overlay** e **0x6A Animate Sim**: animação u16
  0; flags byte 2: 0x01 espelhada, 0x02 velocidade no temp 2, 0x04 índice num
  parâmetro ("anim id in s.v."), 0x08 interrompível, 0x10 começa na tag do
  temp 0, 0x20 (Sim: "Trans to Idle"; objeto/overlay: repetições no temp 1),
  0x40 sem blend de saída, 0x80 sem blend de entrada; árvore de evento u16 4
  com o lugar s8 7 (Sim) / s8 9 (objeto) / s8 14 (overlay); lista byte 6 (Sim)
  / byte 9 (overlay): 0x80 global, 0x81 adulto, 0x82 criança, 0x83 social,
  0x89 bebê que anda, 0x8A adolescente, 0x8B idoso, 0x91 bebê, 0x8C gato,
  0x8D cão (os dois já existem no exe base 1.0); 0x99/0x9A filhote de cão/
  gato: **só BV**; objeto (dono 6, dado u16 7)
  (objeto/overlay). Sim: flags byte 8 (0x01 espelhamento no temp 3, 0x02
  sincroniza com a última animação, 0x10 usa o objeto controlador como fonte,
  0x20 não apressável), objeto IK (dono 9, dado u16 10), prioridade s8 12 (0
  baixa, 1 média, 2 alta). Objeto: flags byte 10 (0x01 espelhamento no temp 3,
  0x04 sincroniza com a animação de quem chama, 0x08 alinha o fim, 0x80 não
  apressável). Overlay: flags byte 15 (0x01, 0x10, 0x20 como acima, 0x40 usa a
  lista do dono da árvore), byte 12 bit 0 não apressável, prioridade s8 11.
- **0x6C Animate Stop**: modo s8 7 (0 uma animação: u16 0 da lista byte 6;
  1 overlays, 2 corpo inteiro, 3 todas, 4 poses de carregar, 5 idle, 6
  gestos, 7 reações, 8 normais, 9 faciais, 10 faciais idle, 11
  receptividade); objeto (dono 3, dado u16 4); flags byte 2 (0x01 espelhada,
  0x02 com blend, 0x04 o índice do modo 0 está no parâmetro u16 0 ("anim id
  in s.v.", como no Animate Sim/Overlay), 0x20 blend curto, 0x08
  espelhamento no temp 3, 0x40 para as normais e as espelhadas); prioridade
  s8 8 (0 qualquer, 1 baixa, 2 média, 3 alta). A 0x04 vem dos dados (rodada
  20, r20-stand): "Breathe - Overlay - Stop All" (`0x7F074B38`/`0x205A`) põe
  o parâmetro 1 = 0x44, 0x6A, 0x7E, 0x96 antes de cada Animate Stop
  `[01 00 54 …]`, e "Stance - Stop" (`0x201B`) para a pose de conversa do
  parâmetro 0; lida como índice literal, o "breatheLoop50" de pé (overlay
  de prioridade 1, nível 3) e as poses "…Pose-SWAT" (deltas na coluna)
  ficavam depois da conversa por cima de andar, assentos e Animate Sim
  (Sim inclinado, andando de pernas retas, "em pé" na poltrona).
  `docs/screenshots/r20-stand-antes-depois.jpg`.
- **0x6D Change Material**: material u16 0; byte 2: 0x08 o material é
  procurado no objeto (dono 8, dado u16 9) em vez de em mim, 0x10 índice do
  material no temp 0, 0x02 tabela do grupo global, 0x04 do semi-global
  (rodada 13); 0x01 o grupo de malha é
  procurado no objeto (dono 8, dado u16 9), 0x20 índice do grupo no temp 1,
  0x40/0x80 tabela global/semi-global do grupo (o nome do grupo não é
  usado no BV, §4g); grupo de malha s16 6 (< 0 = o
  modelo todo); afeta (dono 5, dado u16 6); byte 13: 0x01 índice de "Moving
  Texture" em vez de material, 0x02 usa o material gerado por snapshot.
- **0x6E Look At**: flags byte 0 (0x01 parar, 0x80 câmera, 0x02 sem saída
  antecipada, 0x04 inclui a coluna, 0x08 velocidade no temp 1, 0x10 duração
  no temp 0, 0x20 ignora cômodo, 0x40 ignora o frustum); alvo (dono 1, dado
  u16 2); velocidades byte 4 / byte 5 × 2 °/s; slot byte 8 do tipo s8 14 (0
  altura 3/4, 1 alvo, 2 rota, 3 contenção); árvore u16 9 com lugar s8 11;
  byte 15 (0x01/0x02 velocidade de volta nos temps 1/2, 0x04 não apressável).
- **0x6F Change Light**: objeto (dono 2, dado u16 3); luz s8 8 (−1 todas);
  duração u16 5 (ou temp 1 com byte 1 & 0x01); intensidade byte 7 % (ou temp
  0 com byte 1 & 0x02).
- **0x70 Effect Stop/Start**: modo byte 0 (0/1 começa suave/forte, 2/3 para
  suave/forte, 4/5 para todos, 6 dispara e esquece, 7 interroga o osso, 8
  para tudo e limpa a fila, 9 para TODOS, 10–13 estado 1–4, 14 para TODOS
  suave); objeto (dono 1, dado u16 2); efeito u16 4 (byte baixo −1 = o efeito padrão);
  slot byte 6 do tipo s8 9; flags u16 10 (0x01 STR# 0x8F global, 0x02
  semi-global, 0x08 ID do efeito no temp 0, 0x40 temp 1 passado ao início,
  0x80 fila de prioridade, 0x04/0x10/0x20/0x400/0x800/0x1000/0x2000 ícones);
  (dono 12, dado u16 13) = variável do ícone; byte 15 = entrada do STR# 0x95
  (atlas do ícone). Detalhes em `docs/effects-mac.md` §2.
- **0x71 Snap Into**: objeto (dono 0, dado u16 1) no objeto (dono 3, dado u16
  4), slot byte 6 (ou temp 0 com 0x01); flags byte 9: 0x01 slot no temp 0,
  **0x02 só testa ("TEST ONLY")**, 0x04 reseta os ossos raiz. Resolve o
  "0x02 not understood" do `routing.cpp`.
- **0x72 Assign Locomotion Animations**: modo u16 0 (0 tira todos, 1 tira o
  último, n ≥ 2 empilha o grupo n − 1); byte 2: 0x02 semi-global, 0x04
  global (senão privado).
- **0x73 Debug**: ação byte 13 (0 imprime, 1–7 janelas/cheats), quatro (dono,
  dado) em 0, 3, 6, 9, byte 12, flags byte 14.
- **0x75 Age**: idade byte 0 (0 adulto, 1 criança, 2 bebê que anda, 3
  adolescente, 4 idoso, 0xD bebê, 0xE jovem adulto — **0xE só BV**: "Young
  Adult" não existe no exe base 1.0), byte 1 bit 0 = idade no temp 0.
- **0x76 Array Operation**: de quem byte 2 (0 meu, ≠ 0 do stack object);
  array u16 3; operação byte 1 (0 limpa, 1 tamanho, 2 redimensiona, 3
  preenche, 4/5 insere na frente/atrás, 6 insere no iterador, 7/8 tira da
  frente/de trás, 9 tira no iterador, 10 avança o iterador até o valor, 11
  troca, 12/13 ordena); (dono 5, dado u16 6) e (dono 8, dado u16 9).
- **0x77 Message**: ID da mensagem (dono 15, dado u16 1); prioridade byte 8;
  alvo: com byte 4 & 0x02 um alvo (dono 5, dado u16 6), senão o conjunto byte
  3 (0 Sims selecionáveis, ..., 6 objetos, 7 tudo) e o filtro byte 0 (0 mesmo
  cômodo, 1 mesmo nível, 2 lote, 3 dentro, 4 fora); dados (dono 9, dado u16
  10) e (dono 12, dado u16 13).
- **0x79 Change Outfit**: flags byte 0 (0x10 reconstrói a roupa atual, 0x01/
  0x02/0x40 fonte: GUID u32 4, stack object, temps 0/1; 0x04 índice numa
  variável (dono 1, dado u16 2), senão byte 8 (STR# 0xFA); 0x20 limpa os GUIDs
  dos dados de pessoa; 0x08 grava no arquivo); Sim (dono 9, dado u16 10).
- **0x7C Want Satisfy**: Sim (dono 0, dado u16 1), desejo u32 3; tipo do
  desejo (0 valor, 1 pessoa, 2/3 objeto, 4 habilidade, 5 carreira) com (dono 7,
  dado u16 8) e nível (dono 10, dado u16 11).
- **0x13 Make New Character**: sem byte 9 & 0x01: idade/sexo/cor da pele nos
  locais dos bytes 1, 2, 0; com 0x01: bebê dos pais (dono 6, dado u16 7) e
  (dono 3, dado u16 4), 0x02/0x04 por neighbor ID; 0x08/0x10 dados do objeto
  com GUID nos temps 0/1 / no token; 0x20 personagem do "Bin"; 0x40 GUID
  externo para a roupa da miniatura (0x80: nos temps 2/3, senão u32 10).

## 5b. Conferência com o jogo base (Mac 1.0)

O executável de Mac 1.0 (`/mac/sims2_base_1.0.ppc` no mesmo projeto Ghidra,
auto-analisado) não tem nomes, mas tem RTTI: `paramvtables.py` acha as 64
classes de operando dele, e o `OnTryElement` (0x00B3A8F4, slot do vtable de
`cTSObject`) dá os handlers. Binário (base):

- O despacho é o mesmo do BV para 0x00–0x7C (0x0A também devolve verdadeiro);
  0x7D Follow Sim e 0x7E Run Script não existem.
- `SwizzleParams` dá **a mesma largura de campos** que no BV para todas as
  classes, menos: Set to Next (no base só o GUID u32 0: os u16 9 e 11 do
  filtro "where" são **só BV**) e Add/Change the Action String (o u16 14 é só
  BV); Run Tree (chamada de árvore) muda a conversão de versão.
- Classes com conversão de versão no base: Animate Overlay, Dialog, Look At,
  Relationship e Run Tree. **O BV acrescenta versões novas de Alter Budget,
  Add/Change the Action String, Manage Inventory, Change Material e Set to
  Next**: nessas o BV tem campos que os BHAVs do base (versão 0) não têm
  (zerados no load).
- Conferidos iguais ao BV no código do base: Relationship (inclusive 0x08 e
  "dois Sims = pelos neighbor IDs"), Create New Object Instance (0x20, 0x40,
  0x80, erro 0x36), Test Object Type (`0x4C7CAB2B`, flags 1/2/4), Random
  Number (limite 0 → 0), Find Location For (o modo 0 tenta o mesmo cômodo e
  depois qualquer lugar), Play Sound (tabelas por 10000/20000), Set to Next
  (alvo explícito no byte 4 bit 7; tipos 0–37, sem filtro de espécie).
- Run Functional Tree: a tabela é a mesma, mas no base a função 27 "Global
  Awareness" (→ 0x35) não existe (dá falso).
- Set to Next: nos tipos que percorrem listas (1 pessoa, 4/18 GUID, 3, 16, …)
  **o alvo vira 0 quando nada é achado** (base e BV). Contradiz o "Set to
  Next sem achar não mexe no alvo" do `docs/TODO-HISTORICO.md`.
- Strings do editor que só existem no BV (ausentes no exe base): "Young
  Adult", "Kitten list"/"Puppy list", "Scratch-Chew", "including disabled
  objects", "To Sub Queue", "Disabled (NonPropagating)", "Receptivity
  Animations", "Transportation Picker", "Hire Employees", erros 0x98–0x9E
  ("Another cinematic…", "Bad family id", "Invalid species", …).

## 6. `tools/symxfer`: como os nomes chegaram ao binário

O executável só tem ~20 mil símbolos (externos e weak, todos no
`__textcoal_nt`); o `__text` (25 MB, 33.101 funções) não tem nome nenhum.
`references/sims2_mac_decomp/baserom.sym` lista 82 mil funções de **outro**
build ppc do mesmo port (dezembro de 2007), ligado de outro jeito (inlines
no meio do objeto, objetos em outra ordem). O que liga os dois é o
`__eh_frame`: uma FDE por função, com início e tamanho exatos.
`tools/symxfer/symxfer.py` alinha, por seção, a sequência de tamanhos das
nossas funções com a do `baserom.sym` (âncoras: nomes iguais no coal e 4
tamanhos seguidos únicos nos dois lados; cadeias de âncoras; DP entre elas;
funções que mudaram de lugar dentro do objeto; thunks `addi r3,r3,-k; b
alvo` nomeados pelo alvo).

Cobertura (rodada 3): 22.461 das 33.101 funções do `__text` (68%). **Rodada
9**: com as vtables (RTTI) e um segundo alinhamento sobre os nomes já
postos, **27.300 (82,5%)**, `exact` confirmado pelos thunks em 95%; detalhes,
classes de confiança e medidas em `docs/symxfer.md`. A tabela de despacho
(§2) bate com o nome de todos os 40 handlers que ela chama, e cada classe de
operando bate com a primitiva. Há erros isolados mesmo em `exact`: conferir
pelo código antes de confiar num nome.

- `tools/symxfer/apply_clone.sh <pasta>`: clone do projeto com os nomes
  atuais (`docs/symxfer.md` §1).
- `tools/symxfer/run.sh [mapa] [--ghidra]`: gera o mapa (`addr nome
  confiança`, ~5 MB, fora do repositório) e, com `--ghidra`, reimporta a
  fatia ppc em `~/sims2:/mac/sims2_bv.ppc` aplicando os nomes
  (`ApplySymxferMap.java`). `map_simantics.txt` é o recorte da VM.
- `manual.txt`: nomes postos à mão (com a justificativa);
  `paramvtables.py`: vtables das classes de operando pela RTTI (nomes de
  `SwizzleParams`, `UpgradeVersion`, ...).
- `vtables.py`: vtable de uma classe com o nome de cada slot (para resolver
  `(**(code **)(*this + 0x4d4))`); `annotate.py` põe esses nomes no C do
  Ghidra; `DecompileList.java <saída> <padrão|0xEND>...` decompila em lote.

## Apêndice A. Códigos de erro da VM (`cTSObject::GetErrorString`)

| código | texto |
|---|---|
| 0x0 | No Error |
| 0x1 | Bad operator. |
| 0x2 | Temporary reference out of range. |
| 0x3 | Bad parameter to update primitive. |
| 0x4 | Random number range evaluated to zero. |
| 0x5 | Primitive not supported for this type of object. |
| 0x6 | Attribute number out of range. |
| 0x7 | Data number out of range. |
| 0x8 | Stack number out of range. |
| 0x9 | Illegal owner field in data reference. |
| 0xa | Reference to tree tree parameter when no parameter exists. |
| 0xb | Gosub?? |
| 0xc | Reference to target object when none exists. |
| 0xd | Flag reference is out of range. |
| 0xe | Undefined transition found. |
| 0xf | Reference to stack object out of range. |
| 0x10 | Bad alert tree number. |
| 0x11 | Bad gosub tree number. |
| 0x12 | Zero distance in finding direction. |
| 0x13 | Expression attempting to write in a read-only area. |
| 0x14 | Data value out of range. |
| 0x15 | No stack object present when required. |
| 0x16 | Reference to tree table entry that does not exist. |
| 0x17 | Could not find object reference. |
| 0x18 | Standard number for a weight assignment is out of range. |
| 0x19 | Weight number for a weight assignment is out of range. |
| 0x1a | Motive number out of range or not an acceptable value. |
| 0x1b | Slot number out of range. |
| 0x1c | Object referred to must be a person. |
| 0x1d | Balloon Type Is Invalid. Values are 0:thought, 1:scream, 2:speak. |
| 0x1e | Search type is invalid. Types are 0: all motive, 1:mental motives, 2:physical motives. |
| 0x1f | Tree table not found |
| 0x20 | Bad target slot specified in routing slot. |
| 0x21 | Person data field is out of range. |
| 0x22 | Show hide parameter is out of expected range. |
| 0x23 | Index into object definition out of range. |
| 0x24 | Invalid snapping mode specified |
| 0x25 | Invalid routing slot specified |
| 0x26 | Divide by zero. |
| 0x27 | Bad parameter in look towards primitive. |
| 0x28 | Bad room id specified |
| 0x29 | Bad room value requested. |
| 0x2a | Invalid global sim call. |
| 0x2b | Named tree does not exist. |
| 0x2c | Bad parameter to call named tree. |
| 0x2e | Invalid constant. |
| 0x2f | Invalid suit location. |
| 0x30 | Invalid suit index. |
| 0x31 | Invalid dynamic sprite index |
| 0x32 | Invalid object id. |
| 0x33 | Local out of range. |
| 0x34 | Stack object for routing either does not exist or is out of world. |
| 0x35 | Invalid action in tutorial primitive. |
| 0x36 | More than one passing value specified. |
| 0x37 | Missing neighbor for data access. |
| 0x38 | Bad expense type specified. |
| 0x39 | Career id not found. |
| 0x3a | Job data not found. |
| 0x3b | Invalid skin color. |
| 0x3c | Neighborhood data index out of range. |
| 0x3d | Start object required. |
| 0x3e | Tree break encountered. |
| 0x41 | 3D object is not animatable. |
| 0x42 | 3D object is not a person. |
| 0x43 | Animation name is empty string. |
| 0x44 | IsGlobal flag not implemented yet. |
| 0x45 | Material Change on Non 3D object. |
| 0x46 | Missing Material / Mesh Group Name. |
| 0x47 | Animation Primitive called on non Animating object - look at 3DObjectID in obj def. |
| 0x48 | Change graphic expression called on non 3D object |
| 0x49 | Event Tree handler called on deleted object or invalid object ID |
| 0x4a | Trying to use a Global Animation on a non Edith Person object. Naughty naughty! |
| 0x4b | Trying to look_at a non 3D object |
| 0x4c | Trying to change a light on a non 3d Object |
| 0x4d | Missing Light Name |
| 0x4e | Requesting to play an animation on an object for the third time |
| 0x4f | Missing Effect Name |
| 0x50 | Trying to change an effect on a non 3d Object |
| 0x51 | Turn rate must be positive |
| 0x52 | Trying to change an effect on an object with too many Particle Emitters |
| 0x53 | Missing requested bone for slot |
| 0x54 | No bone on model corresponding to name in routing slot on destination object |
| 0x55 | Can't place in world for some reason |
| 0x56 | Temporary token field out of range. |
| 0x57 | Inventory doesn't exist for ID passed to Manage Inventory. |
| 0x58 | Index passed to Manage Inventory out of bounds. |
| 0x59 | Bad parameter value set in Manage Inventory Primitive. |
| 0x5a | Trying to perform an animation from the Social Interaction list against a non person. |
| 0x5b | Hit Break Point Primitive. |
| 0x5c | Missing table for model bone names. |
| 0x5d | Missing (or empty) model bone name. |
| 0x5e | Missing Action String in table. |
| 0x5f | 3D object is not locomotable. |
| 0x60 | Invalid pop: locomotion stack is empty |
| 0x61 | Locomotion stack is too deep: check for imbalanced push/pops |
| 0x62 | IK target animation requires other object |
| 0x63 | IK target animation requires 3D model on other object |
| 0x64 | IK target animation cannot find bone on other object |
| 0x65 | IK target animation cannot find bone on self |
| 0x66 | Trying to modify variable in const type tree |
| 0x67 | Requesting a look_at operation that is already running on the object |
| 0x68 | The relationship matrix requested could not be found. |
| 0x69 | Cant run Reach/Drop on non Sim object. |
| 0x6a | No Object for Reach to work on. |
| 0x6c | Bad Object Array |
| 0x6d | Trying to access array element via iterator that does not exist. |
| 0x6e | Reach animation does not contain event 0. |
| 0x6f | Trying to push nested interaction on someone not in an 'allow nested' interaction |
| 0x70 | Invalid number of loops |
| 0x71 | Missing reach animation. |
| 0x72 | Missing Job Title / Phone Services Table (table 147) in Object. |
| 0x73 | Invalid animation type specified in animation string |
| 0x74 | Inventory token index out of range. |
| 0x75 | Inventory token propert index out of range. |
| 0x76 | Request for babies count out of range. |
| 0x77 | Temp0 does not point at a Sim ObjectID. |
| 0x78 | Bad destination for a 32bit write. |
| 0x79 | Could load the specified memory icon. |
| 0x7a | You sent me a crappy GUID, please fix it. |
| 0x7b | Trying to route while out of world. |
| 0x7c | Not enough local vars. |
| 0x7d | Bad or empty button or window GUID. |
| 0x7e | Animation speed cannot be zero |
| 0x7f | Couldn't load memory icon |
| 0x80 | Trying to route while in an pre route callback. |
| 0x81 | Trying to route while in an pre route callback. |
| 0x82 | Trying to route while in an post route callback. |
| 0x83 | Trying to run default effect - not supported anymore. |
| 0x84 | Invalid Lot ID. |
| 0x85 | Hit data break point - Value Changed! |
| 0x86 | Person is not selectable. |
| 0x87 | Cinematic scene not found |
| 0x88 | Cinematic animation not found |
| 0x89 | Required cinematic actor is missing |
| 0x8a | Required cinematic actor is invalid |
| 0x8b | Required cinematic actor has no 3D model |
| 0x8c | Cinematic origin actor (for camera and light rig) is invalid |
| 0x8d | Cinematic origin actor (for camera and light rig) has no 3D model |
| 0x8e | Cinematic autoCreate actor is not 0 |
| 0x8f | Trying to animate a cinematic actor that is not animatable |
| 0x90 | Trying to create instance of Template Object |
| 0x91 | Last Find Best Action was Not Successful. Quit trying to run gosub. |
| 0x92 | Attempt to change light on object that has no model. |
| 0x93 | Attempting to Save UI Mode to mode it's already saved at. |
| 0x94 | Attempting to create object type that's been unlinked (ie is dead, and has had data removed from it's .package file). |
| 0x95 | Attempted too many look_at requests in one simulator turn on this Sim |
| 0x96 | Target object out of world. |
| 0x97 | Unknown cinematic error. |
| 0x98 | Another cinematic already is active. |
| 0x99 | Cinematic camera does not have a choreo queue. |
| 0x9a | Bad family id. |
| 0x9c | Trying to save a temp token with instance data into inventory. |
| 0x9d | Trying to overwrite an inventory token with instance data. |
| 0x9e | Invalid species. |
| 0x9f | Bad number of function parameters. |
| 0xa0 | NULL tree table entry. |
| 0x3e8 | Stack overflow. |
| 0x3e9 | Stack underflow. |
| 0x44c | Undefined Transition. |
| 0x44d | Too many iterations. |
| 0x44e | Behavior not found. Probably a reset() problem. |
| 0x44f | Tree not found. Probably a reset() problem. |
| 0x450 | Transition to node that does not exist. |
| 0x451 | Check tree primitive blocked completion. |
| 0x452 | Max stack depth exceeded. |

Os outros códigos até 0x452 caem em "Unrecognized error condition : %d". Binário.

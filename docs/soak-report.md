# Soak report (round 20, o subconjunto com o índice da instalação)

Rodada curta (23/10) para conferir as mudanças da véspera — o `autonomy` passando a
indexar a **instalação** inteira (como o app), as chamadas **54–84** da Generic Sims Call,
o **tipo 13** do Set to Next e o `notifyRoomChanges` por conjunto —: 4 lotes, **1 dia de
jogo** cada (43.200 ticks, `--every 1800`, `--name-babies`, `S2PKG_ERRORS=1`), com
`OS2_DATA=...\Apartment Life` (o índice pega todos os packs). Não é a rodada dos 69 lotes:
é a verificação de regressão destas mudanças, e o que ela deixa aberto está no fim.

| Lote | Resets / orçamento | Detalhe |
|---|---|---|
| N001_Lot11 | 106 / 0 | 21 "no stack object" (Run Tree by Name), 20 "attribute 8 of 8" (o controlador do assistente social), 11 "stack object 0 does not exist", 5 break points; os Sims agem (17/94/25 buscas) |
| N001_Lot68 | 103 / 20 | 30 "Snap Into" no carro da empregada (`Car - MaidMobile - 0,8`, "Sub - Create Driver"), 20 orçamento no **gambá** do AL (0x7FFC7F75), 20 "attribute 8 of 8", 20 "no stack object"; os Sims agem (76/47/39/63 buscas) |
| N002_Lot10 | 111 / 0 | break points (6x cada), 20 "attribute 8 of 8", 20 "no stack object"; os Sims agem (61/78/52/12 buscas) |
| E001_Lot127 | **0 / 0** | o prédio de apartamentos do AL roda um dia inteiro limpo |

Nenhum crash e nenhum hang. O `bhav-sweep` do AL fica **igual** (14.093.824 / 23.864 true
/ 6.385 falhas) e o do base também (7.234.312 / 11.107 / 2.461); build + ctest 39/39.

**Cuidado ao comparar com os dados do base**: um lote com objetos de EP (o 68 tem os
controladores do Seasons, do AL) rodado só com o `Double Deluxe\Base` acusa "Invalid
constant" neles (as BCONs do EP não estão lá) — a comparação válida é com a instalação
inteira (o que o `autonomy` passou a indexar em 22/10, `b720d14`).

**O que a rodada deixa aberto** (âncoras para a próxima):

- o **gambá** (`NPC - Unique - Skunk`, grupo 0x7FFC7F75) gasta o orçamento de 100.000 em
  "Skunk - Wander Like An Idiot" e "Find Something To Eat": a árvore anda pelos objetos
  **não-pessoa** com `Set to Next 2` (alvo = o stack object, dono 0x0A) e testa o dado 29
  (sala) em 0 (instruções 2/3 e 22/23); o laço não termina no port — ver o `Set to Next 2`
  com esse alvo num lote com muitos objetos fora de sala (o 68).
- o **carro da empregada** (`Car - MaidMobile - 0,8`): "Sub - Create Driver"
  (`0x7F793929/0x101D`) falha no **`Snap Into` #4** com o stack object 0. A cadeia: #10 põe o
  stack object em "o objeto no meu slot 0" (dono 0x14), que está vazio → #16 (global 0x14B)
  → #8 (global 0x185 "NPC - Create OOW - ID in T[0]") → #9 (`stack object := temp 0`) → #3
  (`my attribute 0 := o stack object`) → #4. O `OS2_TRACE_NAMED=1` (novo, mostra grupo e
  instância) mostra o global 0x185 chamando "CT - Generate NPC From Pool"
  (`0x7FB55274:0x1008`), que despacha por uma **constante** (dono 0x1A, tabela 50 do global,
  entradas 43/54/55/68/76) e cai no privado **"NPC - Create Out Of World - ID in T[0]"**
  (`0x1019`): esse mexe no inventário do pool (Manage Inventory, o token `0x4CB4FDC6`), cria
  o NPC e na **#28 (`temp 0 := o stack object`)** é que o ID iria para o temp 0. No port o
  caminho termina na **#18 com `false`** (a árvore volta false) e o stack object segue 0 —
  o NPC não é criado. O `OS2_TRACE_INV=1` (novo, mostra cada Manage Inventory) mostra que a
  árvore **não chega** ao `Manage Inventory` do token do pool (`kind 3`, `op 0`, GUID
  `0x4CB4FDC6`): as únicas chamadas do run são `kind 1`/`op 3, 6, 7` no GUID `0x4CB2BCF7`.
  Então o `false` vem antes (a cadeia `#0..#35`, a `#10 param 0 == 1`, e o despacho do pool
  pelas constantes do dono 0x1A, tabela 50 do global). Próximo passo: a instrução exata (o
  `--trace` do `autonomy` só cobre Sim) e o que `#1009`/`#100C` do pool devolvem. São 30
  reinícios das peças do carro em ~1 minuto às 07:16. **A causa raiz é o `0x7E`** (ver
  `data-findings.md` §21): "Array - Fill With Valid NPCs" pergunta "NPC - Is Type?"
  (`0x3AD`, uma instrução só, o `0x7E`) e com a recusa **nenhum** NPC é do tipo — o array
  fica vazio, a árvore volta `false` e o pool não cria o motorista. Fechar isso é o
  interpretador de Lua + a API dos scripts (`.objLua`), a maior peça que resta do AL.
- **"attribute 8 of 8"** (assistente social) e **"stack object attribute 10 of 9"**
  (telefone): contagens de atributos **menores** no estado do que as árvores do AL leem. O
  `cTSObject::Read` adota a contagem salva (o port faz `assign`); falta um save do jogo
  nunca tocado para separar "o jogo cresce o array" (bug do port) de "estado do lote".

# Soak report (round 19, o pacote de lote vazio)

O item 8: o `N002_Lot32` (o stub de 96 bytes, sem nenhum recurso) deixava de
ser um erro de carga. `Lot::load` agora distingue o pacote vazio
(`EmptyLotPackage`, em `formats/lot.hpp`) do erro "no elevation array", e o
`tools/soak.sh` conta os lotes vazios à parte (`empty|autonomy|...`,
`empty|lot-save|...`), sem os transformer em `error|`.

Rodada só do lote afetado (`tools/soak.sh --one N002_Lot32`, `SOAK_TICKS=1800`,
`SOAK_NO_APP=1`):

| Lot | Category | Where | Detail |
|---|---|---|---|
| N002_Lot32 | empty | autonomy | empty lot package: the lot package has no resources (the empty lot stub) |
| N002_Lot32 | empty | lot-save | empty lot package: the lot package has no resources (the empty lot stub) |

A linha "Load errors | 1 (N002_Lot32)" das rodadas 18 e anteriores é o histórico
de antes da mudança; a rodada completa dos 69 lotes (que a repete) fica para a
próxima rodada do soak.

# Soak report (round 18, visitors' side effects)

Runs of `tools/soak.sh` over the 69 lots (3 game days), headless (`SOAK_NO_APP=1`), 2 lots at a time, seeded (`OS2_SEED=1` and `2`), `S2PKG_FIRES=1`, `SOAK_KEEP_LOGS=1`. **Before** is main 3bcfe516 (r17-loops merged: the same numbers as its "after"); **after** is the `r18-visitfx` branch. Counts from the kept autonomy logs; "fire outbreaks" counts fires started while none burned.

| Category | before, seed 1 | after, seed 1 | before, seed 2 | after, seed 2 |
|---|---|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| Load errors | 1 (N002_Lot32) | 1 | 1 | 1 |
| Resets after an error, outside Tutorial_Lot3 | 1 | 0 | 3 | 0 |
| Instruction budget hit | 0 | 0 | 0 | 0 |
| Sims that died ("Falecer") | 23 | 5 | 17 | 20 |
| Sims stuck > 6 game hours | 15 | 17 | 16 | 24 |
| of them not sleeping, working or at school | 4 | 2 | 4 | 2 |
| Motives pinned at -100 > 6 game hours | 47 | 22 | 33 | 25 |
| Save round trips that differ | 1 | 0 | 1 | 0 |
| Autonomous interactions ended true / false | 3372 / 372 | 4381 / 311 | 3783 / 297 | 3820 / 298 |
| Visitors on the lots at the end (all lots) | 86 | 17 | 87 | 12 |
| "Fogo!" runs | 283 | 33 | 208 | 133 |
| Fires made / fire outbreaks | 509 / 10 | 134 / 3 | 567 / 10 | 421 / 8 |
| "Pisar" ended false | 98 | 62 | 84 | 41 |
| Longest "No Berço" of N001_Lot36's toddler | 32.3 h | 8.9 h (night) | 16.6 h | 8.9 h (night) |

What was found (docs/TODO-HISTORICO.md "Rodada 18"; `docs/primitives-mac.md` §4g "Rodada 18"):
- **Visitors used everything as residents.** The binary leaves out, for a Sim whose person data 32 "Person Type" is not 0, the TTAB entries without flag 0x0001 (`cEdithObjectTestSim::TestInteraction`); the VM had no such test, and every person made in play gets Person Type 1 in the game (the object-made event's handler) while the VM kept the 0 of the neighbor's home lot. Walk-bys came in, cooked TV dinners (N001_Lot49: Dina Caliente's dinner burned the kitchen), took the household's objects and stayed the night (14 h); now they pass in about half an hour, and the visitors left on the lots at the end fell from ~87 to ~15. All the fire outbreaks sampled in the after runs (`S2PKG_FIRES` now names what is near the first fire and who was using it) are residents cooking, the game's; deaths and pinned motives follow the fires and the seeds (seed 2 after: two burning households, N001_Lot68 and N003_Lot52, and ghosts that "die" again, another front).
- **"stack object N does not exist"** (Sims reacting to one who died and was taken away): the removal now resets a person whose current interaction object, sub-queue object or route target it was (`cTSPerson::OnCleanup`, the router's `DoObjectDeletedWhileWalking`), without the error.
- **The trash "Dispose" recursion to a stack overflow**: "CT - Route Blocked Animation" got its parameters swapped, returned at once, and a blocked Sim retried in the same tick (`TriggerBlockedAnimation`: pauses in 0, the blocker in 1).
- **The toddler stuck in the crib**: the crib's "Animate - Events - Get" runs on the overlay's object (the toddler), which it snaps into the carrier's hands on 9998; the VM ran overlay event trees as the Sim that started them. Now the toddler is carried and bathed (and the social worker takes him off the lot when it comes to that).
- **Round trips that differ**: an overlay's event-tree code compared by GUID, as the save keeps it.
- **"Pisar" failing in series**: result 7 (destination occupied): another Sim stands on the roaches' tile after stomping them (N002_Lot3, Loki); the game's goal test refuses it the same way. Fewer now with fewer Sims around.

# Soak report (round 17, loops)

Runs of `tools/soak.sh` over the 69 lots (3 game days), headless (`SOAK_NO_APP=1`), 2 lots at a time, seeded (`OS2_SEED=1` and `2`), `S2PKG_FIRES=1`, `SOAK_KEEP_LOGS=1`. **Before** is main 9e3b2f43 (with r17-saveanim); **after** is the `r17-loops` branch rebased on it (every neighbor's definition on again, so visitors and walk-bys come). Counts from the kept autonomy logs.

| Category | before, seed 1 | after, seed 1 | before, seed 2 | after, seed 2 |
|---|---|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| Load errors | 1 (N002_Lot32) | 1 | 1 | 1 |
| Household Mains reset | 0 | 0 | 0 | 0 |
| Resets after an error, outside Tutorial_Lot3 | 0 | 1 | 0 | 3 |
| Instruction budget hit | 0 | 0 | 0 | 0 |
| Sims that died ("Falecer") | 20 | 23 | 10 | 17 |
| Sims stuck > 6 game hours | 25 | 15 | 34 | 16 |
| of them not sleeping, working or at school | 1 | 3 | 2 | 3 |
| Motives pinned at -100 > 6 game hours | 22 | 47 | 15 | 33 |
| Save round trips that differ | 2 | 1 | 2 | 1 |
| Autonomous interactions ended true / false | 3518 / 287 | 3372 / 372 | 4311 / 365 | 3783 / 297 |
| "Tomar um Drinque" ended false | 23 | 27 | 44 | 15 |
| Teddy bear "Fazer de conta..." ended false | 0 | 0 | 0 | 0 |
| Visitors on the lots at the end (all lots) | 13 | 96 | 9 | 107 |
| "Fogo!" runs | 104 | 283 | 102 | 208 |

Intermediate builds (not in the table): every neighbor's definition on with only the queue fix, seed 1: 2 budget hits (N001_Lot68, the ghost's "Function - Goal Check"); with the object list for every Set to Next type: 28 budget hits (N001_Lot49's "Dartboard - Shoo Sims" and others); N001_Lot36 seed 3 before the queue fix: 9 budget hits and a 15-minute run (a carry object a pass).

What changed (docs/TODO-HISTORICO.md "Rodada 17 (ramo `r17-loops`)"; `docs/primitives-mac.md` §4g "Rodada 17"):
- **The toddler-bath loop** (N001_Lot36): Push Interaction refused the "continue as current" pushes of "Pickup - Do Carry Sim" onto a full queue (the toddler in the crib with six "*Push Look At" waiting); the binary tests the queue and sub-queue limits only without the continuation (`TryPushAction`, the interaction's `SetContinuation`). "Do Carry Sim" returned false after making a carry object and pushing the carry onto the adult, and the bath tree went round without a yield. The continuation is saved now (bit 0x02 of the interaction's +0x54), and an object made with no free ID is a VM error instead of a wrapped ID (the crash).
- **Every neighbor's definition** in every GUID lookup again: the visitors come (about ten times as many Sims on the lots at the end).
- **Set to Next's order**: the object types walk the object module's list (the lot's ObjM order; objects made in play in front, inferred), people and a GUID's instances their own vectors, where a current object that is not one ends the search. Fixed the ghost's loop over the stereo's two parts (N001_Lot68) and kept "Dartboard - Shoo Sims" ending.
- **hood-save** writes the family membership Generic Sims Call 4/6 change.
- **"Tomar um Drinque"** failures are the bar trees redirecting to the glass the tray's slot order gives (the data's; each is followed by the real drink). The teddy bear loop did not come back in any run.

Left (TODO): the toddler in the crib that no one takes out ("No Berço" 16-32 h on N001_Lot36 after); "stack object N does not exist" in reactions to a Sim removed that tick (N001_Lot16, N001_Lot68); a trash "Dispose" recursion to a stack overflow (N001_Lot68 seed 2); one nested-push error (N002_Lot10); "Pisar" on roaches failing in series. The extra fires follow the visitors' and Sims' changed timelines; the ones looked at were food left to burn (a TV dinner at N001_Lot68), the game's. The pinned motives follow the deaths and fires. The round trips that differ (N003_Lot192, N001_Lot18 before) are an overlay's event object after a reload, also on main (the saved animation queue).

# Soak report (round 17, animations and routes resumed)

`tools/soak.sh` (`SOAK_NO_APP=1`, `SOAK_JOBS=2`, `S2PKG_FIRES=1`, 3 game days) on N001_Lot11, N001_Lot36, N002_Lot2, N002_Lot3, N003_Lot52, N001_Lot16, N003_Lot45 and N002_Lot10; **before** is main 76b04555, **after** the r17-saveanim branch (the lots' animation queues read: the overlays and Animate Sims of the game's saves come back at load, so the idle system gets its events). `hood-save N001 --ticks 43200 --check`: 24 of 26 lots the same after (the 2 left are a queue flag and a family number of the neighbourhood, `docs/save-mac.md` §7c), 26 of 26 before (the before build does not compare animations, overlays and routes).

| Category | before | after |
|---|---|---|
| crash, hang, error, dead, budget | 0 | 0 |
| reset | 1 (Juliette: stack object 326 does not exist) | 0 |
| died | 10 | 3 |
| stuck (> 6 h in one interaction) | 13 | 9 |
| pinned (a motive at -100 > 6 h) | 21 | 9 |
| save round trips that differ | 0 | 0 |

# Soak report (round 17, stuck and save)

`tools/soak.sh` on N001_Lot36, N002_Lot3, N001_Lot11 and N002_Lot2 (3 game days, headless, `S2PKG_FIRES=1`, `SOAK_KEEP_LOGS=1`, babies named), seeds 1 and 2, on the r17-stuck branch (main 8757a0c + the save fixes). The round-14/15 findings checked again:

| Finding | seed 1 | seed 2 |
|---|---|---|
| Brandi in "Segurar Bebê" (N001_Lot36) | gone | gone |
| "Jogar Fora" on the trash pile failing | 2, then "Limpar" | 0 |
| Instruction budget (100000), also in check trees (`OS2_CHECK_TREE_ERRORS=1`, N002_Lot3) | 0 | 0 |
| Social bunny / Ophelia "Sim on Tile" (N002_Lot2) | 0 | 0 |
| Save round trips that differ | 0 | 0 |
| Long skill loops (> 6 h) | Alexander, Cassandra "Tocar" (N001_Lot11) | Mortimer "Praticar", "Tocar" |

The skill loops are the trees' (`Exit Object - Motive Failing?`, `docs/primitives-mac.md` "Rodada 15 (sobrevivência 5)"). The one reset left is the bottle's "Animation name is empty string" (another front). `hood-save N001 --ticks 43200 --check`: 26 of 26 lots the same. The new `lot-save --continue N` runs the saving and the reloaded VM on together; what it found (motive fractions, running Set Motive Changes, trailing attributes) is fixed, and what is left (animations and routes restart at the reload) is in `docs/save-mac.md` §7b.
# Soak report (round 16, vm2)

Runs of `tools/soak.sh` over the 69 lots (3 game days), headless (`SOAK_NO_APP=1`), 2 lots at a time, `S2PKG_FIRES=1`, `SOAK_KEEP_LOGS=1`. **Before** is main 8142006 (seeds 1 and 2); **after** is the r16-vm2 branch rebased on it, seed 1 (seed 2 was cut short by the user's scope change: wrap up). An intermediate build that let every GUID lookup reach the neighbourhood's character files had a crash, 43 budget hits and 17 deaths on seed 1 (N001_Lot36's toddler-bath loop with visitors); the final branch keeps that lookup to data owner 0x26 and the sweep.

| Category | before, seed 1 | after, seed 1 | before, seed 2 |
|---|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 | 0 / 0 |
| Load errors | 1 (N002_Lot32) | 1 | 1 |
| Household Mains reset | 2 | 2 | 3 |
| Sims that died ("Falecer") | 7 (5 lots) | 7 (3 lots) | 21 (6 lots) |
| Sims stuck > 6 game hours | 28 | 30 | 25 |
| of them not sleeping, working or at school | 5 | 6 | 4 |
| Motives pinned at -100 > 6 game hours | 25 | 25 | 35 |
| Save round trips that differ | 0 | 0 | 0 |
| Instruction budget (100000) hit | 0 | 0 | 0 |
| Autonomous interactions ended true / false | 4040 / 376 | 3733 / 371 | 3466 / 403 |

Seeded autonomy on N001_Lot68, N002_Lot10, N001_Lot16, N001_Lot11 (seeds 1-8, 43200 ticks): main 8142006 true 2713 / false 346, 13 deaths, 2 resets; the branch 2728 / 275, 15 deaths, 1 reset. The Manage Inventory match flags with the GUID-0 rule alone (before the rebase): true 2926 -> 2920, false 676 -> 635, deaths 9 -> 9.

Error resets left in both (not engine bugs as far as read): "Controller - CLP - Community Lot - Populator" and "Controller - NPC Pool" (attribute 9 of 8) and "Controller - NPC" (16 of 16) on Tutorial_Lot3: the lot saved those objects with fewer attributes than their OBJD has now, and cTSObject::Read takes the saved count (BV 0xb14cb0), so the game errors the same way; "Template - Police Officer: Tree table not found" (N003_Lot192): "Party Break-Up - Try Push Turn Off Stereos" pushes onto a multi-tile stereo part without a TTAB, which cTSObjectSelector::GetTreeTable (BV 0x51edf0) also refuses (error 0x1F); which part the game's Set to Next finds was not checked. "Controller - Emit - Timing" did not reset in any of these runs.
# Soak report (round 16, autonomy and errors)

Runs of `tools/soak.sh` over the 69 lots (3 game days, 129600 ticks), headless (`SOAK_NO_APP=1`), 2 lots at a time, seeded (`OS2_SEED=1` and `2`), babies named, `S2PKG_FIRES=1`, `SOAK_KEEP_LOGS=1`. **Before** is main 71cfb0f; **after** is the `r16-autonomy-errors` branch rebased on it. "Resets" counts every "reset after an error" line in the kept autonomy logs (any object), "household Mains" only the household's.

| Category | before, seed 1 | after, seed 1 | before, seed 2 | after, seed 2 |
|---|---|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| Load errors | 1 (N002_Lot32) | 1 | 1 | 1 |
| Household Mains reset | 2 | 0 | 3 | 2 |
| All resets after an error | 117 | 94 | 105 | 96 |
| of them outside Tutorial_Lot3 | 23 | 0 | 11 | 2 |
| Sims that died ("Falecer") | 7 (5 lots) | 11 (6 lots) | 21 (6 lots) | 21 (11 lots) |
| Sims stuck > 6 game hours | 28 | 25 | 25 | 27 |
| Motives pinned at -100 > 6 game hours | 25 | 24 | 35 | 29 |
| Save round trips that differ | 0 | 0 | 0 | 0 |
| Instruction budget hit | 0 | 0 | 0 | 1 (N001_Lot36) |
| Failed autonomous interactions | 376 | 415 | 403 | 447 |
| Lookups of "~" label rows as animations | 29 | 0 | 35 | 0 |
| "Fogo!" runs | 32 | 74 | 125 | 165 |

Every reset and VM error in the logs, and what it is:
- **"Animation name is empty string"** with babies (before: N001_Lot36, N002_Lot4, both seeds): an engine bug, two of them. Animate Overlay from the global list read the list of the target's age; the binary uses the age of the Sim running it (`try_animate_overlay` calls `FindGlobalName` on its own animatable): "CT - Route Failure Feedback - Animation" plays the adult list's entry 90 "b2a-routeFail-carry-baby" on the baby in arms, and the baby's list has "." there. And "." was taken as an empty name: the binary only errors (0x43) on an empty one; "." is an animation that does not exist, nothing plays and the primitive returns true (a baby put down on the floor and asked to move played the baby list's "." at 84). Fixed; 0 after.
- **"Tree table not found (object 204)"** (N002_Lot4) and **(object 324)** (N003_Lot192, the police officer's "Party Break-Up - Try Push Turn Off Stereos", 20/8 resets): an engine bug. A multi-tile part whose tree table ID is -1 uses its master's (`GetEffectiveTreeTableID`), whatever its interaction group; the VM gave it only to group leaders and 0,0 parts without groups, and the changing table's "Change - Common - Subqueue" pushes the baby's entry from the 0,0 part (the lead tile, interaction group 1). Fixed; 0 after.
- **"tree 0x1008 not found"** (seen while testing, N003_Lot192, 21 a run): an engine bug. A removed object's thread went on as the new object given its ID (the school bus controller's Main as the bus). Fixed (object serials).
- **Lookups of "~Spine Overlays", "~Get Out", "~Snooze Animations"**: an engine bug. The binary does not skip "~" rows; it reads the list of the object whose behavior holds the running tree, not the stack object's (a drink's sit-carry event on a bar stool read the stool's " ~Get Out"). Fixed; 0 after.
- **Tutorial_Lot3: "Attribute number out of range"** (Community Lot Populator "9 of 8", NPC Pool "9 of 8", Controller - NPC "16 of 16"; 94 a run, both before and after): **the game's own data**. The tutorial lot was saved with older controllers (8, 8 and 16 attributes) than the shipped OBJDs (16, 16 and 20); `cTSObject::Read` (BV `0xb14cb0`) takes the saved count and `Reset` does not resize it (the "needs reset" of `ObjectSaveTypeTable::Read` only resets the object), so the game hits the same errors.
- **N001_Lot36 seed 2, after: "instruction budget exhausted"** (Dustin, "Interaction - Give Toddler a Bath"): the tub's tree loops between "Pickup - Get Sim Push Interaction" (its push with "force check tree" fails: the toddler is in Brandi's arms) and "Pickup - Drop Off" within one tick, making a carry object each turn; the binary's 100000-node limit would end it the same way: **the game's data** (inferred; the state that leads there is the seed's).
- **N001_Lot36 seed 2, after: "stack object 270 does not exist"** (Brandi, "Read - Loop Reading"): the book's attribute 4 keeps the listener (Beau) after the social worker took him; the binary does not clear other objects' attributes on removal either: **the game's data** (inferred).
- N002_Lot32 does not load ("no elevation array"): the package, as before.

What changed (docs/TODO-HISTORICO.md, "Rodada 16 (g)"; `docs/primitives-mac.md` §4g "Rodada 16 (autonomia e erros)"):
- **Candidate order of Find Best Interaction** as `FBA_FindStaticInteractions` builds it: motive by motive of the mask, each object's entries last first, dynamic interactions after all the static ones. Two entries with the same ads now break the qsort tie as the game does: the later entry first (the trash pile's "Limpar" before "Jogar Fora" in short lists; with 7 or more ranked candidates the qsort's partition still reorders ties: "Jogar Fora" on a pile failed 6 → 10 times on seed 1, N001_Lot36 2 and N003_Lot46 8, and 0 → 0 on seed 2). The objects' order within one motive is the hash table's (object pointer modulo the bucket count) and cannot be reproduced; object ID order stands in.
- **s2pkg budget**: every command uses the binary's 100000 nodes a turn.
- More failed interactions after: each sofa seat and crib part now offers its interactions (as in the game), and the teddy bear's dynamic "Fazer de conta que o bichinho fala com..." fails in a loop on N003_Lot192 seed 1 (8 → 113; the bear stays in the chooser's hand and the "Verify - Person ID" of its object data 2 fails; a pre-existing VM problem, TODO). Deaths are fires, as in round 15; the seeds move them.

# Soak report (round 15, planner)

Runs of `tools/soak.sh` over the 69 lots (3 game days, 129600 ticks), headless (`SOAK_NO_APP=1`), 2 lots at a time, seeded (`OS2_SEED=1` and `2`), babies named, `S2PKG_FIRES=1`, `SOAK_KEEP_LOGS=1`. **Before** is main cc8cfe1 (with r15-vm-confirm; the s2pkg autonomy budget 20000); **after** is the r15-planner branch rebased on it (the binary's budget, 100000). The "failed interactions", "Dispose on a pile" and "Fogo!" rows are counted in the kept autonomy logs.

| Category | before, seed 1 | after, seed 1 | before, seed 2 | after, seed 2 |
|---|---|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| Load errors | 1 (N002_Lot32) | 1 | 1 | 1 |
| Household Mains reset | 0 | 3 (3 lots) | 1 | 3 (1 lot) |
| Sims that died ("Falecer") | 29 (9 lots) | 7 (5 lots) | 20 (9 lots) | 21 (6 lots) |
| Sims stuck > 6 game hours | 27 | 28 | 27 | 25 |
| of them not sleeping, working or at school | 6 | 4 | 6 | 2 |
| Motives pinned at -100 > 6 game hours | 30 | 25 | 12 | 35 |
| Save round trips that differ | 0 | 0 | 0 | 0 |
| Instruction budget hit | 0 | 0 | 1 (Relationship Decay, 20000) | 0 (100000) |
| Failed autonomous interactions | 214 | 376 | 452 | 403 |
| "Jogar Fora" on a trash pile failing | 0 | 6 (2 Sims, then "Limpar") | 16 | 0 |
| "Fogo!" runs | 367 | 32 | 66 | 125 |

What changed (docs/TODO-HISTORICO.md, "Rodada 15, planejador"; `docs/routing-mac.md` §2 and §8.5, `docs/primitives-mac.md` §4g "Rodada 15 (planejador)"):
- **Pool exit**: the pool is level −1 with a floor; objects' portals as the binary gathers them (routing slots marked as a portal pair, SLOT flag 0x200000, joined by their "own,other" bone names): the ladder links the pool's bottom and the deck, the diving board the deck and the water. The portal trees get the two ends' levels as parameters 2/3 (they were the rooms), so the board's TEST locks the way up and the ladder's tree climbs out. N001_Lot68 (pie: Mary-Sue dives, then "Usar" the toilet upstairs): she swims to the ladder, plays "a2o-poolLadder-climbOut" and walks up; before she starved in the pool (seed 1, main c3dbe82). No Sim dove in these four runs (the seeds' fires and choices moved), so the soak does not show it.
- **Trash pile**: "Jogar Fora" (Dispose, wants the pile in hand; guard "Hide Menu") and "Limpar" have the same ads; the FBA sorts with the Mac libc's qsort (not stable) and the binary's comparator: N001_Lot36 seed 1, 20 failures in a row → 2, then "Limpar".
- **Planner admission**: stairs in by their footprints; the 0x4b = 0x93 exception (no base-game OBJD has it); the route penalty's scale has no writer (cost 1, as before).
- **Create New Object Instance** tests the place (TestPlaceInWorld, objects by footprints, after the Init): Fires on tiles without floor, ashes on doors and helpers on chairs are no longer made. The fire counts swing with the seed either way (N002_Lot22 seeds 2–4 with/without: 125/71, 0/162, 0/73 Fires).
- **Budget**: s2pkg autonomy and the app use the binary's 100000 nodes a turn; the Relationship Decay needs 22798 at N003_Lot192's tick 77371 (seed 2).
- The resets after are pre-existing: "Animation name is empty string" with the babies' bottles (N002_Lot4, N001_Lot36; also in the round-15 survival-5 logs) and "Tree table not found (object 204)". The deaths are still fires and what follows them (N001_Lot68 seed 2: three Sims burn, Daniel mourns and starves); the pinned motives follow the same lots.

# Soak report (round 15, survival 5)

Runs of `tools/soak.sh` over the 69 lots (3 game days, 129600 ticks), headless (`SOAK_NO_APP=1`), 2 lots at a time, seeded (`OS2_SEED=1` and `2`), babies named (`--name-babies`). **Before** is main c3911d1 (the round's branch point); **after** is the r15-survival5 branch rebased on main 1782c4d, run with `S2PKG_FIRES=1` and `SOAK_KEEP_LOGS=1` (new: keeps the autonomy logs).

| Category | before, seed 1 | after, seed 1 | before, seed 2 | after, seed 2 |
|---|---|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| Load errors | 1 (N002_Lot32) | 1 | 1 | 1 |
| Household Mains reset | 2 | 2 | 0 | 1 |
| Sims that died ("Falecer") | 9 (5 lots) | 12 (6 lots) | 22 (8 lots) | 20 (8 lots) |
| Sims stuck > 6 game hours | 35 | 35 | 23 | 24 |
| of them not sleeping, working or at school | 5 | 3 | 4 | 4 |
| Motives pinned at -100 > 6 game hours | 21 | 15 | 29 | 22 |
| Save round trips that differ | 0 | 0 | 0 | 0 |
| Instruction budget (20000) hit | 5 (2 lots) | 0 | 0 | 1 (N003_Lot192, "Controller - Relationship Decay") |

What changed (docs/TODO-HISTORICO.md, "Rodada 15, sobrevivência 5"; `docs/primitives-mac.md` Set to Next and "Rodada 15 (sobrevivência 5)", `docs/routing-mac.md` §2):
- **Three-Sim socials**: Push Interaction's link (flag 0x40) is a chain as `TryPushAction` makes it (the new one takes the linked one's link, the linked one points to the new one). "Social - Baby - Talk To" is A → C → B, so A ending tells the baby's holder: Brandi no longer holds the baby for 11 h.
- **Set to Next 9** is one object a side (the tile's last), +X, +Y, −X, −Y, going on from the target's side; **Set to Next 8** walks into the slots (a Sim on a chair, a plate on a counter), as the binary's tile iterator. "Set to Next - Good Dining Chair" no longer spins (N002_Lot22: 4 resets of the cooks a seeded game → 0; the budget hits of seed 1, 5 → 0), and the social bunny's "Don't Stand on Sims" sees the Sim on the chair and moves away (Olive/Ophelia's "Chair - Route Fail - Sim on Tile" for hours: gone).
- **Path planner admission** as `AddThisToPathPlanner`: only the ceiling filter (FootprintBlocksCeiling) is the binary's; the others (small things on surfaces, detached, wall-hung, "can be stepped over", landing pieces) are gone, each object blocks by its footprint; models without a footprint list by their meshes' box cut to the tile; the part's tile by GetLocalOffsetFromLeader (OBJD sub index; same tile as before on every bed and piano of the lots); EvalTileForGoal by footprints, not tiles. Half-soak A/B of this step alone: failed interactions 192/159 → 141/244 (seed 2: N001_Lot68's diving board is now reached and the divers cannot leave the pool; trash piles), resets 20/46 → 7/6.
- **Reset's free place** as FUN_00af87c8 (square spiral of tile centres, room first). A walker standing in a diagonal wall's cells escapes through them (N002_Lot4: Vidcund failed every route from a diagonal doorway and starved with Pascal in a seed-1 run).
- **Long loops are the trees'**: piano, easel, darts and the newspaper exit only on failing motives (hunger ≤ −50, energy ≤ −80, ...; the newspaper at fun ≥ 60); Alexander's 7.7 h "Tocar" ends at hunger −51. Not a bug.
- **Fires** checked against the engine-re reading: first spread try ≥ 900 ticks (seen ≥ 1117), tries ≥ 200 apart (≥ 219), ≤ 8 per Fire (≤ 5 seen), never a second Fire on a tile; lifetimes 37–4281 ticks with the trees' flame 73–176 and the 20 × Idle 30 wait. The deaths are still mostly fires (seed 1: 9 of 12; seed 2: all but Olive's old age), so the counts follow where the seed starts a fire.
- Left (TODO): the pool exit (N001_Lot68's Mary-Sue starves in the pool, seed 1), "Jogar Fora" on kicked-over trash piles failing over and over, stairs and route penalties still outside the planner.
- The remaining non-sleep/work stuck: piano/painting/darts loops (the trees' rules). Beau's social worker car (seed 2) and Lilith's "Desaparecido" (seed 1) did not recur.

# Soak report (round 14, survival 4)

Runs of `tools/soak.sh` over the 69 lots (3 game days, 129600 ticks), headless (`SOAK_NO_APP=1`), 2 lots at a time, seeded (`OS2_SEED=1` and `2`). **Before** is main b14e777 with its own `tools/soak.sh`; **after** is the r14-survival4 branch rebased on it, with its `tools/soak.sh` (which names the babies: `autonomy --name-babies`).

| Category | before, seed 1 | after, seed 1 | before, seed 2 | after, seed 2 |
|---|---|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| Load errors | 1 (N002_Lot32) | 1 | 1 | 1 |
| Household Mains reset | 1 | 2 | 2 | 0 |
| Sims that died ("Falecer") | 10 (7 lots) | 9 (5 lots) | 17 (9 lots) | 22 (8 lots) |
| Sims stuck > 6 game hours | 22 | 33 | 17 | 23 |
| of them not sleeping, working or at school | 4 | 5 | 3 | 4 |
| Motives pinned at -100 > 6 game hours | 31 | 21 | 26 | 29 |
| Save round trips that differ | 1 (N001_Lot80) | 0 | 1 (N001_Lot80) | 0 |
| Instruction budget (20000) hit | 31 | 5 | 5 | 0 |

What changed (docs/TODO-HISTORICO.md, "Rodada 14, sobrevivência 4"; `docs/routing-mac.md` §2, `docs/primitives-mac.md` §4f/§4g):
- **Object footprints** as the binary's `AddThisToPathPlanner`: the model's CRES "footprint" mask (16x16 a tile, halved to 1/8-tile cells) or the OBJD box, not the whole tile; the step test and Snap use them too. "Tocar" on N001_Lot11's piano: 43 failures a day to 0 (Cassandra now plays for hours). With the planner alone the step test (whole tiles) refused what the planner allowed: N002_Lot2 went from 22 failed interactions to 167; with the footprint step test and Snap, and a Snap in a portal resetting the locomotor (a modular stair's tree snapped Olive down and the walk carried her back to the top's point on the lower level, walled in), 9.
- **Births**: "Dar à Luz" waited for the name dialog (type 23, no time limit in the game either); the soak now names the babies. Pascal (N002_Lot4) and Brandi (N001_Lot36) get their babies.
- **N001_Lot80's round trip**: an evicted family's lot; the check's reload cleaned up after the eviction again (the save clears the flag).
- **Reset** as `cTSPerson::OnCleanup`: the person goes to a free place near; object data 7 = OBJD field 45.
- **Deaths** (`S2PKG_FIRES=1`): in the after runs every death but four is a fire (the Sim burning on a Fire's tile after "Fogo!"); two are old age (Olive, Patrizio, good motives), two starvation (Angela, Tybalt). One fire on a lot without a smoke alarm kills the whole family (N003_Lot46, N003_Lot52, N003_Lot192, N002_Lot4 and N002_Lot22 lose 3-4 each), so the counts follow where the seed starts a fire. The panic does take Sims 2 tiles away (global slot 9); they die because the fire spreads (10-32 Fires for hours) and the next panic, about another Fire, can send them onto a third Fire's tile (Fires allow person intersection, as in the game). Fires start in ~6.5% of cooks (unskilled cooks and forgotten food), per cook, not per tick.
- The stuck rise is sleep and work (Sims reach beds and leave for work more); the others: Cassandra's piano and painting (6-8 h), Brandi holding the baby in a three-person social whose starter left ("Person C - Baby Holder", 11 h), Beau's social worker car (seed 2), Lilith "Desaparecido" (in both).
- The budget hits left: N002_Lot3's "Set to Next - Good Dining Chair" check tree (`OS2_CHECK_TREE_ERRORS=1`).

# Soak report (round 13, survival 3)

Runs of `tools/soak.sh` over the 69 lots (3 game days, 129600 ticks), headless (`SOAK_NO_APP=1`), 2 lots at a time, all with this branch's `tools/soak.sh` (which ends a Sim's interaction when it is removed from the lot). Main moved twice during the round, so there are two pairs: **A** is main 98c5c85 against the r13-survival3 work rebased on it (without the Set to Next fix); **B** is main 494c8fe (with r13-anim-choreo and the walking merge) against the final branch rebased on it. The branch point (main 461ec3d) is given too.

| Category | 461ec3d | A: main 98c5c85 | A: + survival3 | B: main 494c8fe | B: + survival3 (final) |
|---|---|---|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| Load errors | 1 (N002_Lot32) | 1 | 1 | 1 | 1 |
| Household Mains reset | 1 | 0 | 0 | 3 (budget, N001_Lot11) | 1 |
| Sims that died ("Falecer") | 23 (10 lots) | 11 (7 lots) | 11 (8 lots) | 27 (10 lots) | 23 (10 lots) |
| Sims stuck > 6 game hours | 18 | 26 | 17 | 21 | 29 |
| Motives pinned at -100 > 6 game hours | 59 | 37 | 33 | 30 | 23 |
| Save round trips that differ | 68 lots | 68 | 1 (N001_Lot80) | 68 | 1 (N001_Lot80) |
| Instruction budget (20000) hit | 3 | 16 | 6 | 54 | 17 |

What changed (docs/TODO-HISTORICO.md, "Rodada 13, sobrevivência 3"; `docs/primitives-mac.md` §4g):
- **Round trips**: since the global sim objects (r12-life3) every `lot-save --check` differed: the curtain and the tutorial controllers delete themselves after running and the reload makes them again, as the game does. The check now skips the objects the reload made (`VM::globalsMadeAtLoad`). N001_Lot80 (an empty lot whose 125 global objects make children) still differs.
- **The shrubs** no longer reset every hour: `TryChangeMaterial` looks the mesh group's name up and never reads it, so a missing STR# 0x87 entry is no error.
- **Reset after an error** as `HandleError` → `Cleanup`: the Reset path zeroes the object data but for the binary's list and runs Init (fewer budget hits: a looping object starts clean).
- **The social worker's car** already left (r12-life3); the child goes with it (removed by "Sub - Send Passengers Out of World"); the "stuck in Levando a criança" findings were the soak's (3 of the 26 in A). With the walking merge (B) N001_Lot36's Beau is again kept in the car: to check with the walking front.
- **Set to Next's "where" filter** only on node version 1 (the base game's version-0 nodes have other bytes there): this broke the baby's care on N002_Lot10 since r12-survival2 (414ef03; bisected).
- Nobody calls the firefighters but the smoke alarm in the fire's room (game behavior).
- **The walking merge** (9d7607c, "the per-step placement test takes the Sims' bodies") raises the deaths: bisected on N001_Lot68 + N002_Lot10, 1 death at caf8207 and 8 at 9d7607c (B's 27 against A's 11). Outside this front (walking/routing).

# Soak report (round 12)

Run of `tools/soak.sh` over the 69 lots (3 game days of autonomy each, 129600 ticks), headless (`SOAK_NO_APP=1`), 2 lots at a time. "Before" is main b87a616 (the round-11 report's own code, re-run at the start of this round); "after" is the r12-survival2 branch rebased on main 1389fd8. The soak now also counts the household's Mains **reset** after an error (the game resets the object, `docs/primitives-mac.md` §4g); "dead" Mains no longer stay dead. "Died" counts every Sim that ran "Falecer" (they are now reaped: the Grim Reaper's Main no longer dies).

## Before / after (round 12, survival)

| Category | Before | After |
|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 |
| Load errors | 1 lot (N002_Lot32, not a lot) | 1 lot |
| Household Mains dead | 2 (Ophelia, Circe) | 0 (and 0 reset); the Grim Reaper's Main no longer dies |
| Sims that died ("Falecer") | 23 (8 lots) | 20 (9 lots) |
| Sims stuck > 6 game hours | 12 (8 lots) | 15 (8 lots: 7 long sleeps, 3 children carried off in the social worker's car that never leaves, 2 "Usar" deadlock in N001_Lot49, Pascal's birth) |
| Motives pinned at -100 > 6 game hours | 187 | 88 (47 in the run before the last fix) |
| Save round trips that differ | 0 | 0 |
| Instruction budget (20000) hit | 0 | 13 (N002_Lot22, during a fire; the object is reset) |

Engine causes fixed this round (docs/TODO-HISTORICO.md, "Rodada 12, Sims vivos e destravados"): the Grim Reaper had no neighbor (Force Recalc of Wants of neighbor 0) and reaped again the Sim it had just turned into an urn (Set to Next skips objects on the kill queue); text notifications need only the tree's owner; a failed Main is reset as the game does; Find Best Object for Function looks only in the rooms the Sim can reach, which found the diagonal doors that never opened (upstairs of N001_Lot68 and N003_Lot52); a multi-tile object's bone slots are placed from its lead (the straight staircase: nobody reached the upper floor of N001_Lot11, so "Dormir" always failed); bone slots with directions stand at the bones (the cheap fridge put N002_Lot22's family on the counter: they starved); Set to Next 3 on the last part of a burnt multi-tile object (51 budget hits in N001_Lot36); a Sim on my goal steps aside 1-3 tiles (the oven of a stove whose cooktop is in use); Idle for Input nests a higher-priority interaction over the running one (the child the social worker takes). The deaths left are mostly kitchen fires of cooks without skill on lots without a smoke alarm (game behavior, §4g "Cozinheiros que saem do forno"), and N003_Lot46/N003_Lot192.

# Soak report (round 11)

Run of `tools/soak.sh` over the 69 lots of N001, N002, N003 and Tutorial (3 game days of autonomy each, 129600 ticks), headless (`SOAK_NO_APP=1`), 2 lots at a time. "Before" is the round-11 branch point (main 3872cc0); "after" is the r11-food branch rebased on main 55e68fe (so it also has main's overlay and blocked-walker changes).

## Before / after (round 11, food and fire)

| Category | Before | After |
|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 |
| Load errors | 1 lot (N002_Lot32, not a lot) | 1 lot |
| Household Mains dead | not counted (the soak grepped a message autonomy no longer prints) | 2 (N002_Lot2 Ophelia: "Force Recalc of Wants: no neighbor 0"; N002_Lot3 Circe: text notification without a stack object) |
| Sims that died ("Falecer") | 26 (11 lots) | 23 (8 lots) |
| Sims stuck > 6 game hours | 23 (11 lots) | 22 (8 lots; 12 are long sleeps or music) |
| Motives pinned at -100 > 6 game hours | 167 (36 Sims) | 165 (34 Sims) |
| Save round trips that differ | 0 | 0 |
| Instruction budget (20000) hit | 0 | 0 |

Lots that no longer lose anyone: N001_Lot16 (Don threw his frozen dinner out of the oven and set the kitchen on fire), N001_Lot36 (Brandi's bath push had no toddler: dead Main, starved), N001_Lot49, N002_Lot10, N002_Lot3. Lots that lose more: N003_Lot192 (2 -> 4), N003_Lot46 (1 -> 4), N002_Lot4 (0 -> 2); with N001_Lot68 and N003_Lot52 (4 each) the deaths left are almost all **fires**: a low-skill cook burns the food ("Main - Cook - Loop": 20% once it overcooks) or cooked food is left in the oven (with "Free Will", global 30, on, "Cook - _Type - Bake" puts the dish in and leaves; "Push - Get Food" calls the cook back, who often arrives after the 10 minutes before it burns), and these lots have no smoke alarm in the kitchen (only 6 of the 69 lots have one), so no firefighter comes and the household's Sims only panic (they extinguish only when the player says so, docs/primitives-mac.md §4f). With an alarm the firefighter now comes and puts it out (N002_Lot10 with a fire set at tick 600: out in about an hour, nobody dies).

Engine causes fixed this round (docs/TODO-HISTORICO.md, "Rodada 11, comida e fogo"): a waiting entry interrupted the running one only when its priority is higher (cTSPerson::ShouldInterrupt; FreeSO lets autonomous entries interrupt each other, so every "*Push Look At" cancelled cooking); Find Best Object for Function as the binary (in use, burning, disabled objects skipped; check tree for the reference; first place flag; hypotenuse); fire trucks drive in and firefighters run their own extinguish test (Run Tree by Name picked the global table); arriving Sims get their object and lead tile IDs (a passer-by's Main died) and lot-save --check reads a moved-in lot back right; Push Interaction with the last FBA's entry takes its parameters (the toddler of the bath); new objects get their birth date (every new meal was 22,000 hours old and spoiled); action strings from the running code's semi-global ("Ask For Lessons From ..." on food). Special dialogs (types >= 5) wait for the player in the game too (no timeout): Circe's "Atender" (N002_Lot3) is that.

## Round 9

Run of `tools/soak.sh` over the 69 lots of N001, N002, N003 and Tutorial (3 game days of autonomy each, 129600 ticks), without the app step (`SOAK_NO_APP=1`: headless only this round), 2 lots at a time. "Before" is the branch point (main de88a7d, the dialogs waiting for the player forever); "after" is the round-9 sim-fixes branch. Dialogs now time out as the game's (the last button after 4000 + 40 x length ms of game time), so the soak no longer needs `--dialog-answer`.

## Before / after

| Category | Before | After |
|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 |
| Load errors | 1 lot (N002_Lot32, not a lot) | 1 lot |
| Mains dead | 0 | 0 |
| Sims stuck > 6 game hours (besides sleep, work, school) | 19 (10 in "Atender") | 12 (1 "Atender": N002_Lot3 Circe, a special dialog type with no time limit) |
| Sims that died ("Falecer") | 24 (13 lots) | 27 (11 lots) |
| Motives pinned at -100 > 6 game hours | 182 (38 Sims) | 126 (29 Sims) |
| Save round trips that differ | 14 lots | 0 |
| Instruction budget (20000) hit | 1 (N001_Lot36) | 0 |

Deaths: before, 10 Sims were frozen in a phone dialog and could not run "Falecer" even with every motive at -100; now they live on, and the deaths left are starvation. Engine bugs behind deaths fixed this round: every modular deck stair locked (N003_Lot192: the Sims in the pit could not reach the fridge), the "Desespero" pushes failing (Push Interaction read the found entry from the wrong object; the Main died), glasses and trays dropped on the floor closing kitchen tiles, "Align Trashcan To Sim" and "Chess - Reset Sim" spinning out the budget. Still open (TODO.md): cooking fails in a loop ("Food Chain - Failure - Cook", no prep surface found) and each failure leaves a tray; "Fogo!" (N001_Lot49 Darren, 7-8 hours); special dialogs (type >= 5) have no time limit.


# Generated tables (after, seed 1)

Generated by `tools/soak.sh` on 2026-10-12: 69 lots, autonomy for 129600 ticks (72 game hours, 1800 ticks each), lot-save of 300 ticks with --check, app run of 600 VM ticks. Hang limit 900s (app 300s).

## Counts by category

| Category | Lots | Findings |
|---|---|---|
| crash | 0 | 0 |
| hang | 0 | 0 |
| error | 1 | 2 |
| dead | 0 | 0 |
| reset | 1 | 2 |
| stuck | 11 | 35 |
| died | 6 | 12 |
| pinned | 6 | 15 |
| roundtrip | 0 | 0 |
| instruction budget (20000) hit | 0 | 0 |

## Findings per lot

| Lot | Category | Where | Detail |
|---|---|---|---|
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Dormir" for 6.8 game hours |
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Dormir" for 9.3 game hours |
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Tocar" for 8.1 game hours |
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Tocar" since tick 118115 (6.4 game hours at the end) |
| N001_Lot11 | stuck | autonomy | Cassandra#179 in "Trabalhando" for 6.6 game hours |
| N001_Lot16 | pinned | autonomy | Don#197 comfort at -100 for more than 6 game hours |
| N001_Lot16 | pinned | autonomy | Don#197 energy at -100 for more than 6 game hours |
| N001_Lot16 | pinned | autonomy | Don#197 hygiene at -100 for more than 6 game hours |
| N001_Lot18 | pinned | autonomy | Dina#271 hygiene at -100 for more than 6 game hours |
| N001_Lot18 | pinned | autonomy | Dina#271 social at -100 for more than 6 game hours |
| N001_Lot18 | pinned | autonomy | Nina#210 social at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Beau#270 hygiene at -100 for more than 6 game hours |
| N001_Lot36 | stuck | autonomy | Dustin#248 in "Dormir" for 6.3 game hours |
| N001_Lot49 | stuck | autonomy | Darren#347 in "Cochilar" for 7.4 game hours |
| N001_Lot49 | stuck | autonomy | Darren#347 in "Dormir" for 7.1 game hours |
| N001_Lot68 | died | autonomy | Mary-Sue#37 in "Falecer" since tick 51470 (43.4 game hours at the end) |
| N001_Lot68 | stuck | autonomy | Daniel#116 in "Dormir" for 6.3 game hours |
| N002_Lot10 | died | autonomy | Jenny#386 in "Falecer" since tick 51852 (43.2 game hours at the end) |
| N002_Lot10 | died | autonomy | Johnny#389 in "Falecer" since tick 51059 (43.6 game hours at the end) |
| N002_Lot10 | stuck | autonomy | Jenny#386 in "Cochilar" for 6.3 game hours |
| N002_Lot10 | stuck | autonomy | Jill#454 in "Jogar" for 9.6 game hours |
| N002_Lot10 | stuck | autonomy | Johnny#389 in "Dormir" for 7.6 game hours |
| N002_Lot2 | died | autonomy | Olive#119 in "Falecer" since tick 95059 (19.2 game hours at the end) |
| N002_Lot2 | stuck | autonomy | Ophelia#230 in "Cochilar" for 7.1 game hours |
| N002_Lot2 | stuck | autonomy | Ophelia#230 in "Dormir" for 9.2 game hours |
| N002_Lot22 | died | autonomy | Buck#151 in "Falecer" since tick 15325 (63.5 game hours at the end) |
| N002_Lot22 | died | autonomy | General in "Falecer" since tick 15980 (63.1 game hours at the end) |
| N002_Lot22 | died | autonomy | Ripp#149 in "Falecer" since tick 67564 (34.5 game hours at the end) |
| N002_Lot22 | died | autonomy | Tank#144 in "Falecer" since tick 68211 (34.1 game hours at the end) |
| N002_Lot22 | pinned | autonomy | Ripp#149 hygiene at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Ripp#149 social at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Tank#144 comfort at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Tank#144 fun at -100 for more than 6 game hours |
| N002_Lot3 | pinned | autonomy | Circe#147 social at -100 for more than 6 game hours |
| N002_Lot3 | pinned | autonomy | Nervous#182 hygiene at -100 for more than 6 game hours |
| N002_Lot3 | stuck | autonomy | Circe#147 in "Trabalhando" for 9.6 game hours |
| N002_Lot32 | error | autonomy | exit 1: error: no elevation array (3ARY 0x01): not a lot package? |
| N002_Lot32 | error | lot-save | exit 1: error: no elevation array (3ARY 0x01): not a lot package? |
| N002_Lot4 | reset | autonomy | Pascal#142: 2 x Tree table not found (object 204) |
| N002_Lot4 | reset | autonomy | Vidcund#152: 2 x Animation name is empty string. |
| N002_Lot4 | stuck | autonomy | Lazlo#137 in "Dormir" for 9.5 game hours |
| N002_Lot4 | stuck | autonomy | Lazlo#137 in "Trabalhando" for 6.7 game hours |
| N002_Lot4 | stuck | autonomy | Lazlo#137 in "Trabalhando" for 6.8 game hours |
| N002_Lot4 | stuck | autonomy | Pascal#142 in "Trabalhando" for 6.6 game hours |
| N002_Lot4 | stuck | autonomy | Vidcund#152 in "Trabalhando" for 6.9 game hours |
| N003_Lot192 | died | autonomy | Oberon#39 in "Falecer" since tick 81367 (26.8 game hours at the end) |
| N003_Lot192 | died | autonomy | Puck#49 in "Falecer" since tick 80438 (27.3 game hours at the end) |
| N003_Lot192 | died | autonomy | Titania#12 in "Falecer" since tick 79685 (27.7 game hours at the end) |
| N003_Lot192 | stuck | autonomy | Bottom#1 in "Dormir" for 7.9 game hours |
| N003_Lot192 | stuck | autonomy | Bottom#1 in "Na Escola" for 6.6 game hours |
| N003_Lot192 | stuck | autonomy | Bottom#1 in "Na Escola" for 6.7 game hours |
| N003_Lot192 | stuck | autonomy | Oberon#39 in "Dormir" for 6.2 game hours |
| N003_Lot192 | stuck | autonomy | Oberon#39 in "Dormir" for 6.3 game hours |
| N003_Lot192 | stuck | autonomy | Titania#12 in "Dormir" for 6.6 game hours |
| N003_Lot46 | died | autonomy | Patrizio#135 in "Falecer" since tick 83913 (25.4 game hours at the end) |
| N003_Lot46 | stuck | autonomy | Isabella#137 in "Dormir" for 6.0 game hours |
| N003_Lot46 | stuck | autonomy | Mercutio#381 in "Dormir" for 6.8 game hours |
| N003_Lot46 | stuck | autonomy | Mercutio#381 in "Dormir" for 7.6 game hours |
| N003_Lot46 | stuck | autonomy | Patrizio#135 in "Cochilar" for 6.3 game hours |
| N003_Lot46 | stuck | autonomy | Romeo#402 in "Cochilar" for 6.3 game hours |
| N003_Lot46 | stuck | autonomy | Romeo#402 in "Dormir" for 6.1 game hours |
| N003_Lot52 | pinned | autonomy | Juliette#140 fun at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Tybalt#198 fun at -100 for more than 6 game hours |
| N003_Lot52 | stuck | autonomy | Consort#136 in "Trabalhando" for 7.3 game hours |
| N003_Lot52 | stuck | autonomy | Juliette#140 in "Cochilar" for 6.2 game hours |

## Stacks of crashes


## Timing (seconds, per lot: autonomy / lot-save / app)

- N001_Lot11: 13 / 0 / 
- N001_Lot16: 6 / 0 / 
- N001_Lot18: 8 / 0 / 
- N001_Lot22: 3 / 1 / 
- N001_Lot24: 2 / 1 / 
- N001_Lot32: 3 / 1 / 
- N001_Lot36: 5 / 0 / 
- N001_Lot38: 2 / 1 / 
- N001_Lot40: 3 / 0 / 
- N001_Lot47: 4 / 1 / 
- N001_Lot49: 9 / 1 / 
- N001_Lot51: 3 / 1 / 
- N001_Lot53: 3 / 0 / 
- N001_Lot55: 4 / 0 / 
- N001_Lot57: 4 / 0 / 
- N001_Lot59: 6 / 1 / 
- N001_Lot64: 3 / 1 / 
- N001_Lot66: 4 / 1 / 
- N001_Lot68: 12 / 1 / 
- N001_Lot70: 2 / 1 / 
- N001_Lot73: 8 / 1 / 
- N001_Lot75: 5 / 1 / 
- N001_Lot77: 8 / 1 / 
- N001_Lot79: 7 / 1 / 
- N001_Lot80: 1 / 1 / 
- N001_Lot82: 16 / 1 / 
- N002_Lot10: 12 / 0 / 
- N002_Lot2: 10 / 0 / 
- N002_Lot22: 16 / 1 / 
- N002_Lot23: 4 / 1 / 
- N002_Lot25: 6 / 0 / 
- N002_Lot27: 2 / 1 / 
- N002_Lot29: 13 / 0 / 
- N002_Lot3: 16 / 1 / 
- N002_Lot31: 11 / 0 / 
- N002_Lot32: 0 / 0 / 
- N002_Lot4: 14 / 0 / 
- N003_Lot126: 6 / 0 / 
- N003_Lot128: 7 / 0 / 
- N003_Lot133: 6 / 0 / 
- N003_Lot135: 4 / 0 / 
- N003_Lot144: 4 / 0 / 
- N003_Lot149: 3 / 0 / 
- N003_Lot153: 4 / 1 / 
- N003_Lot157: 4 / 0 / 
- N003_Lot159: 3 / 1 / 
- N003_Lot164: 3 / 0 / 
- N003_Lot166: 3 / 1 / 
- N003_Lot168: 5 / 0 / 
- N003_Lot171: 2 / 1 / 
- N003_Lot175: 2 / 1 / 
- N003_Lot179: 6 / 1 / 
- N003_Lot181: 3 / 0 / 
- N003_Lot183: 5 / 0 / 
- N003_Lot187: 1 / 1 / 
- N003_Lot192: 30 / 1 / 
- N003_Lot195: 5 / 1 / 
- N003_Lot45: 3 / 1 / 
- N003_Lot46: 14 / 0 / 
- N003_Lot47: 4 / 1 / 
- N003_Lot48: 4 / 1 / 
- N003_Lot50: 2 / 1 / 
- N003_Lot52: 18 / 1 / 
- Tutorial_Lot1: 1 / 1 / 
- Tutorial_Lot14: 3 / 0 / 
- Tutorial_Lot18: 7 / 0 / 
- Tutorial_Lot19: 7 / 0 / 
- Tutorial_Lot3: 6 / 0 / 
- Tutorial_Lot4: 2 / 1 / 

# Inventário do binário: o que já é fiel, o que é inferido e o que falta

Feito em 2026-10-15, sobre o main `44b9280c`. Só documentação: nada no código
mudou. Fontes: o `baserom.sym` (100 mil nomes, o mesmo que o
`syms.txt` já desmanglado), o `ppc_syms.txt` do BV, o nosso `src/`, os
`docs/*-mac.md`, o `TODO.md`, o `docs/TODO-HISTORICO.md` e as issues. Sem
Ghidra: tudo saiu dos nomes e dos textos que já temos.

## 1. Método

**Classes.** Cada símbolo do `baserom.sym` foi desmanglado (`c++filt`) e
cortado em `Classe::método`. Isso dá 2.485 classes. Ficaram de fora:

- bibliotecas e motor genérico: `std`, `__gnu_cxx`, `nGZNumerics`,
  `nGZGraphic4`, `nGZSceneGraph`, `cGZWin*`, `cRZ*`, `cGZDB*`, `cGZLua*`,
  `cHTML*`, `Snd`, D3D, templates;
- rede, compressão, áudio, Lua, depuração, trapaças, testes, miniaturas,
  tutorial e vídeo de TV;
- tudo de expansão, pelo nome (Pet, Season, Business, Vacation, College,
  Apartment, Hobby, Trait, Awning, HalfWall, CurvedPool, Inventory,
  Shopping, Garden, Weather, Rewards, Collection, Album, Web e outros). O
  filtro é por palavra; não temos a lista de classes do base 1.0 (o
  executável base só tem RTTI dentro do Ghidra).

As 816 classes que sobram foram postas num sistema por expressão regular
sobre o nome (`cTSRoute*` → rotas, `nGZSceneDBAnim::*` → animação, e assim
por diante). Métodos `Write`/`Read`/`SaveToStream`/`LoadFromStream` de
qualquer classe contam em "salvar/carregar". Ficam fora construtores,
destrutores, operadores e `QueryInterface`/`AddRef`/`Release`. Sobram
**13.977 métodos relevantes**.

**Casamento.** Um método conta como **nomeado** quando:

1. o par `Classe::método` aparece em `src/` ou nos `docs/` (sem
   `docs/issues/` e `docs/ideias/`, que listam o que *ainda* falta ler); ou
2. o nome do método aparece sozinho em `src/` ou nos `docs/`, tem 8 letras
   ou mais, existe em no máximo 2 classes relevantes e não é genérico
   (`Get…`, `Set…`, `Init`, `Update`, `Read`, ...).

Conferi 40 casamentos da regra 2 sorteados: 38 são a função certa (95%).
"Nomeado em src" é o mesmo teste, só no código.

**Marcadores "inferido".** Linhas com `inferred` ou `inferid` em cada
arquivo de `src/`, com o arquivo posto num sistema pelo caminho (tabela da
§3). Também contei os dos docs. São um piso: muita aproximação não traz a
palavra.

**Fórmulas.**

- **Cobertura de nomes** = métodos nomeados ÷ métodos relevantes do sistema.
- **Índice de fidelidade** = Σ notas ÷ número de linhas da tabela do
  sistema, com **fiel = 1**, **inferido = 0,5** (feito, mas com partes
  deduzidas por nós, aproximadas ou sem conferir) e **faltando = 0**. A
  nota de cada linha veio dos docs, do `TODO.md` e de olhar o código
  (conferi, por exemplo, que `EvalTileForGoal` e `AskBlockerToMove` já
  estão em `src/vm/routing*.cpp`, e que a regra do corte de paredes está em
  `src/formats/lot_model.cpp`).
- **Total** = Σ notas de todas as linhas ÷ todas as linhas.

**O que os números medem e o que não medem.**

- A cobertura de nomes mede **largura do estudo**: quantas funções do jogo
  alguém já leu e citou. Ela é baixa de propósito. O denominador tem
  milhares de getters, funções de editor e caminhos que o jogo base não
  usa. Funções lidas só pelo endereço (`FUN_…`) não contam. Um nome citado
  como "falta ler" no `TODO.md` conta, por engano.
- O índice de fidelidade mede **o que importa para jogar**: as partes que
  se vêem. É um julgamento por linha, não uma medida automática.
- Nenhum dos dois diz que o código está sem bugs. Fiel quer dizer "segue o
  que lemos no binário".

## 2. Resumo

| Sistema | Classes (estudadas) | Cobertura de nomes | Índice de fidelidade | "inferido" em src / docs |
|---|---|---|---|---|
| VM e primitivas | 54 (19) | 233 / 1.610 = 14,5% | 10,5 / 11 = **95%** | 125 / 80 |
| Rotas | 27 (13) | 113 / 622 = 18,2% (não recontado na n18 nem na n49) | 8 / 8 = **100%** | 59 / 38 |
| Animação e IK | 78 (22) | 88 / 1.364 = 6,5% (não recontado na n23, n34, n43 nem n50) | 8,5 / 9 = **94%** | 36 / 14 |
| Sentar | (usa rotas, slots, IK) | 22 / 34 funções-chave = 65% | 9,5 / 10 = **95%** | 10 / 8 |
| Colocação e construção | 180 (71) | 311 / 2.556 = 12,2% (não recontado na n20, n36, n37 nem n48) | 10,5 / 11 = **95%** | 64 / 50 |
| Corte de paredes | 11 (5) | 32 / 159 = 20,1% (não recontado na n19) | 6 / 6 = **100%** | 4 / (na ui-live §8c–8d) |
| Autonomia | 11 (2) | 64 / 251 = 25,5% | 5 / 5 = **100%** | 9 / (em primitives) |
| Motivos, desejos, memórias, relacionamentos | 21 (7) | 41 / 355 = 11,5% (não recontado na n17 nem na n33: a n33 nomeia os ~20 métodos do `cTSEnvironmentScoreManager`) | 5 / 5 = **100%** | 3 / 7 |
| Salvar e carregar | 32 (5) | 78 / 936 = 8,3% (não recontado na n09 nem na n47) | 7 / 7 = **100%** | 10 / 2 |
| Bairro e famílias | 64 (29) | 105 / 1.250 = 8,4% (não recontado na n12 nem na n27) | 6 / 7 = **86%** | 41 / 45 |
| CAS | 89 (25) | 87 / 1.534 = 5,7% (não recontado na n10, n21 nem n51) | 7,5 / 8 = **94%** | 33 / 28 |
| Desenho | 211 (36) | 129 / 2.309 = 5,6% (não recontado na n13, n16, n19, n22, n25, n36, n40 nem n50) | 9,5 / 10 = **95%** | 55 / 41 |
| Câmera e UI de jogo | 59 (21) | 90 / 1.031 = 8,7% (não recontado na n11 nem na n27) | 12,5 / 13 = **96%** | 43 / 52 |
| **Total** | **816 (253)** | **1.371 / 13.977 = 9,8%** (só src: 699 = 5,0%) | **105,5 / 110 = 95,9%** (recontado das tabelas da §3 em 2026-10-18, rodada n51) | **505 em src** |

"Estudadas" = classes com pelo menos um método nomeado. Os 519 marcadores
de src incluem 15 de áudio, fora dos sistemas. Os 4 do corte são 2 do `wall_view` e 2 do `lot_model.cpp`, que também contam no desenho. "Sentar" não tem classes
próprias: a cobertura é das 34 funções que a issue 001 manda ler.

O índice é a soma das notas das tabelas da §3; recontado delas em
2026-10-16 (o resumo vinha sendo somado à mão e tinha saído do lugar em
cinco sistemas). Em uma frase: **a lógica do jogo (VM, autonomia, rotas,
motivos) e o desenho estão quase todos lidos; o que mais falta são
sobras espalhadas: a colocação, a água, o CAS e o bairro (§8).**

## 3. Tabelas por sistema

Coluna "nomeados" = métodos nomeados / relevantes da classe.

### 3.1 VM e primitivas

Código: `src/vm/vm.*`, `primitives.cpp`, `queue.cpp`, `dialog.cpp`,
`engine*.{hpp,cpp}`, `message.cpp`, `timer.cpp`, `person_prims.cpp`,
`world.*`, `lot.*`, `src/formats/simantics.*`, `bhav.*`,
`src/app/dialog_window*.cpp`, `live_dialogs.cpp`, `lot_vm*.{cpp,mm}`.
Docs: `primitives-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Despacho e operandos: `cTSObject::OnTryElement`, `cTSBehaviorNode::GetNewPrimitiveByID`, `*Param::SwizzleParams`, `XPrimParam::Read` | `vm/primitives.cpp`, `formats/simantics.*` | fiel | 40 de 40 handlers com nome; largura de todos os campos. |
| Donos de dados e contas: `cTSObject::InterpValue`, `TryExpression`, `TryArrayOperation` | `vm/vm.cpp`, `primitives.cpp` | fiel | |
| Pilha e chamadas: `cTSTreeSim`, `cTSTreeStack`, `cTSTreeStackElem`, `DoNodeAction`/`Gosub` | `vm/vm.cpp`, `thread_state.*` | fiel | Até 8 argumentos, 1000 quadros, 100000 nós (constante única `vm::kNodeLimitPerTurn`). |
| Primitivas lidas uma a uma: `TrySetToNext`, `TrySnap`, `try_new_reach`, `TryFindFunctionalObject`, `TryDialog`, `TryOnTimer`, `TryChangeMaterial`, `TryCreateObject`, `TryInventoryAction`, `TryGenericSimCall` | `vm/primitives.cpp`, `reach.cpp`, `timer.cpp`, `dialog.cpp` | fiel | Sobram itens "Confirmar" no `TODO.md` (Set to Next 13/33/37, tabelas 0x02/0x04 do Change Material). |
| Fila de interações: `cTSInteraction` (77 métodos, 14 nomeados), `TryPushAction`, `cTSPerson::TryIdleForInput`, `SetCurrentAction` | `vm/queue.cpp`, `links.cpp`, `records.cpp` | fiel | Confirmado: sub-fila e aninhadas (erro 0x6F), `SetCurrentAction` (dados 39, 164, 166), bits do `+0x54`, `TestInteraction` (idade, visitante, depuração, objeto pegando fogo ou com as interações desligadas, controle dos pais), `ActionSkipped` (entrada 4 do OBJf), a fila cheia (`AnyLowerPriorityInteractions`), o 109 da ação e da sub-ação, a interrompida largada como no base (sem a pilha do BV). **Inferido:** o menu de pie não aplica os testes do objeto; o GUID da interação vazia não é salvo. (`primitives-mac.md` §4g, §4j) |
| Relógio: `cTSSimulator::SimulateOneTick`, `Simulate` | `vm/vm.cpp` | fiel | 30 ticks por minuto, hora do dia, meia-noite. |
| Erros: `cTSObject::HandleError`, `GetErrorString` | `vm/vm.cpp`, `errors.hpp` | fiel | O reset do object data 7 é o campo 45 do OBJD (`GetDefaultNicenessMultiplier`, vtable `0x578`; no `VM::resetObject`, pinado no `testResetAfterError`). |
| Módulo de objetos: `cEdithObjectModule` (120, 30 nomeados): `MakeNewOutOfWorldObject`, `FlushKillQueue`, `PostLoad`, `UpdateObjectsInRoom`, `UpdateObjectsAdjacentToWall`, pontos de entrada (load, cleanup, roomChanged, wallAdjacency) | `vm/vm.cpp`, `kill.cpp`, `lot_resume.cpp`, `rooms.cpp`, `placement.cpp`, `build_edit.cpp` | fiel | Todos os pontos de entrada feitos. dynMultiTile (entrada 8): nenhum OBJf do jogo base a tem. (`primitives-mac.md` §4i) |
| Dados mantidos pelo motor: object data "room" (`SetLocation`/`SetRoom`), od 5 (wall adjacency), od 62 (uso), dados de pessoa das rotas (37/38/40/55), campos de globais, `cTSGlobals` (58, 2 nomeados), `cTSDataTables` (39, 0) | `vm/world.*`, `rooms.cpp`, `placement.cpp`, `records.cpp`, `routing_plan.cpp` | inferido | Confirmado: sala e od 5 pelo binário, od 62 pelos registros de objetos, 37/38/40 pelo `UpdatePersonDataFields` a cada planejamento (o 37 de um slot global + 1000), `cTSGlobals` (o registro de serviços, nada a portar) e `cTSDataTables` (não existe no base). **Inferido:** a sala é a do tile (o binário usa o quarto de tile do ponto), o 55 é sempre −1 (o planejador não parte dos slots do contêiner). **Falta:** quem chama o `SetGlobal` no motor. (`primitives-mac.md` §4i, §4j) |
| Som: `TryPlaySound` | `vm/sound.cpp` | fiel | Confirmado: tabelas pelo \|n\|, o 0x40 é parar (`cTSAudioEventHandler::DoEvent`), índice negativo pelo valor. **Inferido:** o laço (o binário o deixa à trilha). **Falta:** o volume (não lido). (`primitives-mac.md` §4j) |
| Atributos de tipo: `cTSObjectTypeAttributeManager` | `vm/vm.cpp` | fiel | |

### 3.2 Rotas

Código: `src/vm/routing*.cpp`, `path_planner.*`, `locomotion.*`,
`sim_motion.*`, `turn.cpp`, `src/app/door_portals.*`. Docs:
`routing-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Planejador: `nPathPlanner::cPathPlanner` (48, 14), `cQuadTree`, `cAStarSearch`, `CreateResultPath` | `vm/path_planner.*` | fiel | Célula 1/8 de tile, A* com ruído. |
| Obstáculos: `cTSRouteManager` (70, 12), `WorkOutTileCostAndPassability` | `vm/path_planner.cpp`, `routing_plan.cpp`, `world.cpp` | fiel | Confirmado: estado e custo por célula, corte de quinas, o custo do piso do base 1.0 (sem o `+10`: é a neve do BV), o estado 5 (7 no BV: o terreno debaixo de um piso a menos de 3,0), o `GetFootprint` que encosta cada retângulo na grade de 1/8 (a caixa do Sim também), o tile da parte de multi-tile (o subíndice do OBJD), a caixa do modelo sem lista (a flag 4 é o `cShapeRefNode`: os limites das formas). **Inferido:** o "escondido" (`0x114`) como o volume do espelho. (`routing-mac.md` §9.1, §9.5) |
| Objetivos: `cTSRoute::EvalTileForGoal`, gerador de pontos | `vm/routing.cpp` | fiel | Confirmado: filtro do cone antes do `EvalTileForGoal`, pessoa no ponto a 0,283, a pessoa pela caixa encostada do `GetFootprint`, o "drop to floor below" (`0x98` = RS `0x1000000`, só do destino de tipo 2). **Falta:** a varredura não lê as RS `0x100000` e `0x1000000` (só slots sintéticos; o tipo 2 não é feito). (`routing-mac.md` §9.3, §9.5) |
| Máquina do roteador: `cTSRoutingObjectAggregate` (95, 48): pós-rota, goal check, portais, `AskBlockerToMove`/`MoveOutOfWay` | `vm/routing*.cpp` | fiel | O mais estudado da rota. Confirmado: goal check, pilha de rotas, bloqueio e `MoveOutOfWay` como o binário, as direções dos tipos 1/2 relativas ao próprio destino (o `SetPosition` não o gira) e os setters do slot sintético. (`routing-mac.md` §9.2–9.5) |
| Locomotor: `nTSSG::cLocomotable` (57, 14): `FollowOneStep`, `ComputeTurn`, `CollisionCheck` | `vm/routing_loco.cpp`, `routing_walk.cpp` | fiel | Confirmado: `CollisionCheck`, trajetória prevista do `GlobalLocationEstimate`, teste por passo (`LocoPlaceFromSceneGraph`). (`routing-mac.md` §9.1–9.3) |
| Slots de rota: `cTSRoutingSlot` (58, 3), `cTSSlotDescriptor` (39, 0), `cTSSlotLoader` (19, 0) | `vm/routing.cpp`, `formats/` (SLOT) | fiel | Confirmado: `Init`, `Read`/`Write`, bits das RS flags, vtable dos setters (`0xd0`, `0xcc`, `0xd4`, `0xb8`–`0xc8`, `0x28`–`0x40`, `0x70`–`0x94`), o `Read` pela versão do cabeçalho (x,y e resolução em dezesseis avos antes de `0x47`, proximidades por versão, o facing das flags antes da 9), o 37 de um slot global = índice + 1000. (`routing-mac.md` §8.6, §9.5) |
| Portais de objeto e escadas: `cPathPlanner::AddDoorPortal`, pares de slots | `vm/routing_plan.cpp` | fiel | Confirmado: escadas pelos slots das peças, mão única, `PortalAddToPathPlannerImpl`/`MakeNodesForPortal`/`ConnectPortalNode`, os retângulos de custo 10 dos pés do corrimão (modelos 2–4, pela caixa da pegada no mundo). **Inferido:** a outra ponta da escada reta pela ordem dos IDs das partes. (`routing-mac.md` §8.6, §9.4, §9.5) |
| Rota salva: `cTSRoute::SaveToStream` | `vm/lot_save.cpp` | fiel | `save-mac.md` §7c. |

### 3.3 Animação e IK

Código: `src/vm/animation.*`, `src/formats/anim*.{hpp,cpp}`,
`scene_anim.*`, `skeleton.*`, `skinning.*`, `src/app/anim_snaps.*`,
`look_at.*`, `cinematic_*`, `lot_actors.cpp`. Docs: `anim-mac.md` (fila de
coreografia, expiração, peso dos canais, solucionador de IK; rodada n02),
`primitives-mac.md` (Animate, deltas), `cas-mac.md` §17.4b, `cinematics-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Arquivo ANIM: `cAnimResourceConst::ReadFrom` (67, 7) | `formats/anim.*` | fiel | Confirmado: `ReadFrom`/`WriteTo` (os 11.757 ANIM regravados byte a byte), tangentes, chaves a cada 33,333 ticks, Euler XYZ. (`anim-mac.md` §9) |
| Canais e deltas: `cActiveAnimSkeletal::SetupChannel`, `SetApplyDeltaDataToIK` | `formats/anim_player.cpp` | fiel | |
| Mistura por níveis: `cChannelUberOutlet`, `cChannelUber`, `cBlendFunction2` (29, 0), `cBlendInterval` | `formats/anim_player.cpp` | fiel | Confirmado: peso do canal, memória do osso, `cBlendInterval::Value` (BV `0xd28880`), `CreateWeightBlendFunction` (`FUN_00a46c00`). (`anim-mac.md` §2–3, §13.1–13.2) |
| Fila de coreografia: `cChoreoQueue` (81, 0), `cChoreoRequest` (62, 3), `cChoreoHandler*`, `cAnimatable::OnPreSimulatorTick` | `formats/anim_player.cpp`, `vm/animation.cpp` | fiel | Confirmado: ordem do quadro, expiração, fim da ação, sincronia do byte 8, dado de objeto 85. **Inferido:** o nível −1 dos objetos e das pegas ("grasp"), que não têm função de peso própria. **Falta:** a sincronia dos overlays, a pressa da locomoção. (`anim-mac.md` §1–2, §13.3–13.4) |
| Locomoção: `cActiveAnimLoco::CreateStepCycle`, `PushTurnBlendCycles`, `StopLocomotion`, `cLocoCycle` (63, 0) | `vm/locomotion.*`, `vm/routing_loco.cpp`, `formats/anim_player.cpp` | inferido | Confirmado: relógio das fases, `cLocoCycle` (`+0xec`), pé da parada (`0x178`), `StopLocomotion`. **Inferido:** a troca centrada (±b/2), a rampa de velocidade (`CreateVelocityBlendFunction`, parâmetros não seguidos), a parada fraca. (`anim-mac.md` §8, §13.6) |
| IK: `cActiveAnimIK` (28, 5), `cIKChain` (47, 0), `cIKStrategy7DOF`, `ComputeIKRotationSolution` | `formats/anim.cpp` (`applyIk`), `formats/anim_player.cpp` | fiel | Confirmado: solucionador de duas juntas (dobradiça em X, giro pelo polo), a partilha do braço pela Euler XYZ (ordem `0x01020100`), mistura de alvos pelo contato, a última solução com os contatos em 0, o alvo do outlet do nó. **Inferido:** a partilha do pulso (a ordem `0xff010200` dá o ângulo em Z, como lido), o primeiro alvo antes de qualquer solução. (`anim-mac.md` §4–5, §14) |
| Olhar: `cActiveAnimLookAt` (29, 1), `cIKStrategyLookAt` | `app/look_at.*`, `formats/anim.cpp` (`lookAtIkRotation`) | fiel | Confirmado: `AnimUpdate` (BV `0xd41e04`) e as duas cadeias da estratégia (BV `0xd46cdc`, `0xd46568`). **Inferido:** o `root_rot` como quadro da torção, a rotação de repouso da cabeça, a forma linear do blend. **Falta:** os slots (byte 14), a câmera, os eventos "look_state" (9901–9906), o teste de alcance sem 0x40. (`anim-mac.md` §12) |
| Eventos: `cEventChannel`, `cActiveAnimEvent`, árvores de evento | `vm/animation.cpp` | fiel | Confirmado: parâmetros do `cCustomEventHandler2::HandleEvents`, tipos do `cActiveAnimEvent` (BV `0xd3a290`; 9997/9998/9999). **Falta:** o tempo do evento do `RunTree`, o erro de uma árvore de overlay (descartado). (`anim-mac.md` §10, §12.1) |
| Cinemáticas: `cCinematicScene::ReadFrom` (23, 9), `TryCinematic` | `formats/cinematic.*`, `app/cinematic_*`, `app/object_fade.hpp` | fiel | Confirmado: andar do ator, "ObjectHiding", `try_animate_sim` (BV `0x69e6d8`), `cSpotLight` (BV `0xc9f3ac`), o `Prep`/`Start`/`Stop` (atores, objetos nos slots, grupos de render), o fade da câmera de cinema (só com a flag `0x80` ou na mudança). **Inferido:** a velocidade normal forçada (o `cCustomAnimTimer2` lido, o corte de 1 ms do `Start`; quem chama o `UpdateFrameTime` e quem muda a escala não achados). (`cinematics-mac.md` §8–10) |

### 3.4 Sentar

Código: `src/vm/reach.cpp`, `slot_bones.cpp`, `routing.cpp`,
`src/app/seat_module.*`, `seat_check.*`, `vm_objects.cpp`, `lot_vm.cpp`.
Docs: `docs/seat-mac.md` (leitura do binário, rodada n01), `docs/issues/001-sentar-fiel-ao-binario.md` (parcialmente feita).

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Achar a cadeira: `TryFindFunctionalObject` (função 26) | `vm/reach.cpp` | fiel | |
| Snap: `cTSObject::TrySnap` | `vm/reach.cpp` | fiel | |
| A. Objetivos de sentar: `cTSRoute::ScanConeForSittingGoals` (BV `0x4788cc`), `GetPossibleSittingSlotsFromModel`, `IsPersonSittingOnChairGoal`, `CreateContainmentGoalPoint` | `vm/routing.cpp`, `routing_walk.cpp` | fiel | Cadeira na pós-rota; o objetivo de contenção não roda no jogo base. |
| B. Snap Into e slot: `TrySnapInto`, `PlaceInSlot`, `AggregateOnPlaceInSlot`, `TestPlaceInSlot` | `vm/placement.cpp`, `routing_walk.cpp` | fiel | Falta a árvore 0x34 "allow in slot". |
| C. Corpo no slot: `cObject::OnPlaceInSlot`, `ObjectSlotOffset`, `cTSPerson::UpdateSlotDisplacement` | `app/seat_module.*` | fiel | O grafo de cena prende ao osso do slot, como o app; a VM só tem a posição do contêiner. |
| D. Salto da raiz: `PostSnap`, `cObject::ApplyAnimationOffsets`, `GlobalRootTransPosition` | `formats/anim_player.*` (`stayInWorld`), `app/seat_module.*` | inferido | Confirmado: o quadro de cada pedido e o `UpdateObjectTransform` (o `PostSnap` do `LocoPlaceFromSceneGraph`: na entrega do caminho todas as camadas ficam no mundo); os od 108–111 nunca escritos; o `GlobalRootTransPosition` é do `KillLocomotion`. **Inferido:** o `stayInWorld` nos pulos da VM (Snap, contenção, fim de escada: o Snap do jogo não chama o `PostSnap`, como lido). **Falta:** o Snap Into 0x04 (`ResetRootBones`). (`seat-mac.md` §D) |
| E. Sentado e parado (idles da cadeira, fila vazia) | `app/lot_vm.cpp`, `formats/anim_player.cpp` | fiel | O motor não tem idle; osso sem canal fica como estava (`anim-mac.md` §6). A idle em pé do app segue nossa. |
| F. Animação de entrar/sair (STR# 0x92) | `vm/reach.cpp` | fiel | Confirmado: o índice explícito (flag 0x08, byte 15), a altura e a distância do ponto do alcance no quadro do Sim, as listas (superfície, semi-global, global). **Inferido:** o andar como 3 de altura e o terreno plano. (`seat-mac.md` §F) |
| G. IK das mãos e pés: `cChoreoHandlerIK::HandleChoreoRequest`, `FindIKTargetBone` | `formats/anim.cpp` | fiel | O solucionador do binário (`anim-mac.md` §4). |
| H. Levantar: `TrySnap`, `RemoveFromWorld`, `DoesPathStartFromContainment` | `vm/reach.cpp`, `app/seat_module.*` | fiel | Confirmado: Snap num ponto só (o primeiro osso ou o deslocamento do slot) com os testes do binário (`seat-mac.md` §H). **Inferido:** a sala do teste como a do tile. |

### 3.5 Colocação e construção

Código: `src/vm/placement.*`, `build_edit*.{hpp,cpp}`, `rooms.*`,
`src/formats/build_catalog.*`, `catalog.*`, `design_catalog.*`,
`lot_edit.*`, `lot_write.*`, `lot_roof.*`, `src/app/buy_mode.*`,
`build_mode*.{hpp,cpp}`, `design_tool.*`. Docs: `buy-build-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Catálogo: `cTSCatalog` (106, 4), `cTSWinProcCatalog`, `cTSWinProcBuyPanel` | `formats/catalog.*`, `app/buy_mode.cpp` | fiel | Ordem entre preços iguais inferida. Colocação na tela medida no original (800x600 e 1280x720; `buy-build-mac` §1.6): o catálogo estica até `largura − 36`, a grade fica fixa em x 589 / `altura − 126`. |
| Preço, venda, depreciação (`DayPassed`) | `app/buy_mode.cpp`, `vm/` | fiel | |
| Colocação: `cTSUserToolObject` (93, 17), `TestPlacementCommon`, `TestPlacementWithRespectToWalls/Objects`, `AutoRotateObjectToMatchWalls`, `cTSUserToolFireplace` | `vm/placement.*`, `vm/build_edit_fireplace.cpp`, `app/buy_mode.cpp`, `app/build_mode*.cpp` | fiel | Confirmado: o teste de objetos do base (`FUN_00ffb990`: exclusivo do od 63, flags de interseção, árvore "allow intersection" 5, camadas, 0,283 entre pessoas, as pegadas 0 e 2 das portas), o `TestPlacementCommon` do base (piso visível pela camada 3ARY `0x15`, água, piso/terreno `0x8C`/`0x8D`, moirão `0x79`, od 30 pela sala de definição 0), a altura (`0x2c(1)` = 1), o Create New Object Instance na ordem do binário, `HasSupport` só na escadaria, `IsFlat`, piscina sob o tile; a ferramenta de lareira (`TryPlace`, a cornija na frente de uma chaminé, a marreta): 36 das 48 lareiras salvas são a chaminé dela. **Inferido:** objetos de piscina passam sobre a piscina; o preço do produto na cornija; a pessoa no tile de uma peça. **Falta:** a camada de visibilidade ao pôr um "Hide Floor" na sessão, as miniaturas e as cópias da chaminé da lareira, altura livre e teto fora da escadaria. (`buy-build-mac.md` §7.24–7.26) |
| Desfazer: `cTSUserToolUndoable*`, `cTSUserToolUndoManager` | `app/build_mode*.cpp`, `app/undo_order.hpp` | fiel | Confirmado: gerente (`0x2807200`), estados e ordem, pilha única, dinheiro por `AdjustFunds` (vtable `0x68`); o lote regravado fica igual byte a byte (`tests/build_undo.sh`). (`buy-build-mac.md` §7.24–7.25) |
| Paredes: `cTSUserToolWall`, `nTSWorld::cWallGraph`, `cPartitionManager` (92, 13) | `vm/build_edit_walls.cpp` | fiel | |
| Salas: `cRoomManager::FloodRoomRecurse`, `cWallGraph::SetRoomsPostAttach` | `vm/rooms.*`, `formats/lot_edit.cpp` | fiel | Confirmado: `FUN_009a7dd8` aplaina o retângulo; IDs novos pelo `SetRoomsPostAttach` (paridade `FUN_00c1c480`). **Inferido:** a direção da aresta nova, os quartos sem piso dos andares de cima fora das salas. (`buy-build-mac.md` §7.24–7.25) |
| Pisos e revestimentos: `cTSUserToolFloorPattern`, `cTSUserToolWallpaper` | `vm/build_edit.cpp` | fiel | |
| Terreno, lagos, piscinas: `cTSUserToolTerrain*`, `cTSUserToolWater`, `cTSUserToolDeckFoundation`, `cTSUserToolUndoablePool` | `vm/build_edit_ground.cpp` | inferido | Confirmado: teste de vértice `FUN_009e9550`, flags do pincel (`0x2BF7`), piscinas e o `ModifyFloorVisibility`, nivelamento, marcadores 3ARY `0x14`, `ConformsToCeiling` (o nó `post_trans`) e os marcadores da pasta de objetos do `ObjectMayBeInFoundation`/`HasNonCeilingConformingObjects`, o `UpdateWaterElevations` inteiro (com a flag de piso ponto por ponto), as paredes que o nivelador de cômodo apaga (`FUN_009ac8d0`). **Inferido:** as flags de lagoa no começo de cada mudança. **Falta:** `ChangeFloorPatterns`/suportes, `FUN_009dae20` (forma do tile), `RemoveOldWalls`. (`buy-build-mac.md` §7.23–7.26) |
| Escadas modulares: `cTSUserToolModularStairs`, `FindEnd` | `vm/build_edit_stairs.cpp` | fiel | `FindEnd` fiel; o +0,75 é de deck no nível do próprio tile de cima, a "elevation" é a altura absoluta (`+0x20c`), a escavação usa o teste de vértice do jogo (`0x2BB7`). (`buy-build-mac.md` §7.24) |
| Telhados: `cTSUserToolRoof` (48, 8), `cAutoRoofData`, `nTSWorld::cWorldRoof` (47, 10) | `formats/lot_roof*.cpp`, `vm/build_edit_walls.cpp` | fiel | Confirmado: `IsRoofValid`, pré-teste e caixa com o beiral; 245 dos 275 telhados do jogo aceitos (os 30 têm janelas postas depois, que o jogo também recusaria). (`buy-build-mac.md` §7.22) |
| Portas e janelas: máscaras `_wallmask`, `HideWhenWallDown`, auto-giro | `formats/lot_model.cpp`, `vm/placement.cpp` | fiel | Confirmado: face pelo nome do `WallCutoutMaterialName`, UV1 com o `tsMirrorWallMask`, topo com a máscara da face A. **Inferido:** o referencial do objeto dos nomes, o nível pela altura, os lados com recorte (`+0x124`) pelos nomes que existem. (`walls-floors-mac.md` §6.2–6.3) |

### 3.6 Corte de paredes

Código: `src/app/wall_view.*`, `src/formats/lot_model.cpp` (regra do
corte), `src/app/lot_view.mm`, `live_ui.cpp` (botões). Docs:
`ui-live-mac.md` §8b–8d, `buy-build-mac.md` §7.6.1, issue 002.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Modos: `cTSMainToolbar::SetWallMode`, `cITSMainToolbar::eWallMode`, `cTSWinProcUCP::onWallChanged` | `app/wall_view.*`, `live_ui.cpp` | fiel | 4 modos; telhado só no modo 3. |
| Regra: `cWallCutawayManager::KeepOnlyFrontFacingOutsideWalls`, `CollectWallsEligibleForCutaway`, `nTSWorld::PartitionStyleSupportsCutaway`, `UpdateSceneGraph` | `formats/lot_model.cpp` (`lotCutawayWalls`), `app/lot_view.mm` | fiel | Pela posição da câmera; refeito só quando o conjunto cortado muda (`ui-live-mac.md` §8d). **Inferido:** o mapa de cômodos das faces (o de luz). |
| Corte pelo Sim: `cutawayForSelectedSim`, `SetSelectedSimLocation`, `CutawayLocationChanged`, `nTSWorld::cWallGraph::WallsBetweenPointSetAndPoint` | `app/lot_view.mm`, `formats/lot_model.cpp` | fiel | Raio do Sim até 3,0 de altura, passo de tiles e regra das arestas do jogo (`lotWallsBetweenPointsAndCamera`). (`ui-live-mac.md` §8b–8d) |
| Memória e alvos: `cutawayMemorySize` (3), `AddTargetLocation`, `Expose`, `SetCinematicsCutaway`, `SelectWallsBlockingPoints`, `SelectRoomBlockers`, `SelectNearSemicircleBlockers` | `formats/lot_model.cpp`, `app/cinematic_host.cpp` | fiel | **Inferido:** os vértices do meio círculo, o cômodo de um ponto, o flag +0x194 do Start (desligado), quando a memória zera (troca de lote). (`ui-live-mac.md` §8b–8d) |
| Geometria cortada: `nTSSG::cWallManager` (62, 16), `cWallGeometryNode` (66, 11), `cSimplifiedWall` (10, 0) | `formats/lot_model.cpp` | fiel | Confirmado: corte pelas pontas das paredes cortadas. **Inferido:** fundações e piscinas no modo baixo, meias paredes fora do corte. `cSimplifiedWall` é só dos imposters do bairro. (`walls-floors-mac.md` §3) |
| Objetos com a parede baixa: `cTSObject::HideWhenWallDown`, `SuppressHideWhenWallDownFlag` | `app/build_mode*.cpp`, `vm/placement.cpp` | fiel | Issue 002. |

### 3.7 Autonomia

Código: `src/vm/autonomy*.{hpp,cpp}`, `motives.cpp`, `src/app/pie_menu.*`.
Docs: `primitives-mac.md` (0x03, 0x21, 0x1D, menu de pie),
`data-findings.md` §16.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| `cTSPerson::TryFindBestAction`, `FBA_ScoreInteraction`, `FBA_CheckInteractions`, `FBA_RunCheckTree` | `vm/autonomy.cpp` | fiel | Parâmetro 0 = 1 das check trees conferido (`FUN_004c5cb0` com autônomo 1). **Inferido:** o uso da memória de falhas (8 por 9999 ticks); o filtro de objetos em uso (od 62 somado nas partes; o `FBA_FindStaticInteractions` do BV não lê o od 62 e pula fora do mundo, fila de morte, `AreInteractionsDisabled(1)`, od 56 bit 0: `primitives-mac.md` §4j). |
| `TryFind5WorstMotives`, `Set Motive Change`, laço de `cTSPerson::OnSimulatorTick` | `vm/motives.cpp` | fiel | |
| Menu de pie: `cEdithObjectTestSim::TestInteraction`, `AppendInteractionsForAuto`, `sMakeNewMenuItem` | `app/pie_menu.cpp` | fiel | |
| Vieses (dono, memória, relação, clima) | `vm/autonomy.cpp` | fiel | No base, só o de dono pesa. |
| Registros de objetos da pessoa: `cTSPerson::cObjectRecord`, `+0x60c` | `vm/records.cpp` | fiel | Confirmado: o `OnGosubObjectTree` os cria a cada `GosubObjectTree` (objeto, profundidade, a flag do Run Functional Tree), o `StackJustPopped` os tira, são o od 62, vão no PERS, o reset reseta os objetos deles e quem os usa (`CleanupPeople`), o Test Sim Interacting With os lê. **Inferido:** as árvores de um tick rodam numa thread à parte (os registros delas saem com ela); os quadros que o Reach e as Animate Sim/Overlay empurram não existem aqui; o filtro de uso do Find Best Interaction soma o od 62 das partes. (`primitives-mac.md` §4j) |

`cTSPerson` tem 238 métodos e 63 nomeados: é a classe de jogo mais lida
depois do `cTSObject` (409, 129).

### 3.8 Motivos, desejos, memórias e relacionamentos

Código: `src/vm/motives.*`, `environment.*`, `wants.*`, `src/formats/wants.*`,
`src/app/wants_panel.*`. Docs: `wants-mac.md`, `primitives-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| `cTSMotives` (17, 4): `GetMotiveRef`, `GetOldMotiveRef`, `UpdateMood`, a atualização de 60 ticks (vtable 0x10); `cTSEnvironmentScoreManager` | `vm/motives.cpp`, `vm/environment.cpp`, `formats/environment_catalog.cpp` | fiel | Confirmado: decaimento, trava ±100, humor do base, ambiente (bate com os 55 Sims salvos a menos da luz), passo de 60 pelo tique do simulador. **Inferido:** `HasModel` de todo objeto, sala −1 fora do mundo; sem host (headless) a luz fica de fora. (`motives-mac.md` §3–§5, §8) |
| Curvas: `cTSMotiveCurve`, `cTSMotiveCurveSet`, `cTSPiecewiseFunction` (15, 0), `cTSMotiveEffects` | `vm/motives.cpp`, `vm/autonomy.cpp` | fiel | Confirmado: leitura, inserção, conjuntos e pontuação do base. (`motives-mac.md` §1–§2, §6) |
| Desejos e medos: `cTSWantSimulator` (92, 28), `cTSWantNode`, `cTSWantInstance` (41, 0), `cTSWantTreeManager` | `vm/wants.cpp`, `vm/gnu_sort.hpp` | fiel | Confirmado: raízes por id de árvore, `std::sort` da libstdc++, operadores, check tree, `RawAspirationScore`. **Inferido:** NPCs com neighbor ID; ordenação lida só no BV. (`wants-mac.md` §9) |
| Memórias: `cTSMemoryDefinition`, `cTSMemoryInstance` (18, 0), `cTSMemorySimulator`, `cTSPerson::UpdateMemories`/`PruneMemories` | `vm/memories.*`, `app/wants_panel.cpp` | fiel | Confirmado: tokens, decaimento diário, poda, pontuação (bate nos 50 Sims dos lotes). **Inferido:** o `$Constant` dos textos. (`relationships-mac.md` §4) |
| Relacionamentos: `TryNewRelationship` (fiel), `cTSRelationshipTable` (18, 0), `cTSRelationshipEntry`, `cTSFamilyRelationship` (21, 0), gravador das `SREL` | `vm/primitives.cpp`, `vm/lot.cpp`, `vm/lot_records.cpp`, `vm/hood_*.cpp`, `vm/ownership.cpp` | fiel | Confirmado: tabelas por objeto (XOBJ) e por vizinho (SDSC), chave crua, ordem do `GUIDHash`, SREL versão 2, morte de objeto. **Inferido:** ordem num mesmo balde, apagar as SREL de quem morreu. (`relationships-mac.md` §1–3, §6) |

### 3.9 Salvar e carregar

Código: `src/vm/lot_save.*`, `lot_records.*`, `lot_resume.*`,
`save_types.*`, `hood_save.*`, `family_write.*`,
`src/formats/lot_objects*.cpp`, `character_write.*`, `neighborhood_write.cpp`,
`src/app/game_save.*`. Docs: `save-mac.md`, `game-flow-mac.md` §7.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| XOBJ, XMTO e PERS: `cTSObject::Write/Read`, `cEdithMTObject::Write/Read`, `cTSPerson::Write` | `formats/lot_objects*.cpp`, `vm/lot_save.cpp` | fiel | Layout pela versão do ObjM. |
| ObjM: `cEdithObjectModule` (`read_pre_object`, `SplitLoad`) | `formats/lot_objects.cpp` | fiel | Órfãos do persist DB ignorados, como no jogo. |
| Campos sem nome: cabeçalho, token, iteradores, timers, XMTO, NGBH (`cTSObject::WriteHeader`/`WriteTimers`/`AddTimer`/`RunTree`, `cTSTreeStack::Write`, `cEdithMTObject::Read`, `cTSNeighborhood::TileCountX`/`SetSize`) | `formats/lot_objects*.cpp`, `formats/neighborhood*.cpp`, `vm/lot_save.cpp`, `vm/lot_resume.cpp`, `vm/timer.cpp` | fiel | Confirmado: todos com nome e lidos/gravados pelo sentido; XMTO conferido (9.562 de 9.562); no NGBH, +0x34/+0x38 o tamanho do bairro em tiles e +0xFD um bool que só o Read e o Write tocam; o dono 0x22 só de leitura, 1–15. **Falta:** os dois campos que fazem o `cTSObject::Read` descartar 6 u32 + u16 (nunca nos lotes do jogo). (`save-mac.md` §8, §9.4, §9.5) |
| Rota e animação em andamento | `vm/lot_resume.cpp` | fiel | `save-mac.md` §7c. |
| Controladores: `cTSLoadLotController::LoadLot`, `cTSSaveLotController`, `cTSSaveNeighborhoodController` (8, 6) | `app/game_save.*`, `app/lot_loading.mm`, `vm/hood_save.cpp` | fiel | Confirmado: `LoadLot`, `SaveLot` e os seis passos do `SaveNeighborhood` (checkpoint do grupo do bairro, `SanifyInventories` e o NGBH sempre, famílias, lotes/terreno só no completo, vizinhos, genética, árvore, desejos só no forçado, commit); o app grava o mesmo conteúdo e troca os arquivos juntos no fim. **Falta:** as duas marcas do salvar forçado (os vizinhos dos objetos `0x2ED43D0D` e o pedido da vtable 0x6C dos LotInfo) e o inventário zerado do vizinho-modelo. (`save-mac.md` §6, §9.1, §9.3) |
| Persist DB: `cTSPersistSystem` (122, 4), `cTSPersistDBRecord`, cache, `cGZDBSegmentPackedFile` | `formats/dbpf.*`, `app/game_save.*` | fiel | Confirmado: a transação (checkpoint, commit e descarte da lista de segmentos), o `Flush` do pacote (tipo de chave pelas instâncias altas, DIR, compactação a 15% de buracos, índice, buracos, cabeçalho 1.1 com datas 0) e a regra de compressão (todos, de 50 bytes a 16 MB, só abaixo de 0,8); o app escreve o arquivo inteiro como a compactação do jogo. **Falta:** o QFS do jogo (o nosso comprime um pouco menos; a ordem dos dados e do índice é a nossa, que nenhum leitor usa). (`save-mac.md` §9.2) |
| Inventários do bairro: `cTSNeighborhood::Write`/`Read`, `cTSInventory::Read`/`Write` | `formats/neighborhood*.cpp`, `vm/hood_save.cpp` | fiel | Ordem contados/singulares confirmada; os 16 dados do bairro (dono 0x22) lidos e salvos. |

### 3.10 Bairro e famílias

Código: `src/formats/neighborhood*.{hpp,cpp}`, `hood_*.{hpp,cpp}`,
`lot_arrivals.*`, `lot_evict.*`, `src/vm/hood_bridge.*`,
`lot_neighborhood.*`, `src/app/hood_*.{hpp,cpp}`, `main_menu.*`. Docs:
`neighborhood-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Terreno: `cTSNHoodTerrain` (66, 16), `nTSSG::cNHoodTerrain` (58, 8) | `formats/hood_terrain.*`, `formats/hood_scene.*`, `app/hood_view.cpp` | fiel | Confirmado: pesos e mapa alfa, normais, variantes, `nhoodLightMap`. **Inferido:** o gradiente sob a água, a cor `+0xd8` do estado. (`neighborhood-mac.md` §20) |
| Ruas, flora, enfeites: `cTSNHoodOccupantManager` (61, 6) | `formats/hood_scene.*` | fiel | Alisar e nivelar ruas. |
| Imposters: `cLotImposterManager` (38, 2), `cLotImposterDisplay` (31, 12) | `formats/hood_lots.cpp`, `formats/hood_scene.cpp` | inferido | Desbotamento lido: só na saia do lote, que o app não desenha. **Inferido:** `terrainShade`. **Falta:** a cor do `cTerrainShadeLight`; 7 marcadores. |
| Lotes: `cTSUserToolPlaceLotBase` (35, 16), `cTSUserToolMoveLot`, `cTSLotInfo`, entrar e sair | `app/hood_mode.cpp`, `formats/hood_lots.cpp` | fiel | |
| Famílias e vizinhos: `cTSFamily` (82, 10), `cTSNeighbor` (61, 3), `cTSNeighborManager`, `cTSFamilyManager` | `vm/hood_bridge.cpp`, `formats/neighborhood.*` | inferido | O jogo recusa despejar a família do lote aberto (`EvictFamily` BV `0x1434cc`); o app igual. **Inferido:** os 7 marcadores do `hood_bridge` (nomes das flags do FAMI/SREL, unidades da distância). |
| Mudar, despejar, valor do lote: `cTSUserToolNeighborhoodMoveIn`, `cTSUserToolEvictFamily`, `GetLotValue` | `formats/lot_evict.*`, `app/hood_*` | fiel | Os 3 CDs ficam fora da soma também no jogo (`IsInWorld` = `OnInit` feito e `IsOnLot` do tile, BV `0xaf627c`/`0xd1604`); a diferença está só nos valores gravados pela Maxis. |
| Câmera do bairro: `nTSSG::cCameraNeighborhood` | `app/hood_view.cpp` | fiel | |

### 3.11 CAS

Código: `src/app/cas_*.{hpp,cpp}`, `src/formats/cas_*.{hpp,cpp}`,
`sim_character.*`, `sim_censor.*`, `src/vm/genetics.*`,
`baby_character.*`, `appearance.cpp`, `src/app/head_shot.*`,
`dialog_window_clothes.cpp`, `src/render/cas_camera.*`. Docs:
`cas-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Telas: `cTSUISimEditor` (105, 8), `nTSUICAS::cTSUICASComponent*` | `app/cas_mode*.cpp` | fiel | Confirmado: sem biografia da família no CAS do base (botão `0x8CB7B638` escondido), passo → câmera do rosto (`EditSimFace`), aba → painel do rosto e dos cosméticos (`InitComponent`), a aba de cada passo ao abrir e as abas desligadas sem itens (`Activate`), as categorias e partes das roupas (`HandleTabCategory`), o PRONTO do `ValidateSim` (sem aspiração, pontos sobrando ou sem nome: recusa; o jogo não sugere aspiração), os dados 47–51 = cópia dos traços (`SetupPersonData`). **Inferido:** os dados 91/156/118/158 de um Sim novo (vêm do SimAntics), a caixa "Sim Changes" desenhada como a linha de estado, as contagens das abas pelas listas do app. (`cas-mac.md`) |
| Corpo e roupas: `nTSCASCore::cTSSimOutfitData` (133, 12), `cTSCASBodyPartSystem` (141, 7), `cTSBinCatalog` | `formats/cas_catalog.*`, `formats/character_write.*`, `app/cas_mode*.cpp` | fiel | Confirmado: corpo gordo (`0x8141c4`, `SetFitnessLevel`), botão aleatório (só a aparência), flags das categorias (`CheckInit`), o gerador de números (`FUN_00171674`). **Inferido:** a ordem dos sorteios e da lista dos tons 1–4, o cabelo de bebê. **Falta:** `MatchHairStyle`… do pedido de NPC (não lido). (`cas-mac.md`) |
| Rosto: arquétipos, modificadores, LxNR, `BlendDeltas` | `formats/cas_face.*` | fiel | Confirmado: máscaras (`BuildRegionMask`), `hairneck` parado, `ApplyModifier` (BV `0x809fc4`), rosto do filho pelo `BlendFaces`. **Inferido:** as normais refeitas. (`cas-mac.md`) |
| Pele, maquiagem, overlays: `cTSSkinTextureCompositor` (27, 0) | `formats/cas_face.*`, `sim_character.*`, `formats/character_write.*` | fiel | Confirmado: camadas do `GetMaterialLayersToBeComposited` e mistura do compositor (`dst += (src − dst)·α`). Máscaras e quantidades não aparecem nos dados do base. O shader faz a mesma conta em ponto flutuante, sem o reempacotamento DXT1. (`cas-mac.md`) |
| Genética: `nTSGenetics`, `cTSSimDNA` | `vm/genetics.*` | fiel | |
| Personalidade, signo, aspiração | `formats/cas_personality.*` | fiel | Tabelas não conferidas no base 1.0. |
| Gravar o personagem | `formats/character_write.*` | fiel | |
| Câmera do CAS: `cCameraCreateASimController` | `render/cas_camera.*` | inferido | Confirmado: pares spec ↔ localizador, roda, passo do zoom, a conta do olho inteira (`R(q)·Rx(e)·Rz(h)·y`, `q` do efeito de câmera). **Inferido:** a direção do zoom: nos eixos da câmera o zoom fica sempre `acos(cos h·cos e)` fora do eixo de visão (36,8° no adulto) e os números do `CASCamera.txt` só apontam para a câmera do rosto nos eixos do mundo (o app); falta o eixo de visão do jogo ou a convenção da matriz do efeito. (`cas-mac.md`) |

### 3.12 Desenho

Código: `src/render/*`, `src/formats/lot_model.*`, `lot_lightmap.*`,
`model.*`, `gmdc.*`, `txmt.*`, `effects.*`, `lighting_config.*`,
`src/app/lot_lighting.*`, `lot_effects.*`, `plumbob_effects.mm`, `lot_water.*`. Docs:
`shaders-mac.md`, `lighting-mac.md`, `effects-mac.md`, `roof-fence-water-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Materiais e shaders (caminho DX8 dos `.matShad`) | `render/shaders.metal`, `renderer.cpp`, `formats/model.cpp`, `formats/image.cpp` | fiel | Confirmado: passe de bump do DX8, recorte da pia (`CreateAndRegisterObjectCutoutMaterial`), `celShading.matShad`, a conversão altura→normal no envio de toda TXTR `Raw8` (base 1.0 `FUN_00322058`, `GenerateBumpMap_L8_to_A8R8G8B8` `FUN_0010379c`, o bump scale da TXTR). **Inferido:** os padrões de registrador do bump sem tangente. (`shaders-mac.md` §13.2–13.5) |
| Luz e light maps: `cLightingManager` (88, 31), `cLightMapManager`, `cDiffusionManager`, `cFloorColorGrid` (29, 17), `cWallDiffusionManager` (29, 13), `cDiffusionWall` (24, 10) | `formats/lot_lightmap.*`, `lot_diffusion.*`, `app/lot_lighting.*` | fiel | Confirmado: grade de piso, paredes, portais pelo construtor do jogo (`0x7149fc`). **Inferido:** a altura da luz, as retas montadas das faces, o `+0x20` da supressão (andar), a cor das células de parede fora da difusão, quais 8 luzes ficam num objeto. (`lighting-mac.md` §10–11) |
| Sombras: `nTSSG::cShadow` (32, 25), `cShadowManager` (`DoMessage`, `UpdateShadowsAdjacentToWall`, `UpdateLevelShadows`), height map do sol | `formats/object_shadow.*`, `formats/lot_lightmap.cpp`, `app/vm_objects.*`, `render/` | fiel | Confirmado: planos GUOB, "LM_sunHM", `UpdateGUOBs` do base 1.0. **Inferido:** a caixa de um GMDC sem bounding mesh (os vértices), o andar visível, o volume do espelho fora. **Falta:** a vtable `0x1f4` do base no `UpdateGUOBs` e o `0x1a0` que zera o estilo 3 (não lidos); o recálculo pelas salas de luz (não feito). (`lighting-mac.md` §11.6, §12) |
| Paredes e pisos: `cWallGeometryNode`, `cFloorGeometryNode` (32, 5), `cFloorManager` (31, 6) | `formats/lot_model.cpp` | fiel | Confirmado: nós de parede, chanfros, papel, tampas, encontros, pisos (UV, rotação), borda dos pisos (`cFloorThicknessGeometryNode`), UV das máscaras. **Inferido:** o +0,01 do UV1 do topo no lado A. (`walls-floors-mac.md` §3, §6) |
| Telhados: `nTSSG::c…Roof`, `nTSWorld::c…RoofType`, classes `c{Even,Odd}Triangle{First,Second}…` | `formats/lot_roof_game.*`, `lot_roof.cpp` | fiel | Confirmado: tipos 1–6 como o jogo. **Inferido:** a inclinação global (0,5), a ordem dos getters das texturas das faixas. **Falta:** `IsTriangleCenterSupportedByOtherRoof` do anel da mansarda; diagonais e pagodes ainda no OpenTS2. (`roof-fence-water-mac.md` §1.4–1.5) |
| Cercas: `cFenceGeometryNode` (57, 0), `cFenceManager` (41, 0) | `formats/lot_model.cpp`, `formats/lot.*` (marcadores) | fiel | Confirmado: trilho, poste, arcos, modelos, +0,75 do 3ARY `0x14`. **Inferido:** quem passa o `ishalfwall` ao tipo 3, um poste de arco em cada ponta, o estilo 4 como palco de expansão. (`roof-fence-water-mac.md` §2.1; `walls-floors-mac.md` §6.4) |
| Água e piscina: `cWaterGeometryBuilder` (35, 0), `cPoolGeometryNode` | `formats/lot_water.*`, `lot_model.cpp`, `render/shaders.metal` | fiel | Confirmado: alfa por profundidade (`Terrain.ini`), reflexo, leito e praia; a passada 2 da água do terreno (os dois normal maps animados, alfa 0,3); a piscina pelo caminho DX8 do Mac (ondas no `frameInfo.w` módulo 30 s, reflexo, camadas de fundo; sem passada de bump); as cáusticas (`causticsTiled` 8 × 4, 32 quadros por segundo, nos pisos `FloorPool` e nas paredes da piscina azul pelo v do papel). **Inferido:** o uv da malha da água e a escala/ordem da sua animação, os quads sem o vértice do centro, as coordenadas das camadas nas paredes (dados de tempo de execução), o espaço global = o do lote. (`roof-fence-water-mac.md` §3.1–3.3) |
| Efeitos: `cTSEffectsManager` (55, 1), `cTSParticlesEffect`, `cTSMetaParticlesEffect`, `cTSSequenceEffect` | `render/particle_system.*`, `render/particle_layer.*`, `app/lot_effects.*` | fiel | Confirmado: emissão, física, meta-partículas, LOD, colisores, `cTSSequenceEffect`. **Inferido:** as paredes e o quarto do piso do colisor (consultas do `WorldDB` não lidas), o terreno como piso, o padrão do `effectLODSoftChange`. **Falta:** terreno das partículas e metas, o atrator do alvo, o `screenBloom`. (`effects-mac.md` §4e–4f) |
| Objetos e Sims: `nTSSG::cObject` (140, 20), `nTSSG::cPerson` (50, 5), censura | `app/vm_objects.cpp`, `app/lot_view.mm`, `formats/sim_censor.*` | inferido | Confirmado: a pose salva, os bits de cinema do `cObject::OnInit` (`+0x108`), as malhas de limite do raio no `cGeometryDataContainer` (a rígida e uma por osso). **Inferido:** o clique pelos triângulos desenhados (os animados pelos posados) em vez das malhas de limite (o teste da folha do raio não foi achado). (`camera-ui-mac.md` §2.6, `save-mac.md` §7) |
| Espelhos: `MirrorReflectionMaterial`, alvo `ScreenReflection` | `render/renderer.cpp` | fiel | Confirmado: um observador por espelho, stencil dos quads. Nosso: no máximo 8 por quadro. **Falta:** o filtro de visibilidade do observador (máscara `0x07440100`, `InvisInReflections`), não resolvido. (`shaders-mac.md` §13.1) |

### 3.13 Câmera e UI de jogo

Código: `src/render/camera.*`, `src/app/camera_*.{hpp,cpp}`,
`live_ui.*`, `live_panels.*`, `options_panel.*`, `game_cursors.*`,
`game_flow.*`, `pie_menu.*`, `src/formats/camera_config.*`, `ui_*.{hpp,cpp}`.
Docs: `camera-ui-mac.md`, `ui-live-mac.md`, `game-flow-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Câmera: `cTSCameraSmoothSims1Controller` (58, 9), `cCameraLive` | `render/camera.*`, `app/camera_*` | fiel | Confirmado: teclado, arrasto do botão direito, borda da janela (`0x3b6c780f`/`0xbb6c7808`), pivô e pitch em 7 passos, `rotateCameraSims1Mode`. **Inferido:** as setas do mapa de dispositivo (o acelerador as consome), a amostra de altura do piso. (`camera-ui-mac.md` §1.11) |
| Vista de cima: `cTSMainToolbar::SetTopDown`/`SetMode`, a câmera "Build" do `cCameraLive` | `app/camera_mode.hpp`, `LotCamera::setDefinition`, painéis de Comprar e Construir | fiel | T só em Comprar e Construir, o flag lembrado no Viver, a troca copia pivô, rumo, distância e pitch; o `pivotheightabovefloor` do Build é o 1,2 do `cCameraLive::Reset` (BV `0x6a3c70`). (`camera-ui-mac.md` §1.7, §1.11) |
| Seguir o Sim: `cTSMainToolbar::AutotrackSim`/`CenterOnSim`, `cTSCameraSmoothSims1Controller::Update`/`HandleRequest`, `cTSWinProcSimChooserThumbnail` | `app/camera_follow.*`, `live_ui.cpp` | fiel | Zona morta medida do alvo do pivô ao ponto do piso do Sim, os movimentos do jogador acabam com o seguimento, o clique direito centra e segue, o esquerdo na selecionada centra e para. (`camera-ui-mac.md` §1.8) |
| Fade de objetos: `cTSSGSystem::ObjectFadeParams`, `cObject::UpdateVisibilityAndRendering` | `app/object_fade.hpp`, `lot_actors.cpp`, `Renderer::setInstanceAlpha` | fiel | "Obstructs View" (od 40 bit 3), alfa = (profundidade/distância)^queda. Confirmado: o `ParentFadeAlpha`; o fade da câmera de cinema (`FUN_0065deb0`, a caixa da cena e os parâmetros do `FUN_007b8834`, os bits `+0x108` do `cObject::OnInit` e do `Prep`/`Start`). (`camera-ui-mac.md` §1.10) |
| Câmera livre: `cCameraCameraManController` e a base de primeira pessoa | `render/free_camera.*`, `LotCamera::matchView`, `app_view.mm` | fiel | Confirmado: Tab/Y/Escape, números do `FirstPersonCameras.txt`, olhar com o mouse, órbita, "CameraManNH". **Inferido:** o FOV de partida, o sentido da órbita, o centro só no piso. **Falta:** a "First" (nenhuma tecla). (`camera-ui-mac.md` §1.9, §1.11) |
| Menu de pie: `cTSWinPieMenu` (36, 5) | `app/pie_menu.*` | fiel | |
| Velocidade, relógio, dinheiro: `cTSMainToolbar` (89, 10), `cTSWinProcLivePanel`, `cTSSimulator::Simulate` | `app/live_ui.cpp` | fiel | |
| Painéis: motivos, relacionamentos, emprego, desejos (`cTSWinProcPanel*`, `cTSWinProcMotiveBar`) | `app/live_panels.*`, `wants_panel.*` | fiel | |
| Painel de memórias: `cTSWinProcPanelMemories` (21, 0), `cTSWinProcMemoryThumbnail`, `cTSWinProcMemoryDescription` | `app/live_panels.cpp`, `app/wants_panel.cpp` | fiel | Miniaturas `0x4906500A` (fundo bom/ruim, ícone com máscara, seleção), descrição `0x4906500C`. **Inferido:** páginas por setas (o jogo rola); o painel expandido não é desenhado. (`relationships-mac.md` §5) |
| Fila de ações: `cTSUIActionQueue`, `cTSWinActionQueueIcon` | `app/live_ui.cpp` | fiel | |
| Clique e cursores: `cTSWinSceneGraph::ObjectAtGameWindowPoint` | `app/game_cursors.*`, `lot_view.mm` | fiel | |
| Fluxo do jogo: `cTSGameStateController` (65, 14), `cTSStartupController` | `app/game_flow.*`, `scene_*.mm` | fiel | Confirmado: caso Construir do dormitório (só no BV), estado da barra salvo no lote (recurso `0x8B0C79D6`), a pausa salva também no começo de um episódio do tutorial. **Inferido:** quem roda com o jogo pausado (o `SimFlag` 2; no app, os controladores do tutorial). (`game-flow-mac.md` §7.5–7.6) |
| Diálogos: `cTSUIDialogManager`, `cTSObjectDialog` | `app/dialog_window*.cpp`, `vm/dialog.cpp` | inferido | Confirmado: `SetParams`, `SetupDialog`, `DoLayout`, 35 tokens do `ParseUIString`, o formatador de números e hora do idioma (`cRZLanguageUtility`), o arquivo das strings (o dono do código), a caixa de fechar (não existe no jogo base), a ordem e as linhas dos telefones (a festa), as células das prateleiras, o painel pintado. **Inferido:** as colunas do livro de vizinhos, o slide da aspiração sem animação, o espaçamento do catálogo de desejos, o passo e o arrasto da barra de comida, o lugar das células na grade. **Falta:** os tokens de inventário, o ícone nomeado ("my", "rel", "job", a imagem do "gz"), o fundo pintado sob as imagens com `transparentbkg=no`. (`primitives-mac.md` "Dialog") |


## 4–7. Planos já feitos

Os planos das rodadas 1 a 42 (§5, §6 e §7, todas feitas) estão em
`docs/inventario-historico.md`, com a numeração de antes. Os achados de
passagem da antiga §4 foram corrigidos nos arquivos de origem.

## 8. Plano de rodadas 43 em diante (as linhas que seguem "inferido" ou "faltando")

Feito em 2026-10-16, com a §7 toda feita (índice 86,8%, 95,5 / 110). As
rodadas vêm das linhas de §3 que não são "fiel" e das sobras de cada rodada
no `TODO.md`. **Uma rodada por vez** (pedido do usuário, 2026-10-16): a
seguinte só depois da anterior entrar no main. Opus com Ghidra (só
leitura). Ordem por impacto no jogo base: primeiro o que se vê em todo Sim.

| # | Rodada | Linhas de §3 | Área |
|---|---|---|---|
| 43 | ~~**Corpo do Sim: transições** — como um pedido de animação passa ao seguinte: a forma dos intervalos de blend (`cBlendInterval::Value`, baserom `0x3436c8`; o app faz entrada e saída lineares), a saída do pedido velho enquanto o novo entra (`CreateTransitionBlend`/`FUN_00a46c00`, `EndActionTicksOfPreviousBlockingPrimitive`; hoje o nível −1 é nosso), a velocidade dos pedidos pelo dado 85 (`OnPreSimulatorTick` `0x6980d8`, vtable `0x34`) e as sobras da locomoção (n14: a janela do `ComputeLocoStartTicks` com o fim da primitiva anterior, a troca centrada ±b/2, a rampa de velocidade `FUN_00a47144`, a parada fraca, o fade de entrada ao replanejar andando, o começo 5/100 ms adiantado)~~ **feita** (n43, `anim-mac.md` §13): o intervalo de blend é linear e a função de peso do pedido foi portada (entrada/saída encolhidas num pedido curto); o Animate Sim acaba quando o pedido começa a sair e o pedido segue saindo (eventos, 9998/9999) enquanto o próximo entra (o nível −1 saiu dos Sims), sem 9997 sem saída, com a sincronia do byte 8; a pressa do dado de objeto 85; a janela do `ComputeLocoStartTicks` e o pedido novo ao replanejar; lidos e não feitos a troca centrada, a rampa de velocidade, a parada fraca e a pressa da locomoção (o 5/100 ms não se aplica) | 3.3 mistura, coreografia, locomoção | animação |
| 44 | ~~**Sentar, resto**: o salto da raiz (`PostSnap`, `cObject::ApplyAnimationOffsets`, `GlobalRootTransPosition`; o `stayInWorld` inferido), a faixa do STR# 0x92 pela altura em metros e a bandeja do bar, o susto com o Sim sentado (o jogo tira do slot antes?), as sobras de IK da n02 (ordem de Euler `0x01020100`, contatos em 0, alvo do outlet do nó)~~ **feita** (n44, `seat-mac.md` §D/§F/§I, `anim-mac.md` §14): o quadro de cada pedido e o `UpdateObjectTransform` lidos (na entrega do caminho todas as camadas ficam no mundo; nos pulos da VM o `stayInWorld` segue inferido: o Snap do jogo não chama o `PostSnap`); o Reach/Put pelo índice explícito e pela altura e distância do ponto (o xadrez nos quatro cantos, o copo do bar); o susto sentado é das árvores (o "Pre Route" é que levanta); a partilha do braço pela Euler XYZ, a última solução com os contatos em 0 e o alvo do outlet do nó | 3.4 D, F; 3.3 IK | animação |
| 45 | ~~**Diálogos de escolha**: `dialog_window_pickers.cpp` (telefone, arara, aspiração; 10 marcadores), o formatador de números e hora do idioma (`FUN_0006e164`), o `SetBuffer` da caixa de fechar, a pausa dos lotes 1 e 4 do Tutorial, as strings do diálogo de árvore semi-global~~ **feita** (n45, `primitives-mac.md` "Dialog", `game-flow-mac.md` §7.6): o formatador do idioma (milhares com "." em português, "5:00 pm" em inglês), as strings do arquivo do dono do código (a carta de chance lê o objeto de emprego), a caixa de fechar confirmada sem botão, os lotes 1 e 4 do Tutorial pausados, a ordem, as colunas e os ícones dos telefones, as células das prateleiras; não achados o slide da aspiração e a barra de comida, o ícone nomeado lido em parte | 3.13 diálogos | UI |
| 46 | ~~**VM, resto**: os registros de objetos da pessoa (`cTSPerson +0x60c`, a única linha "faltando": quem os cria, trocar o `useCount` calculado), o 109 da sub-ação, o 37/38/40/55 do `UpdatePersonDataFields`, `cTSGlobals`, `cTSDataTables`, as sobras da fila (dado 164, evento 4 do `ActionSkipped`, `0x214`/`0x2e8`, a troca da fila cheia `0x1FC`)~~ **feita** (n46, `primitives-mac.md` §4j): os registros pelo `OnGosubObjectTree`/`StackJustPopped` (o od 62 guardado, o reset em cascata, o Test Sim Interacting With), o 109 e o 164, o `UpdatePersonDataFields` a cada planejamento, a entrada 4 "queue skipped", os testes do objeto e o controle dos pais, a fila cheia que dá lugar, a interrompida largada como no base; `cTSGlobals` é o registro de serviços e `cTSDataTables` não existe no base | 3.1 fila, dados; 3.7 registros | VM |
| 47 | ~~**Salvar, resto** (a 26 da §6 do histórico): `cTSSaveNeighborhoodController`, `cTSPersistSystem`, o u32 `+0x34`/`+0x38` e o u8 `+0xFD` do NGBH~~ **feita** (n47, `save-mac.md` §9): os seis passos do salvar do bairro, a transação do persist system e o `Flush`/compressão do pacote lidos; o app grava o índice com o tipo de chave e a compressão do jogo, troca os arquivos do save juntos e regrava o NGBH saneado; +0x34/+0x38 = tiles, +0xFD um bool sem uso, o dono 0x22 só de leitura 1–15; o `PersonalInventory` é de expansão | 3.9 controladores, persist | salvar |
| 48 | ~~**Colocação, resto**: a camada de visibilidade do piso, a árvore "allow intersection", as pegadas dentro do tile, a ferramenta da lareira (`TryPlace` lido na n37), o `0x2c(1)` do `0xFBA4DA53`; terreno: `ConformsToCeiling`, `ObjectMayBeInFoundation`, a água ligada (`FUN_008b02d4`)~~ **feita** (n48, `buy-build-mac.md` §7.26): o teste de objetos e o `TestPlacementCommon` do base (camada 3ARY `0x15`, árvore 5, pegadas, portas, água, moirão, od 30), o Create New Object Instance na ordem do binário, a lareira no BA30 (a chaminé da ferramenta em 36 das 48 salvas), o `post_trans`, os marcadores da pasta de objetos, o `UpdateWaterElevations` inteiro e o `FUN_009ac8d0`; ficaram `ChangeFloorPatterns`/suportes, `FUN_009dae20`, `RemoveOldWalls` | 3.5 colocação, terreno | construção |
| 49 | ~~**Rotas, resto**: obstáculos (tile da parte de multi-tile, a caixa sem lista, o `+10` do `WorldOutsideSupport`, o estado 7), a escala 1/16 do slot por versão, os retângulos de custo 10 da escada, o "drop to floor below" (`0x98`), a direção relativa dos tipos 1/2; a quina da mesa do N001_Lot68 (semente 7)~~ **feita** (n49, `routing-mac.md` §9.5): o custo do piso do base (sem `+10`: neve do BV; o estado 5 = o terreno debaixo de um piso baixo), o `GetFootprint` encostado na grade de 1/8 (a caixa do Sim também: o passo de partida junto da quina passa), a caixa do modelo pelos limites das formas, o subíndice do multi-tile, os retângulos de custo 10 dos corrimãos, o SLOT pela versão do cabeçalho (x,y em dezesseis avos antes de `0x47`), o destino de sair do caminho com a própria direção, o "drop to floor below" lido (só o tipo 2), o 37 de um slot global + 1000 (o 1033 da árvore social) e o `Get Distance To` com 10 por andar (o 13 a cada tick do Mortimer) | 3.2 obstáculos, slots, objetivos | rotas |
| 50 | ~~**Água, objetos e cinema**: o bump e as cáusticas da piscina, os objetos animados pela malha de repouso (`nTSSG::cObject`), o fade da câmera de cinema (caixa `0x148`, bits `+0x108`), a velocidade da cena (`cCustomAnimTimer2`)~~ **feita** (n50, `roof-fence-water-mac.md` §3.3, `shaders-mac.md` §13.5, `camera-ui-mac.md` §1.10/§2.6, `cinematics-mac.md` §10): as cáusticas da piscina (gerador, fundo e paredes) e a passada 2 da água do terreno (a piscina não tem passada de bump no Mac), a conversão altura→normal do base 1.0 em toda TXTR `Raw8`, o `frameInfo.w` módulo 30, o fade da câmera de cinema inteiro (só na mudança); lidos e não feitos o clique pelas malhas de limite do GMDC (teste da folha não achado) e o efeito do 1 ms do relógio da cena (quem o chama não achado) | 3.12 água, objetos; 3.3 cinemáticas | desenho |
| 51 | ~~**CAS, resto**: qual aba abre qual painel de rosto, a pose dos traços, a aspiração sugerida, a rotação do zoom (`SetActive`), o gerador de números e os tons 1–4~~ **feita** (n51, `cas-mac.md` "Rodada n51"): aba → painel confirmado, cada passo abre na aba do `Activate` (cabelo; maquiagem ou a primeira com itens; top), abas sem itens desligadas, as partes das roupas pelo `HandleTabCategory`, o PRONTO do `ValidateSim` (o jogo recusa sem aspiração em vez de sugerir), os dados 47–51 do `SetupPersonData`, o gerador do jogo (`FUN_00171674`) no aleatório, que não mexe mais na personalidade; a conta do zoom relida inteira e mantida inferida (os dados só batem nos eixos do mundo) | 3.11 telas, câmera, corpo | CAS |
| 52 | **Bairro, resto**: a cor do `cTerrainShadeLight` (`terrainShade`), os nomes das flags do FAMI/SREL e as unidades da distância do `hood_bridge` | 3.10 imposters, famílias | bairro |
| 53 | **Locomoção, resto**: a troca centrada (±b/2), a rampa de velocidade (`CreateVelocityBlendFunction`, seguir os parâmetros), a parada fraca (`anim-mac.md` §8, §13.6) | 3.3 locomoção | animação |
| 54 | **Clique em objetos e Sims**: o teste da folha do raio nas malhas de limite do GMDC (`cGeometryDataContainer`: a rígida e uma por osso), no lugar dos triângulos desenhados (`camera-ui-mac.md` §2.6) | 3.12 objetos e Sims | desenho |
| 55 | **Sentar D, resto**: o `stayInWorld` nos pulos da VM (Snap, contenção, fim de escada), o Snap Into `0x04` (`ResetRootBones`) (`seat-mac.md` §D) | 3.4 D | animação |
| 56 | **Dados do motor, resto**: a sala pelo quarto de tile do ponto, o dado de pessoa 55 pelos slots do contêiner, quem chama o `SetGlobal` no motor (`primitives-mac.md` §4i, §4j) | 3.1 dados | VM |
| 57 | **Diálogos, resto**: as colunas do livro de vizinhos, o slide da aspiração, o espaçamento do catálogo de desejos, a barra de comida, a grade das células, os tokens de inventário, o ícone nomeado, o fundo sob `transparentbkg=no` (`primitives-mac.md` "Dialog") | 3.13 diálogos | UI |
| 58 | **Terreno, resto**: as flags de lagoa no começo de cada mudança, `ChangeFloorPatterns`/suportes, `FUN_009dae20` (forma do tile), `RemoveOldWalls` (`buy-build-mac.md` §7.23–7.26) | 3.5 terreno | construção |
| 59 | **Câmera do CAS**: o eixo de visão do jogo ou a convenção da matriz do efeito de câmera, para decidir a direção do zoom (`cas-mac.md` "Rodada n51") | 3.11 câmera | CAS |

As rodadas 53–59 (pedido do usuário, 2026-10-19: levar o índice ao máximo)
cobrem as últimas 9 linhas "inferido" de §3. A 54 e a 59 já foram tentadas
(n50, n51) sem achar a peça que falta; se de novo não acharem, a linha fica
inferida com o motivo escrito.

Cada rodada marca a sua linha "feita" com uma linha do que foi feito, como no
histórico, e põe o que sobrar no `TODO.md`.

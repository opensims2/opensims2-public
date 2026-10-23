# O corpo do Sim no binário de Mac: fila de coreografia, mistura e IK

Leitura do binário de Mac (Bon Voyage 1.10, `sims2_bv.ppc`) para a rodada
n02 (`docs/inventario-historico.md` §5, linha 2), 2026-10-15. Endereços BV.

Modo: **Ghidra MCP** (só leitura: nada renomeado, retipado nem salvo no
projeto do usuário) e, para as funções que o projeto não tem como função
(`0xd195a0`, `0xd37848`, ...), **Ghidra sem janela** numa importação nova e
descartável do binário (`-noanalysis`, com o mapa do `tools/symxfer`
aplicado e o `DecompileList.java`). Os nomes vêm do symxfer; a confiança de
cada um vai entre parênteses quando não é "exact".

Complementa o `docs/primitives-mac.md` §4 (níveis dos pedidos, canais delta,
tempo de blend `T` = 233,333 ms, Animate Stop) e o `docs/seat-mac.md` (as
partes E, G e I da issue 001 ficaram para esta rodada).

## 1. A ordem de um quadro (`cChoreoQueue::AnimUpdate`)

`cChoreoQueue::AnimUpdate(tempo, passo, b1, b2, marcar)` (BV `0xd195a0`,
"vtsize"; não é função no projeto, lida na listagem) faz, nesta ordem:

1. **Os pedidos** (`+0x30`, vetor de `cIChoreoRequest`): cada um recebe
   `AnimUpdate(tempo)` (vtable `0x0c`): estado (`+0x0C`: 0 parado, 1 tocando,
   2 saindo, 3 expirado), o início do blend de saída e a porção feita
   (`cChoreoRequest::AnimUpdate`, `0xd93bfc`).
2. **Os tratadores** (`+0x3C`: o esqueletal, o de IK, o de olhar): cada um
   `AnimUpdate(tempo)` (vtable `0x14`). O de IK (`cChoreoHandlerIK`) cria um
   `cActiveAnimIK` por cadeia de IK do alvo do pedido
   (`HandleChoreoRequest`, `0xd3ca08`) e tira os de pedidos que expiraram
   (`RemoveExpiredActiveAnimIKs`, `0xd3c2f0`, que também tira os outlets de
   IK do segundo passo).
3. **Os outlets** (um `cChannelUberOutlet` por osso; `+0x54` no primeiro
   passo, `+0x60` no segundo): cada um mistura os canais que o alimentam
   (vtable `0x10`). Os ossos das cadeias de IK ficam no **segundo passo**
   (`AddToSecondPass`): primeiro o corpo inteiro se mistura, depois o IK
   resolve a partir dele e os ossos das cadeias se misturam de novo com os
   canais de IK.
4. Com o último argumento ligado, os pedidos **expirados** (vtable `0xfc`)
   são marcados para sair (vtable `0x24`) e a fila os tira
   (`RemoveMarkedChoreoRequests`, `0xd1915c`).

Quem chama: o `nTSSG::cAnimatable::OnPreSimulatorTick` (`0x6980d8`, "near"),
a cada tick da simulação, roda a fila com "marcar" ligado e depois, para
cada `cAnimRequest` do TS cujo pedido de coreografia expirou, roda a fila de
novo no passo 2 e **tira o pedido da lista** (vtable `0x2ec`). Não há um
"guardar o último expirado": o `MarkExpiredRequestsForRemovalExceptMostRecent`
(`0xd1864c`) existe mas este caminho usa o comum (`0xd180f8`).

O mesmo `OnPreSimulatorTick` escala a velocidade dos pedidos pelo **dado de
objeto 85** "Hurry Speed" (em %; `v = 1 + p/100` se p > 0, `1/(1 − p/100)`
se p < 0) — feito na rodada n43 (§13.4).

## 2. Quando um pedido expira (`cChoreoRequest::IsExpired`)

`cChoreoRequest::IsExpired` (`0xd93a18`) põe o estado 3 conforme a **regra
de expiração** (`+0x30`):

| regra | expira quando |
|---|---|
| 2 | na hora |
| 3 | quando a porção feita chega ao fim (`porção ≥ fim − 1e-6`) |
| 4 | quando já começou (`+0xAC`) e o peso (`+0x38`) ≤ 0,001 |
| 5 | quando o peso ≤ 0,001 **e** a porção chegou ao fim |

O Animate Sim/Overlay salvos usam 5 com blend de saída e 3 sem
(`docs/save-mac.md`); o Animate Stop com blend usa 4 (`AbortAnimRequest`).
Um pedido em loop nunca chega ao fim pela porção. A locomoção
(`cChoreoRequestLoco::IsExpired`, `0xd539f8`) é igual, com o tempo e o fim do
caminho.

**O que acontece com os ossos.** O pedido expirado sai da fila no mesmo tick
(§1). Os canais dele saem dos outlets; um outlet sem peso nenhum **não
escreve o osso** (`docs/primitives-mac.md` §4), e o osso fica com o último
valor escrito. Ou seja:

- um Animate Sim que tocou uma vez e acabou deixa o corpo **parado na última
  pose misturada** onde nada mais toca;
- onde um pedido de nível mais baixo ainda toca (uma idle da cadeira, nível
  1), esse pedido **toma o osso na hora**, sem blend (o expirado já não pesa
  nada);
- a "pose segurada" não é de nenhum pedido: é a memória do osso. Se o corpo
  estava misturando dois pedidos quando o último expirou, fica a mistura.

No `AnimPlayer` isso era feito com o pedido terminado posto num nível −1
abaixo de todos (inferido, rodada 15/21), e o osso que nenhuma camada
escrevia voltava à pose base (a de repouso, num Sim). **Feito** (rodada
n02): cada osso que nenhuma camada escreve fica com o valor do quadro
anterior (`AnimPlayer::held_`; só os canais base contam: um delta sem canal
base, o da cabeça, não se acumula), também depois de um salto da raiz
(`stayInWorld`). Teste `test_anim` "held bones". **Continua inferido** o
pedido terminado no nível −1: é o nosso modelo da sobreposição no fim de um
pedido (o `CreateTransitionBlend` e o fim da ação do primitivo bloqueante,
que deixam o pedido velho sair por blend enquanto o novo entra, não foram
lidos); tirá-lo na hora faria cada troca de animação saltar, porque um
canal sozinho entrando já toma o osso inteiro (normalização). **Rodada
n43** (§13.3): o jogo não segura o pedido no fim; o Animate Sim acaba quando
o pedido começa a sair, e o próximo entra enquanto ele sai. Feito; o nível
−1 ficou só para o que não tem função de peso própria.

## 3. O peso de um canal (`cChannelUber::Weight`)

`cChannelUber::Weight(float* resto)` (`0xedbb90`, "fill"; o código bate com
o nome):

- peso = `Weight()` do pedido (vtable `0xbc`, o float `+0x38` do
  `cChoreoRequest`), que a função de blend do pedido (`cBlendFunction2`)
  atualiza;
- para os canais de IK (tipo do canal `+0x10` entre 4 e 6), multiplica pelo
  **peso de IK** da cadeia (o canal de peso de IK do ANIM, `+0x2C`,
  limitado a 0–1): o canal de IK pesa `peso × w`; o canal de FK do mesmo
  osso (flag `+0x30` = 0) pesa `peso × (1 − w)`. Por isso `applyIk`
  mistura a solução com a pose da animação por `w` (nlerp), como já fazia;
- o canal de tipo 6 ainda multiplica pelo canal de **contato** (`+0x4C`) do
  alvo: os vários alvos de uma cadeia entram no outlet do nó do alvo com
  peso `peso × contato`, e o outlet faz a média normalizada (§5).

**A forma do blend.** `cBlendFunction2::Init` (`0xd3363c`, "near") monta a
função de peso em intervalos (`cBlendInterval`, CLSID `0x4b96aa16`): entrada
(tipo 0, de `inicial` a `alvo`), começo em rampa (5, opcional), sustentação
(1), fim em rampa (4) ou meia-transição (3), saída (2, até `final`); com a
flag de "rampa" e sem duração de rampa, as rampas viram 50 ticks no máximo,
metade tirada de cada lado. `Value(t)` (`0xd32498`) acha o intervalo pelo
tempo desde o início (`+0x18`) e pede o valor a ele (vtable `0x34`).
O `cBlendInterval::Value` (BV `0xd28880`; o `0x3436c8` do baserom) é
**linear** — lido na rodada n43 (§13.1): entrada e saída lineares,
confirmado.

## 4. O solucionador de IK de duas juntas (`cIKStrategy7DOF`)

A estratégia `0x48EE7A41` ("7DOF"; vtable `0x28326a8`). Cada elemento da
cadeia ganha um **tipo de canal de IK** (`SetUberChannelIKTypes`,
`0xd36d38`), pela posição:

| cadeia | elementos (osso) e tipos |
|---|---|
| 3 ossos (pernas) | coxa **1**, panturrilha **4**, pé **6** |
| 5 ossos (braços) | braço **2**, bíceps **3**, antebraço **4**, pulso **7**, mão **8** |

**Translação** (`ComputeIKTranslationSolution`, `0xd37848`, lida no Ghidra
sem janela). Entradas: o alvo `G` e o vetor de torção (polo) `P`, os dois
lidos dos *outlets* dos nós (`cActiveAnimIK::ComputeIKTranslationSolution`,
`0xd1e698`: só resolve se os dois outlets têm peso > 0), no quadro do osso
de começo (o pai dele no mundo, mais a translação do osso).

1. Comprimentos: `a` = soma do Y da translação local (de repouso) dos
   elementos de tipo < 5 depois do primeiro (bíceps + antebraço; a
   panturrilha), `b` = a dos de tipo ≥ 5 (pulso + mão; o pé). Os ossos do
   esqueleto ficam ao longo do +Y.
2. `d = |G|`; `n = normaliza(G × P)`; `m = normaliza(n × G)` (no plano de G
   e P, do lado do polo).
3. `cos A = (a² + d² − b²) / (2 a d)`. Se `cos A > 0`: a direção do osso de
   cima é `v = normaliza(G + m · d · tan A)` (A a partir de G, para o lado do
   polo); senão `v = m` (a 90°; com `a > b`, como em braços e pernas, não
   acontece).
4. Rotação do osso de cima: a menor rotação que leva +Y a `v`, e depois um
   giro em torno do **próprio Y** do osso (`θ = acos(−n·X)`, com o sinal de
   `n·Z`) que deixa o eixo X do osso perpendicular ao plano de G e P: a
   dobradiça.
5. A junta (tipo 4) ganha **só um giro em X**: `π − acos((a² + b² − d²) / (2 a b))`
   (limitado a −1..1). É uma dobradiça: nenhum outro eixo.
6. Braços (tipo 2): **65 %** (`FLOAT_0274b6cc` = 0,65) do giro em Y do braço
   passa para o bíceps (tipo 3: `(0, 0,65 y, 0)`), e o braço fica com o resto
   (`Ry(−0,65 y)` aplicado de volta).

**Rotação** (`ComputeIKRotationSolution`, `0xd36ecc`; o `cActiveAnimIK` em
`0xd1e900` lê a rotação do outlet do nó de rotação do alvo): a rotação do
alvo no quadro do pai do primeiro osso de baixo (o antebraço; a
panturrilha). O pé (tipo 6) recebe a rotação inteira; no braço, **75 %** do
giro em Y vai para o pulso (tipo 7: `(0, 0,75 y, 0)`) e a mão (tipo 8) fica
com o resto (`Ry(−0,75 y)` antes).

Os valores saem como ângulos de Euler (`ConvertToEulerAngles`, ordem
`0x01020100`) para os canais de IK dos ossos, que entram no outlet do osso
com o peso da §3. A ordem exata dos eixos de Euler não foi decifrada (a
biblioteca `Math::` é de templates de expressão): **o "giro em Y" usado na
partilha de torção é inferido** como a torção em torno do eixo do osso
(decomposição giro-torção).

**Diferença no nosso código** (`formats/anim.cpp`, `applyIk`, antes desta
rodada): os dois primeiros ossos giravam pela menor rotação até o cotovelo e
até o alvo, mantendo a torção que a animação dava (o braço e a junta podiam
girar em qualquer eixo); a mão ficava com a rotação toda e o bíceps e o
pulso só "iam junto". O cotovelo caía no mesmo lugar (mesma fórmula de `A`),
mas a torção dos ossos e da pele não era a do jogo. **Feito** (rodada n02):
o `applyIk` usa o solucionador acima (tipos por posição, comprimentos pelo
Y, dobradiça em X, giro do braço pelo polo, 65 %/75 % da torção no bíceps e
no pulso); testes `test_anim` "arm IK" (dobradiça, partilha da torção).
Continua nosso: sem nó de polo, o polo é a junta como está.

## 5. Vários alvos e o que o IK lê

- `cActiveAnimIK` lê o alvo e o polo pelos **outlets** dos nós (base ×
  delta, já feito na rodada 15). Os alvos de uma cadeia (`cIKTarget`, tipos
  0/1/2) entram no outlet do nó do alvo com peso `peso do pedido × contato`;
  o outlet faz a média normalizada por peso — o que o `applyIk` já fazia
  (posição linear, rotação por nlerp, a soma normalizada). **Confirmado.**
- Sem peso nenhum no outlet (todos os contatos em 0), o `cActiveAnimIK` não
  resolve naquele quadro (`ComputeIKTranslationSolution` devolve falso) e os
  canais de IK ficam com a última solução. O `applyIk` usa o primeiro alvo
  nesse caso (inferido; continua assim: guardar a última solução por cadeia
  fica no `TODO.md`).
- O alvo é o do **outlet do nó**, isto é, a mistura de todos os pedidos que
  chaveiam aquele nó, não o do pedido sozinho. O `AnimPlayer` lê o alvo da
  própria camada (com os deltas do outlet). Igual quando só uma camada
  chaveia o nó (o caso comum); continua inferido no resto.

## 6. Partes E, G e I da issue 001

- **E. Sentado e parado.** Não existe um "sistema de idle" no motor: as
  idles são pedidos que as árvores tocam (Animate Overlay de nível 1). Entre
  duas idles, ou com a fila vazia, nenhum canal escreve os ossos e eles
  ficam com o último valor (§2). A regra "segura a pose" do `SeatModule`
  (regra 4) é isso, e vale para qualquer osso que nenhuma camada escreve,
  sentado ou em pé (§7). A idle em pé
  que o app toca quando a VM não toca nada (`LotSimulation::sampleAll`,
  `b.idle`) continua um substituto **nosso** das árvores de idle (inferido).
- **G. IK das mãos e pés.** §4 e §5. `HandleChoreoRequest` cria os
  `cActiveAnimIK` de cada cadeia; `FindIKTargetBone`/`ValidateIKTargetBones`
  (`0x94b4f4`/`0x94b7fc`) acham os ossos dos alvos de tipo 2 no objeto de IK
  (já feito na rodada 15).
- **I. Sustos e expressões sentado.** O motor não troca a animação por uma
  versão sentada: o `try_animate_sim` (rodada 13) cria o pedido sem olhar o
  slot, e a escolha do nome (`AnimPrefix` `0x68f440`,
  `DetermineAppropriateAnimTable` `0x68f7c8`) só olha espécie, idade e sexo
  (dados de pessoa 186 e 58: `a-`/`c-`/`p-`/`e-`, `d-`/`l-`/`f-`, "male-"/
  "female-"). Se a árvore toca o susto em pé com o Sim contido, o jogo também
  toca em pé, a partir da raiz no slot. Nada a mudar no motor; se a cena do
  `N001_Lot11` ficar estranha, é a árvore (a VM roda as árvores do jogo). A
  "a-noExpression" (só o rosto) toca por cima do corpo como parcial, pela
  mistura por ossos — igual ao jogo.

## 7. O que mudou no código e o que sobrou

Feito (rodada n02, um commit por parte, testes verdes, bateria de assento
sem problemas):

1. **IK** (`formats/anim.cpp`, `applyIk`): o solucionador do `cIKStrategy7DOF`
   (§4). Testes `test_anim` "arm IK: the joint turns about its X only",
   "twist split".
2. **Memória do osso** (`formats/anim_player.*`, `held_`): osso que nenhuma
   camada escreve fica com o último valor (§2). Teste `test_anim` "held
   bones".
3. **E, G, I** da issue 001 (§6): E e I confirmados (sem mudança de regra), G
   pelo item 1.

Sobras (no `TODO.md`): a forma dos intervalos de blend
(`cBlendInterval::Value`), a ordem de Euler da partilha de torção, a última
solução de IK guardada quando o contato é 0, o alvo misturado do outlet (e
não o da camada), a velocidade pelo dado de pessoa 85, e as transições de
locomoção (`cActiveAnimLoco::PushTurnBlendCycles`, `StopLocomotion`; lidas e
feitas em parte na rodada n14, §8).

## 8. Locomoção: o relógio das fases, as transições e as paradas (rodada n14)

Leitura da rodada n14 (`docs/inventario-historico.md` §6, linha 14),
2026-10-15. Modo: **Ghidra MCP** (só leitura); as funções que o projeto não
tem como função (o `PushLocoCycle`, BV `0xd3144c`) foram lidas na listagem
(desmontagem sem gravar nada). Endereços BV. Complementa o
`docs/routing-mac.md` §6.1 (o locomotor) e **corrige** a leitura da rodada
15 ("anim3") sobre os 5/100 ms.

### 8.1 As tabelas virtuais usadas

- `cLocomotable` (vptr `0x27d367c`): `0x2f0`/`0x2f4`/`0x2f8`
  `SetEndTicks`/`SetEndActionTicks`/`SetFinalCyclePortionCompleted`
  `OfPreviousBlockingPrimitive` (do `cAnimatable`), `0x340`/`0x344`/`0x348`
  os três getters, `0x41c` `LocoPlaceFromSceneGraph`, `0x420`
  `KillLocomotion`, `0x42c` `CreateLocoRequest`, `0x430` `FollowOneStep`,
  `0x43c` `CopyActiveLocoStateToEndingLocoState`, `0x440`
  `MarkActiveLocoStateAsCompleted`, `0x44c`/`0x450`/`0x454`
  `BeginShortStepPath`/`BeginCachedForwardPath`/`BeginCachedEndTurnPath`,
  `0x458` `IsFollowActive(estado)`, `0x478` `EstimateWhenPathWillBeBlocked`,
  `0x480` `ComputeLocoStartTicks`, `0x484` `CreateEventsForActiveLocoState`.
- `cChoreoRequestLoco`, vtable primária (`0x2837568`): `0x11c`
  `SetLocoSequence` (6 normal, 5 começo e parada sem ciclos, 1 virar no
  lugar), `0x158` `StartHeading`, `0x15c` `EndHeading`, `0x164`
  `LocoSequence`, `0x168` `VelocityMPS`, `0x16c` `VelocityMPT`, `0x174`
  `CycleWeight`, **`0x178` `CyclePortionCompleted`**, `0x184`
  `EndCycleWeight`, `0x188` `FinalCyclePortionCompleted`, `0x198`
  `CurrentPathLocation`, `0x19c` `CurrentLocoHeading`, `0x1a0`
  `CurrentPlacementHeading`. Base virtual `cIChoreoRequest` (vtable
  `0x28378c8`): `0x14` `AnimUpdate`, `0x9c` `EndTicks(bool)`, `0xa8`
  `CurrentTicks`, `0xec` `HasStartTicksPassed`, **`0xf0`
  `HasFadeOutStartTicksPassed`**, `0xf4` `ActualFadeOutStart`, `0xfc`
  `IsExpired`.
- O relógio é o de animação do scene graph, em ms (`TSSGSystem` → `0xd0` →
  `0x38`); `+0x1ac` do locomotor é o **início** da fase (o `Begin*Path(t)`
  e o `ComputeLocoStartTicks` o escrevem; o `DistanceCompletedEstimate`
  mede a distância desde ele).
- (Rodada n30.) O timer é o `nTSSG::cCustomAnimTimer2` (`DesiredElapsedTicksPerFrame`,
  `MaxElapsedTicksPerFrame`: ticks por quadro), e quando ele salta o
  `cLocomotable::AnimationTimerAdjustment` (BV `0x6ad1c4`) desloca os
  instantes guardados da fase (`+0x1ac`, `+0x1b0`, `+0x280`, `+0x284`) e o
  `cAnimatable` os dos pedidos (BV `0x689b7c`): a locomoção e as animações
  andam no mesmo relógio, o do simulador. No app o relógio da cena parava
  só com a pausa do jogador; um diálogo que bloqueia o jogo parava a VM e
  não as animações (o clipe de partida acabava debaixo do diálogo e, depois,
  a VM levava o Sim pelo resto do caminho com as pernas paradas:
  `tests/glides.sh`). Agora para com os dois (`LotSimulation::speedScale`;
  que o diálogo pare o timer como a pausa: inferido).

### 8.2 O começo da primeira fase (`ComputeLocoStartTicks`, BV `0x6ad69c`)

O `BeginFollow` põe o estado 1 e chama `ComputeLocoStartTicks(curto)`
(`curto` = a rota começa por um passo curto). **Não há 5/100 ms na primeira
fase**: a rodada 15 leu a vtable `0x41c` do `FollowOneStep` como o
`BeginFollow`, mas ela é o `LocoPlaceFromSceneGraph`; os 5/100 ms são da
troca de fase (§8.3). O que a função faz:

```
início = agora
eA = EndActionTicks da primitiva bloqueante anterior (NENHUM: fica agora)
se o Sim segue andando (+0x1b4 = CycleWeight ≥ 0): fica agora
se o estado "terminando" (+0x25c) ainda toca:      (um andar acabando)
    início = max(início, EndTicks desse pedido, ou o EndTicks guardado)
senão se não curto e |agora − eA| < 140:  início = eA + 70
senão, com e = EndTicks guardado e W = 40 (curto) ou 140:
    |agora − e| < W e agora < e:  início = e − W/2   (senão agora)
nesses casos a porção do ciclo (+0x1bc) = FinalCyclePortionCompleted guardado
por fim os três valores "da primitiva anterior" voltam a NENHUM
```

O **140** é o global `DAT_02c11c04`, escrito uma vez no inicializador
estático (`li r0,0x8c` / `stw r0,0x1c04(r2)` em `0x20c22b4`; antes dado
como "sem escrita achada"). É 0,6 × 233,3 ms, o fade de saída de um pedido
de locomoção sem parada (§8.3).

### 8.3 A troca de fase (`FollowOneStep`, BV `0x6b80e8`)

A cada tick, no estado ativo (`+0x188`) com o pedido já começado
(`HasStartTicksPassed`): o corpo vai para a raiz do pedido
(`LocoPlaceFromSceneGraph`; se falha, `EndFollow(1)` e o passo devolve 0:
**o teste por passo bloqueia na hora, sem clipe de parada**); guarda
`CycleWeight` (`+0x1b8`), `CyclePortionCompleted` (`+0x1bc`),
`VelocityMPS` (`+0x1d4`) e `CurrentLocoHeading` (`+0x1d0`). Quando o
**fade de saída do pedido começou** (`HasFadeOutStartTicksPassed`):

1. guarda `SetEndTicks(max(EndTicks(false), +0x254))`,
   `SetEndActionTicks(ActualFadeOutStart)` e
   `SetFinalCyclePortionCompleted(FinalCyclePortionCompleted)`;
2. se não foi interrompido (estado < 5) — e, fora da sequência 1 (virar
   no lugar), se o caminho passa de 0,5 ou faltam menos de 40 ms e o
   `EstimateWhenPathWillBeBlocked` não vê bloqueio —, **põe o Sim no fim
   do caminho com a direção final** (`+0x1c4`); passou:
3. `CopyActiveLocoStateToEndingLocoState`: o pedido velho segue tocando no
   estado "terminando" (`+0x25c`, que o `FollowOneStep` também roda, mas sem
   pôr o corpo);
4. sem mais fases: `MarkActiveLocoStateAsCompleted` e devolve 1 —
   **a rota acaba quando o último pedido começa a sair**, não quando o
   clipe de parada termina;
5. com fase seguinte (passo curto `+0x190` → caminho para a frente
   `+0x19c` → virada do fim `+0x1a0`): ela começa em **EndTicks − 5 ms**
   (ou − 100 ms com o começo da rota `+0x24c` > 0,25), estado 1, e os
   EndTicks guardados voltam a NENHUM.

Os fades do pedido (`InitChannelNetwork` → `FUN_00a46ea8`): entrada
**233,3 ms** (`DAT_02d3ee9c`, escrito em `0x24db194` do valor de
`0x274b6c0`), saída 233,3 ms com clipe de parada e **0,6 × = 140 ms sem**;
os dois encolhem na proporção quando o pedido dura menos que a soma
(× 0,9999 · duração / (entrada + saída)).

**Estado 3** (o pedido expirou sem a troca acima: bloqueio previsto,
caminho curto, lugar final tomado): a fase seguinte começa no **início da
fase que acabou** (`+0x1ac`) se ele foi há menos de N ms (N = 25 com
`+0x24c` ≤ 0,25, senão 140/2 = **70**), senão agora. Ou seja: uma fase que
acabou quase no mesmo instante em que começou não gasta tempo. No estado 1
o pedido é criado (`CreateLocoRequest`) e recebe um `AnimUpdate(min(agora,
início) − 1)`.

### 8.4 Dentro de um pedido: o `cLocoCycle` (mistura por distância)

- O `PushLocoCycle` (BV `0xd3144c`) dá a **cada** ciclo do pedido (o clipe
  de começo, os ciclos de andar, o de parada) a `BlendDistance` = `+0xec`
  do `cActiveAnimLoco` (`lfs f1,0xec(r24)` e a vtable `0x28` do ciclo,
  `SetBlendDistance`), além de início e fim (em distância), porções,
  laços e pesos. O `+0xec` é o 0,2 (≤ 0,95 × a distância do começo e da
  parada) já lido para as poses: **é a mesma mistura para todas as trocas
  dentro do pedido**.
- `cLocoCycle::AnimUpdate` (BV `0xd1190c`), com `D` a distância do pedido
  no caminho e `b` a `BlendDistance`: entrada `(D − início + b/2)/b`,
  saída `(fim − D + b/2)/b`, `w = clamp(min(entrada, saída), 0, 1)` e
  **`w = 0,5 − 0,5·cos(π·w)`**: a troca é centrada em cada fronteira
  (±b/2) com rampa cossenoidal. `b ≤ 0`: troca seca. Virar no lugar
  (sequência 1): por tempo, `(t − início + 55)/110` e `(fim − t + 55)/110`.
- A reprodução também é por distância: os ciclos de andar (tipos 100–199)
  na fração de `(D − origem)/distância do ciclo`; os outros (começo, parada,
  passo) em `(D − início)/(fim − início)` limitado a [0, 1] — o clipe anda
  junto com o quadro e fica no primeiro (antes) ou no último quadro
  (depois) enquanto mistura.
- O peso walk/walkShort de cada ciclo sai da distância dele:
  `(escala·d₁ − D_ciclo)/(escala·(d₁ − d₀))`.

### 8.5 O pé da parada (getter `0x178`)

No `InitChannelNetwork`, `p` = `CyclePortionCompleted` do pedido (`0x178`):
a porção em que o andar começa (`+0x1bc`: 0,5 parado, no `BeginFollow`; a
porção atual quando segue andando; 0 depois de um `Begin*Path`; a final da
primitiva anterior na janela do §8.2). O começo (tipo 6) e a parada (tipo 7)
pedem o clipe da porção **0,5 se 0 < p ≤ 0,5, senão 0** (0,5 = os clipes
normais, 0 = os "R"); a parada troca de pé a cada `n` ímpar do ajuste da
passada, até duas vezes.

### 8.6 Rotas curtas (`InitChannelNetwork`, `0xd2dc58`–`0xd2de0c`)

Com clipe de começo e `d ≤ 0,9·(começo_l + parada_l)` (`0x274b6d0`):
sequência 5 (sem ciclos); `S_l`/`S_s` = começo + parada longos/curtos;
peso curto `w = (S_l − d)/(S_l − S_s)`, no máximo 1, igual no começo e na
parada (cada um a sua mistura). Com `d ≥ S_s` os pesos "para a frente" dos
dois (`ComputeForwardWeightFromTurnDistance`) entram pela média; com
`d < S_s` os clipes curtos são **espremidos**: começo `d·s/(s + p)`,
parada `d − começo`. Entre 0,9·`S_l` e `S_l` o ramo geral dá o mesmo `w`
(`c = 0`).

### 8.7 A parada (`StopLocomotion`)

`InterruptActiveLocoState(t)` (BV `0x6b4fa0`): com velocidade > 0 chama o
`cChoreoRequestLoco::StopLocomotion(agora, forte = t == 2, 0)` (BV
`0xd53e20`) → `cActiveAnimLoco::StopLocomotion(forte, …)` (BV `0xd2f9b4`);
parado, o pedido expira (regra 3). Estado → 6. A parada:

- não faz nada se a sequência não é 5/6 e o pedido já começou (um passo de
  lado ou um virar no lugar tocam até o fim), se o ciclo atual é o clipe de
  parada (tipo 7), ou (forte) se é um ciclo de andar já na última metade de
  ciclo antes do fim dele;
- o ciclo atual é o último começado que contém `D`; os seguintes saem;
- **fraca** (outros tipos de fim): sem clipe; o ciclo de andar acaba no
  próximo apoio, na porção **1/6 ou 2/3** (`DAT_02d3ee98` = 0,6667), e a
  porção final fica 0,5 (em 1/6) ou 0 (em 2/3);
- **forte** (bloqueado): de um clipe de começo, a porção dele; senão o
  ciclo de andar **segue até o fim da meia passada atual** (`h = 0,5 − p`
  ou `1 − p`; 0 com `p = 0`), mais 5% de uma passada de sobreposição, e
  então o clipe de parada de 0° do pé da porção (0,5 quando `0 < p ≤ 0,5`
  bate com o espelho do ciclo, senão 0), longo/curto pela passada
  (`ComputeTurnWeight`); o caminho termina depois da parada; a função de
  velocidade é refeita (`FUN_00a47144`, a velocidade até agora e uma rampa
  de 25 ms), fades de 233,3 ms e regra de expiração 4.

### 8.8 O que mudou no código (rodada n14) e o que sobrou

Feito (um commit por parte, `ctest -j2` verde com os dados do jogo):

1. **O começo** (`routing_loco.cpp` `locoStep`): a primeira fase começa
   agora, parada no início do clipe no tick em que a rota começa (`lead`
   −1 tick: o `FollowOneStep` cria o pedido e o atualiza até agora, 0 ms) e
   entra pelo fade de entrada do pedido (233,3 ms, encolhido num pedido
   curto). Os 5/100 ms da primeira fase saíram.
2. **As trocas dentro do pedido**: começo → andar → parada cruzam pela
   `BlendDistance` (`+0xec`, a mesma das poses) na velocidade do andar, com
   rampa cossenoidal (`AnimPlayer::Options::ease`, contada desde a deixa).
3. **O pé da parada**: o do ajuste da passada a partir da porção do pedido
   (0,5 parado; a do andar quando segue), `stopFootOther`, trocando a cada
   meio ciclo ímpar (`fitStride`, `Loco::stopOtherFoot`).
4. **Rotas curtas** (`fitStride`): o ramo do binário (0,9, peso pelo
   comprimento, espremido abaixo dos clipes curtos), sem ciclos: a parada
   segue o começo direto (`Loco::shortWay`).
5. **A parada ao bloquear**: o começo toca até o fim e o andar termina o
   meio ciclo antes do clipe de parada do pé certo; o teste por passo
   (próximo lugar tomado) bloqueia na hora, sem clipe.
6. **O fim da rota**: a rota acaba quando o último clipe começa a sair
   (233,3 ms antes do fim da parada, 140 de um passo de lado; regra dos
   0,5/40 ms); o Sim vai ao fim do caminho e a próxima primitiva começa
   enquanto o clipe termina onde estava no mundo (`Object::locoHandOff`,
   regra 3 do `SeatModule`).

Medido (`OS2_DEBUG_GLIDE`, que agora mede também o pé apoiado desenhado, e
`OS2_DEBUG_LOCO`; 4.000 ticks headless, antes → depois): N001_Lot11 pé
apoiado 0,485 → 0,461 tile/s (começo 0,366 → 0,328, andar 0,538 → 0,537,
parada 0,467 → 0,376), fases começo/parada 54/25 → 48/47 (as rotas curtas
já não tocam um tick de andar entre as duas, e cada começo tem a sua
parada); N001_Lot68 0,539 → 0,490 (começo 0,386 → 0,357, parada 0,426 →
0,471), 37/17 → 47/48, paradas ao bloquear 2 → 7. Deslizes sem pernas: 1 e
0 episódios, como antes. No VM, o "slide" da parada (quadro contra o
clipe) sobe porque o quadro vai ao fim do caminho no tick da entrega; o
desenho fica onde estava (regra 3). Em 12.000 ticks, falhas de rota
(`OS2_ROUTE_FAILS`) main → n14: Lot68 19 → 28, Lot11 17 → 4 (os mesmos
tipos, 11/13 do "Route - Dynamic w/Lead in").

Inferido / sobrou (no `TODO.md`):

- ~~a janela do `ComputeLocoStartTicks` com o fim da primitiva anterior
  (`EndTicks`/`EndActionTicks`/porção final de um Animate: começar em
  `eA + 70` ou `e − 70`) — o VM não guarda esses tempos~~ — **rodada n43**
  (§13.6): feito;
- a troca centrada (±b/2 em volta da fronteira): aqui começa na fronteira
  (rodada n43, §13.6: lido, não feito);
- a rampa de velocidade do pedido (`FUN_00a47144`, também a da parada
  forte) e o fim do ciclo na parada fraca (1/6 ou 2/3) (rodada n43, §13.6:
  lidos em parte, não feitos);
- ~~o espaço da parada ao bloquear continua limitado aos 2 passos da previsão
  (inferido)~~ — **rodada n18** (`routing-mac.md` §9.1): o `CollisionCheck`
  bloqueia na primeira colisão do horizonte e a parada (meio ciclo e clipe)
  só toca quando acaba 2 passos antes dela; senão o andar acaba na hora
  (`EndFollow`). No jogo base quase sempre na hora (paradas com clipe nas
  medidas: 80 → 16 em 14 sementes);
- o começo 5/100 ms adiantado de um pedido seguinte (a virada do fim depois
  de uma parada não acontece aqui: a parada já vira para a direção final;
  rodada n43: não se aplica);
- ~~um novo pedido ao replanejar andando (o `BeginFollow` de novo) não entra
  pelo fade de entrada~~ — **rodada n43** (§13.6): feito.

## 9. O arquivo ANIM e a leitura das curvas (rodada n23)

Leitura da rodada n23 (`docs/inventario-historico.md` §6, linha 23),
2026-10-15. Modo: **Ghidra MCP** (só leitura) e, para as funções que o
projeto não tem como função, **Ghidra sem janela** num clone APFS do
projeto (`-readOnly -noanalysis`, `DecompileList.java`). Endereços BV.

### 9.1 `cAnimResourceConst::ReadFrom` (BV `0xd491a8`) e `WriteTo` (`0xd4a20c`)

O `ReadFrom` lê o bloco inteiro num buffer e só acerta ponteiros nele
(`Swizzle*Record`): o layout é o que o `opensims2` já lia (OpenTS2), e o
`WriteTo` mostra o que vai em cada campo que pulávamos:

- **Os "ponteiros de execução"** (16 bytes do cabeçalho; 8 + 12 de cada
  alvo; 8 + 4 + 4 de cada canal; o u32 de cada componente; 8 + 4 de cada
  cadeia de IK; 8 de cada alvo de IK; 4 + 4 de cada evento) são escritos
  como o marcador **`0x11BA05F0`**; o byte depois do formato de cada
  componente é 0; cada CRC das cadeias e dos alvos de IK é seguido de um
  byte 0. Antes de gravar, o `WriteTo` põe 7 na versão dos dados (`+7`); os
  11.757 ANIM do jogo base têm 6 (gravados por outra versão da ferramenta)
  e bloco versão 6.
- **O preenchimento depois das strings** (as duas do cabeçalho, as etiquetas
  dos alvos, os nomes dos canais) é de `comprimento % 4` bytes — o
  `ReadFrom` pula `(soma + 2) & 3` / `soma & 3` bytes **sem olhar o valor**
  e o `WriteTo` escreve 0, 1, 2. Não alinha a 4 (o que era a regra do
  OpenTS2, agora confirmada). Não há preenchimento depois das chaves nem
  dos eventos.
- Para blocos de versão 3–5 o `ReadFrom` usa proxies antigos
  (`cSharedChannelCubicKeysConstProxy1/2/3`, registros de 0x3C bytes); os
  do jogo base são todos versão 6 (o `Proxy4`, registros de 0x18). Versão
  < 7 troca os nibbles dos bytes `+9`/`+10` do cabeçalho (prioridade e
  tipo de locomoção) — o que lemos como `priority`/`locomotionType`.

### 9.2 As chaves (`cSharedChannelCubicKeysConstProxy4`, BV `0x1c65e00`, `0x1c65388`)

O decodificador das chaves (`FUN_01c65e00`, chamado pelo
`HermiteCubicInterpolation` do proxy, BV `0xee4cb0`) trata o formato
(`& 0x1f`) caso a caso:

| formato | chave | valor |
|---|---|---|
| 0, 4, 0xC | 2 bytes (assada) | s16 × 1/128, 1/1024, 1/4096 |
| 1, 5, 0xD | 6 (tempo, valor, tangente) | 17 bits (o bit baixo no bit 15 do tempo) × 1/128, 1/2048, 1/8192 |
| 2, 6, 0xE | 8 (tempo, valor, tangente de entrada, de saída) | idem |
| 0x10 | 4 (assada) | float |
| 0x11 | 8 (u16 tempo, float, tangente) | float |
| 0x12 | 10 (u16 tempo, float, entrada, saída) | float |
| outros | 0 | 0 |

- **Tangentes.** Não são half floats: bit 15 o sinal, bits 10–14 o expoente
  `e`, bits 0–9 a mantissa `m`, **sem bit implícito**: `±m · 2^(e − 31)`
  (o código faz `(1 << e) · 4,7683716e-7 · m · (1/1024)`), em **valor por
  tick**. A interpolação é a Hermite cúbica com `m0` = tangente de saída da
  chave `a` × `Δt` e `m1` = de entrada da chave `b` × `Δt` (`Δt` em ticks).
  Era inferido como half float "por ~154 ticks"; nos dados (`s2pkg
  anim-sweep`, 1.045.374 chaves contínuas): tangente / inclinação central =
  **0,999** de mediana (5 %–95 %: 0,85–1,01).
- **Chaves assadas.** A chave `k` fica em `int(k · 33,333332 + 1/6)` ticks
  (o global `DAT_02dbc030`, escrito pelo inicializador estático em
  `0x2694630` do float `0x274b518` = 33,333332: **30 por segundo**),
  **qualquer que seja a duração do canal**; entre duas, a Hermite com as
  tangentes = diferença dos valores (linear). Líamos `duração · k / (n − 1)`:
  em 76.960 das 115.232 curvas assadas isso erra mais de um tick (até 3.163;
  o `eyedirection` do `t2o-mirror-applyZitCream`, 148 chaves num canal de
  4.700 ticks: a última chave fica em 4.900, a duração da animação).
- Antes da primeira chave vale a primeira; da última em diante, a última
  (`FUN_01c65388` acha a chave e o `HermiteCubicInterpolation` decide).

### 9.3 Rotações: Euler e amostragem por quadro (`cChannelStrategyQ`, BV `0xd451e8`)

- **Ordem de Euler:** `nGZSceneDBAnim::EulerToQuaternion` (BV `0x1b95508`;
  o `TemporalCoherentEulerToQuaternion`, `0x1b90288`, faz a mesma conta
  guardando senos e cossenos) dá `q = qz · qy · qx`: gira em X, depois Y,
  depois Z — **termo a termo o nosso `quatFromEulerXYZ`**. Graus: o canal é
  multiplicado pelo double `0x3f91df46a0000000` (π/180). Confirmado.
- **Amostragem por quadro.** O `ChannelDataFromCubicKeys` da estratégia de
  rotação (vtable `+0x24` da `cChannelStrategyQ`, vtable `0x28348b0`) não
  avalia a curva no tempo pedido: com o "frame sampling" desligado (o
  padrão: a variável `animationFrameSampling`, propriedade `0x0C7BFEA6`,
  registrada com `false` em `0x7bd990`, liga
  `cSceneDBAnimSystem::SetFrameSamplingEnabled`), pega o quadro
  `f = int(t · 30 / 1000)` (−1 se `t < 0`), avalia os ângulos em
  `t0 = int(f · 33,333 + 1/6)` e `t1 = int((f + 1) · 33,333 + 1/6)`, faz os
  dois quatérnios e devolve o **slerp** por `(int(t) − t0) / (t1 − t0)`
  (limitado a 0–1; `FUN_00dc75a0`: linear normalizado com |cos| ≥ 0,95, e
  pelo arco menor). Ligado, arredonda para o quadro mais próximo. As
  translações (`cChannelStrategyXYZ`) e os canais de 1 a 4 floats avaliam
  no tempo pedido.

### 9.4 O que mudou no código (rodada n23)

`formats/anim.*`: tangentes pelo `animTangentToFloat` (valor por tick ×
`Δt`), chaves assadas a cada 33,333 ticks, `Channel::rotation` (os dois
quadros e o slerp, usada pela pose, pelos deltas, pelos nós virtuais do IK,
pela câmera das cinemáticas e pela raiz da locomoção na VM),
`Animation::write` (o `WriteTo`), o preenchimento pulado sem conferir.
`s2pkg anim-sweep`: **os 11.757 ANIM regravados byte a byte** como o
`WriteTo` os grava, a razão das tangentes e as chaves assadas. Testes
`test_anim` ("tangent decoding", "baked keys a frame apart", "rotation
slerped between frames", "written back byte for byte").

## 10. O canal de eventos e o que as árvores de evento recebem (rodada n23)

### 10.1 Quem cria as chaves de evento

- As chaves do ANIM (`cEventChannel`, registros de 14 bytes, §9.1) são os
  tipos 1005 ("event=N", os snaps), 1014 (som) e 8 (efeito); as do motor
  são feitas pelo `cActiveAnimEvent::AnimUpdateDefault` (BV `0xd38bec`), que
  guarda quatro chaves próprias (`+0x3C`…`+0x48`) e as põe na lista de
  eventos do quadro: uma quando o pedido foi abortado (`IsAborted`, uma vez
  só), uma quando o fade de saída começa (`HasFadeOutStartTicksPassed`, no
  `ActualFadeOutStart`), uma no fim de cada volta (ao passar da duração; ao
  contrário, ao passar de 0) e uma no fim do pedido. Os tipos delas
  (−1, 7, 6/3) vêm do construtor, que não foi achado: que a do fim de cada
  volta seja a 6 (9998) e a do fim do pedido a 3 (9999) segue **inferido**
  (bate com a ordem 9998, 9999 no fim).
- **Toda** animação de um `cAnimatable` tem esse tratador: o
  `AddChoreoAnimRequest` (BV `0x696644`) cria um `cCustomEventHandler2`
  (`0x29d10617`/`0x3b4265f2`) para todo pedido, Animate Sim incluído. Logo a
  árvore de eventos do **Animate Sim** também recebe 9997, 9998, 9999 e −1 —
  o `opensims2` só mandava os "event=N" a ela. Nos dados (`s2pkg
  event-tree-sweep`): "Event - Get", "Event - Clean", "Event Tree - Read
  Stop", "Event - Walk and Sip", "Event - Introduce", "Animate - Burned -
  Events" e outras árvores de Animate Sim comparam o parâmetro 0 com 9997 ou
  9999.

### 10.2 `nTSSG::cCustomEventHandler2::HandleEvents` (BV `0x780ad4`)

Lido no Ghidra sem janela (a função não existe no projeto). Para cada chave
na janela do quadro:

- tipo 1005: o número é o `atoi` do campo "event"; tipo 7 → **9997**,
  6 → **9998**, 3 → **9999**, −1 → **−1**; 0x3F6 (som: `HandleSoundEvent` e
  `TriggerFootstepFX`); 8 (efeito, `FUN_00d85e68`); 0x3F7 ("nodeindex"); 4
  ("look_state", o olhar); 2 ("action" begin/end: muda o fim da ação do
  pedido); os outros vão ao `cEventHandler` base.
- O pedido (`cAnimRequest`, pelo `cAnimatable` vtable `0x264`) dá a árvore
  (`EventTreeID`, vtable `0xa4`), o stack object (`StackObjectID`, `0x9c`)
  e o GUID do código (`ObjectScopeGUID`, `0xa0`: o comportamento desse
  objeto, vtable `0x48`); sem árvore ou com o `State` < 0, nada roda.
- **Os parâmetros** (5 shorts em `sp+0xb8`): `[0]` o evento; `[1]` o
  **`StackObjectID` do pedido** (que o `try_animate_sim`, BV `0x69d634`,
  grava com o stack object do quadro — vtable `0x30` do
  `cITSTreeStackElem`, o campo `+0x0A` — e o `try_animate_overlay` igual);
  `[2]` `int(PortionCompleted × 100)` (`FLOAT_0274b5b0` = 100, o
  `cChoreoRequest::PortionCompleted`, vtable `0xac`); `[3]` `NumLoops`
  (`+0x44`, vtable `0xc8`: 1, ou o temp 1 do Animate Overlay com a flag
  0x20, −1 até parar); `[4]` `IsAborted` (vtable `0x88`).
- **Onde roda:** `cTSObject::RunTree(comportamento, stack object, árvore,
  parâmetros, tempo do evento, false, true)` (vtable `0x3ac`, BV
  `0xb02278`) no **objeto animado**: com o último `true` ele guarda os 8
  temps do objeto (`+0x8A`), roda `RunOneTickTree` (vtable `0x524`,
  `cTSTreeSim::RunOneTickTree` BV `0xe1ab54`) e **põe os temps de volta**:
  o que a árvore escreve nos temps não vaza para a thread. Se a tree sim do
  objeto já tem um erro (`+0x34`), nada roda; o erro da árvore vai para a
  tree sim do objeto (o que acontece depois com ele não foi seguido).
- O tratador do alcance (`cEventHandlerReach`, BV `0x752b3c`, o do
  `try_new_reach`) monta os mesmos 5 parâmetros e roda "CT - Reach -
  Events" e "CT - Grab"/"CT - Put" pelo nome.

**O que o `opensims2` fazia** (inferido): parâmetro 1 = o objeto de IK do
Animate Sim (o `TargetID` do pedido; nos casos comuns é o stack object
também), 2–4 = 0, os temps da thread do Sim compartilhados com a árvore de
evento (e escritos de volta), e os overlays com temps zerados e parâmetro 1
= 0. Nos dados, 25 árvores de overlay leem o parâmetro 4 (abortado: "Idle -
Events", "Event - Talk", "Facial Overlay - Continue", ...), "Facial -
Background - Events", "Events - Notify Out Of Idle" e "Events - Animate
Overlay Complete" leem o 1, "Facial Overlay - Continue Loop" o 2 e o 3, e as
árvores do alcance ("Event - Reach - Grab/Put") o 1.

### 10.3 O que mudou no código (rodada n23)

`vm/animation.cpp` (`runEventTree`): as árvores de evento do Animate Sim,
do Animate Overlay e do Animate Object recebem os 5 parâmetros do
`HandleEvents` (evento, stack object do pedido, porção em %, voltas,
abortado), rodam no objeto animado com os temps dele **postos de volta**
(o Animate Sim não compartilha mais os temps da thread com a árvore), e a
do Animate Sim recebe também 9997 (no começo do fade de saída, `kBlendTicks`
antes do fim; no fim com a flag 0x40 "sem blend de saída" — rodada n43,
§13.3: sem blend de saída o jogo não manda 9997), 9998 e 9999 no
fim. O `AnimationState` guarda o stack object do quadro (`eventStackObject`,
também ao recarregar) e o objeto de IK continua só para o IK e o save.
`OS2_DEBUG_ANIM_EVENTS=1` lista cada árvore com os parâmetros; `s2pkg
event-tree-sweep` conta o que as árvores do jogo comparam e leem.

Prova sem janela (`--lot N001_Lot68 --sim-id Angela --pie Fridge
--pie-select 3 --run-vm 1500`, 363 árvores): `params 100 553 0 1 0` (o
"event=100" do pegar, stack object 553), `params 9997 141 86 1 0`, `params
9999 141 100 1 0`, e os idles parados `params -1 27 42 1 1`. Na bateria de
assento, o "Facial - Background - Events" (que lê o parâmetro 1) mantém o
piscar dos Sims (`a-blinkUnderlay-*`), que antes sumia nos primeiros
segundos; o caso "chess" agora começa pelo menu (`--pie Chessboard`) em vez
de 8.960 ticks de autonomia, cujo caminho mudou e passou por um corte
(uma piada contada encostada num namoradeira, `TODO.md`).

Inferido / sobrou: qual chave do motor é a 6 e qual a 3 (o construtor do
`cActiveAnimEvent` não foi achado); o evento do fim de **cada volta** de um
overlay em laço (o `AnimUpdateDefault` manda uma chave a cada volta; aqui só
no fim); o `NumLoops` de um overlay que segura a pose (aqui 1); o tempo do
evento que o `RunTree` recebe (o 5º argumento) não é usado; o erro de uma
árvore de overlay é descartado (no jogo vai para a tree sim do objeto).

## 11. Look At no corpo (rodada n23)

### 11.1 `nTSSG::cAnimatable::try_look_at` (BV `0x6a19f8`)

O `XPrimParam` tem os operandos a partir do byte 4 (byte 0 = `param_2[4]`).
O que faltava ler além do que o `vm/view.cpp` já fazia:

- **O ponto do alvo, tipo 0 ("default 3/4 height", byte 14 = 0):**
  `FUN_00a45e54(&caixa, 0xF, nó, nó)` junta as caixas de todas as formas
  do nó raiz do alvo (o nó achado pelo `0xFA1C39F7`), no **quadro desse
  nó** (a transformação da raiz invertida), e o alvo é o centro da caixa em
  x e y e **`0,75 · (zmin + zmax)`** em z (`local_148 = c·0,5 + c`, com
  `c` o centro): três quartos de uma caixa que começa no chão. O pedido de
  olhar guarda o nó e esse deslocamento (`CreateChoreoLookAtRequest`, e o
  `SetTargetOffset` do `cAnimRequest`). Era inferido como o osso da cabeça
  (Sim) ou 0,75 acima do piso (objeto).
- **Slots** (byte 14 = 1 slot de mira — vtables `0x3e4`/`1000`/`0x13c` —,
  2 de rota — `0x3d8`/`0x3dc`/`0x138` —, 3 de contenção — `0x104`/`0x10c`/
  `0x134`): o slot do byte 8 (passou do número, erro 0x1B), o osso do slot
  pelo nome (vtable `0x16c` do pedido) e a posição do slot como
  deslocamento. Byte 0 `0x80`: a câmera (o nó da câmera, sem deslocamento).
- **Velocidades:** byte 4 × 2 (ou temp 1 com a flag 0x08) e byte 5 × 2 (ou
  temp 1/2 pelas flags 0x02/0x01 do byte 15), × π/180
  (`DAT_02c0e68c`, escrito do `0x274b4f8` pelo inicializador em
  `0x20b2fc4`): **radianos por segundo** ("TurnTowardsRateRPS",
  "TurnAwayRateRPS" do `cChoreoRequestLookAt`). O STOP (`0x01`) aplica a
  velocidade de volta do próprio STOP aos olhares que para (estado <
  `0x26B0` → `0x26B2`/`0x26B0` pela fila).
- Duração: s16 em 6 (ou temp 0 com 0x10), −1 = `DAT_02c0e324` (sem fim).
  A sala (sem 0x20, tem de ser a mesma, `0x204`), o "frustum" (sem 0x40,
  `FUN_00c49c44`: a cabeça alcança o alvo), o pedido `cAnimRequest` (ID
  `0x80000000`, "lat", tipo 7, árvore de eventos u16 9, `TargetID` o
  objeto, `SlotID` = slot | tipo << 12) e os dados de pessoa 41–44 (alvo,
  slot, estado `0x26AD`, duração).

**Os slots, no código (18/10).** O byte 14 escolhe a família de slots e o
byte 8 o índice **dentro dela**, na tabela SLOT do alvo: 1 → as entradas de
tipo **4**, 2 → as de tipo **3** e 3 → as de tipo **0** (as três famílias
que a comunidade documenta — target, routing e container; o `SlotID` do
pedido empacota os dois, `slot | tipo << 12`); passou da contagem daquele
tipo, erro 0x1B. Medido no conteúdo: a sonda nas varreduras do AL (8 Look
Ats), do base (7) e do N001_Lot11 (2) não achou nenhum índice fora da
contagem — a `Sub - LookAt` (tipo 3, índice 10 nas 22 entradas de tipo 0 do
Sim), a `Sub - Cat - Set Look At` (tipo 1), o Eye Attractor 7F1C288F (índice
3 nas 4 de tipo 4) e o índice 7 nas 9 de tipo 4 da fábrica de doces
7F959124 — e um censo estático (a passada do sweep, sem executar) achou
**316 leituras com tipo** no AL (157 de tipo 1, 124 de tipo 3, **nenhuma de
tipo 2**; 35 de `RayTrace`, que usa a mesma convenção) e 200 no base: o
maior índice de tipo 3 é **37**, e os **únicos** objetos que o cabem são os
dois tabuleiros de xadrez (**67** entradas de tipo 0 — as casas; todo o
resto tem ≤ 25), o que fecha a família de contenção; o avião de papel, que
tem as duas famílias no mesmo SLOT (0 1 3 3 4), tem Look Ats próprios
(7F385F33/1008 e 0x1022) com índice 0 nos dois casos, dentro das contagens.
Censo dos **17 `objects.package`** do UC (**4.121 SLOTs distintos**,
35.426 leituras — os pacotes dos EPs/SPs repetem): um objeto usa os tipos
**0, 3 e 4**; o **1** só aparece em **quatro objetos** — Baby Bottle
(7FBA6191), Baby Bottle - Smart Milk (7F228D60), Toy - Paper Airplane
(7F385F33) e Newspaper (7F08E23C) — e o **2** em nenhum. O tipo 1 (o
"location" da nomenclatura Sims 1) **não tem família no Look At** (aberto: se
a mira incluísse o 1, o avião de papel — o único com os dois — miraria o
`(0,0,1)` do entry em vez do osso; ficamos com o tipo 4, a família que o
nome "mira" e o conteúdo medem). O `vm/view.cpp` valida e guarda o slot
(`LookAt::slotKind`/`slotIndex`); o app mira o ponto do entry pelo osso
(STR# `0x90` "Model - Bones", os ossos desenhados de um Sim inclusive) ou,
sem osso, pelo deslocamento (`objectSlotPoint`).

### 11.2 O que o `cActiveAnimLookAt::AnimUpdate` faz (BV `0xd41e04`; lido inteiro e portado na rodada n34, §12)

É uma função de ~11 KB: a cadeia de IK de olhar (estratégia `0x09F9E87E`,
`cIKStrategyLookAt`) vira a cabeça e, com o "spine twist" ligado, 0,6 do
giro passa à coluna; o giro é limitado (`DAT_02d44e1c`, calculado no
inicializador, e 10° de folga, `DAT_02d44e20` = 0,1745; 30°, `DAT_02d44e24`;
200°, `DAT_02d44e18`) e segue um perfil de velocidade com rampas (fator
0,3–1 pela distância angular, rampa de 25–300 ticks, 0,7 da velocidade na
frenagem). Os estados do olhar são `0x26AD` (virando), `0x26AE` (segurando,
`HoldTicks`), `0x26AF`, `0x26B0` (voltando), `0x26B2` (acabou); cada troca
dispara o `TriggerLookStateEvent` (a árvore de eventos recebe o
"look_state", tipo 4 no `HandleEvents`). **Continua inferido** no
`app/look_at.cpp`: a divisão pescoço 40 % / cabeça 60 %, os limites de 75° e
30° e a velocidade constante (agora a do primitivo).

### 11.3 O que mudou no código (rodada n23)

`app/lot_vm.cpp` (`lookTarget`): o alvo é o ponto de 3/4 da caixa do alvo
— de um Sim, a caixa da pose de repouso das partes (inferido: a caixa das
formas de um modelo com pele); de um objeto, a caixa do modelo onde ele é
desenhado (`objectLookPoint`, pelas caixas de clique e o `VmObjectView`),
0,75 acima do piso só sem modelo. `vm/view.cpp` guarda as velocidades
(`LookAt::towards`, `Object::lookAwayRate`, também a do STOP) e o
`HeadLook` vira a essa velocidade (era 120°/s). `OS2_LOOK_AT_HEAD=1` volta
ao osso da cabeça para comparar. Captura:
`docs/screenshots/n23-olhar-cabeca.jpg` (`--lot N001_Lot68 --sim-id
Angela --run-vm 262 --pivot 15.1,22.1,1.9 --zoom 6 --orientation 1`,
dois Sims dançando que se olham: antes o rosto do outro, agora o queixo, a
3/4 da altura dele). Faltam os slots (tipos 1–3), a câmera, os estados e
eventos do olhar e o `AnimUpdate` inteiro (`TODO.md`).

## 12. O olhar inteiro, as chaves de evento e a IK de olhar (rodada n34)

Leitura de 2026-10-15 (`docs/inventario-historico.md` §7, linha 34). Modo:
**Ghidra MCP** (só leitura) e, para o que o projeto não tem como função
(`0xd46cdc`, `0xd3f46c`, `0xd38bec`, os inicializadores estáticos),
**Ghidra sem janela** num clone APFS do projeto (`-readOnly -noanalysis`,
`DecompRange.java`/`Listing.java`). Endereços BV.

### 12.1 As chaves do motor (`cActiveAnimEvent`, construtor BV `0xd3a290`)

O construtor (`FUN_00d3a290`, o que põe a vtable `0x2832d84`) cria as quatro
chaves do pedido e lhes dá o tipo (vtable `0x20` da chave):

| campo | tipo | `HandleEvents` | quando o `AnimUpdateDefault` (BV `0xd38bec`) a posta |
|---|---|---|---|
| `+0x3C` | 3 | 9999 | no fim do pedido (o tempo do fim, ou o atual se abortado) |
| `+0x40` | 6 | 9998 | no fim de **cada volta**: quando o tempo passa da duração e dá a volta (`t1 < t0`), ou quando chega nela sem dar a volta (o fim de um pedido de uma volta) |
| `+0x44` | 7 | 9997 | no começo do fade de saída (`ActualFadeOutStart`, intercalada com as chaves do ANIM pelo tempo) |
| `+0x48` | −1 | −1 | uma vez, quando o pedido é abortado (`IsAborted`) |

**Confirmado:** a 6 (9998) é a do fim de cada volta e a 3 (9999) a do fim
do pedido (era a ordem 9998, 9999 no fim, inferida). Num pedido em laço
(Animate Overlay com o temp 1 > 1, ou −1 até parar) o 9998 vem a cada volta,
com as chaves do ANIM da volta seguinte de novo; o 9999 só no fim. A volta
final só posta o 9998 se o fim do pedido cai na duração (`dVar21 ≥ dVar19`).

### 12.2 O `cActiveAnimLookAt` é uma IK de duas cadeias

`InitIKChannelNetworkPart1` (BV `0xd402fc`) cria duas cadeias de IK, ambas
com a estratégia de olhar (`0x09F9E87E`, `cIKStrategyLookAt`, vtable
`0x28349f8`), com o começo e o fim dados pelas vtables `0x120`/`0x124` do
`cActiveAnimLookAt` (`0xd3f18c`/`0xd3f1c4`, lidas no Ghidra sem janela):

| cadeia | começo → fim | canais (vtable `0x20(i)` do ativo) |
|---|---|---|
| `0x833D8054` (a mesma dos ANIM) | `head` → `head_grip` | 1 alvo (ponto), 2 polo (ponto) |
| `0x171B63B7` | `spine0` → `neck` (`LastBoneOfSpineIKChain` = "neck", BV `0xd3f200`) | 0 torção (quatérnio) |

`InitIKChannelNetworkPart2` (BV `0xd1fa78`) põe um canal de IK em cada osso
da cadeia: **base** de nível 8 (`AddBaseChannel(canal, 8, 2)`) na cadeia da
cabeça, **delta** (`AddDeltaChannel(canal, 8)`) na da coluna (começo
`0xE446CAC7`). `SetUberChannelIKTypes` da estratégia (BV `0xd46864`): duas
juntas → tipos 1 e 9; senão todos 6.

- **`ComputeIKTranslationSolution`** da estratégia (BV `0xd46cdc`, a cadeia
  da cabeça; só com 2 elementos): o quadro F = (posição do osso de começo,
  **rotação do pai dele**), no espaço do modelo (`FUN_00946890`: o pai
  relativo à raiz); G = F⁻¹·alvo, U = F⁻¹·polo, d = |G|, L = o Y da
  translação local do osso do fim (`head_grip`: 0,0818). Se d² − L² > ε:
  c = U × G (normalizado, se não nulo), b = normaliza(G × c), A = acos(L/d)
  (≤ 1; π/2 se L/d ≤ ε), e a rotação local do osso de começo é a matriz de
  colunas (c, b, G/d) vezes Rx(π/2 − A) (`FUN_00edd6e0`: a rotação em X de
  sempre). O Z do osso aponta do `head_grip` para o alvo, o X perpendicular
  ao plano do alvo e do polo (a cabeça de pé). Sem solução, os canais ficam
  como estavam.
- **`ComputeIKRotationSolution`** (BV `0xd46568`, a cadeia da coluna): o
  ângulo do quatérnio da torção, a = 2·asin(q.y), vai como Euler (0, a·fᵢ,
  0) a cada osso da cadeia, fᵢ = 0,2; 0,08; 0,05; 0,1 (`DAT_0273a700`), do
  começo ao fim (spine0, spine1, spine2, neck; a ordem é inferida pela da
  cadeia da cabeça, cujo canal 0 recebe a solução do osso de começo).
- **Nos ANIM:** 338 animações têm a cadeia `0x833D8054` com essa estratégia
  (`s2pkg anim-sweep`: "strategy 0x09F9E87E, 1 targets"; `head` →
  `head_grip`, alvo na translação de `head_ikctr`, polo `head_ikpole`):
  `a2o-sink-clean-loop2`, as do xadrez, das prateleiras, ... O `applyIk`
  pulava toda cadeia que não fosse 7DOF.

### 12.3 `cActiveAnimLookAt::AnimUpdate` (BV `0xd41e04`)

Campos: `+0x94` o pedido corrente, `+0x98` o último tempo, `+0x9c` o estado,
`+0xa0` o quatérnio do olhar q (no quadro de referência da cabeça),
`+0xb0` os Euler dele, `+0xbc` a torção, `+0xc4` "já dentro dos limites",
`+0xc8` o ângulo da rampa, `+0xcc` a velocidade, `+0xd0` os ticks da rampa,
`+0xd4` o q no começo da fase, `+0xe4`/`+0xe8` os fatores das velocidades de
ida e de volta, `+0xec` a rotação de repouso da cabeça. O tempo é o do
relógio de animação (ms).

1. **Pedidos.** O mais recente (`FindMostRecentChoreoRequest`) manda; os
   outros vão a `0x26B2`. Um pedido novo: estado `0x26AD` ("virando"); o
   `SetState` (BV `0xd3f46c`) guarda q em `+0xd4` e põe a velocidade em
   0,3 × a do pedido (`FLOAT_0274b670`); na volta (`0x26AF`/`0x26B0`) idem
   com a de volta; ao entrar na volta o pedido perde a função de blend e o
   peso vai a 1. Só se avança de estado (ou se volta ao `0x26AD`).
2. **Os quadros** (no espaço do mundo; o `0x18c` da fila, a raiz para a
   torção): Fref = (posição do `head` pelo `neck` do mundo, rotação do
   `neck` do mundo × a de repouso da cabeça), a cabeça "parada"; Fh =
   (posição do `head_grip` em Fref, rotação de Fref). Na primeira vez q =
   repouso⁻¹ × a rotação local da cabeça na animação.
3. **A torção** (só fora do primeiro quadro do pedido): o ponto 10 à frente
   de q (em Fh) no quadro da raiz dá o desvio a = atan2(x, z); com a flag
   0x04 ("including Spine", vtable `0x178`) o alvo da torção é a − 10° se a
   ≥ 0 (a + 10° se a < 0), preso em ±90° (`DAT_02d44e1c` = π/2, do
   inicializador `0x24edca0`); sem a flag, 0. Ela anda até ele a
   velocidade × 0,001 × ticks.
4. **O alvo** G = Fh⁻¹ · (o ponto do alvo) dá d = |G|, a = atan2(−y,
   √(x² + z²)) e b = atan2(x, z) − 0,6 × (o quanto a torção andou neste
   quadro); limites (`FUN_00c49a8c`, inicializador `0x2447bb0`): a entre
   −51° e +36° (positivo = para baixo), b ±70°, giro ±40° (sempre 0). O q
   desejado é o dos Euler presos; "preso" lembra se o alvo saiu dos limites.
5. **O perfil de velocidade** (estados `0x26AD`/`0x26AE`, alvo a 0,3 ou
   mais): θr = ângulo entre o Z de q e o desejado, θt = entre o de `+0xd4` e
   o desejado. No primeiro quadro: f = θt / 30°, preso em 0,3–1; a
   velocidade de ida (graus/s × π/180 do pedido) e a de volta × f; rampa =
   θt / 4 ÷ (velocidade × 0,001), presa em 25–300 ticks; o ângulo da rampa
   = velocidade × 0,001 × rampa / 2. A cada passo: na rampa (ângulo andado
   < ângulo da rampa) a velocidade sobe de 0,3 a 1 × a cheia (desacelera
   0,7 × cheia / rampa por tick); longe do fim (θr > ½ rampa) a cheia; perto
   do fim cai a (0,3 + 0,7 · θr / (½ rampa)) × cheia − 0,7 × cheia / rampa
   × ticks, nunca abaixo de 0,3 × cheia. O passo = média das velocidades ×
   ticks × 0,001; q vai ao desejado por slerp (passo / θr). Segurando
   (`0x26AE`), a velocidade cheia, seguindo o alvo.
6. **Chegou** (passo ≥ θr): q = o desejado; virando → `0x26AE`, ou
   `0x26B0` (voltar) se o alvo estava fora dos limites e sem a flag 0x02
   ("no early exit", vtable `0x16c`). Segurando, a duração (`HoldTicks`,
   `0x7FFFFF00` = sem fim) desconta os ticks; zerada → `0x26AF`. Alvo a
   menos de 0,3: a volta a 200°/s (`DAT_02d44e18`) e `0x26B0`.
7. **A volta** (`0x26AF`/`0x26B0`): o mesmo perfil para o q = identidade
   (a cabeça "parada" sobre o pescoço), com a desaceleração a partir de 3 ×
   o ângulo da rampa; chegando, `0x26B1`/`0x26B2` (o pedido acaba).
8. **O peso** (`cBlendFunction2` do pedido): ao virar, sem função ainda,
   sobe do peso atual (0,001 no primeiro pedido) a 1 em metade do tempo
   para chegar (θr / velocidade); na volta, desce de 1 a 0,001 no tempo da
   volta (θt / velocidade de volta), e acabado o blend acaba o olhar.
9. **A saída** (o ativo com os três canais): canal 1 = Fh · (q · (0, 0,
   d)) no espaço do modelo (o alvo da IK da cabeça, à distância do alvo na
   direção do olhar), canal 2 = Fh · (0, 1, 0) (o polo), canal 0 = giro em
   Y de peso × torção. Com `displayLookAtBoxes`, duas caixas de 6 cm
   ("Anim_LookAtBoxMaterial1/2") nos dois pontos.
10. **Os estados viram eventos:** o `SetState` posta uma chave
    "look_state" = o novo estado (`0x26AD` = 9901 ... `0x26B2` = 9906) para a
    árvore de eventos do olhar (u16 9 do Look At; tipo 4 no `HandleEvents`).

Assim a cabeça vira sozinha (o pescoço não), e com a flag 0x04 a coluna
(spine0 0,2; spine1 0,08; spine2 0,05; neck 0,1 da torção, como delta: peso
× peso × torção, inferido que o peso do delta é o do pedido) — não a
divisão pescoço 40 % / cabeça 60 %, 75°/30° e velocidade constante que o
`app/look_at.cpp` fazia (inferidos).

Inferidos: o nó da vtable `0x18c` da fila, o quadro da torção (tomado o
`root_rot`); `+0xec` ser a rotação de repouso da cabeça (`0x190` da fila);
a forma linear do blend; que a coluna recebe as frações na ordem
spine0→neck; o 0,3 de distância e o desconto da duração seguidos ao pé da
letra (inclusive o `lVar26 −= sobra` da chegada, que parece um descuido).

### 12.4 O que mudou no código (rodada n34)

- `formats/anim.cpp`: `lookAtIkRotation` (a solução de translação da
  estratégia) e o `applyIk` resolvendo as cadeias de olhar dos ANIM
  (`head` → `head_grip`, alvo `head_ikctr`, polo `head_ikpole`, pelo peso
  da cadeia). Teste `test_anim` "look-at IK".
- `app/look_at.*` (`HeadLook`): o `AnimUpdate` como acima — estados, rampa,
  limites, torção (com "including Spine"), "no early exit", a duração
  contada a partir da chegada (0 = uma olhada; −1 sem fim), a volta a 200°/s
  com o alvo a menos de 0,3, o STOP pela velocidade dele, o peso entrando e
  saindo — e a IK das duas cadeias sobre a pose animada (a cabeça pela
  solução de translação misturada pelo peso; spine0..neck girando em Y
  pelas frações, como delta). Era: pescoço 40 % e cabeça 60 %, 75°/30°,
  velocidade constante e a duração contada do pedido. Teste `test_look_at`.
- `vm/view.cpp`: o Look At guarda a duração em ms (`durationMs`; antes a
  duração 0 virava "sem fim"), as flags 0x04 e 0x02 e um número por pedido
  (`serial`: um pedido novo começa outra virada); o `lookTarget` do app não
  corta mais o olhar pelo tempo (quem conta é o `HeadLook`).
- Capturas: `docs/screenshots/n34-olhar-*.jpg` (antes = main, depois =
  n34).
- `vm/animation.cpp` (`tickOverlayEvents`): o 9998 no fim de **cada
  volta** de um overlay em laço (temp 1 > 1), com o resto das chaves da
  volta antes e as da volta seguinte de novo; um overlay de Sim até parar
  (temp 1 = −1, agora `forever` também nos Sims e salvo como `0xFFFFFFFF`
  voltas, como o jogo) roda a árvore de eventos a cada volta (as chaves do
  ANIM e o 9998; o 9999 não). Antes: o 9998 só no fim, e o overlay de Sim
  até parar não andava na VM (nenhuma chave). Medido no `N001_Lot68`, 12.000
  ticks: 378 → 1.162 eventos 9998 (o "a-facePose-frown-loop" de −1 voltas
  da árvore `0x1004` do 156 em dois Sims, a maior parte), o resumo da
  autonomia igual; `N001_Lot11` 313 → 345. Teste `test_animation_vm`.


## 13. Transições: como um pedido passa ao seguinte (rodada n43)

Leitura de 2026-10-16 (`docs/inventario-binario.md` §8, linha 43). Modo:
**Ghidra MCP** (só leitura, nada renomeado nem salvo), nos dois programas;
endereços BV, salvo onde diz "base" (o base 1.0, `sims2_base_1.0.ppc`).

### 13.1 A forma do blend (`cBlendInterval::Value`, BV `0xd28880`)

O `cBlendInterval` do BV fica logo depois do CLSID (`0xd28700`,
`0x4B96AA16`; o `0xd6cb84` que o symxfer chama de construtor não é):
`SetType` (`0xd287fc`, `+0x08`), `SetStartValue` (`+0x0C`), `SetEndValue`
(`+0x10`), `SetDurationTicks` (`+0x14`), os getters, `IsConstant`
(`0xd2883c`), `IsNegative` (`0xd28854`), **`Value` (`0xd28880`)** e
`Integral` (`0xd288d0`) — as mesmas 20 instruções do `0x3436c8` do baserom:

```
Value(t) = início                              se t ≤ 0
         = início + t · (fim − início) / d      se t < d
         = fim                                  senão
```

**Todo intervalo é linear** (o `Integral` é a integral dessa reta, com o
`0,5` de `0x274b4d4`). A função do pedido (`cBlendFunction2`, vtable
`0x2831c48`: `0x14` `Init`, `0x40` `BlendOutStartTicks` `0xd31ec8`, `0x48`
`BlendInDurationTicks` `0xd320d0`, `0x4c` `BlendOutDurationTicks`
`0xd321f0`, `0x58` `Value`, `0x74`/`0x84` o intervalo i e quantos) é uma
sequência deles; o `Init` (`0xd3363c`) põe a entrada (tipo 0, do valor
inicial ao alvo), a rampa opcional (5) ou meia-transição (3), a sustentação
(1), a rampa (4) ou meia-transição (3) e a saída (2, ao valor final), e o
tipo de blend diz onde a entrada começa: **0** no começo do pedido (a
sustentação é a duração − entrada − saída), 1 centrada (−entrada/2), 2
antes (−entrada). Com a flag de "degrau" (o `bool`) a entrada fica no
valor inicial e um 3 de no máximo 50 ticks faz a troca no meio dela.

### 13.2 A função de peso do pedido (`CreateTransitionBlend`, `FUN_00a46c00`)

`cAnimatable::CreateTransitionBlend` (BV `0x68b4c4`; argumentos pelos
slots do ABI do Darwin: `this`, agora, deslocamento, `tARType`, recurso,
porção inicial, 1, peso, voltas, velocidade, os dois `bool` de blend e a
saída) chama o `FUN_00a46c00` (= `nRZSceneDBAnim::CreateWeightBlendFunction`,
`0x8330ec` no baserom) com o **tipo de blend 0**, entrada = `T` se o pedido
tem blend de entrada (a flag 0x80 do Animate Sim desliga), saída = `T` se
tem blend de saída (0x40 desliga), `T` = 233,333 ms, valor inicial e final
**0,001** (`0x274b474`) e o alvo = o peso do pedido. O `FUN_00a46c00`:

- duração = `d·(voltas − 1) + d·(1 − porção inicial)`, dividida pela
  velocidade (em módulo); voltas −1 = sem fim;
- se `duração + 1e-6 < entrada + saída`, as duas encolhem na proporção:
  `× 0,9999 · duração / (entrada + saída)`; menos de 1 ms vira 0;
- `Init(0,001, peso, 0,001, entrada, saída, duração, ...)`, tipo 0.

Ou seja, o peso **sobe em reta** de 0,001 ao peso durante a entrada desde
o começo do pedido, fica, e **desce em reta** durante a saída, que termina
no fim do pedido. A única troca explícita é a das rotas: com `tARType` 5 o
pedido de rota anterior (vtable `0x32c(5)`) ainda ativo e com blend de
entrada sai pelo `FUN_00a47bf0` (`FadeOutChoreoRequestWeight`, tipo 2, `T`)
a partir do deslocamento, se ele acabaria mais de `T` depois.

### 13.3 O pedido velho saindo enquanto o novo entra: o fim da ação

- `cChoreoRequest::AnimUpdate` (BV `0xd93bfc`): com `BlendOutDurationTicks`
  > 0 e finito, quando `agora − início ≥ BlendOutStartTicks`, liga
  `+0xAD` (`HasFadeOutStartTicksPassed`, `0xd93a00`), estado 2 e guarda
  `+0xB0` = início + `BlendOutStartTicks` (`ActualFadeOutStart`,
  `0xd93a08`). **Sem blend de saída isso nunca acontece.**
- `cAnimatable::OnPreSimulatorTick` (BV `0x6980d8`; base `0x94765c`,
  igual), a cada tick antes das árvores, para cada `cAnimRequest` (do último
  ao primeiro; vtables do `nTSSG::cAnimRequest` pela ordem do baserom:
  `0x40` `SetHurrySpeed`, `0x48` `SetEndActionEventTicks`, `0x54`
  `SetActualEndActionTicks`, `0x88` `Type`, `0x90` `ChoreoRequest`, `0xac`
  `IsBlockingPrimitive`, `0xb4` `HurrySpeed`, `0xbc` `EndActionEventTicks`,
  `0xc8` `ActualEndActionTicks`): se o pedido de coreografia não expirou,
  o fim da ação ainda é NENHUM (`0x7FFFFF00`), não há chave "action end" no
  ANIM (`EndActionEventTicks` = −1: nenhum ANIM do jogo base tem uma, `s2pkg
  anim-events` só acha os tipos 1005, 1014 e 8) e o fade de saída começou,
  **o fim da ação = `ActualFadeOutStart`**; num pedido bloqueante o
  `cAnimatable` guarda `+0x13C` = `EndTicks(false)` (o fim do pedido),
  `+0x140` = o começo do fade de saída e `+0x144` = 0,5 (sem locomoção;
  os três "da primitiva bloqueante anterior", `0x68b06c`–`0x68b07c`). Se
  expirou com o fim da ação ainda NENHUM (sem blend de saída, ou cortado):
  o fim da ação = `EndTicks` e os mesmos três valores (`+0x140` = o fim);
  o pedido sai da lista, menos o bloqueante cujo fim da ação foi posto
  neste tick (sai no seguinte).
- `try_animate_sim` (BV `0x69d634`; base: o fim em `0x9538b8`, `subis
  0x7fff`/`cmplwi 0xff00`) devolve 2 enquanto `ActualEndActionTicks` é
  NENHUM e **1 quando ele foi posto**. Então **o Animate Sim acaba quando o
  pedido começa a sair** — `T` antes do fim (menos num pedido curto), ou no
  fim sem blend de saída — e o pedido segue tocando e saindo, com as chaves
  dele (as do ANIM, 9998 e 9999 no fim), enquanto a árvore segue. O
  próximo Animate Sim entra pelo blend de entrada **enquanto** o velho sai:
  é essa a sobreposição, e não um pedido segurado no fim.
- A chave 9997 (`cActiveAnimEvent::AnimUpdateDefault`, `0xd38ffc`) só é
  posta quando `HasFadeOutStartTicksPassed`: **um pedido sem blend de saída
  não recebe 9997** (a VM mandava no fim, inferido).
- **Sincronia** (byte 8 do Animate Sim): 0x02 começa o pedido em
  `EndTicks` da primitiva anterior e 0x08 em `EndActionTicks`, se a
  diferença para agora é de no máximo **140 ms** (base: o dado `0x1581bec`
  = 140; no BV o `DAT_02c0df64`, sem escrita achada); senão agora. Depois
  de criar o pedido os dois voltam a NENHUM (`0x69e650`). No jogo base,
  0x02 em ~400 e 0x08 em ~370 dos 4.010 Animate Sim (`s2pkg prim-usage 6A`):
  as cadeias "sem blend de saída" → "sem blend de entrada, sincronizado"
  (`C0` com 0x02, 202 usos) tocam emendadas, sem mistura.
- Interrompido (o objeto `0x228`), o pedido sai com `T` (vtable `0x3b4`) e a
  primitiva dá falso.

### 13.4 A pressa: o dado de objeto 85 (`OnPreSimulatorTick`)

O dado lido (`cITSObject::GetObjectData(85)`, vtable `0x34`) é o de
**objeto** 85, "Hurry Speed" no STR# `0x8D` (antes dado aqui como de
pessoa). Fator: `p/100 = f`; `1 + f` se `f > 0`, `1/(1 − f)` se `f < 0`;
**`p = 0` dá fator 0, que não muda nada** (um pedido apressado segue
apressado até acabar). O fator é 0 também com o bit 0 do `+0x108` do
`cObject` (vtable `0x1fc`; base `0x230`), não identificado. Para cada
pedido não expirado com `HurrySpeed` > 0 e diferente do fator: a
velocidade vira `fator · velocidade / HurrySpeed` (`FUN_00a4757c`: o que
tocou fica, o resto refaz a função de peso com a duração nova e as mesmas
entrada e saída) — a locomoção (tipo 7) escala os valores `0x160`/`0x164`
e o inteiro `0x174` do seguidor de rota (interface `0x6A6FD04D`, não
identificados) — e o `HurrySpeed` vira o fator. Todo pedido começa com
`HurrySpeed` 1, ou **0 quando "não apressável"** (Animate Sim byte 8 0x20:
o `try_animate_sim` chama `0x40(0)`). Quem escreve o 85 no jogo base:
"Function - Pre Route" (149), "Setup Kid" (200), "CT - Setup" (o
parâmetro), "Hurry - End" e "Interaction - Cancel" (0).

### 13.5 O que mudou no código (rodada n43, partes 1–3)

- **VM** (`vm/animation.cpp`): o Animate Sim tem a função de peso do pedido
  (`requestFades`: entrada e saída de `T`, 0 com 0x80/0x40, encolhidas num
  pedido curto) e **devolve verdadeiro quando a saída começa** (`T` antes do
  fim; no fim sem saída), no mínimo um tick depois de começar. O resto do
  pedido segue como "cauda" (`AnimationState::tails`, rodada pelo
  `tickOverlayEvents`): as chaves do ANIM que faltam e 9998/9999 no fim. O
  9997 só com saída. O Sim guarda o fim do pedido e o fim da ação
  (`previousEndMs`/`previousActionEndMs`, os `+0x13C`/`+0x140`); o byte 8
  0x02/0x08 do Animate Sim seguinte começa ali (dentro de 140 ms) e os
  limpa. Os overlays têm as mesmas entradas/saídas e o 9997 quando a saída
  começa (as flags do byte 2 como as do Animate Sim: inferido).
- **Pressa** (`tickHurry`, antes das árvores a cada tick): o dado de objeto
  85 escala a velocidade do Animate Sim, das caudas, dos overlays e dos
  eventos do Animate Object como acima; o "não apressável" de cada primitiva
  (Animate Sim byte 8 0x20; Animate Overlay byte 12 bit 0 e Animate Object
  byte 10 0x80, pelas strings do editor: inferido). Fica de fora a da
  locomoção (os valores `0x160`/`0x164`/`0x174` do seguidor de rota não
  foram identificados).
- **App** (`formats/anim_player.*`, `app/lot_vm.cpp`): o Animate Sim toca
  com a entrada e a **saída própria** do pedido (`Options::fadeOut`: linear
  nos últimos segundos, até o fim, onde a camada sai; um pedido novo da
  mesma prioridade não a corta, e os eventos de som dela seguem até o fim) e
  na velocidade do pedido (antes tocava a 1); os overlays com as entradas,
  saídas e velocidade deles. O próximo pedido entra enquanto o velho sai: é
  a sobreposição do jogo. O nível −1 do pedido expirado fica só para o que
  não tem função de peso própria (animações de objetos, os "grasp"),
  inferido. A idle em pé (nossa, §6 E) mistura sobre a pose que o último
  pedido deixou (`Options::overHeld`) em vez de tomar o osso inteiro.
- Testes: `test_animation_vm` (fim da ação 8 ticks depois num pedido de 0,5
  s, a cauda, a sincronia, as entradas/saídas, a pressa), `test_anim` ("own
  blend out", "over held"). O teste `front_door` passou a mover as portas a
  cada passo sem janela (como um quadro ao vivo) e a conferir porta por
  porta.
- Medido (`s2pkg autonomy`, 14 sementes × 12.000 ticks, `OS2_ROUTE_FAILS=1`;
  main → n43): `N001_Lot68` interações autônomas que acabam verdadeiras 615 →
  653, falsas 26 → 17, escolhas 664 → 684, falhas de rota 366 → 387 (sem o 13
  dos sociais 78 → 53), bloqueios 167 → 100; `N001_Lot11` 404 → 429 / 0 → 0,
  escolhas 439 → 459, falhas 426 → 391 (74 → 79), bloqueios 70 → 98. Cada
  Animate Sim acaba 7 ticks antes: cabem mais interações no mesmo tempo.

### 13.6 As sobras da locomoção (rodada n43, parte 4)

**Lido e feito:**

- **A janela do `ComputeLocoStartTicks`** (BV `0x6ad69c`, lido inteiro):
  `início = agora`; com `eA` = `EndActionTicks` da primitiva bloqueante
  anterior NENHUM, ou o Sim seguindo andando (`+0x1b4` ≥ 0), nada; senão,
  se o estado "terminando" (`+0x25c`, o pedido da rota anterior ainda
  tocando) está ativo, `início = max(início, EndTicks dele` — ou o guardado
  —`)`; senão, fora do passo curto e com `−140 < agora − eA < 140`, `início
  = eA + 70`; senão, com `e` = `EndTicks` guardado e `W` = 40 (curto) ou
  140, `−W < agora − e < W`: `início = e − W/2` se `agora < e`; nesses casos
  a porção do ciclo (`+0x1bc`) = `FinalCyclePortionCompleted` guardado; no
  fim `EndTicks` e `EndActionTicks` voltam a NENHUM (vtables `0x2f0`/`0x2f4`).
  O `FollowOneStep` (BV `0x6b8848`) guarda, no começo do fade de saída do
  último pedido, `max(EndTicks(false), +0x254)`, `ActualFadeOutStart` e
  `FinalCyclePortionCompleted` (`0x188`). **Feito** (`routing_loco.cpp`):
  com o Animate Sim acabando quando o pedido começa a sair (§13.3), a rota
  que vem logo depois começa 70 ms depois desse começo (1 a 2 ticks depois
  de agora); a entrega da rota guarda os três valores e o fim da parada que
  segue tocando (`AnimationState::locoEndingUntilMs`): uma rota logo depois
  de outra espera a parada acabar. O passo do quadro em cada tick é a sua
  fração do caminho do clipe com a fase começada `lead` ticks antes (menos:
  depois, parado até lá), e o app começa os clipes atrasado ou adiantado
  nisso. A porção guardada escolhe o pé do começo ("turnAndGoR" a partir de
  0, §8.5). Inferido: o "passo curto" (`BeginShortStepPath`) como o passo
  de lado da VM. Teste `test_routing` "loco start window".
- **O pedido novo ao replanejar andando**: um `BeginFollow` de novo é um
  pedido novo de rota, que entra pelo fade de entrada (linear) enquanto o
  anterior sai (`CreateTransitionBlend` com `tARType` 5, §13.2). Feito: o
  app toca de novo as camadas do andar na porção em que o andar está (a VM
  roda igual).

**Lido e não feito:**

- **A troca centrada** (±b/2 em volta de cada fronteira, `cLocoCycle::AnimUpdate`,
  §8.4): o app só fica sabendo da troca de fase no tick em que a VM a faz;
  centrar pede a deixa b/2 antes (o próximo clipe parado no primeiro quadro
  antes da fronteira). Segue começando na fronteira (inferido).
- **A rampa de velocidade** (`FUN_00a47144` = `CreateVelocityBlendFunction`,
  chamada pelo `CreateLocoRequest` `0x6b2390`, o `InitChannelNetwork`
  `0xd2ef8c` e o `StopLocomotion` `0xd30afc`): a velocidade do quadro em
  intervalos de rampa entre a de partida, a do andar e a final, com as
  durações encolhidas por `√(disponível/pedido)` e 0,98 quando não cabem;
  os parâmetros de cada chamada (os campos `+0x4c`, `+0x50`, `+0xa4` do
  pedido) não foram seguidos. A VM move o quadro pela distância de cada
  clipe no comprimento dele (inferido).
- **A parada fraca** (§8.7: o ciclo de andar até o próximo apoio, 1/6 ou
  2/3, sem clipe): vem do `InterruptActiveLocoState` (BV `0x6b4fa0`) com um
  tipo de fim que não é o bloqueio (2); ele é virtual e quem manda os outros
  tipos não foi achado. A VM não tem esse fim.
- **O começo 5/100 ms adiantado** do pedido seguinte (§8.3): é a troca para
  a fase seguinte de uma mesma rota (passo curto → caminho → virada do fim);
  a rota da VM é um pedido só (começo, andar, parada) e a parada já vira
  para a direção final: não se aplica.
- **A pressa da locomoção** (§13.4: os `0x160`/`0x164`/`0x174` do seguidor
  de rota): não identificados.

Medido (48 sementes × 12.000 ticks do `N001_Lot68`, `s2pkg autonomy`,
antes → depois da janela): interações autônomas verdadeiras 2.216 → 2.162,
falsas 53 → 62; bloqueios por semente média 11,1 → 14,4 (mediana 7 → 6);
falhas de rota sem o 13 dos sociais média 4,9 → 6,0 (mediana 2 → 3). Os
episódios de bloqueio variam muito de semente para semente (o main tinha 54
numa das 14).

## 14. As sobras do IK e o quadro de cada pedido (rodada n44)

Leitura de 2026-10-16 (`docs/inventario-binario.md` §8, linha 44). Modo:
**Ghidra MCP** (só leitura, nada renomeado nem salvo); endereços BV. O
quadro de cada pedido (`PostSnap`, `UpdateObjectTransform`) está no
`docs/seat-mac.md` §D.

### 14.1 Os ângulos de Euler da partilha de torção

`ConvertToEulerAngles<Matrix>` (`0x01003ee0` → `FUN_00edd840`; a ordem é um
`u32`: `i` = byte 0, `j` = byte 1, `k` = bits 16–17, o sinal = o byte alto
como `char`; `FUN_01bcb0c8` testa `i == k`, a ordem repetida). Na ordem não
repetida, com `M[r][c]` a matriz por linhas: `e1 = atan2(−M[k][i],
√(M[i][i]² + M[j][i]²))`, `e0 = atan2(M[k][j], M[k][k])`, `e2 =
atan2(M[j][i], M[i][i])`, cada um vezes o sinal. **A ordem `0x01020100`**
(X, Y, Z, +1) é a inversa de `Rz(e2)·Ry(e1)·Rx(e0)` (a mesma Euler XYZ dos
canais do ANIM, §9.3): **`e1` é o ângulo em Y dessa decomposição**.

- **O braço** (`ComputeIKTranslationSolution`, `0xd37848`, lido de
  `0xd38040` a `0xd381e0`): `M = Ry(θ)·S` (`FUN_01b9427c`/`FUN_00edd790`:
  `Ry` por linhas; θ o giro pelo polo da §4; `Math::operator*`, `0x01003eb0`),
  `e = Euler(M, 0x01020100)`; o bíceps (tipo 3) recebe `(0, 0,65·e1, 0)`
  (`FLOAT_0274b6cc`) e o braço `Euler(Ry(−0,65·e1)·M, 0x01020100)`
  (`FUN_01b94238`, também `Ry`). Na coxa (tipo 1) o mesmo `Ry(θ)·S` sem
  partilha.
- **A mão** (`ComputeIKRotationSolution`, `FUN_00d36ecc`): `E` = a rotação do
  alvo no quadro do pai do primeiro osso de baixo; o pé (tipo 6) leva
  `Euler(E, 0x01020100)` inteiro; no braço `e = Euler(E, 0xff010200)` (X, Z,
  Y, sinal −1), o pulso (tipo 7) recebe `(0, 0,75·e1, 0)` e a mão (tipo 8)
  `Euler(Ry(−0,75·e1)·E, 0x01020100)`. **Pelas fórmulas acima, o `e1` da
  ordem `0xff010200` é o ângulo em Z** (uma torção pura `Ry(α)` dá `e1 = 0`;
  `Rz(β)` dá `e1 = β`), enquanto o `r_wrist` é um osso de torção do
  antebraço (no meio dele, rotação de repouso identidade): não fecha com a
  intenção aparente; o produto do quaternion (`FUN_00dc6f60`) e a matriz do
  quaternion (`ConvertToMatrix<Quaternion>`) não foram conferidos.

**Feito**: a torção do braço é o `e1` da Euler XYZ do braço
(`twistY` → a decomposição `Rz·Ry·Rx`, `eulerY`); antes era a torção em
volta do Y (giro-torção), inferida. **Continua inferido** o pulso e a mão
(a torção em volta do Y, como antes), pela dúvida acima.

### 14.2 Os contatos em 0

`cActiveAnimIK::ComputeIKTranslationSolution` (`0xd1e698`) só resolve com
peso > 0 nos outlets do alvo e do polo; sem solução, os canais de IK do
pedido ficam com o último valor (o `cActiveAnimIK` vive enquanto o pedido
vive). **Feito**: cada camada guarda a última solução de cada cadeia
(`AnimPlayer::Layer::ikMemory`) e, com todos os contatos em 0, a cadeia
toma essa solução (pelo peso de IK, como os canais); sem nenhuma ainda, a
pose da animação (inferido: o valor inicial dos canais de IK não foi
lido). Antes: o primeiro alvo.

### 14.3 O alvo do outlet do nó

O `cActiveAnimIK` lê o alvo e o polo pelos outlets dos nós (§5), a mistura
de todos os pedidos que chaveiam o nó. **Feito**: o `AnimPlayer` resolve
cada cadeia com os nós de alvo e de polo do esqueleto como a mistura os
deixou (todas as camadas, com os deltas); os nós que o esqueleto não tem
(o filho sem `*_handcontrol0`) seguem os canais da própria camada.

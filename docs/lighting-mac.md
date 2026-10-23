# Iluminação: o que o binário de Mac faz com `Lighting.txt`

Fontes e convenções como em `docs/shaders-mac.md` ("binário", "texto",
"inferido", "só BV"). Dados: `TSData/Res/Lights/Lighting.txt` (raiz),
`LotLights.txt`, `objects/*.txt`, `windows/windows.txt`, `LightRigs/*.txt`,
`CAS_*.txt`. Classes: `nTSSG::cLightingManager` (quase sem nomes no BV: o
`tools/symxfer` não alinhou esse objeto; os nomes abaixo vêm do
`baserom.sym` casados pelo conteúdo e são marcados com o endereço BV),
`cTSLightingState`, `cTSLightingParserState`, `nTSSG::cLerpLightSet`,
`nTSSG::cLightMapManager`, `nTSSG::cDiffusionManager`.

Resumo para o renderer:

1. **Só dia e noite** no jogo base: estado `day` das 7:00 às 18:59 e
   `night` das 19:00 às 6:59 (§2). `morning`/`evening` existem, mas
   `morningEvening` é `false` no `Lighting.txt`.
2. As luzes do estado (`sun`, `sky`, `fill1`, `fill2`, `simFill1..3` +
   `skylight`) **só iluminam o que está fora de casa** (sala 0). Objetos e
   Sims dentro de uma sala recebem as **luzes da sala**: um ambiente + N
   direcionais "Delta" calculadas da irradiância da sala em 6 direções (§4),
   que sai dos light maps (§4.1), não direto das lâmpadas.
   Paredes, pisos e terreno não usam luzes: usam os light maps (§5), uma
   difusão em grade de meio tile que divide a luz por `1 + k` a cada passo.
3. Cada luz tem uma máscara de grupo: as `simFill*` só iluminam Sims; com
   alguma `simFill` presente, `fill1`/`fill2` deixam de iluminar Sims (§3).
4. Na versão "WithAmbient" a cor de cada direcional é **subtraída** da cor
   do `skylight` (§3).

---

## 1. Os arquivos e o parser

`Lighting.txt` é um script do mesmo `nGZCommandParser` dos `.matShad`
(`set*`, `define`/`create`, `include`/`sinclude`). Comandos de
`cTSLightingParserState` (registro em `FUN_01ba4da4`, textos de ajuda no
binário):

| comando | argumentos |
|---|---|
| `lightingState nome [: pai]` ... `end` | bloco; herda tudo do pai |
| `control` | `-edith <bool>`, `-debugPortals <bool>` |
| `light nome (dir) (cor) [intensidade]` | `nome` ∈ `sun`=0, `sky`=1, `fill1`=2, `fill2`=3, `simFill1`=4, `simFill2`=5, `simFill3`=6 (tabela em `0x283fefc`, binário) |
| `skylight (cor) força` | bit 7 do estado |
| `fog (cor) perto longe` | bit 8 |
| `environment (cor) [força]` | bit 9 (luz `environmentCube`: escurece env cubes/partículas) |
| `particles (cor)` | bit 10 |
| `lightMap id -strength f -tint (cor)` | light map pré-calculado (bairro) |
| `transitionTimes maxIn maxOut` | tempos máximos de transição |
| `unlitRoomColor (cor) [-threshold f]` | cor de sala sem luz |
| `UIShade`, `flatShade (cor) f [-sims -objects -indoors]` | qualidade de luz baixa |
| `projectiveShadowStrength`, `portalSaturation`, `objectLightThreshold`, `diffParam`, `modifiers` | sombras, janelas, difusão |

`cTSLightingState` (binário, `TSCreateLightsFromLightingStateWithAmbient`):
`bitset<14>` em `+0xC` (quais campos foram dados); luz i em `+0x10 + i·0x1C`:
cor (3 floats), intensidade (`+0x1C`), direção (`+0x20`, 3 floats);
`skylight` em `+0x154`; `environment` `+0x160`; `particles` `+0x16C`; fog
cor `+0x1A8`, perto `+0x1B4`, longe `+0x1B8`.

Globais lidos por `UpdateFromConfigFile` (`FUN_007175b8`): `roomSaturation`,
`roomCeilingStrength`, `roomPortalStrength`, os parâmetros de sala (§4)
com prefixos `sim:`/`object:`/`wall:`, `thresholdLightsByArea`,
`portalSuppressionRadius/Amount`, `portalCornerSuppressionAmount`,
`portalSuppressionAffectsRoomLighting`, `hm*` (sombra do sol por height
map), `outdoorStrength`, `terrainStrength`, `wallDrivenPortals`,
`morningEvening`, `objectLightJitter`, `portalLightJitter`,
`spillPlaneOffset`, `shadowStrengthLimit`, `incrementalUpdates`,
`objectLightTransitionTime`, `doorOrArchDirectMultiplier`,
`minimalLightingDirectionalMix`, `lightRangeCutoff`,
`lightRadianceRangeCutoff`.

Luzes de objeto: as luzes vêm do próprio modelo (nós de luz do CRES,
`cPointLight`/`cSpotLight`/`cDirectionalLight` com nome, ex.
`lightingCeilingValue_light`); `objects/lightObjects.txt` e
`windows/windows.txt` só sobrescrevem atributos por nome
(`lightAttribOverride nome atributo valor`: `intensity`, `falloffRate`,
`color`, `coneAngle`, `conePenumbra`, `diffusionSource*`). As janelas têm
uma spot "portal" cada (`window..._light`).

## 2. Hora do dia → estado (binário, base 1.0 = BV)

`cTSSimulator` recalcula a fase a cada hora cheia (`SimulateOneTick` →
vtable `0xC8`, `FUN_004984d4`; no 1.0 `FUN_00b1fee8`) e a grava no global
SimAntics 4 ("Time Of Day"):

| hora | fase | estado (`morningEvening` false) | com `morningEvening` |
|---|---|---|---|
| 0–5 | 2 | `night` | `night` |
| 6 | 3 | `night` | `morning` |
| 7–17 | 0 | `day` | `day` |
| 18 | 1 | `day` | `evening` |
| 19–23 | 2 | `night` | `night` |

(`UpdateLightingState`, BV `FUN_00706408`, 1.0 `FUN_009ef630`.) O nome
passa por `SetLightingStateByName` (BV `FUN_00702280`, 1.0 `FUN_009ef740`),
que primeiro tenta `<nome>NoPortals` quando uma opção global está desligada
(`DAT_02c1bf1c`; inferido: as janelas como luzes desligadas na qualidade
baixa) e cai no nome puro; e liga as variantes de textura dia/noite
(`FUN_006fdec8`). No BV o dia também pode virar `overcast`/`spring`/
`autumn`/`winter` e a noite `vacationNight` (**só BV**). Para `day`/`night`
o BV também troca o céu refletido do skirt
(`skymap-generic-day-highres-envcube` / `skymap-generic-night-envcube`).
A troca é interpolada (`cLerpLightSet::UpdateLights(t)`, tempos de
`transitionTimes` e `objectLightTransitionTime`).

## 3. Luzes de fora (binário)

`AddOutdoorLights` (BV `FUN_0070cd3c`) chama
`TSCreateLightsFromLightingState` → `...WithAmbient`:

- `skylight` (bit 7) → `cAmbientLight` "LM_Outdoor_Ambient", cor = valor
  guardado em `+0x154`, escala 1.
- Cada `light` i (bits 0–6) → `cDirectionalLight` "LM_Outdoor_Dir_i":
  **cor = cor_i − cor_skylight**, escala difusa = intensidade_i,
  orientação = `−direção_i` (a direção do arquivo aponta da cena para a
  luz, Z para cima; a negação é binário). Fog/environment/particles viram
  luzes "LM_Fog"/"LM_Environment"/"LM_Particles".
- Sala = 0 (fora; `FUN_00bfdd60`: sala < 0 vira 0x8000) e máscara de grupo:
  `simFill1..3` (4–6) só o grupo dos Sims (`+0x4B8`); `fill1`/`fill2` (2–3),
  se houver alguma `simFill`, objetos + outros + extra (`+0x4BC|0x4C0|0x4C4`),
  sem Sims; o resto, todos. `fog` todos; `environment` máscara `+0x4CC`.
- Com sombras de height map ligadas (dia), o `sun` ganha uma luz extra
  "LM_sunHM" (sombra do terreno); fora isso "LM_OutdoorShade".

O valor de `skylight` é guardado já × força (`(1,1,1) 0.5` → 0,5; abaixo).
Rodada 15: `TSCreateLightsFromLightingStateWithAmbient` (BV `0xd741dc`) e
`FindIllumFromLightingState` (`0x702ae0`) usam a cor de `+0x154` e as
direções `+0x20 + i·0x1C` como estão (escala 1 no ambiente, nenhuma
normalização); quem faz as duas coisas é o parser (rodada 15, binário:
os comandos são classes `cTSDiffusionParams::c…Command`, achadas pelo
typeinfo, com a execução no slot `0x10` da vtable): **`light`**
(`cLightCommand`, BV `0x1ba7f68`) **normaliza a direção**
(`nGZNumerics::Normalize`) e guarda cor e intensidade (1 sem o 4º
argumento); **`skylight`** (`cSkylightShadeCommand`, `0x1ba82c4`) guarda
**cor × força**; `projectiveShadowStrength` (`0x1ba4f80`) guarda o número
no `+0x184` do estado. Confirmado o que o código faz.

## 4. Luzes das salas (binário)

`AddRoomLights` (BV `FUN_00710a3c`): para cada sala ≠ 0, três conjuntos de
luzes, cada um com a sua afinação (`cTSRoomLightTuning`, 0x3C bytes) e
máscara: **objetos** (`+0xD0`, máscara `+0x4BC`), **Sims** (`+0x10C`,
`+0x4B8`) e **paredes/outros** (`+0x148`, `+0x4C0`); e uma luz
`environment` da sala ("LM_Room%d"). Campos da afinação
(`UpdateFromConfigFile`; prefixo `sim:`/`object:`/`wall:` sobre o valor
sem prefixo):

| off | nome | `Lighting.txt` (sem prefixo / sim / object / wall) |
|---|---|---|
| 0x00 | `roomLightingType` (int) | 4 |
| 0x04 | `roomOverallStrength` | 0,4 / 0,8 / 0,5 / 0,2 |
| 0x08 | `roomBounceStrength` | 0,25 |
| 0x0C | `roomBounceCrossover` | 1 |
| 0x10 | `roomSideStrength` | 1,2 |
| 0x14 | `roomSideCrossover` | 1 |
| 0x18 | `roomSideSlope` | 0,75 |
| 0x1C | `roomSpecular` | 0,8 |
| 0x20 | `roomFromLightMapMix` | – / 0 / 0 / 1 |
| 0x24 | `roomFromLightMapStrength` | – / 2 / – / 3 |
| 0x28 | `roomDirectionalBoost` | – / 4 / 4 / 1 |
| 0x2C | `roomCrossBias` | 0 |

`roomLightingType` escolhe o construtor (`FUN_007109bc`): 0 →
`FUN_0070f718`, 1 → `FUN_0070e950`, 2–8 → `FUN_0070daa8` com N = tipo
luzes direcionais. Com 4 (o do jogo), `FUN_0070daa8`:

```
I[0..3] = irradiância da sala nos 4 lados, I[4], I[5] = cima/baixo (cores RGB)
S = I0+I1+I2+I3
lados  i=0..3: L_i = (I_i + SideCrossover·I4)·SideStrength·Overall
vert.  j=4,5:  L_j = (I_j + BounceCrossover·S)·BounceStrength·Overall
A = (L0+…+L5)/6 ;  L_k −= A
ambiente: FromLightMapMix = 0 → cAmbientLight cor A, escala 1
          senão → luz de ambiente do light map (mix, FromLightMapStrength·0,25)
repetir N vezes:
   d = direção com maior max(R,G,B) de L (N < 3: só os lados)
   dir = tabela[d]; se lado: dir −= SideSlope·V        (V inferido = (0,0,1))
   cDirectionalLight "Delta<k>": cor L_d, escala DirectionalBoost,
                                 especular relativa roomSpecular
   L_d = 0
   (k = 0 e CrossBias > 0: ajusta o par oposto)
```

**Tabela das 6 direções** (binário: `DAT_02c1bf2c`, preenchida no
inicializador estático `FUN_020e6a90` com os vetores unitários e o negador
`FUN_0017267c`): `0 = +X`, `1 = −X`, `2 = +Y`, `3 = −Y`, `4 = +Z`, `5 = −Z`.
`V` = `(0, 0, 1)` (`DAT_02c1c874`). O vetor é a direção **em que a luz
viaja** (mesma convenção das luzes de fora, que usam −direção do arquivo):
`I[0]` é a luz que anda para +X (vem das paredes do lado −X), e a Delta
dos lados aponta `tabela[d] − SideSlope·(0,0,1)`, isto é, para o lado e
para baixo. `I[4]` (+Z) é a luz que sobe do piso, `I[5]` (−Z) a que desce
do teto (§4.1). Todas essas luzes recebem o ID da sala
(`SetRenderGroupIDAndCompareMode`): **só objetos daquela sala as recebem**.

### 4.1 De onde vem `I`: o light map, não as lâmpadas (binário)

`GatherRoomLighting` (BV `FUN_007147c8`) **não soma as lâmpadas**: lê a
luz já difundida nos light maps (§5) de cada cômodo e as janelas. Cada
cômodo tem um `cRoomLightingDesc` (vetor em `+0x1AC` do gerenciador):
`I[6]` (cores, `+0x00`), pesos `w[6]` (`+0x48`), contadores em `+0x60..`,
e a lista de luzes de portal (`+0x7C`).

```
para cada cômodo: I[k] = 0, w[k] = 0
GatherFloorLighting (FUN_00714258), cada quarto de tile do cômodo:
   c   = difusão do piso naquele ponto (light map, §5)
   alb = Mix(branco, cor do piso, roomSaturation)        (roomSaturation 0,5)
   I[4] += 0,25·c ⊙ alb ;  w[4] += 1                      (luz que sobe do piso)
   I[5] += 0,25·c · roomCeilingStrength ;  w[5] += 1      (teto, 1)
GatherWallLighting (FUN_00713ca0), cada lado de parede do cômodo:
   c   = difusão da parede (light map) · 0,5
   alb = Mix(branco, cor do papel de parede, roomSaturation)
   n   = normal do lado (dy, −dx)/|…| (aponta para dentro do cômodo)
   n.x > 0: I[0] += n.x·c⊙alb, w[0] += n.x ;  n.x < 0: I[1] += |n.x|·c⊙alb, w[1] += |n.x|
   n.y > 0: I[2] += …,          w[2] += n.y ;  n.y < 0: I[3] += …,           w[3] += |n.y|
FUN_0070010c, cada luz de portal (janela/porta) do cômodo:
   cor = cor do portal · roomPortalStrength (0,75); (dx, dy) = direção do portal
   mesmo rateio por sinal em I[0..3]/w[0..3] no cômodo de destino
depois: I[k] /= w[k] onde w[k] ≠ 0     (média ponderada)
```

A cor do piso/papel é a cor média da textura do material (consulta por
nome, `FUN_000c82f8` → vtable `0xC`). Daí `AddRoomLights` (§4) monta o
ambiente e as Delta. Conferido em BV; no 1.0 só se conferiu que os mesmos
nomes de configuração existem.

As lâmpadas entram na luz dos objetos por dois caminhos: como **luzes
práticas** (a `cPointLight`/`cSpotLight` do modelo, que ilumina objetos e
Sims diretamente, com `modifiers insideObjects` *practical* na intensidade
e *practical falloff* no `FalloffRate`; inferido pelo comentário do
`Lighting.txt` e pela estrutura de `FUN_0070ae3c`, que faz o mesmo com as
janelas) e como **fontes de difusão** (escala *diffusion* e queda
*diffusion falloff*, §5), que chegam a `I` pelo light map.

Janelas e portas (`SetAndAddPortalLights`, BV `FUN_0070ae3c`): para cada
portal, `FindPortalLightValues` (`FUN_0070a884`) dá cor e as quatro forças
do `modifiers insidePortals/insidePortalSpills/outside*`; a luz prática é
uma spot "LM_PortalDirect" com intensidade `jitter · intensidade ·
practical · supressão` e queda `falloff · practicalFalloff`, mais a
"LM_PortalSpill"; e o portal vira fonte de difusão com cor × *diffusion* e
queda × *diffusion falloff*. A supressão vem de `FindPortalSuppression`
quando `portalSuppressionAmount > 0` (fórmula do comentário do
`Lighting.txt`: `1 − (1 − r/R)·f`; não conferida). `portalLightJitter` 0
→ fator 1.

## 5. Light maps de parede, piso e terreno: a difusão (binário)

`cLightMapManager` mantém as texturas `floorLightMap_<página>`
(`floorTextureSize` 512), `wallLightMap_<página>` (tiles de parede 8×24
texels: `wallTileSizeX/Y`) e os mapas de incidência. **Página 0 do piso =
o lote**: texels por tile = `tamanho >> 6` (8 com 512, a página cobre 64
tiles); o tile (x, y) ocupa `u ∈ [8x, 8x+8)`, `v ∈ [512 − 8y − 8, 512 − 8y)`
(V invertido) (`FloorTileLightMapTexels`, BV `0x6f65f8`). As outras
páginas (andares de cima) guardam tiles soltos numa tabela.

`cDiffusionManager` (BV `0xd7cad4..0xd7fdc0`) calcula a luz por **grades
de difusão**: uma `cFloorColorGrid` por andar e o `cWallDiffusionManager`
para as paredes. A cada atualização, `DoDiffusionPass` (BV `0xd7dd80`):
limpa cada grade, semeia as luzes, propaga, copia para os light maps, e
depois faz o mesmo nas paredes.

### 5.1 A grade do piso

(Rodada n16: lida inteira e corrigida no §10.4.)

Amostras em **meio tile** (grade `2·largura × 2·altura` do lote); cada
amostra tem duas metades (os dois triângulos de um tile cortado na
diagonal, `+0x00` e `+0x24`), cada uma com cor RGB, ID da sala (`+0x20`),
"existe" (`+0x1B`), "de fora" (`+0x1A`) e os bits de vizinhança (`+0x18`:
para quais das 8 direções a luz passa, isto é, sem parede no caminho;
`+0x19`: qual metade do vizinho).

1. **Limpa** (`FUN_00eeb8a4`): dentro = `insideAmbientColor ·
   insideFloorAmbient`; fora = `outsideDiffusionAmbientColor ·
   outsideFloorAmbient` (do estado: `diffParam`; dia 0,001 dentro, 0,25
   fora).
2. **Fontes** (`cDiffusionLightSource`, montada de cada luz de objeto com
   `cDiffusionLightExtension`, `FUN_00d82498`): cor = `diffusionSourceIntensity
   · diffusionSourceColor`; `diffusionSourceFalloff` (`+0x38`),
   `...DirectionalFalloff`, `...WallIntensityScale` (`+0x3C`),
   `...FloorIntensityScale` (`+0x40`), `...InterFloorFalloff` (`+0x44`),
   `...IsDirectional`; retângulo `diffusionSourceMin/Max` (em tiles,
   relativo à luz) girado pela direção do objeto e somado à posição
   (`FUN_00d82334`); sala = a do ponto um pouco à frente do objeto. Esses
   valores vêm da extensão da luz no modelo e dos `lightAttribOverride` de
   `objects/*.txt` e `windows/windows.txt`.
3. **Semeia e propaga** (`FUN_00eeccc0`, por grade): pega as fontes do
   andar da grade ou acima, que tocam uma amostra da sua sala
   (`FUN_00eeba10`), **ordena pela queda** (`+0x38`, decrescente) e trata
   juntas as de queda igual (|Δ| < 0,01):
   - semente (`FUN_00eebdec`): em cada amostra do retângulo **da mesma
     sala**, `cor = max(cor, corFonte · FloorIntensityScale)` por canal;
     fonte em andar acima: × `(1 − InterFloorFalloff)` por andar, e só se
     o piso deixa passar;
   - propagação (`FUN_00eec3a4`), até `floorPropagationSteps` (50)
     passadas ou até nada mudar: cada passada varre o retângulo ativo nos
     4 sentidos, e cada amostra puxa das vizinhas que mudaram na passada
     anterior, pelas 8 direções (`d`: 0 (0,−1), 1 (−1,−1), 2 (−1,0),
     3 (−1,1), 4 (0,1), 5 (1,1), 6 (1,0), 7 (1,−1); `DAT_02747cb8`):

```
k = falloff_da_fonte · floorThreshold          (diffParam floorThreshold: dia 0,4, noite 0,2)
diagonal (d ímpar): k ·= 1,4
por canal:  se (viz − cor) > k·cor:  cor = viz / (1 + k) + 0,01   (FUN_00ee7c6c)
```

   Ou seja: cada passo de meio tile divide a luz por `1 + k` e a luz não
   atravessa paredes (só segue os bits de vizinhança).
4. **Cômodos escuros** (`BrightenRoom`, BV `0xd7d028`): soma
   `0,5 · unlitRoomColor` em todas as amostras (piso e paredes) do cômodo.
   Quem decide quais cômodos (`FindUnlitRooms`/`BrightenUnlitRooms`, com
   o `-threshold`) não foi decompilado.
5. **Cópia** (`FUN_00eea560` → `FUN_00ee9874`/`FUN_00ee95d0`): por tile,
   as 4 amostras dos cantos (amostra vazia = o ambiente de dentro/fora)
   viram os texels do tile por interpolação; metade diagonal à parte.

### 5.2 Paredes

(Rodada n16: lida inteira no §10.5, que substitui este parágrafo.)

`cWallDiffusionManager` (BV `0xee6000..0xef5000`, sem nomes): mesmos
parâmetros com prefixo de parede (`insideWallAmbient`, `outsideWallAmbient`,
`wallThreshold`, `wallPropagationSteps`). Ordem em `DoDiffusionPass`:
limpa cada parede (`FUN_00eee87c`, ambiente por parede); semeia as fontes
da mesma sala com `WallIntensityScale` e `k = wallThreshold · falloff`,
atenuada por andar como no piso (`FUN_00ef28ac` → `FUN_00ef2594`, a
`AddDirectLight`); traz o piso para a parede (`FillWallsFromFloors`,
`FUN_00ef0e68`: `brightWallFloorContribution` 1 para paredes com luz
direta, `darkWallFloorContribution` 0,5 para as outras); propaga; escurece
cantos junto de paredes escuras (com uma opção global); copia para as
páginas de parede. A fórmula da propagação na parede não foi lida (deve
ser a mesma do piso; não conferido).

### 5.3 Fora de casa: terreno e pisos ao ar livre (binário)

O lado de fora tem **duas camadas somadas** no light map de piso:

1. **A difusão** (§5.1) cobre também as amostras de fora: limpas a
   `outsideDiffusionAmbientColor · outsideFloorAmbient` ((1,1,1)·0,25) e
   semeadas pelas lâmpadas que estão fora (mesma regra: semente na sala da
   fonte, que fora é a sala 0, e propagação com `k = falloff ·
   floorThreshold`, só parando em parede). As fontes de fora usam
   `modifiers outsideObjects` (*diffusion*, *diffusion falloff*): de dia
   `0 0`, à noite `0,6` e `0,65`. **As poças de luz em volta das lâmpadas
   de jardim vêm daqui.**
2. **A luz direta do estado**, somada por cima (`AddToARGB32` soma no
   texel que a difusão deixou) por `AddDirectFloorLighting` (BV
   `0x708ebc`), que percorre os tiles de piso **da sala 0** (o gerenciador
   pede ao mundo os tiles do cômodo 0):

```
E(n) = FindIllumFromLightingState(n)                    (BV 0x702ae0)
     sem lightMap no estado (o caso de day/night):
       w_i = max(0, n · dir_i) · intensidade_i,  i = sun, sky, fill1, fill2 (as 4 primeiras)
       E   = (1 − Σ w_i) · skylight + Σ w_i · cor_i
     com `lightMap id` (só os estados de bairro): E = texel do mapa
       ambiente na posição ((n.x+1)/2, (n.y+1)/2) × a cor da luz guardada
texel += E(n) · terrainStrength (1) · escala do light map de piso
         (cLightMapManager vtable 0x5C(1))
```

   `n` é a normal do terreno, interpolada bilinearmente entre as normais
   dos vértices do tile a cada texel (`LightTerrainTile`, BV `0x704274`;
   meio tile `0x703de8`); um tile com piso ou fora do térreo usa `n` =
   (0,0,1) constante (`FUN_00700368` ou a soma direta). Com sombras por
   height map (`+0x6C`), as variantes "WithShadows" (BV `0x705068`,
   `0x702fd8`; "AlphaShadows" `0x705850`, `0x703740`, opção global
   `DAT_02c1beb0`) fazem o mesmo e põem a **sombra do sol no alfa** do
   texel: `alfa = 255 · min(1, s · hmSunLightTransitionZone)` com `s` a
   amostra do height map de sombra (terreno) ou a altura da sombra acima
   do tile (pisos).

**A sombra do sol** (rodada 15, binário: `LightTerrainTileWithShadows` =
BV `0x705068`, a versão de piso `0x702fd8`; `FUN_01807114` lê as duas
grades do `cHeightMapShadow`): para cada texel, `d` = topo da sombra − altura
da superfície (as duas grades do height map, `+0x18` − `+0xc`; nos pisos,
a grade `+0x228` menos a altura do tile), em texels de 8 por tile (o mesmo
índice do light map). `d ≤ 0`: `E(n)` normal. `d > 0`: `E(n)` com o peso
do `sun` × `max(0, 1 − d·hmSunLightTransitionZone)` (`0x7047e4`; a zona é 1:
sombra cheia a 1 tile de fundura) e o alfa do texel = `255·√max(0, 1 −
d·zona)`. Ou seja, **a sombra escurece o RGB** do light map (tira o sol) e
fica também no alfa (que só as sobreposições usam).

**Como o height map é montado** (rodada 16, binário; nomes do `baserom.sym`
casados pelo tamanho e pela ordem, `TSSGHeightMapShadow.cpp`):
`UpdateHeightMapShadows` (BV `0x6fea68`, chamado nas atualizações do
gerenciador `0x7167d0`/`0x715fe0`) põe `hmLotShadowStrength` (1,5) no
`cHeightMapShadow` (`+0x210` do gerenciador) e chama, com 8 texels por tile:

1. `FillFromTerrain` (`0xd7661c`): para cada quarto de tile (os 4
   triângulos do tile cortado nas diagonais) desce do andar de cima ao de
   baixo até achar um piso ali; a grade 0 (o "topo") recebe a altura desse
   piso (`SampledFloorElevation` no centro do texel) e a grade `+0xc` (a
   "superfície") a do térreo.
2. `FillFromWalls` (`0xd76f40`, recebe a direção do sol): cada segmento de
   parede com nível ≥ 0 tem um topo em cada ponta: no andar de cima de
   tudo, a altura da parede sobre o piso; abaixo dele, o telhado sobre a
   ponta menos um deslocamento, ou (meia parede, palco) a altura da meia
   parede, ou o piso do andar de cima; cerca: a altura do estilo (0 com
   `FenceStyleHasNoShadow`). O topo é **escrito** (não somado) nos texels ao
   longo da parede, interpolado de uma ponta à outra: numa parede ao longo
   de uma linha da grade, só no lado **oposto ao sol** (coluna `x−1` com
   sol·x ≥ 0, coluna `x` com sol·x ≤ 0); numa diagonal, os texels da
   diagonal e os vizinhos que completam a escada.
3. `FillFromRooves` (`0xd77b54`): cada telhado que não é de vidro
   (`IsGlassRoofType`) escreve a altura do telhado (vtable `0x78` do
   gerenciador de telhados) em todos os texels do seu retângulo (diagonais
   por `GetMinMaxForDiagonals`, os tipos `0x13..0x15` meio tile maiores).
4. `FillFromAwnings` (`0xd75acc`): os tiles de piso com toldo (vtable `0xcc`
   do mundo ≠ −1) escrevem altura + 1 na caixa dos cantos projetados ao
   longo do sol. Nenhum lote do jogo base tem toldo.
5. `FindHeightMapShadows` (`0xd76e68`): sem sol (z ≤ 0) ou com o sol a
   pino, a grade de sombra (`+0x18`) fica em −1000; senão uma varredura
   por **colunas** (`0xd76254`, |sol.x| ≥ |sol.y|) ou por **linhas**
   (`0xd76b74`) a partir do lado do sol: uma frente de alturas (−1000 no
   início) toma em cada texel `max(frente, topo)`, grava isso como topo da
   sombra do texel, cai `célula·sol.z/|sol.x|` por coluna e anda de lado
   pela mistura linear com o vizinho (`frente[r] += (−sol.y/sol.x)·(frente[r±1]
   − frente[r])`).

`AddBlobToHeightMap` (o `+0x278` do gerenciador, retângulo inteiro +
altura) **não** entra nessa grade do lote: só `FUN_00706920` lê a lista,
para a textura por vértice do height map que a luz "LM_sunHM"
(`cHeightMapShadowLight`) usa nos objetos, com a caixa aumentada por
`hmBlobWidthModifier` e a altura × `hmBlobHeightModifier`, em unidades de
um décimo (`+0x1b8` = 0,1). Objetos e árvores do lote não fazem sombra no
chão pelo height map: a sombra deles é a GUOB (estilos 2 e 3 do `cShadow`,
§9). Quem chama o `AddBlobToHeightMap` no BV não foi achado (no
`baserom.sym` ele tem 0x64 bytes; no BV não aparece com o nome).

**Paredes de fora** (`AddDirectWallLighting` `0x708590` →
`LightWallTileWithShadows` `0x704d54`): com sombras e o lado virado para o
sol, cada texel da parede pega o ponto no quad do lado um pouco afastado
pela normal e `d` = topo da sombra ali − altura do ponto (`0x1807ad4`); com
`d > 0` a luz é a de sombra (`0x7047e4`).

**No código** (rodada 16; `fillSunOccluders` em `lot_model.cpp`,
`sunShadowTops` e `computeLotLightMap` em `lot_lightmap.cpp`): a grade
estática guarda pisos de cima e telhados (máximo, sem pular os de vidro:
nenhum nos lotes), o terreno entra no centro de cada texel, as paredes
ficam como segmentos com o topo nas pontas e são desenhadas na grade para o
sol de cada estado, e a varredura é a do binário (colunas ou linhas, queda
e deslocamento lateral). As paredes de fora viradas para o sol pegam a
sombra texel a texel (ponto a meio texel da face). Inferido: a altura das
cercas (a da meia parede), o deslocamento do telhado sobre a parede (o do
sótão), os vizinhos da diagonal; sem toldos. Capturas:
`docs/screenshots/r15-sombra-sol-height-map.jpg`,
`docs/screenshots/r16-sombra-sol-paredes.jpg`.

A escala do light map de piso (`cLightMapManager` vtable `0x5C`, tipo 1,
BV `0x6f5350`) é o campo `+0x68`, que o construtor (BV `0x6fa6e0`) põe em
**0,5** (o de parede, tipo 0, `+0x6C`, também 0,5); `+0x1a8` do gerenciador
de luz multiplica junto (`terrainStrength`, inferido pelo nome lido no
`UpdateFromConfigFile`). Então `texel += E(n) · terrainStrength · 0,5`: com
o `2·textura·light map` do shader, a luz direta chega à tela como
`textura · E`. Não há a subtração do skylight que as luzes de objeto de
fora usam (§3).

A difusão é copiada sem escala (`CopyDiffusionToLightMaps` passa a mesma
escala 0,5, e `FUN_00ee9120` multiplica por `2·escala` antes do
`PackToARGB32`, que leva [0, 1] a [0, 255]), mas, com a flag `+0x40` da
grade ligada, as amostras de fora (`+0x1A`) **perdem o ambiente de fora**
antes da interpolação (`FUN_00ee9874`, `FUN_01808a08` = subtração de
`outsideDiffusionAmbientColor · outsideFloorAmbient`; amostra inexistente
vale o ambiente de fora ou de dentro). É o que o comentário do
`Lighting.txt` diz ("If auto-lighting is active, this has no absolute
effect, but it does factor into how quickly lights fall off"): o 0,25 só
molda a queda das poças. Inferido: a flag `+0x40` ligada no térreo (não se
achou quem a escreve). O parser normaliza a direção das `light` (§3,
`0x1ba7f68`).

**Quem é "fora"**: no binário, os tiles da sala 0 (`AddDirectFloorLighting`
pede os tiles do cômodo 0; o render group de sala < 0 vira 0x8000). Os
lotes guardam pátios e a faixa da borda como salas com ID próprio no
`3ARY` (o que o código achou); como o jogo junta essas salas à sala 0 não
foi achado (as funções do `cRoomManager` não estão nomeadas no BV).

### 5.4 Janelas e portas: `FindPortalLightValues` (binário, BV `0x70a884`)

Para cada portal (`cPortalLight`: sala do outro lado `+8`, direção
`+0xC/+0x10`, supressão `+0x14`, posição `+0x18`):

```
outro lado é uma sala (≠ 0): modificadores insidePortals / insidePortalSpills
senão:                       outsidePortals / outsidePortalSpills
   (4 floats cada: practical, diffusion, practical falloff, diffusion falloff;
    practical × supressão)
direta = 0, f_direta = 1 ; spill = 0, f_spill = 0
iluminação ligada:
  portal de um cômodo para fora (lado 0):
     sem skylight no estado: direta = E(−direção do portal)
     com skylight: f_direta = max(0, sol · direção do portal) ;
                   direta = cor_sol · intensidade_sol ; spill = skylight ; f_spill = 1
                   portalSaturation < 1: f = (f − 1)·sat + 1 ; cores = lerp(E(−dir), cor, sat)
     direta += L ; spill += 0,5·L    (L = light map de parede fora, na posição do portal)
  outro caso: direta = 0,5·L ; com skylight: spill = direta, f_spill = 1
cor do portal (+0x30) = direta·f_direta + spill·f_spill
   (× supressão se portalSuppressionAffectsRoomLighting; porta/arco: f_direta × doorOrArchDirectMultiplier)
```

Essa cor alimenta a "LM_PortalDirect"/"LM_PortalSpill" (§4.1), a fonte de
difusão do portal e o rateio de `I[0..3]` (× `roomPortalStrength`).

Lido no BV (`SetAndAddPortalLights` → `FindPortalLightValues(gerenciador,
sala, portal, ...)`): os portais ficam na lista de cada cômodo (`+0x7C` do
`cRoomLightingDesc`), e "lado 0" é **a sala dona da lista** (`param_2 == 0`)
com o outro lado (`+8`) ≠ 0: a luz de fora entrando num cômodo. Os
modificadores escolhem-se pelo **outro lado**: ≠ 0 → `insidePortals(Spills)`,
0 → `outsidePortals(Spills)` (à noite 4 4: a luz de um cômodo saindo pela
janela faz poça no gramado). As luzes vão para o grupo da sala do outro
lado; a prática "LM_PortalDirect" tem intensidade `jitter · intensidade da
luz · practical · f_direta` e cor `cor da luz ⊙ direta`; a fonte de difusão
tem cor `cor da fonte ⊙ direta · diffusion · f_direta` e queda × *diffusion
falloff*, e outra igual para o spill com os modificadores `*Spills`. Cada
janela/porta tem uma luz por lado no modelo (`..._n_light`/`..._s_light`).

## 6. Luzes de objeto: `FalloffRate`, alcance e cone (binário)

`cStandardLightBase`: cor (`+0x50`), `DiffuseScale` (`+0x48`),
`FalloffRate` (`+0x60`), alcance (`+0x64`), cone (`+0x68`/`+0x6C`).
**`FalloffRate` é a própria constante `C`** da atenuação
`I / (1 + C·d²)`; o alcance é onde ela chega ao corte
(`cPointLight::OnLightRadiantPropertiesChanged`, BV `0xca0e48`; spot em
`0xca0ab8`):

```
I = |DiffuseScale · cor|            (norma do vetor RGB)
c = lightRadianceRangeCutoff = lightRangeCutoff do Lighting.txt (0,275; sem ele 0,5)
I/c ≤ 1 → alcance 0 ;  C ≤ 0 → alcance 10000
senão     alcance = sqrt((I/c − 1) / C)
spot: ângulo e penumbra em radianos, valor absoluto, a soma presa num
      máximo; cos externo = cos((ângulo + penumbra)/2), interno = cos(ângulo/2)
```

**Rodada 16, no código:** os spots (`cSpotLight`: `ReadFrom` dá cone,
penumbra e alcance depois da queda) iluminam só dentro do cone:
`Renderer::spotCosines` (ângulo e penumbra absolutos, soma presa em π;
cheio dentro de cos(ângulo/2), nada fora de cos((ângulo + penumbra)/2),
linear no cosseno entre os dois, o falloff 1 do D3D, inferido) e o
shader (`lightVector`). Para onde o spot aponta: o **−Z** da luz
(confirmado na rodada n34 pelo `cSpotLight` medindo a luz num ponto, BV
`0xc9f3ac`, `docs/cinematics-mac.md` §9; era inferido dos modelos: os `lightingCeilingCanister` sem rotação apontam para baixo,
os `gardenuplight` para cima, os `_n_light`/`_s_light` das portas para
dentro do seu lado e para baixo, a lâmpada da geladeira para fora). Vale
para as luzes práticas dos objetos (no jogo base: canister de teto,
arandela de muro, uplight de jardim, lareira, geladeira) e para os spots do
rig das cinemáticas (cone 40° + penumbra 10°). Captura:
`docs/screenshots/r16-spots-noite.jpg` (antes/depois, `N003_Lot192`).

Change Light (`docs/primitives-mac.md`) muda a intensidade em % do nível
de cada luz do modelo; o renderizador faz o fade em
`objectLightTransitionTime`.

## 6.1 Cômodos sem luz: `FindUnlitRooms` / `BrightenUnlitRooms` (binário)

Endereços no BV (achados pela ordem das funções do `baserom.sym`, que o
symxfer não transferiu): `FindUnlitRooms` `0x701a28`, `BrightenUnlitRooms`
`0x707820`, o preenchimento de meio tile `0x700368`, a soma das luzes da
sala `0x70009c`/`0x700710`. Lidos de `unlitRoomColor (r, g, b) -threshold t`
do estado de luz (`Lighting.txt`: `(.3,.275,.275) -threshold .33`, ...;
o arquivo avisa "do not set threshold to 0").

```
FindUnlitRooms (só se a cor ≠ preto e a difusão está ligada, +0x54):
  para cada sala r ≠ 0:
      L = média das 6 cores de luz da descrição da sala (soma / 6)
      m = maior componente de L
      f[r] = m < t ? 1 − m/t : 0
  (t ≤ 0: f = um valor denormal ≈ 0 para as salas sem luz própria e 0 para
   as que têm: sem efeito visível — daí o aviso do arquivo)
  para cada sala com f > 0: soma cor · f às 5 primeiras cores de luz da
  descrição da sala (o que ilumina os objetos lá dentro)
BrightenUnlitRooms (se alguma sala tem f > 0):
  piso: para cada tile de piso da sala, soma cor · f · escala do light map
        de piso (vtable 0x5C(1)) a cada texel do tile (AddToARGB32, satura);
        tiles diagonais (tipos 3, 6, 12) só no lado da diagonal que é da
        sala (teste x·(±1) + y contra 0,99/−0,01 em coordenadas do tile)
  paredes: para cada lado de parede cuja sala tem f > 0 (o lado e o oposto,
        OppositeWallLocator), soma cor · f · escala do light map de
        parede (0x5C(0)) a todos os texels daquele lado
```

Ou seja, é um **piso de luz por sala**, somado depois da difusão, que some
quando a sala já tem luz ≥ t (no componente mais forte).

## 6.2 Luz direta nas paredes: `AddDirectWallLighting` (BV `0x708590`)

Para cada lado de parede (e o oposto) que dá para **fora** (sala 0) ou é do
tipo 3:

- calcula a iluminação externa no ponto do lado (`FindExternalIllumination`
  com a normal do lado, BV `0x70465c`: sol + céu do estado de luz) e a
  multiplica pela escala do light map de parede;
- com sombras ligadas (`+0x6C` e `+0x6D`) e o lado virado para o sol
  (sol · normal > 0): `LightWallTileWithShadows` (BV `0x704d54`), que
  sombreia texel a texel; senão soma a mesma cor a todos os texels do lado;
- parede do tipo 3 com sala do lado: pula as primeiras `3 · largura` linhas
  de texels do lado (só a parte de cima recebe a luz direta; inferido: muro
  baixo/meia parede).

## 7. O que o código atual faz diferente (`src/formats/lot_diffusion.cpp`, `lot_lightmap.cpp`, `src/app/lot_lighting.cpp`)

Rodada n16: a difusão de piso e de parede, os cômodos sem luz, as fontes
e a irradiância dos cômodos seguem o binário (§10). Diferenças que
sobram:

| tema | jogo | `src/` hoje |
|---|---|---|
| terreno e pisos fora | difusão de fora menos o ambiente de fora, **mais** `E(n)·terrainStrength·0,5`, com o sol tirado na sombra do height map (§5.3) | igual; a luz fria puxada para o cinza no chão (escolha visual, rodada 12) |
| quem é "fora" | sala 0 do mundo (§5.3) | sala 0 do mapa de luz (3ARY `0x0B`); lote sem esse mapa: sala em que metade dos quartos não tem piso (inferido) |
| portais | um `cPortalLight` por luz de janela/porta e peça de parede, para a sala para onde a luz aponta, `L` = `LookupWallDiffusion` do lado de onde vem; fonte de difusão na própria luz (sala do tile da janela); portas/arcos sem luz direta; supressão; práticas "LM_PortalDirect"/"LM_PortalSpill" (§10.6, §11.4) | igual (rodada n35), achando a parede como a parede que bloqueia a luz mais perto e com `L` das lâmpadas (o jogo usa o quadro anterior); inferido: o `+0x20` da supressão como o andar; o cone das janelas é o do modelo (os `coneAngle`/`conePenumbra` do `windows.txt` não são aplicados) |
| lâmpadas nos objetos | luz prática direta (`insideObjects`/`outsideObjects` *practical*) além da luz do cômodo (inferido); no máximo 8 luzes no contexto de um objeto, escolhidas por `RepresentativeIrradiance` (§11.6, quem corta não lido) | `LightSet::points`: até 12 por sala (as mais fortes, com as práticas dos portais), `I/(1 + C·d²)` até o alcance de §6, por pixel |
| Delta e sala dos objetos | as quatro Delta por `MaxAbsElement`, com sinal; o conjunto da sala de luz (§11.1) | igual (rodada n35; antes só as positivas, e a sala da VM) |
| sol nos objetos de fora | "LM_sunHM": o sol × o fator do height map no centro da caixa de cada parte (§11.6) | igual, por instância (rodada n35); inferidos: a flag `0x17` como a dos Sims, o grupo `Portals` como janelas e portas |
| difusão das paredes | só os grafos 3, 8, 10, 11 e as paredes de piscina; no regime, o piso trazido das grades (§11.3, §11.5) | igual (rodada n35); as faces de cercas e decks ficam pretas antes da luz direta (inferido) |
| hora → estado | `UpdateLightingState` a cada hora (§2) | pelo relógio da VM, também durante os ticks das capturas `--run-vm` |
| difusão, `I[k]`, Delta, `unlitRoomColor` | §4, §6.1, §10 | segue o binário; a luz das paredes que o jogo junta por quarto de tile, aqui por meia amostra (`GatherFloorLighting`) |

## 8. O que falta

- O `thresholdLightsByArea`: o parser o guarda no `+0x190` do gerenciador e
  nada o lê (rodada 16 antiga); o `Lighting.txt` não o define: sem efeito.
- ~~A lista de portais de cada sala, a fonte de difusão de cada portal, a
  supressão entre janelas e o padrão de `doorOrArchDirectMultiplier`~~
  (rodadas n16 e n35, §10.6 e §11.4). Falta: quem aplica os
  `coneAngle`/`conePenumbra` dos `lightAttribOverride` (graus? o `windows.txt`
  dá 40 e 20 com `$wConeMul`/`$wPenMul`) e o grupo de render dos objetos de
  portal (janelas e portas: a porta branca do `N003_Lot52` sai clara).
- Quem aplica os `lightAttribOverride` e cria a `cDiffusionLightExtension`
  (a tabela `+0x378` do gerenciador, por propriedade).
- ~~Os grafos de parede 3, 8, 10, 11 do `SetupWalls` e o
  `DoDiffusionPassOptimized`~~ (rodada n35, §11.3, §11.5). Falta: a cor com
  que o jogo limpa as células de parede fora da difusão (aqui preto) e o
  caminho incremental de verdade (tiles sujos, `incrementalLighting`: a
  atualização dividida em passos de `0x715fe0`); o resultado no regime é o
  mesmo.
- O limite de 8 luzes por objeto (o contexto do DX8) e quem escolhe quais
  (pelo `RepresentativeIrradiance`): aqui 4 Delta + até 12 práticas por
  sala.
- Luzes práticas: confirmar que `modifiers insideObjects` multiplica a
  intensidade e o `FalloffRate` (`UpdateObjectLighting`).
- O height map de sombra: onde o `hmLotShadowStrength` (1,5) entra e quem
  chama o `AddBlobToHeightMap` (a textura do `FUN_00706920` é só do
  bairro; a "LM_sunHM" dos lotes é por parte desenhada, §11.6, feita).
- A direção de sombra do gerenciador de luz (vtable `0x78`, §9) e se a cor
  da vtable `0x7c` é o `+0x28c` de `0x705f70`.

**Rodada 12 (escolha visual, não do binário):** no app, a luz direta do
estado sobre o chão (terreno e pisos de fora) é puxada para o cinza quando é
fria, na proporção (B − R)/B: à noite E ≈ (0,32; 0,32; 0,8) virava calçada
lavanda forte com o `2·textura·light map`; de dia (luz quente) nada muda.

### 5.5 Sombra do sol de dia, conferida (rodada 16, QA ao vivo passada 13)

A QA viu a sombra da casa "quase invisível" com a casa à mostra e "inteira,
escura, em degraus de tile" no nível −1 (N001_Lot68). Conferido sem tela
(`OS2_LIGHTMAP_DUMP=<png>` grava o atlas também no caminho sem a VM):

- O atlas é o mesmo com a casa à mostra ou cortada (o height map usa o lote
  inteiro, não a vista). No estado `day` (sol (1,5, 1,2, 2), ~46°) a sombra da
  casa do Lot11 é uma faixa de 4–6 tiles para −x/−y, texel 78 contra 169 ao
  sol; na tela o gramado na sombra fica ~0,5 do gramado ao sol (63 contra 122).
  A fórmula é a do binário (`FUN_007047e4`, BV `0x7047e4`, decompilada de
  novo: o peso do sol × `max(0, 1 − d·hmSunLightTransitionZone)`, e o
  `(1 − Σw)·skylight` cresce quando o sol sai, como no nosso `illum`).
  `hmLotShadowStrength` (1,5) vai para o `cHeightMapShadow +0x4c` e a
  varredura (`0xd76254`) não o lê; quem lê não foi achado.
- A sombra cai do lado oposto ao sol: na orientação da captura da QA ela
  fica atrás da casa e da árvore; nas orientações 2/3 aparece inteira
  (`docs/screenshots/r16-sombra-dia.jpg`). O light map já é amostrado com
  filtro bilinear (`maskSampler_`), 8 texels por tile.
- Os "degraus de tile" escuros do nível −1 eram o terreno **debaixo** da casa
  (sem sol, dentro da fundação), à mostra porque a vista caía para o nível
  −1 quando o Sim mergulhava; a vista não desce mais para o andar da piscina
  (`docs/camera-ui-mac.md` §1.3).

## 9. Sombras projetadas: `nTSSG::cShadow` (binário, rodada 15)

Dentro de casa **não há sombra de luz de cômodo, de lâmpada, de portal nem
nos light maps**: a difusão (§5) só para em paredes, e nenhuma luz de
objeto projeta sombra. O que escurece o chão em volta das coisas é o
`cShadow` de cada objeto (`cObject::CreateShadow`, BV `0x6549f0`), com um
de cinco estilos (`GetShadowStyleFromEdithMode` `0x65480c`,
`GetDesiredShadowStyle` `0x65492c`, `UpdateCurrentStyle` `0x678710`):

| estilo | quem | o que é | opção (`Graphics Rules.sgr`, bit do `+0x2c`) |
|---|---|---|---|
| 0 | — | nenhuma | — |
| 1 | **Sims** (tipo de objeto 2) | silhueta renderizada numa textura e projetada no chão | `simShadows` (Shadows `$High`) |
| 2 | objetos | sombra projetada de objeto (só com os bits 1 e 3; o 3 nunca liga: não aparece) | `objectShadows` + bit 3 |
| 3 | objetos | os planos de sombra do modelo (`groundshadow`, `*wallshadow`: "GUOB", `InitGUOBs` `0x6747c8`; §12) | `guob` (bit 2) |
| 4 | objetos com `+0x102 == 3` | como o 1, grupo "Props" | `objectShadows` |

**Quem escolhe o estilo** (rodada 16, binário): `GetShadowStyleFromEdithMode`
(BV `0x65480c`): objeto do tipo 2 (Sim) → 1; tipos 8–12 → 3 se o objeto
tem (vtable `0x88`/`+0x7a`), senão 0; os outros, com a flag `+0x79`: 3 se
(vtable `0x2e4` e `0x238` do objeto de cena), 3 se `+0x102 == 2`, **4 se
`+0x102 == 3`**, senão 2. O `+0x102` é o **dado de objeto 68** (lido com
`0x34(0x44)` em `UpdateShadowStyleAndVisibility`, `0x666c54`; o construtor
põe 1). `GetDesiredShadowStyle` (`0x65492c`): `+0x102 == 1` → nenhuma; um 2
sem GUOB → 3, com GUOB e `+0x81` (fora) → 4 só com uma flag global
(`PTR_Release`, desligada). `UpdateCurrentStyle` (`0x678710`) liga cada
estilo pela opção: 1 → bit 0 (`simShadows`), 4 → bit 1 (`objectShadows`),
3 → bit 2 (**`guob`**, não `heightMapShadows`: rodada n40, §12.1), 2 → bits 1
e 3, e um dono 2 sem os bits 1 e 3 vira 3. O 3, no `UpdateGUOBs`
(`0x678488`), são as **GUOB** do objeto (o chão sempre, as de parede onde há
parede; §12.2), não o height map do lote. (A coluna "opção" da tabela e o
"+0x81 (fora)" eram leituras erradas, corrigidas na n40.) O estilo 4 é o 1 com o
grupo de render "Props" em vez de "Sims" (`CreateShadow` `0x677390`).
Nos lotes `N001_Lot11/36/68/82`, `N002_Lot10/22`, `N003_Lot192` nenhum
objeto tem o dado 68 = 3 (só 0, e 1 nas alianças e no PlumbBob): o estilo
4 não aparece no jogo base, e o app não o desenha.

**Sims sem a VM** (rodada 16): as vistas de lote sem `--vm` também
desenham a sombra dos Sims (`poseActors` em `lot_actors.cpp`), dentro ou fora pela
sala do light map sob os pés, com a cor e a direção do estado de
`--lighting` (`simShadowLight`, a mesma conta do `LotLighting`). O bairro
não tem Sims; o CAS já tinha. `docs/screenshots/r16-sombra-sim-sem-vm.jpg`.

O 2 e o 3 já existiam no código (planos do modelo; height map só no
bairro). O **estilo 1** (`cShadow::CreateShadow` `0x677390`,
`UpdateInsideOutsideSettings` `0x67538c`, `UpdateShadowLocation`
`0x676290`, `UpdateVisibility` `0x674260`, `OncePerFrameUpdate`
`0x678ba0`):

- Uma textura `shadowTexture%x` de `shadowResolution` × `shadowResolution`
  e um viewer ortográfico próprio que só desenha o grupo de render do
  Sim; os materiais desenham a silhueta na passada `kRenderTypeShadow`
  (`SimShadowDX7`/`DX8ShadowRendering`: cor `stdMatShadowColor` = (0,63,
  0,63, 0,85) do `sims.matShad`) sobre branco.
- Ajustes do gerenciador (lidos em `0x6f2840` com estes nomes; padrões em
  `0x6f3d30`): `shadowSize` 1, `shadowCameraDistance` 10,
  `indoorShadowBlurRadius` 3, `outdoorShadowBlurRadius` 2 (texels; o
  `shadowBlurRadius` os substitui), `shadowResolution` 128,
  `baseShadowResolution` 128, `shadowW0` 0,7, `shadowH0` −1, `shadowW1`
  0,7, `shadowH1` 2 (tiles). Nenhum arquivo do jogo os muda.
- **Dentro** (`IsOutside` falso): o viewer olha **para baixo** (o eixo Z,
  `DAT_02c0bbf8` = (0, 0, 1) negado), girado com o Sim, cobrindo ±W0 ×
  ±W1; a silhueta vai para um quad (`ShadowShader`, camada
  `simShadowLayer`·4 = 4, depois do opaco) na altura dos pés + 0,005, que
  **multiplica** o que já foi desenhado pela textura (`srcFactor(destColor)
  dstFactor(zero)`, sem escrita de z); o quad mostra só o miolo
  `(resolução − 2·blur)/resolução` da textura (a margem do borrão).
- **Fora**: o viewer olha ao longo de `−(direção de sombra do gerenciador
  de luz)` (vtable `0x78` do `cLightingManager`: a do `sun` do estado, conferido na n40, §12.4),
  com a caixa ±W, alturas H0..H1 projetadas; a silhueta vai para
  sobreposições no **piso e no terreno** (`+0x48`/`+0x4c`,
  `OverlayShadowMaterial`: a sombra inteira na cor de sombra onde o alfa
  da textura passa de 100/255, a cor vem da vtable `0x7c` do gerenciador),
  e o quad de dentro fica escondido.
- **A cor de sombra de fora** (`cLightingManager`, BV `0x705f70`, a cada
  troca de estado): `s = shadowStrengthLimit (padrão 1, construtor
  0x7127d8) · projectiveShadowStrength` (0,73 de dia, 0,4 à noite; o
  comando guarda em `+0x184` do estado, `0x1ba4f80`); a luz do estado num
  piso para cima (`FindIllumFromLightingState`) com o peso do `sun` ×
  `max(0, 1 − s·hmSunLightTransitionZone)` (`0x7047e4`) dividida, canal a
  canal e com as duas presas em [0, 1], pela mesma luz sem sombra (`+0x28c`;
  zona 0 → branco). De dia dá (0,80, 0,78, 0,75), à noite ≈ (0,86, 0,86,
  0,83): sombras claras. É essa a cor que a vtable `0x7c` entrega às
  sobreposições (conferido na n40, §12.4).

**No código** (`Renderer::ShadowCaster`, `LotLighting::update`, e no
estúdio do CAS `CasMode::update`, uma sala): cada Sim
na VM com a opção ligada (`GraphicsOptions::simShadows`, Shadows `$High`)
desenha a silhueta num atlas de 8 × 8 texturas de 128, como acima, e os
pisos e o terreno (as superfícies com light map voltadas para cima) a
uma altura entre 0,6 abaixo e 0,3 acima dos pés multiplicam a cor pela
silhueta borrada (dentro) ou pela cor de sombra onde ela cobre (fora);
dentro, como o quad do jogo cobre o que não passa da altura dos pés, as
faces de cima dos objetos opacos até 0,03 acima dos pés (tapetes, o
pódio do CAS) também (`docs/screenshots/r15-cas-sombra-sim.jpg`).
A cor e a direção de fora como acima (`LotLighting::update`; conferidas na
n40). Inferido: o borrão como uma
caixa de 5 × 5 amostras bilineares; projetar nas superfícies em vez de
num quad plano (o quad do jogo flutua sobre um chão mais baixo, ex. o Sim
na escada); o estilo 4 nos lotes falta (o 3 são as GUOB, §12). Captura:
`docs/screenshots/r15-fov-sombras-sims.jpg`.

## 10. Rodada n16 (luz e light maps): o que foi lido

Ghidra MCP (BV, só leitura). Nomes do `baserom.sym` (o BV de dezembro de
2007 do `sims2_mac_decomp`) casados pela ordem e pelo tamanho das funções;
endereços do BV do Ghidra.

### 10.1 A ordem de uma atualização da luz (BV `0x7167d0`)

O `cLightingManager` atualiza tudo numa passada (sem atualização
incremental, `DAT_02c1bed4` = 0 ou o `+0x93` do gerenciador ligado):

1. limpa as luzes de cada sala (`0x713140`), põe os `diffParam` do estado na
   difusão (vtable `0x20` do `+0x34`, a partir do `+0x188` do estado);
2. `AddOutdoorLights` (`0x70cd3c`, §3);
3. monta a lista de fontes de difusão dos objetos (`+0x28`, vtable `0x24`,
   `BuildLightSourceList`) e, com portais (`DAT_02c1bf1c`),
   `SetAndAddPortalLights` (`0x70ae3c`, §5.4) acrescenta as dos portais;
4. `DoDiffusionPass` (vtable `0x38` do `+0x28`, `0xd7dd80`): grades de
   piso, cópia para as páginas, paredes (§10.4);
5. `UpdateHeightMapShadows` (`0x6fea68`, §5.3);
6. `AddDirectFloorLighting` e `AddDirectWallLighting` (vtable `0x9c` e
   `0xa0` do gerenciador: `0x708ebc`, `0x708590`; as versões só de tiles
   sujos são `0xa4`/`0xa8`), **somadas** aos texels que a difusão deixou;
7. `GatherRoomLighting` (`0x7147c8`, §4.1), que lê os light maps já com a
   luz direta;
8. `BrightenUnlitRooms(fontes, n)` (vtable `0xac`, `0x707820`; `0xb0` =
   `BrightenUnlitRoomsDirtyTiles` `0x707f50`; `0xb4` =
   `BrightenUnlitRoomsNEW` `0x701d98`, que esse caminho não chama);
9. `AddRoomLights` (`0x710a3c`, §4).

O vtable de `cLightingManager` com esses slots está em `0x27e37fc..`
(`…, 0x708ebc, 0x708590, 0x7095f0, 0x708a88, 0x707820, 0x707f50,
0x701d98`). O `cDiffusionManager::BrightenRoom` (`0xd7d028`, 0,5 ×
`unlitRoomColor` nas amostras) não tem quem o chame por esse caminho.

### 10.2 Cômodos sem luz, conferido (`FindUnlitRooms` `0x701a28`)

Confirma o §6.1: o estado guarda **uma** cor (`+0x144..0x14c`) e **um**
limiar (`+0x150`); o `day` do `Lighting.txt` tem duas linhas
`unlitRoomColor` e vale a segunda (`(.45, .425, .4) -threshold .4`). Para
cada sala ≠ 0 do vetor de `cRoomLightingDesc` (`+0x1ac`): `L` = média das 6
cores (`0x700710`), `m = MaxAbsElement(L)`, `f = 1 − m/t` se `m < t`; as
salas com `f > 0` ganham `cor · f` nas cores 0 a 4 da descrição (as cinco
primeiras; a sexta, a do teto, não). Depois o `BrightenUnlitRooms` soma
`cor · f · 0,5` nos texels de piso e de parede da sala (§6.1). Como vem
depois do `GatherRoomLighting`, a média usa a irradiância já com a luz
direta, e o clareamento não volta para a irradiância por outro caminho
além das cinco cores.

### 10.3 As fontes de difusão dos objetos (`BuildLightFromObjectLight` `0xd7d404`)

`AddLightsFromObject` (`0xd7ef30`) percorre os `cLightRefNode` do objeto
(interface `0x53d200a`) e, para cada um, `BuildLightFromObjectLight`:

- a fonte começa com a cor `DAT_02d59b98` (preto) e lê a extensão
  `cDiffusionLightExtension` da luz (`GetDataFromLightRef` `0xd82498`,
  interface `0x8a561e23`). **Sem a extensão a cor fica preta e a luz não
  vira fonte de difusão** (a função devolve 0). Com ela: mínimo e máximo do
  retângulo (`+0x48..+0x54`), cor = intensidade × cor da extensão (`+0..+8`),
  direcional (`+0x24`), queda (`+0x38`), queda direcional (`+0x34`),
  escalas de parede (`+0x3c`) e de piso (`+0x40`), queda entre andares
  (`+0x44`). Padrões da fonte (`0xd82248`): queda 1, escalas 1, entre
  andares 0,5, retângulo ±0,5. Padrões da extensão (construtor BV
  `0x79f024`, que o symxfer chamou de `cAwningGeometryNode::InsertIndices`;
  vtable `0x27ef6ac`): mínimo (−0,5, −0,5), máximo (0,5, 0,5),
  **intensidade 0**, cor (1, 1, 1), não direcional, queda 1, queda
  direcional 0, escalas de parede e piso 1, entre andares 0,5 (o comentário
  do `LotLights.txt` diz intensidade 10: não é o que o construtor põe).
- as extensões não estão nos recursos do jogo (nenhum pacote tem a string
  `cDiffusionLightExtension`; os `LGHT` são `cPointLight`/`cSpotLight`
  sem extensão): vêm dos `lightAttribOverride … diffusionSource*`, que o
  comando `cLightAttribOverrideCommand` (vtable `0x2aa3bc0`, execução
  `0x1809b38`) guarda numa tabela de sobrescritas (`+0x378` do gerenciador)
  por nome de luz, com os atributos como propriedades (`diffusionSource*`
  = IDs `0x8a561f18..`, tabela de nomes em `0x283fff0`). Quem aplica a
  tabela e cria a extensão não foi achado.
- posição: a da luz no mundo (o tile tem de estar no lote, vtable `0xf8`
  do mundo); direção = o −Z da luz no mundo, ou a do objeto quando a luz
  aponta para cima ou para baixo (|z| > 0,99); `+0x28/+0x2c` a direção no
  plano, `+0x30` o rumo (`atan2`); a sala (`+0x68`) é a do ponto
  `posição + max(0, mínimo.y) · direção/|direção|²` (o mínimo do retângulo
  à frente da luz; com o padrão −0,5, a da própria luz). `FUN_00d82334`
  monta o retângulo girado pelo rumo.

### 10.4 A grade de difusão do piso (`cFloorColorGrid`, lida inteira)

Correções ao §5.1 em negrito. Uma grade por andar; amostras em meio tile
(`2L × 2A`), a amostra `S(x, y)` = par de metades `h0`/`h1` (0x24 bytes
cada: cor RGB `+0`, `nb` `+0x18` = bit `d` a luz sai nessa direção, `sel`
`+0x19` = bit `d` o vizinho é alcançado na metade 1, fora `+0x1a`, existe
`+0x1b`, suja `+0x1c`, mudou nesta passada `+0x1d`, sala `+0x20`). O tile
`(tx, ty)` tem as amostras `(2tx|2tx+1, 2ty|2ty+1)`. Direções `d` (tabela
`0x2747cb8`): 0 (0,−1), 1 (−1,−1), 2 (−1,0), 3 (−1,1), 4 (0,1), 5 (1,1),
6 (1,0), 7 (1,−1).

**Montagem** (`UpdateFromWorldDB` `0xeeb614` → `SetupTileGridFromWorldInfo`
`0xee8400`, `SetupColorGridFromWorldInfo` `0xeeaaec`):

- o tile existe se está no lote e **tem chão** (`HasAnyGround`, vtable `0xbc`
  do mundo: no andar 0 sempre; acima, algum quarto com piso); as quatro
  amostras ganham `h0` existente, `h1` não;
- **as paredes que bloqueiam a luz** (estilo de partição com `blocksLight`,
  vtable `0x144` do produto, ou estilo 29, parede de piscina; **cercas e
  muros sem `blocksLight` não bloqueiam**) tiram bits de `nb` das amostras
  do lado: borda −y: `s00`, `s10` `&= 0x7C`; −x: `s00`, `s01` `&= 0xF1`; +y:
  `s01`, `s11` `&= 0xC7`; +x: `s10`, `s11` `&= 0x1F`. **Portas e arcos não
  deixam passar** (a máscara vale com porta). Diagonal `x + y = 1` (bit
  `0x10`): `h1 ← h0` nas 4 amostras e `s01.h0 &= 0xCF`, `s01.h1 &= 0xF9`,
  `s11.h0 &= 0x07`, `s00.h1 &= 0x70`, `s10.h0 &= 0x9F`, `s10.h1 &= 0xFC`
  (tile diagonal 1); diagonal `x = y` (`0x20`): `s00.h0 &= 0xF3`,
  `s00.h1 &= 0x7E`, `s01.h0 &= 0xC1`, `s10.h1 &= 0x1C`, `s11.h0 &= 0xE7`,
  `s11.h1 &= 0x3F` (tile diagonal 2);
- a sala: `R(px, py)` = sala de luz (3ARY `0x0B`, definição 0) no ponto do
  tile; tile normal `R(.5, .5)` em todas as `h0`; diagonal 1: `R(.25, .25)`
  nas `h0` e `R(.75, .75)` nas `h1`; diagonal 2: `R(.75, .25)` / `R(.25,
  .75)`. **Fora = sala 0**. As salas **não** entram na ligação: só paredes e
  metades que faltam bloqueiam;
- ligações (`SetupConnection` `0xee78e0` para cada vizinho; vizinho fora da
  grade = bit apagado): `h0↔h0` se as duas deixam, senão a metade que
  deixa liga à metade do vizinho que deixa (`sel`), senão o bit cai;
  diagonais (`SetupDiagonalConnection` `0xeea7f8`): só ficam se um dos dois
  caminhos em L estiver aberto e chegar à mesma metade.

**Ambiente e fontes.** `InitGridColors` (`0xeeb8a4`): fora `outsideColor ·
outsideFloorAmbient`, dentro `insideColor · insideFloorAmbient`.
`DoesLightSeedThisFloor` (`0xeeba10`) e `InitGridColorsForLight`
(`0xeebdec`), sobre o retângulo da fonte em amostras (`sx0 = ⌊2·minX +
.5⌋`, `sx1 = ⌊2·maxX − .5⌋`, idem y; o retângulo = posição + os cantos
mínimo e máximo girados por `rumo − π/2`, `0xd82334`): só as metades da
**sala da fonte**; fonte acima do andar: para cada andar entre os dois, se
há piso no quarto do canto mínimo da amostra a amostra é pulada, senão
`k ·= 1 − interFloorFalloff` (com `interFloorFalloff ≥ 1` nada); cor =
`max(c, rgb · floorIntensityScale · k)`.

**Propagação** (`CalculateGrid` `0xeeccc0`, `RelaxGridColors` `0xeec3a4`):
as fontes que semeiam o andar ordenadas pela queda, **as de menor queda
primeiro**, em grupos com `|Δ| < 0,01`; cada grupo semeia e relaxa com
`k = queda · floorThreshold` (a grade não é limpa entre grupos). Cada
passada (até `wallPropagationSteps`, `+0x70` do gerenciador, 50; o
`floorPropagationSteps` não chega às grades) varre o retângulo ativo em 4
sentidos: por linha, x crescente puxando das direções 1, 2, 3 e x
decrescente de 5, 6, 7; por coluna, y crescente de 7, 0, 1 (sem exigir
vizinho sujo) e y decrescente de 3, 4, 5; só puxa de vizinho **sujo**
(mudou nesta passada ou na anterior), e o retângulo cresce quando muda
na borda. Regra (`0xee7c6c`): `k ·= 1,4` nas diagonais; por canal, se
`n − c > k·c`: `c = n/(1 + k) + 0,01`. Para quando uma passada não muda
nada. No fim da passada a sujeira é a do que mudou (a última linha e
coluna do retângulo ficam de fora, peculiaridade).

**Cópia para a página** (`CopyToTexturePages` `0xeea560`,
`FillInGuardTilePair` `0xee9874`, `cGuardTile::FillTileTexels` `0xee8c88`,
`FillHalfTileTexels` `0xee9120`, `cGuardTilePair::FillTileTexels`
`0xee95d0`): por tile, uma guarda 4×4 de cores: o miolo 2×2 são as 4
amostras do tile (metade que falta = o ambiente de fora/dentro), o anel
de 12 são os vizinhos **pela ligação** (`GetNeighborSample` `0xee9798`);
um vizinho diagonal que falta = a média dos dois vizinhos retos, e um que
não se alcança = a amostra do próprio tile mais perto. Com a flag `+0x40`
(ligada no construtor, **em todos os andares**) as amostras de fora perdem
`outsideColor · outsideFloorAmbient`. Texel da coluna `u` e linha `v` (v =
1 no lado +y do tile, a primeira linha da página): no andar 0
`u = (i + .5)/8`, `v = 1 − (j + .5)/8`; nos outros `u = i/7`, `v = 1 − j/7`
(as bordas sobre a borda do tile); `gx = 2u + .5`, `gy = 2v + .5`, bilinear
entre as 4 entradas da guarda em volta, × `2 · escala` (0,5 → ×1),
`PackToARGB32` (`round(256·c)` preso em 0..255), escrito por cima. Tiles
diagonais: cada metade com a sua guarda, só os texels do seu lado
(`u·s + v < off` contra a metade; diagonal 1: `s = 1`, `off = 0,99`;
diagonal 2: `s = −1`, `off = −0,01`; a versão de meio tile começa em
`u = 0` também no andar 0).

`BrightenRoom` (`0xee86c8`) soma `cor · 0,5` às metades da sala (só salas
de dentro); `DarkenCorners` (`0xeed11c`) puxa para o ambiente as amostras
perto dos cantos de `GetDarkCorners` (pesos 4×4 de `0x2747c40` ou 2×2 de
`0x2747cf8`), mas roda **depois** da cópia do piso: na passada completa
não aparece. `LookupFloorDiffusion` (`0xee88c4`): 2 × a cor da amostra no
ponto (menos o ambiente de fora).

### 10.5 A difusão nas paredes (`cWallDiffusionManager`, lida inteira)

Substitui o §5.2. Nomes de BV corrigidos: `0xeee278` =
`ClearFinalColorColumn`, `0xeee464` = `ClearColumn`; `0xeeeb54` dá o elo de
**cima** (`+0x15c`) e `0xeeeb88` o de baixo (`+0x168`).

**Estrutura.** Uma `cDiffusionWall` (0x1c) é uma reta de peças de parede de
um lado, mesmo rumo (|Δ| < 0,0942) e mesma sala (`0xa70834`), em volta do
laço do lado (`SetupWallSection` `0xeefcf4`, lados 0 e 2 de cada seção;
`SetupWalls` `0xef0324` nos grafos de parede 3, 8, 10, 11 e nas paredes de
piscina, tipo 0x1D); as retas de um laço ficam ligadas (`+0xc` anterior,
`+0x10` seguinte, em anel). Cada segmento (0x178) é um tile de parede:
início e fim, o quarto de tile, **2 colunas × 6 linhas de amostras (meio
tile; linha 0 no alto)** em bytes ARGB, uma moldura 4 × 8 de cores finais,
e os elos para o segmento do mesmo quarto de tile no andar de cima (se lá
não há piso) e no de baixo (se no próprio andar não há piso)
(`ConnectFloors` `0xeef624`). Comprimento da reta = n (× 1,4142 diagonal);
o lado da frente é a normal `(dir.y, −dir.x)`.

**Uma passada** (`DoDiffusionPass`, depois da cópia do piso):

1. `Clear` (`0xeee87c`): amostras = `pack(ambiente)` (dentro
   `insideColor · insideWallAmbient`, fora `outsideColor ·
   outsideWallAmbient`), `aceso = 0`.
2. `AddDirectLight` (`0xef28ac`), para cada fonte e cada reta: só se o
   andar da reta ≤ o da fonte e a sala da reta = a da fonte; `c` = centro
   do retângulo da fonte, `s` = `wallIntensityScale`; descendo da fonte até
   o andar da reta, piso em `c` interrompe (sem atenuar), senão `s ·= 1 −
   interFloorFalloff`. `k = wallThreshold · queda`. Na reta
   (`0xef2594`, `ClosestColumn` `0xeeddac`): `t` = projeção de `c` /
   comprimento; dentro, `dist` = distância perpendicular (com sinal) e a
   coluna `⌊t·2n⌋`; antes do início ou depois do fim, `dist = ±|c − início|`
   e a primeira ou a última coluna; `dist ≤ 0` (atrás) não acende. Fonte
   direcional (`co` = cos entre o rumo da fonte e a direção até a parede):
   `f = (6 − 2co)/(6 + 2co)`, `s ·= ((co + 1)/2)·fd + 1 − fd` com `fd` =
   queda direcional; `passos = max(1, f·2·dist)`; cor = `rgb · s ·
   (1/(1+k))^passos`; direcional: `k += max(co, 0)·(2k/passos − k)`. Daí
   `SetAndPropagateColumnLeft/Right` (`0xef2210`, `0xef1e70`), com `k' =
   1,4k` em parede diagonal e divisão em ponto fixo `M(k) =
   round(4096/(1+k))`, `v·M >> 12`: para a esquerda a partir da coluna, para
   a direita a partir da seguinte (já dividida); em cada coluna as linhas
   1–4 = `max`; se nada mudou, para; divide; **as linhas 0 e 5 são
   sobrescritas** com o valor já dividido. A reta acesa fica `aceso = 1`,
   e as de cima dela também (`SetIlluminatedVertical` `0xeef078`).
3. `FillWallsFromFloors` (`0xef0e68` → `FillFromFloorTexture` `0xef0b58`):
   `c = aceso ? brightWallFloorContribution : darkWallFloorContribution`;
   em cada segmento sem elo de baixo, cada coluna pega o texel do light map
   de piso (o mais perto, × 2) a 1/8 de tile na frente do meio da coluna e
   põe `max(amostra, piso · c · 0,5)` nas **6 linhas** (`0xef062c`).
4. `DoPropagationPass` (`0xef1d84`), só nas retas acesas ou mudadas pelo
   piso, até `wallPropagationSteps` (50) passos ou nada mudar; `k =
   wallThreshold` só (sem a queda da fonte); dentro do segmento, as 8
   vizinhas (diagonal `1,4k`); entre segmentos da mesma reta, pela borda;
   entre andares, pelos elos (linha 0 ↔ linha 5 do de cima). **Não passa de
   uma reta para outra** (canto). Regra: `v = max(v, viz · M(k) >> 12)`.
5. `FillInFinalColors` (`0xeeef34`): moldura 4 × 8 = as amostras e, na
   borda, as dos segmentos vizinhos (lado, cima, baixo, cantos).
6. `DarkenCornersNextToDarkWalls` (`0xef4000`): numa reta acesa ao lado de
   uma apagada que vira 90° (a de antes: rumo da reta = anterior − π/2; a
   seguinte: seguinte = reta − π/2), as colunas finais junto ao canto
   viram `0,8·ambiente + 0,2·antigo` (as duas primeiras) e `0,4·amb +
   0,6·ant` (as duas seguintes).
7. `CopyToTexturePages` (`0xef3bfc` → `0xef2c9c`): célula de 8 × 24 texels
   por segmento (linha 0 da célula = pé da parede): `u = 0,5 + i·2/7`,
   `v = 6,5 − j·6/23` na moldura, bilinear, texel = `pack(0,5 · (cor −
   amb))`, `amb` = o ambiente de fora nas paredes de fora (flag `+0x38`),
   preto nas de dentro; célula mais alta: cada linha acima de 24 = a de
   baixo dividida por `M(0,4·wallThreshold)`.

`BrightenRoom` da parede (`0xeee134`) soma nas cores finais; só o
`cDiffusionManager::BrightenRoom` o chama (sem uso no caminho da
passada completa). `LookupWallDiffusion` (`0xeef45c`, objetos) = a amostra
da coluna 1, linha 3. Configuração (`UpdateFromConfig` `0xd7f764`): o
`diffParam wallThreshold` do estado vai para o `+0x4c` (dia 0,4, noite
0,2), `insideWallAmbient` para o `+0x40`; `outsideWallAmbient`,
`outsideDiffusionAmbientColor`, `bright/darkWallFloorContribution` e
`wallPropagationSteps` vêm dos `set` do `Lighting.txt`.

Não confirmado: o que são os grafos de parede 3, 8, 10, 11 (aqui: todas as
faces com papel, cercas inclusive); se o jogo usa o `DoDiffusionPass` ou o
`DoDiffusionPassOptimized` (os dois estão na vtable); o sinal da
convenção de rumo (os cantos escuros usam a virada à direita, com a sala à
direita, tomada como a do canto de dentro do cômodo).

### 10.6 Irradiância dos cômodos e portais, conferidos

`GatherFloorLighting` (BV `0x714258`, disassembly): para cada sala de
dentro, os tiles da sala (vtable `0x130` do mundo) e cada quarto com piso
(material pela vtable `0x9c`); `c` = `LookupFloorDiffusion` no quarto
(vtable `0x5c` do gerenciador de difusão, BV `0xee88c4`: **2 × a amostra**
mais perto, menos o ambiente de fora se a metade é de fora); `I[4] +=
0,25·c ⊙ Mix(branco, cor do piso, roomSaturation)` e `I[5] += 0,25·c ·
roomCeilingStrength`, pesos +1. `GatherWallLighting` (BV `0x713ca0`): cada
lado de parede de uma sala de dentro com papel, `c = 0,5 ·
LookupWallDiffusion` (vtable `0x60`: a amostra da coluna 1, linha 3 do
segmento, menos o ambiente de fora numa parede de fora), rateado pela
normal. `FindPortalLightValues` (BV `0x70a884`, conferido): o `L` é esse
mesmo `LookupWallDiffusion` na posição do portal (`+0x18`), sem a luz
direta do estado.

**Quem monta os portais** (BV `0x715cb0` → `0x7149fc`): para cada quarto
de parede com um portal (`PortalInWall`, na peça ou na oposta), cada luz
do objeto (os `cLightRefNode` do modelo) vira um `cPortalLight` (0xcc
bytes): a luz projetada na aresta da parede (`WallEdge`); as salas dos
dois lados (`RoomForTilePart` de cada localizador); pelo sinal de
`direção da luz · normal do lado A` o portal vai para a lista da sala **de
onde a luz vem** e o `+8` é a sala **para onde ela aponta** (a luz prática
recebe o grupo dessa sala); `+0xc/+0x10` = a normal do lado de destino;
`+0x18` = o localizador do lado **de onde a luz vem** (o que o
`LookupWallDiffusion` lê); a posição = o meio do quarto de parede +
normal · `spillPlaneOffset` (0,1, `+0x88` do gerenciador), a altura no meio
dos pisos; `+0x3c` = objeto do tipo 8 (porta/arco); a fonte de difusão
(`+0x60`) sai do `BuildLightFromObjectLight` da própria luz. Não lido: a
sala que essa fonte recebe (a da posição da luz, que fica no meio do tile
da janela) e o que acontece numa janela sobre um andar sem piso.

### 10.7 No código (rodada n16)

`src/formats/lot_diffusion.{hpp,cpp}`: `FloorColorGrid` (montagem, fontes,
propagação, cópia pela guarda) e a difusão das paredes (`diffuseWalls`:
segmentos, retas, elos de andar, luz direta das fontes, piso trazido do
light map, propagação, moldura, cantos escuros, células 8 × 24, e o
`LookupWallDiffusion` de cada face). `computeLotLightMap`
(`lot_lightmap.cpp`) roda as lâmpadas, monta os portais com a luz das
paredes das lâmpadas, roda tudo de novo com os portais, soma a luz direta
do estado (pisos da sala 0 e paredes de fora), junta a irradiância dos
cômodos e aplica os cômodos sem luz (§10.2). `LotLighting::modelLights`
só faz fonte de difusão de uma luz com `diffusionSource*` (§10.3), com os
padrões da extensão. Os testes estão em `tests/test_formats.cpp`
(`testLotLightMap`).

Inferidos que ficam: os portais (§7); a altura em que uma luz está (o
andar cujo piso fica abaixo dela; o nível do objeto na VM nem sempre
bate); as retas de parede montadas das faces (junção = outra parede no
ponto pelo lado da frente; o jogo anda o laço do lado); a virada dos
cantos escuros (à direita, com a sala à direita); a altura das paredes que
não têm 3 tiles (as 6 linhas a partir do pé; o jogo não escreve células de
outro tamanho).

Capturas (`--vm --run-vm 30 --lighting day|night`, à noite `--lamps 100`,
paredes cortadas; à esquerda antes da rodada, à direita depois; linhas
N001_Lot11, N002_Lot31, N003_Lot52): `docs/screenshots/n16-luz-noite-dentro.jpg`,
`n16-luz-dia-dentro.jpg`, `n16-luz-fora-dia.jpg`, `n16-luz-fora-noite.jpg`;
e as páginas de piso do light map (`OS2_LIGHTMAP_DUMP`; N001 andar 1 de dia
e de noite, N002 andar 0 de dia, N003 andar 0 à noite; +y para cima):
`n16-luz-lightmap.jpg`. O que mudou à vista: os objetos de dentro deixam
de sair pretos (os cômodos sem luz do jogo os clareiam, de dia e de
noite); as poças das janelas ficam brancas (a cor da extensão, não a da
luz) e por tile (a guarda da cópia); as paredes de cima brancas do N003 à
noite deixam de brilhar; o salão dente de serra do N002 e as faces de
dentro de cômodos sem piso (a torre da igreja) escurecem; os tiles sem
piso dos andares de cima ficam pretos na página (não são desenhados).

## 11. Rodada n35 (luz, resto): o que foi lido

Ghidra MCP (BV, só leitura). Endereços do BV do Ghidra; nomes do
`baserom.sym` casados pela ordem quando o BV não os tem.

### 11.1 As "Delta" têm sinal; o cômodo dos objetos é o da luz

`FUN_0070daa8` (o construtor de `roomLightingType` 2–8, §4) escolhe cada
Delta com `FUN_00700590` (as seis direções; `FUN_00700610`, só os quatro
lados, com N < 3), que compara `MaxAbsElement` (BV `0x1808d8c`:
`max(|r|, |g|, |b|)`, positivo) e fica com a primeira das iguais. A luz
recebe a cor `L_d` **como está, com sinal**: depois de `L_k −= A` (a média
das seis), uma direção mais escura que a média é uma luz **negativa**, que
o vertex shader do DX8 soma como as outras (`difusa = Σ max(N·L, 0)·cor`,
§4.2 do `shaders-mac.md`) e que escurece as faces viradas para ela. O nome
"Delta" é isso: diferenças em torno do ambiente. Num cômodo escuro à noite
(`unlitRoomColor` somado só às cinco primeiras cores, `FindUnlitRooms`
`0x701a28` conferido de novo: o laço vai de 0 a 4), o teto (`I[5]`) fica
bem abaixo da média e a primeira Delta é a do teto, negativa: o topo dos
objetos escurece (balcões e camas quase pretos por cima), os lados
claros. O código antes só pegava as positivas: quatro luzes que só
clareavam, e a geladeira ficava quase branca.

Os conjuntos de luz dos cômodos são por **sala de luz** (o vetor de
`cRoomLightingDesc`, `+0x1ac`, indexado pelo ID da definição de sala 0 =
3ARY `0x0B`; o grupo de render das luzes é `FUN_00bfdd60(ID)`). O app dava
a cada objeto o conjunto do seu cômodo da VM (3ARY `0x09`), que as cercas
dividem e que numera as salas de outro jeito: no `N001_Lot11` a cozinha é
a sala de luz 4 e a VM a chama de 6, que na luz é um armário de 2 tiles;
os objetos da cozinha recebiam a luz do armário. Inferido: que o grupo de
render de um objeto é a sala de luz do quarto de tile onde ele está (o
`cObject` não foi lido nessa parte; é o único que casa com as luzes).

### 11.2 As chaves de luz: o jogo roda o caminho otimizado

As bandeiras globais que a passada lê (`DAT_02c1bf28`, `bf1c`, `bef8`,
`bed4`, ...) não têm escrita no código: são o `bool` (`+4`) de objetos de
12 bytes construídos no inicializador estático (`0x20e6c74..0x20e6ec4`,
`FUN_014bbbbc(obj, padrão)`; destruídos em `0x702878..`), com o nome no
`+−4` vindo da tabela `0x27e3fdc` — são as `boolProp` do `Graphics
Rules.sgr`:

| objeto | `bool` | nome | padrão | `Graphics Rules` |
|---|---|---|---|---|
| `0x2c1bf24` | `bf28` | `lightingEnabled` | 1 | — |
| `0x2c1bf18` | `bf1c` | `portalLighting` | 1 | — |
| `0x2c1bf0c` | `bf10` | `heightMapShadows` | 1 | — |
| `0x2c1bf00` | `bf04` | `floorAndWallNormalMapping` | 1 | — |
| `0x2c1bef4` | `bef8` | `lerpLights` | 0 | **true** |
| `0x2c1bee8` | `beec` | `testLightingUpdates` | 0 | — |
| `0x2c1bedc` | `bee0` | `incrementalLighting` | 0 | **true** |
| `0x2c1bed0` | `bed4` | `useDirtyTiles` | 0 | **true** |
| `0x2c1bec4` | `bec8` | `debugDirtyTiles` | 0 | — |
| `0x2c1beb8` | `bebc` | `allObjectLightsOn` | 0 | — |
| `0x2c1beac` | `beb0` | `integratedShadows` | 0 | — |

O `Graphics Rules.sgr` faz sempre `setOption LightingOptimizations $High`
(`optimizedDiffusion`, `incrementalLighting`, `lerpLights`, `useDirtyTiles`
verdadeiros). Então no jogo: as luzes dos cômodos e dos portais vão pelos
`cLerpLightSet` (`FUN_00d78f78`/`FUN_00d79738`, `SetAndAddPortalLights`
`0x70ae3c` — o `0x70b5a8` sem lerp não roda); a primeira atualização (o
`+0x93` do gerenciador, 1 no construtor) faz a passada completa e as
seguintes a otimizada (§11.3), a luz direta só nos tiles sujos (vtable
`0xa4`/`0xa8`) e o `BrightenUnlitRoomsDirtyTiles` (`0xb0`).

### 11.3 `DoDiffusionPassOptimized` (BV `0xd7d9f0`)

Comparada com `DoDiffusionPass` (`0xd7dd80`, §10.4–10.5): as mesmas grades
(`UpdateFromWorldDB` quando sujas, `CalculateGrid` em todas), a mesma
sequência das paredes (`Clear`, `AddDirectLight`, propagação, moldura,
cantos), mas **sem** copiar as grades para as páginas dentro da passada (o
`CopyDiffusionToLightMapsDirtyOnly` faz isso depois, só nos tiles sujos), e
o piso trazido para as paredes vem das **grades**: `FUN_00ef0a48` →
`FUN_00ef07d0` usa o `LookupFloorDiffusion` do gerenciador de difusão
(vtable `0x5c`: 2 × a amostra de meio tile mais perto, menos o ambiente de
fora nas metades de fora) onde a completa (`FUN_00ef0e68` →
`FUN_00ef0b58`) lê o texel do light map de piso (`LookupFloorLightMap`,
vtable `0x40`). O resto (pontos a 1/8 de tile na frente do meio de cada
coluna, `max(amostra, piso · c · 0,5)` nas 6 linhas) é igual. Fora isso o
resultado é o da passada completa: no regime (depois da primeira
atualização) o jogo mostra as paredes com o piso da grade.

### 11.4 Portais

- **`doorOrArchDirectMultiplier`** é o `+0x64` do gerenciador
  (`UpdateFromConfigFile` `0x7175b8`), que o construtor (`0x7127d8`) põe em
  **0**, e o `Lighting.txt` não o define. No fim do `FindPortalLightValues`
  (`0x70a884`), num portal de porta/arco (`+0x3c`) o `f_direta` é
  multiplicado por ele **depois** de a cor do portal (`+0x30`) ser
  gravada: portas e arcos não têm luz direta (nem a prática
  "LM_PortalDirect" nem a fonte de difusão direta); só o spill, e a cor do
  portal na irradiância do cômodo fica.
- **Supressão** (`FindPortalSuppression`, BV `0x6ff9b0`), por lista de
  portais de um cômodo, quando `portalSuppressionAmount` (`+0x198`) > 0:
  todos começam com 1 (`+0x14`); para cada portal A com destino (`+8`) ≠ 0
  e cada outro B da mesma lista com o mesmo destino e o mesmo `+0x20` (do
  localizador da parede; inferido: o andar), `d` = centro da parede de B −
  centro da de A (`WallCentre`); se `d · dirA ≥ −0,001` e `d · dirB ≤
  0,001` e `0,1 ≤ |d| ≤ R` (`portalSuppressionRadius`, `+0x194`):
  `A −= (1 − |d|/R)·((dirA·dirB)²·(amount − corner) + corner)`, preso em
  0. Padrões do construtor: R = 1, amount = 0, `portalCornerSuppressionAmount`
  = amount quando não dado, `portalSuppressionAffectsRoomLighting` = 1;
  o `Lighting.txt` dá R = 4, amount = 0,2. A supressão multiplica as forças
  práticas (direta e spill) e, com `...AffectsRoomLighting`, a cor do
  portal na irradiância; **não** as fontes de difusão.
- **A sala da fonte de difusão de um portal**: o construtor (`0x7149fc`,
  ramo `lerpLights`) chama `BuildLightFromObjectLight` (vtable `0x2c` da
  difusão) com a própria luz da janela no `+0x60` do `cPortalLight`, e nada
  depois mexe nela: a fonte fica **na posição da luz** (no meio do tile da
  janela, ±0,05 da linha central do tile) com a **sala desse ponto** (com o
  mínimo padrão −0,5 não há empurrão para a frente). A posição `+0x54`
  (meio do quarto de parede + normal · `spillPlaneOffset`, altura no meio
  dos pisos) é só a do spill. As duas luzes de uma janela (`_n_light` e
  `_s_light`) ficam no tile da janela: a fonte das duas semeia a sala do
  lado onde a janela foi posta.
- **As luzes diretas das janelas** (`SetAndAddPortalLights` `0x70ae3c`):
  para cada portal com destino válido, "LM_PortalDirect" é a própria spot
  da janela no `cLerpLightSet` da sala de destino (`FUN_00d7a648`): cor =
  cor da luz ⊙ cor direta, intensidade × jitter (1) × *practical* ×
  `f_direta` (× supressão), queda × *practical falloff*; só para spots
  (vtable `0x178` da luz). O spill (`FUN_00d78aa4`) é uma luz pontual de
  meio-espaço (`FUN_00d80cd0`: `cPointLight` com um plano de normal = a
  direção do portal passando pelo `+0x54`) na posição da luz da janela, cor
  = cor da luz ⊙ cor do spill, intensidade × *spill practical* × `f_spill`,
  queda × *spill practical falloff*. Máscara: objetos, Sims e paredes.

### 11.5 Os grafos de parede da difusão

`SetupWalls` (BV `0xef0324`) monta as retas de parede a partir dos grafos
`{3, 8, 10, 11}` (tabela `0x2747d40`, vtable `0xa0` do gerenciador de
partições: os índices do `WallTopology`, `buy-build-mac.md`): **3 = as
paredes que bloqueiam a luz** (`blocksLight`, WGRA 7), 8 e 11 = meias
paredes e 10 = parede de palco (só BV); e, do grafo 0, só as **paredes de
piscina** (estilo `0x1d`). No jogo base: paredes, sótão, fundação e
piscina. Cercas, saias de deck (4), deck (16) e o estilo 24 não entram na
difusão das paredes.

### 11.6 "LM_sunHM" e `AddBlobToHeightMap`

- Num lote (modo `+0x50` = 1) com sombras de height map (`+0x6c`, a opção) e
  `+0x6e`, `AddOutdoorLights` (`0x70cd3c`) troca a luz do `sun` por uma
  `cHeightMapShadowLight` (`FUN_00d81a10`, 0x78 bytes: `cDirectionalLight`
  com o gerenciador em `+0x64`, `hmObjectShadowingBias` em `+0x68` — que a
  conta abaixo não lê — e os grupos `NoHMShadow`, `Portals`), com a cor e a
  escala da luz do sol. O seu `RadianceInfo` (BV `0xd80dac`, sem função no
  Ghidra) dá a cada parte desenhada a cor do sol × `f`, com `f` = vtable
  `0x80` do gerenciador (`0x6ff4a8`): 1 se o `+0x79` (= as sombras de
  height map calculadas, `UpdateHeightMapShadows` `0x6fea68`) está
  desligado ou o ponto está fora do height map; senão `d` = topo da sombra
  − altura do ponto (`0x1807ad4`) e `f = max(0, 1 − d·hmSunLightTransitionZone)`
  para `d > 0`. O ponto é o centro da caixa da parte no mundo; com a flag
  `0x17` do desenhável, o centro em x, y e o topo + 0,1 em z; grupo
  `NoHMShadow`: `f = 1`; grupo `Portals`: a origem do objeto. Ou seja,
  objetos de fora na sombra da casa perdem o sol (por parte, não por
  vértice). Inferido: que a flag `0x17` é a dos Sims.
- `FUN_00706920` (a textura por vértice com os blobs do `+0x278`,
  `AddBlobToHeightMap`) só roda no modo 2 (o bairro, `0x7167d0` e
  `0x715fe0`) e só com um estado que tem `lightMap` (o do bairro): no lote
  não há blobs nem essa textura. Quem chama o `AddBlobToHeightMap` segue
  sem achar (as árvores do bairro, provavelmente).
- `RepresentativeIrradiance` (vtable da luz, `0xd7ff7c`/`0xd7fff0`) de uma
  direcional é `|DiffuseScale·cor|·0,25`: o peso da luz na escolha das
  luzes de cada objeto (o contexto do DX8 leva no máximo 8, §4.2 do
  `shaders-mac.md`); quem ordena e corta não foi lido.

### 11.7 No código (rodada n35)

- `roomLights` (`formats/lighting_config`): o ambiente e as Delta do
  `FUN_0070daa8`, com sinal; `LotLighting` dá a cada objeto e Sim o conjunto
  da sala de luz sob ele (`lightingRoom`: 3ARY `0x0B`, 0 fora).
- `computeLotLightMap` (`formats/lot_lightmap`): portais com a fonte na
  própria luz, `doorOrArchDirectMultiplier`, supressão, uma luz de portal por
  peça de parede do objeto, e as práticas exportadas em
  `LotLightMap::portalLights`, que o `LotLighting` põe nos conjuntos das
  salas (o spill como luz pontual de meio-espaço: `Renderer::LightSet::Point::plane`,
  `plane` no `Light` do shader).
- `diffuseWalls` (`formats/lot_diffusion`): só as paredes de
  `layout.lightWalls` (que bloqueiam a luz, e piscina); o piso das paredes
  pelo `FloorColorGrid::lookup` (`LookupFloorDiffusion`), como a passada
  otimizada (`DiffusionParams::optimizedPass`).
- "LM_sunHM": `LotLightMap::sunTops`/`sunKept` e, no `LotLighting`, uma
  cópia do conjunto de fora com o sol × o fator (passos de 1/32) para cada
  instância de fora na sombra.
- Depuração: `OS2_DEBUG_LIGHTS` mostra a irradiância e as Delta de cada sala
  (e cone e direção das luzes); `OS2_DEBUG_LIGHTMAP` os pesos de cada sala.

Capturas (à esquerda o código antes da rodada, à direita depois; `--vm
--run-vm 30 --lighting day|night`, à noite `--lamps 100`, paredes cortadas):
`docs/screenshots/n35-luz-dentro.jpg` e `n35-luz-fora.jpg` (linhas
N001_Lot11, N002_Lot31, N003_Lot52, dia e noite); `n35-luz-noite-19h.jpg`
(N001_Lot11 às 19:05 pelo relógio da VM, lâmpadas acesas pelos objetos). O
que mudou à vista: os objetos de dentro ficam com o topo escuro e os lados
claros (as Delta negativas do teto e do piso): a geladeira não sai mais
quase branca, mas balcões, pias e camas ficam quase pretos por cima num
cômodo sem luz; a cozinha do `N001_Lot11` recebe a luz da sua sala (antes a
de um armário); as poças das janelas postas de fora caem do lado de fora (as
de dentro continuam); os objetos de fora na sombra da casa perdem o sol
(janelas do lado da sombra mais escuras).

**As poças "por tile" das janelas** (a outra queixa da QA da n16): é o
modelo do jogo. A fonte de um portal semeia o retângulo ±0,5 em volta da
luz (o mínimo e o máximo padrão da extensão), isto é, as quatro amostras de
meio tile do tile da janela por igual; a propagação divide por `1 + k` a
cada meio tile (`k = queda · floorThreshold`, 0,4 de dia); a cópia para a
página interpola bilinear entre as amostras pela guarda 4×4 (§10.4), 8
texels por tile; e os materiais que leem o `floorLightMap_0` (pisos,
terreno, sobreposições) não dão `textureFilterHint` ao estágio do light map
(o filtro padrão, bilinear, como o app amostra). Não há outra resolução nem
outro filtro a seguir.

## 12. Rodada n40 (sombras): o que foi lido

Ghidra MCP, só leitura, nos dois programas: o BV (`sims2_bv.ppc`, com os
nomes do `symxfer`) e o **base 1.0** (`sims2_base_1.0.ppc`, sem nomes,
achado pelas strings `GUOBOff`/`VisGUOBMaterial`/`groundshadow` e pelos
mesmos campos). Onde os dois diferem vale o base, que é o jogo de 2004 (como
nas outras rodadas: "0x58 no base" — na época o port usava 88; desde 18/10 usa
os conjuntos do AL, 114/60/231); o BV fica anotado.

### 12.1 As opções e os estilos, corrigidos

As opções de sombra são um bitset no `+0x2c` das `ShadowProperties` do
`cShadowManager` (o objeto em `+0x40` do gerente; padrões no construtor BV
`0x6f3d30`: `shadowSize` 1, distância 10, borrão 3/2, resolução 128/128,
W0/H0/W1/H1 0,7/−1/0,7/2, e os bits 4 e 6 ligados, 5 desligado). O
tratamento das variáveis de configuração do `cTSSGSystem` (BV `0x7c0900`,
o `SetFlag` é a vtable `0x18` do gerente em `+0xac`) dá: **bit 0 =
`simShadows`** (`0x8BE5F77B`), **bit 1 = `objectShadows`** (`0x8BE5F77C`),
**bit 2 = `guob`** (`0x0BE33684`). Os bits 3, 7 (`VisGUOBMaterial`) e 9
("todas as GUOB") nenhum arquivo liga: são de depuração. O §9 trocava os
estilos 2 e 3; o certo (`UpdateCurrentStyle`, BV `0x678710`, base
`0xa8e4ac`, iguais):

| dono (`+0x18`) | estilo atual (`+0x1c`) |
|---|---|
| 1 (Sims) | 1 com o bit 0, senão 0 |
| 2 (objetos comuns) | 2 com os bits 1 **e** 3; senão 3 com o bit 2 (`guob`); senão 0 |
| 3 | 3 com o bit 2, senão 0 |
| 4 (dado 68 = 3) | 4 com o bit 1, senão 0 |

Com os bits 2 e 9, 3. O **estilo 3 são as GUOB** (`UpdateGUOBs` só liga
alguma com o estilo atual 3); o 2 é a sombra projetada de objeto, que com o
bit 3 sempre desligado não aparece no jogo base. Ou seja, com `Shadows`
Médio ou Alto todo objeto tem as GUOB, com Baixo nenhum (como o app já
fazia). Um estilo 3 ainda vira 0 por uma função do objeto (base: vtable
`0x1a0` do objeto; BV: o byte `+0x81` com o tipo fora de 8–12 e sem as
vtables `0x2e4`/`0x238`, ligado ao sistema de sombras estáticas que só o BV
tem): **não lido no base**; o app continua dando GUOB a todo objeto.

### 12.2 `UpdateGUOBs` (BV `0x678488`, base `0xa8e138`)

Seis subconjuntos, nesta ordem (`kGUOBSubsetNames`, BV `0x277a8c0`, base
`0x15d6210`; o `roof-fence-water-mac.md` §4 tinha a ordem errada): 0
`groundshadow`, 1 `wallshadow`, 2 `northwallshadow`, 3 `southwallshadow`, 4
`eastwallshadow`, 5 `westwallshadow`. `InitGUOBs` (BV `0x6747c8`, base
`0xa8df88`) guarda o material de cada um (`FindGUOBMaterialFromSubset`, BV
`0x673ea4`: o material ligado ao subconjunto; um `GUOBOff` salvo, comparado
sem caixa, conta como **sem material**, e o subconjunto deixa de ser GUOB:
nunca mais liga) e desliga os de 2 a 5. `EnableGUOB(i, liga, vis)` (BV
`0x67417c`, base `0xa8e6d8`) põe no subconjunto o material guardado, o
`VisGUOBMaterial` (bit 7) ou o `GUOBOff`.

`UpdateGUOBs`: nada sem objeto, sem `InitGUOBs`, com dono 1 (Sim) ou com o
bit 0 do dado de objeto 34 (`0x22`); o base ainda sai com a vtable `0x1f4`
do objeto do jogo (não lida). Com o estilo atual 3 e a sombra visível
(`+0xc`, `SetIsVisible`): o chão (0) liga; o `wallshadow` (1) liga se o
**modo de parede** não é 0 (`cITSMainToolbar`, vtable `0x6c` da interface
`+0x1c` = o `+0x60` do BV / `+0x5c` do base, 0 baixas, 1 cortadas, 2
erguidas, 3 erguidas com telhado); os de 2 a 5 pela presença de parede
abaixo. Com o bit 9 todos ligam. Fora disso (outro estilo, invisível) todos
desligam.

**Presença de parede, base 1.0** (`FUN_00a8e9ac`, as quatro de uma vez):
todas falsas se o andar do objeto não está visível (`cSceneManager`, vtable
`0x50`: inferido "andar mostrado", pelo vizinho `SetLevelVisible`) ou se o
modo de parede **não é 2** (o 3, com telhado, desliga). Senão:

1. a caixa do objeto inteiro no mundo (`GrowGlobalBBox(caixa, −1, objeto)`,
   todas as formas, os planos de sombra inclusive);
2. o retângulo de tiles que a cobre: `x0 = floor(xmin)`, `x1 = ceil(xmax)`,
   `y0`, `y1` igual (feito com o truque do `+12582912` e a correção do
   inteiro exato);
3. quatro lados, cada um verdadeiro se **todo** segmento de um tile dele tem
   parede (`WorldDB` → gerente de partições → `WallExists`, o grafo de
   paredes e cercas, BV `0xc2ef54`): A ao longo de `y = y0`, B de `y = y1`,
   C de `x = x0`, D de `x = x1` (o laço para no primeiro que falta; lado sem
   segmento conta como verdadeiro);
4. pela direção do objeto (dado 1), `r = (−dir / 2) & 3` (divisão
   truncada): r 0 → norte C, sul D, leste B, oeste A; r 1 → norte A, sul B,
   leste C, oeste D; r 2 → norte D, sul C, leste A, oeste B; r 3 → norte B,
   sul A, leste D, oeste C. Isto é: os nomes são lados do modelo (norte =
   +y, leste = +x do modelo) girados pela direção (direção 0 gira o modelo
   90°: o +y do modelo cai em −x do mundo, o lado C).

**No BV** (`FindObjectWallPresence` `0x675d20`, uma por subconjunto): o
modo 2 **ou 3**; a caixa é a do **próprio subconjunto**
(`GrowGlobalBBoxForSubset` `0x6751a0`) arredondada ao inteiro mais próximo
(`GetBBoxIntExtents` `0x674d3c`: o plano curto demais estica um tile para o
lado de que está mais perto); um só lado (x constante: segmentos em y; senão
em x no `y` mínimo); sem a direção.

### 12.3 Quem recalcula (`cShadow::OnMove`, `cShadowManager`)

- **O objeto mexeu**: `cObject::OnObjectMoved` (BV `0x664854`) e
  `ForceToEdithLocation` (BV `0x6678f8`, o objeto indo para onde a VM o pôs)
  chamam o `OnMove` da sombra (vtable `0x34`, BV `0x6789a0`): estilo,
  andar, visibilidade, dentro/fora, posição e `UpdateGUOBs` (vtable `0x3c`).
  Vale para o objeto na mão do modo Comprar (ele anda pela VM): a sombra de
  parede liga e desliga ao vivo.
- **A sombra ficou visível** (`SetIsVisible`, BV `0x674378`): `UpdateGUOBs`.
- **Paredes** (`cShadowManager::DoMessage`, BV `0x6f0d40`, base `0xa90f8c`;
  mensagens `0x0B4CF313`/`0x2B4CF319`, parede posta e tirada):
  `UpdateShadowsAdjacentToWall` (BV `0x6f0a10`, base `0xa92f10`) pega os dois
  tiles dos lados da parede (`OppositeWallLocator`), e cada objeto em cada um
  (`WorldDB` `NumObjectsOnTile`/`ObjectOnTile`) roda `UpdateGUOBs`.
- **Modo de parede e andar** (`0xCB46C21D`, que `SetWallMode` e
  `SetCurrentLevel` postam com o andar atual): o andar entra num conjunto; o
  `OncePerFrameUpdate` do gerente (BV `0x6f2840`) roda
  `UpdateLevelShadows(andar)` (BV `0x6f0c64`): toda parede com uma ponta
  naquele andar faz o mesmo que acima.
- **Salas de luz** (`0x2B4D18FB`): os objetos dos tiles da sala (definição
  0) rodam o `OnMove` inteiro.
- Salvar: `OnPreSave` (BV `0x674aec`, base `0xa8e3a8`) liga todas e
  `OnPostSave` volta: **o lote salvo tem todas as GUOB ligadas** (as
  `GUOBOff` salvas são de outro caminho). Por isso o estado salvo não diz o
  que o jogo mostra.

### 12.4 A sombra dos Sims, conferida

O `OncePerFrameUpdate` do `cShadow` (BV `0x678ba0`) pega a direção do
gerente de luz (vtable `0x78`, BV `0x6ff23c`: o **`+0x20` do estado de luz
atual normalizado**, que é a direção da luz 0, o `sun` (§1)) e a cor da
vtable `0x7c` (BV `0x6fdeac`: o `+0x28c..+0x294` que o `0x705f70` grava, a
cor de sombra de fora do §9) e as passa às sobreposições `+0x48`/`+0x4c`
(vtable `0x14`). Os dois "inferido" do §9 (direção do `sun`, cor da `0x7c`)
ficam confirmados. O estado vem do `FUN_01807a48` (o elemento `+0xcc` da
lista de estados do gerente, ou o `+0x298`); qual dos dois numa transição
dia/noite não foi lido.

### 12.5 No código (rodada n40)

- `formats/object_shadow`: as regras do base 1.0 (`guobIndex`, a caixa do
  modelo `guobModelInfo`, `guobTileRect`, `guobWallSides`, `guobsOn`) e as
  arestas de parede do lote (`LotWallEdges`: paredes, cercas, paredes de
  piscina e fundação, saias de deck; as diagonais em nenhuma aresta). A
  caixa do modelo é a dos **bounding meshes** dos GMDC
  (`ObjectModel::bounds`; o `cGeometryDataContainer::SetRigidBound` é esse
  mesh), que é o corpo sem os planos de sombra; um GMDC sem bounding mesh
  entra com os vértices (inferido: o balcão colonial e a cama, cujos planos
  passam 0,0001 da borda do tile, ganham um tile a mais no retângulo); o
  volume do espelho (`mirrorboundingboxmaterial`) não entra (inferido: o
  teste `0x114` do `GrowGlobalBBox`, nó escondido).
- `Renderer`: uma máscara por instância (`setInstanceGuobs`) e os fatos de
  GUOB de cada modelo (`guobInfo`); os planos sem a máscara ligados (como o
  lote salvou) em tudo que ninguém calcula (bairro, CAS, lote sem a VM).
- `VmObjectView`: recalcula como o jogo (§12.3): o objeto mexeu (a VM, a
  mão do modo Comprar, inclusive enquanto segura), voltou a aparecer, uma
  aresta de parede ao lado mudou (`setWalls` pelo `rebuildLotModel`), o modo
  de parede ou o andar mostrado mudou (os objetos ao lado das paredes desse
  andar). O andar visível é o do objeto até o andar mostrado (inferido, a
  vtable `0x50` do `cSceneManager`). Não feitos: a mensagem das salas de luz
  (`0x2B4D18FB`: o `OnMove` dos objetos da sala; aqui só os vizinhos da
  parede mudada) e o teste da vtable `0x1f4` do base.
- Depuração: `OS2_DEBUG_GUOB=1` (os planos de cada objeto quando calculados,
  com a posição), `OS2_GUOB_AS_SAVED=1` (o estado salvo, para comparar);
  `s2pkg guob <jogo> <lote> [modo]` (os planos salvos e os calculados por
  objeto; `OS2_GUOB_PARTS=<id>`: as partes e os bounds). No modo 2 o
  `N001_Lot11` liga um plano de parede em 93 dos 310 objetos com GUOB, o
  `N002_Lot31` em 80 de 477, o `N003_Lot52` em 77 de 382; no modo 1 nenhum.
- O que muda à vista: com as paredes cortadas (o modo de sempre) os planos
  de parede dos móveis encostados somem (`docs/screenshots/n40-guob-cortadas.jpg`,
  os três lotes, antes/depois/diferença); com as paredes erguidas ficam
  onde há parede, e um móvel afastado da parede não leva mais o plano
  flutuando atrás dele (`n40-guob-movido.jpg`: o piano do `N001_Lot11` na
  parede, os planos dos lados desligam; arrastado no modo Comprar, o de
  trás some). A sombra do sol e a dos Sims ficam como estavam (só
  conferidas, §12.4): `n40-sombras-sol.jpg` (dia e noite nos três lotes).
- Teste `guob_walls` (o piano do `N001_Lot11` com paredes erguidas,
  cortadas, arrastado, com a parede de trás apagada; a janela com paredes
  baixadas) e `test_formats` (as regras).

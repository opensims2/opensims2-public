# Performance

Measured on the dev Mac (RelWithDebInfo, `-O2 -g`), N001_Lot11 and the 69 lots of
`TSData/Res/UserData/Neighborhoods`. The rule for every change below: the simulation
stays bit for bit the same (the full `lot-run` output of the 69 lots at 1800 ticks and
`autonomy N001_Lot11 --ticks 43200` diffed against the baseline, `ctest` and
`ctest -j8` green) and screenshots stay byte identical (`cmp` of
`--lot N001_Lot11 --vm --ui live --run-vm 600` and `--lot N002_Lot3 --vm --run-vm 900`).

## How to measure

- `s2pkg lot-run $(find <data>/TSData/Res/UserData/Neighborhoods -name '*_Lot*.package') --ticks 1800`
- `s2pkg autonomy <N001_Lot11.package> --ticks 43200`
- `/usr/bin/time -l` for wall time, user time, cycles and instructions retired. The
  machine is shared with other builds, so wall time moves a lot; compare **cycles
  elapsed** and **instructions retired**, or run when the load is low.
- `sample <pid> 10 1 -f out.txt` (or `sample <name> -wait`) for a profile: read the
  "Sort by top of stack" section, and the call graph for the callers.
- `OS2_PERF=1` in the app prints, on stdout, `opensims2: perf: <phase> <ms>` for each
  load phase (resource index, lot, model, objects, Sims, audio, VM) and, in the live
  window, the CPU time of a frame (without the wait for the drawable) every 120 frames;
  with `--screenshot ... --frames N` it prints the per-frame VM-step and capture times.

## Baseline (commit a806ade, before this round)

| What | Result |
| --- | --- |
| `lot-run`, 69 lots x 1800 ticks | ~21 s wall on a quiet machine (~33 s under load); 73.1 G instructions, 53.2 G cycles |
| `autonomy N001_Lot11 --ticks 43200` | ~14 s wall (~17.6 s under load); 49.0 G instructions, 34.6 G cycles |
| App, `--lot N001_Lot11 --vm --ui live --run-vm 600 --screenshot` | 5.1 s wall for the whole run; the load alone ~2.3 s: resource index 0.4-0.5 s, objects 0.45 s, Sims 0.9 s, VM 0.2 s |
| App live window | 10 ms per frame including the wait for the next drawable (60 Hz), worst 247 ms (the first frame) |

The profile was dominated by walks over the `std::map<ObjectId, Object>` (each node is
1.8 KB, with the fields the walks read spread over 4 cache lines): `containedIn`,
`objectsOnTile`, `setToNext`, `followContainers`, `handleTimers`, `tickMotives`,
`tickOverlayEvents`, `useCount`, plus `VM::object(id)` (a map lookup per variable
access), `tileFree` building a whole blocked grid to test one tile, and the path
planner rebuilding its quadtree (1 MB leaf map allocated and cleared per plan).

## Changes

Each row is a commit; the numbers are `lot-run` / `autonomy` on the machine at that
moment (wall, user seconds; the load varied from 2 to 19, so they only show the trend).

| Change | lot-run | autonomy |
| --- | --- | --- |
| Baseline | 21 s | 14 s |
| `VM::object(id)` through a direct id table (`byId_`, kept by `createObject`/`removeObject`), `ObjectDefinition::field` inline | 16 s | 10 s |
| `Object::position` and `timers` moved next to `data`, so the scans over all objects touch fewer cache lines | 10 s | 10 s |
| `tileFree` tests one tile instead of building the whole blocked grid; planner keeps its summed-area tables between plans | 11.0 s user | 6.0 s user |
| Planner's pooled leaf maps handed back all -1 by clearing only the quads | 8.4 s user | 6.2 s user |
| Per-tick loops, `Set to Next`, `containedIn`, `objectsOnTile`, `tileFree` scan the id table (`VM::objectTable()`, in id order like the map) instead of walking the map | 6.7 s user | 4.1 s user |
| `useCount` and `neighborSim` through the id table | 5.4 s user | 3.7 s user |
| Package indexes read in parallel (8 threads), 128 open package files instead of 32, CRC name hashes from tables (identical on 200k random strings) | same VM numbers | same |

Final comparison, same load for both binaries, one after the other:

| | baseline (a806ade) | now |
| --- | --- | --- |
| `lot-run` instructions / cycles | 73.1 G / 53.2 G | 59.8 G / 32.8 G |
| `autonomy` instructions / cycles | 49.0 G / 34.6 G | 41.9 G / 22.2 G |
| App run (`--run-vm 600 --screenshot`), wall | 5.1 s | 3.4 s |
| Resource index build | 0.40-0.51 s | 0.22 s |
| Live window, CPU per frame without the drawable wait | not measured (10 ms with it) | 4.1-4.5 ms |

## Round 9: loading

Scenes now load on a worker thread behind the loading screens (`docs/loading-mac.md`);
the main thread keeps drawing at 60 Hz and spends 1-3 ms on the scene's main-thread
part (`OS2_PERF=1` prints `scene loaded in <worker> ms, then <main> ms on the main
thread`). Before, the window froze for the whole load (its delegate was unset).

| What (N001 lot 11, `--hood N001 --hood-lot 11 --vm --ui live`) | before | after |
| --- | --- | --- |
| `loadSimParts` | 0.46-0.47 s | 0.03-0.06 s, and once per app run (shared by lot entries, outfit changes, `--character`) |
| Lot load ("placed in" line) | 1.97 s | 1.46-1.48 s |
| Whole headless run with `--run-vm 600 --screenshot` | 3.0-3.6 s wall | 2.5-2.7 s wall |
| Main thread blocked by a lot load in the window | the whole load (~2 s) | 1-3 ms |

`loadSimParts` read all 26,167 3IDRs of the game to keep the 6,828 that belong to a
GZPS: it now reads only those, on 8 threads, through `ResourceIndex::readConcurrent`
(positioned reads on a descriptor per package, so the threads and the index's own
`read` do not share a file position). The result is the same vector, checked element
by element against the old code. Screenshots stay byte identical (`cmp` of the lot run
above with and without `--loading-shot`, and before/after the change); no VM code
changed, so `lot-run`/`autonomy` were not rerun.

Left: `loadLotObjects` (0.25-0.5 s: about 40% model parsing, 55% texture decoding in
`Renderer::addObject`), the Sims' textures (70-100 ms each), Buy and Build mode setup
(85 ms each, both reading the catalog), `LotSimulation` (0.19 s).

## Round 11: loading

| What | before | after |
| --- | --- | --- |
| `Catalog::load` (Buy) + (Build), cold | 2 x 80 ms (two passes over the OBJDs) | 92 ms for both in one pass |
| Same, next scene or mode on the same index | 2 x 80 ms | 0.6 ms (the entries are kept; each caller gets a copy with its own thumbnail cache) |
| `BuildCatalog::load` (patterns, fences, paints, roofs) | 35 ms | 35 ms cold, 0.3 ms after |
| "Buy mode" + "Build mode" laps of N001 lot 11, first load | 77-80 + 85-87 ms | 78-92 + 42-53 ms |

The caches key on the index (address and the count of the resources of the type), so a
different `ResourceIndex` reads again. The first lot of a run still pays the cold read;
every later scene (lot entries after returning to the neighborhood) skips the 160 ms.
The screenshot of the lot run (`--hood N001 --hood-lot 11 --vm --ui live --run-vm 600`)
has the same pixels as before up to the run-to-run noise it already had (1 to 8 bytes
of 3.7 MB differ between two runs of the same build, before and after).

## Round 14

Baseline = main after the walking merge (locomotion, per-step Sim test), both binaries
built from the same tree one after the other. `lot-run` over the 69 lots (1800 ticks) and
`autonomy` of N001_Lot11 and N002_Lot3 (43200 ticks) give byte-identical output (`cmp`).

| What | before | after |
| --- | --- | --- |
| `lot-run` 69 lots, cycles / instructions / wall | 75.4 G / 233 G / 30.4 s | 27.5 G / 83 G / 9.3 s |
| `autonomy N001_Lot11 --ticks 43200`, cycles / instructions / wall | 21.8 G / 49.9 G / 8.5 s | 15.8 G / 43.5 G / 5.4 s |
| `autonomy N002_Lot3 --ticks 43200`, cycles | 21.4 G | 15.9 G |
| Headless capture per frame (`--run-vm 1430 --frames 12`, includes the offscreen render) | 15.0 ms | 12.8-13.8 ms (noisy) |

What was done, each a commit:

- **Wants library kept between VMs.** `libraryFor` held its cache with `weak_ptr`, so in
  `lot-run` each lot's VM dropped the library and the next lot reread and parsed its 493
  definitions and 211 trees: 55% of the whole run. It is held strongly now, keyed by where the
  definitions/trees/tuning live (package path, offset, size), not by the index's address. A
  faster `decode` (no entities, no copying char by char) cut the cold parse ~5%.
- **Quadtree cache of the path planner.** The planner records the obstacle/cost calls since
  `clearObstacles()`; the quadtrees (squares and leaf maps) are kept in a 4-entry LRU keyed by
  that exact call list (compared, not hashed) and the levels searched. In a 12-hour N001_Lot11
  run 449 of 684 plans hit. The A* and its noise generator are untouched.
- **Object loops through the ID table.** `VM::eachObject()` walks `byId_` in ID order with
  the map's semantics (an object made during the loop is met if its ID is later, a removed one
  is skipped) and replaces the walks over the map's 1.8 KB nodes in 45 loops (`flushDestinations`
  at every tick, `stepBlocker` at every walking step, `queue`, `timer`, `routing`, `reach`,
  `placement`, ...). This was the biggest part of the autonomy gain (21.8 G to 15.8 G).
- **Frustum culling in the renderer.** Static parts (not the skinned Sims) carry their
  model-space box; a part whose 8 corners are all outside one clip half-space of the pass is not
  drawn. The clip volume is convex and the map linear, so such a part draws no pixel. It applies
  to the mirror's reflected pass with its own matrix, which therefore draws less than before.
  The reflection pass was already skipped when no mirror is in view. Resolution was left alone.

Screenshots (`--lot N001_Lot11 --vm --ui live --run-vm 600`, `--lot N002_Lot3 --vm --run-vm 900`,
and 12 frames up to tick 1430): decoded pixels equal to the base build up to the run-to-run noise
the app already has (base vs base differs by 37-217 bytes of 3.7 MB; new vs base by 1-217); 22 of
24 frames of the 1-minute run are identical, the other two differ by 8 and 12 bytes.

`loadSimParts` was already done in round 9 (64 ms cold, 0 afterwards; see above). A lot load is
2.2-2.5 s: `loadLotObjects` 0.68 s (the four Sims' GPU upload 75-110 ms each), `LotSimulation`
0.62 s, Buy/Build mode setup 165 + 125 ms, resource index 80 ms (file reads).

Not done: an index for `containedIn`/`followContainers`. Fields are written directly
(`o.data[objdata::ContainerId] = ...`, `Context::set` by field number, saves) at dozens of
places, so a derived index cannot be kept exact without a setter everywhere; they now cost about
5% of an autonomy run each. The quadtree still misses when a walking Sim's cost rectangle moves
(the summed-area tables are rebuilt, ~6% of the run).

## Neighborhood hover and click (playtest hood-slow)

The hover popup's lot price (`hood::lotFileInfo`, which loads and decompresses the whole lot
package) was computed on every UI relayout, and the neighborhood UI is laid out again on each
mouse move (up to three times: the hover, the cursor and the frame). `HoodUi::lotPrice` caches
it per lot (cleared on `refresh()`). Per mouse move over N001 lot 11, headless
(`OS2_PERF=1 opensims2 --hood N001 --hover x,y ...`, which now also times one relayout per
step): `mouseMoved` 5.5-6.5 ms -> 0.7 ms, relayout 5.5-6 ms -> 0.75 ms.

## Round n28: the route planner kept between plans, object lists for the scans

Baseline = main at 8fad03dd (after round n18b). The rule as above, checked after every
commit: the full output of `lot-run` of the 69 lots (1800 ticks) and of
`autonomy N001_Lot11 --ticks 43200` byte identical to the baseline's (stdout, and stderr
but for `/usr/bin/time`'s lines); two more sensitive runs, also identical at every step: the
same `lot-run` with `OS2_ROUTE_FAILS=1` (every failed route, with its tick) and
`OS2_ROUTE_FAILS=1 autonomy N002_Lot3 --ticks 43200`; the two captures
(`--lot N001_Lot11 --vm --ui live --run-vm 600`, `--lot N002_Lot3 --vm --run-vm 900`) `cmp`
equal (two runs of the base build are equal to each other too); `ctest -j2` green. After
the rebase onto fbdc6c6f (round n29's one node limit, which changes none of these outputs),
onto e1392a26 (round n23's animation, which changes them all) and onto 3bff1964 (round n25's
fences), the same runs and captures of the rebased branch and of a build of that main are
identical too.

The profile before (autonomy of N001_Lot11, `sample`): Set to Next 24 (`nextVisibleObject`)
29%, because it called `objectsOnTile` on every tile of the Sim's view cone, each a walk over
the 600 objects; the planner 9%, 87% of it building the two summed-area tables; `containedIn`
2.4%; `objectsOnTile` from the walkers' per-step object test 2.2%.

Each commit's binary, one after the other on the shared machine (instructions retired /
cycles / user time; the load fell from 6.8 to 5.8 during the run, so the cycles favour the
later rows a little; instructions do not depend on it):

| Commit | `lot-run`, 69 lots x 1800 ticks | `autonomy N001_Lot11 --ticks 43200` |
| --- | --- | --- |
| Baseline (8fad03dd) | 98.0 G / 33.3 G / 16.2 s | 78.5 G / 29.3 G / 14.4 s |
| Planner kept between plans | 97.2 G / 32.7 G / 15.8 s | 74.2 G / 27.2 G / 13.3 s |
| Set to Next 24 in one walk | 90.7 G / 31.7 G / 15.0 s | 53.4 G / 20.0 G / 9.7 s |
| Containment index | 88.1 G / 28.4 G / 13.2 s | 50.7 G / 17.5 G / 7.2 s |
| Tiles round a point in one walk | 88.7 G / 26.6 G / 10.7 s | 49.5 G / 17.2 G / 7.0 s |
| Timers and destinations listed | 87.2 G / 26.3 G / 10.5 s | 48.5 G / 17.1 G / 6.8 s |
| Set to Next 1/4/18 from kept lists | 76.4 G / 24.2 G / 9.2 s | 42.1 G / 15.0 G / 5.8 s |

The last against the baseline again, in the other order (the last first): `lot-run` 76.3 G /
24.2 G against 97.8 G / 31.2 G; `autonomy N001_Lot11` 42.1 G / 14.9 G against 78.2 G / 26.1 G;
`autonomy N002_Lot3 --ticks 43200` 45.9 G / 16.5 G against 76.8 G / 25.6 G (instructions /
cycles). So about -46% instructions and -43% cycles for a day of a lot, -22% for the 69 lots
(whose 1800 ticks weigh less against their loading). The `lot-run` instruction counts move
by up to 1-2 G from one run to the next (the threads that read the package indexes).

Three times the `N002_Lot3` capture differed from the base by 1 to 4 levels in 650-750 pixels
of one small area, each time the first capture of a freshly built app (the last commit's and
two rebased branches'); run again with the same build it was byte identical, as were three runs
of the base build, so it is taken as the renderer's run-to-run noise that round 14 already saw.

What was done, each a commit:

- **The planner's grids and quadtree kept between plans.** `clearObstacles()` copied the
  static grids (a byte and a float per 1/8-tile cell, 1.3 MB a level) at every plan, and the
  quadtree's two summed-area tables (512x512 for a 64-tile lot) were rebuilt from the whole
  grid at every new set of obstacles, which is almost every plan: the walking Sims' cost
  rectangles move. Now the obstacle and cost calls are recorded as *stamps* (the cell
  rectangle each writes, with its bits or cost) and the grids are synced when read: the stamps
  that differ from the last plan's (a multiset difference past the common head and tail; mostly
  a walking Sim's three rectangles) are put back to their static cells and every stamp over
  them applied again. A cell is its static value OR'ed with the bits of, and maxed with the
  costs of, the stamps over it, in any order, so it is the cell a rebuild from scratch makes.
  The quadtree splits from a *pyramid* of blocked counts and cost sums per aligned square of
  2^k cells, updated only over the changed rectangles. Its sums are those of the tables bit for
  bit while every cost is 0 or in [0.5, 16]: every cell is then a multiple of 2^-24 and every
  sum under 2^23, so neither the tables nor the pyramid round (floors are held in [0.5, 10],
  the Sims' rectangles cost 5, 7.5 and 10); a planner with another cost goes to the tables as
  before. The split, the quads' order (their ids are the A*'s tie order) and the 4-entry cache
  of trees by exact call list are untouched. `test_routing` plans 120 rounds of moving Sims and
  objects with a kept planner, a new one each round and one sent to the tables, and checks
  that the cells and the plans are equal.
- **Set to Next 24 in one walk.** The cone's tiles (up to a few hundred) are marked on a map
  of their box and the objects walked once, by ID: one is in when it is in the world, not in a
  slot, on a marked tile of my level, and passes the same tests (not me, not dying, not type 5,
  my room, the line of sight to its tile). An object is on one tile, so the set is the one the
  per-tile walks made, whatever order the tiles came in.
- **Containment index.** Every write of object data 2 (container id) goes through
  `VM::setContainer` (object data 2 and 3) or is followed by `VM::noteContainer`: placement,
  pick up, Snap Into, Reach/Put, Find Location For, a walker leaving its slot, the kill queue's
  sitters, buy mode's preview, the tutorial's controllers, `s2pkg route`, a lot's saved data
  (`restoreState`, the placement fields in `lot.cpp`), the reset after an error, and SimAntics'
  own writes (`Context::set` of owners 0x03/0x04, field 2). The VM keeps the IDs that may be in
  a container, in ID order (a superset: one taken out stays until `followContainers` drops it
  at the next tick; `removeObject` drops it at once). `containedIn`, `followContainers`,
  `objectsOnTileWithContents` and `finishMove` walk that list instead of every object, in the
  same order. `OS2_CHECK_CONTAINED=1` checks at each use that every contained object is listed
  and aborts if not; the 69 lots, both autonomies and `ctest` ran clean with it.
- **The tiles round a point in one walk** (`VM::objectsAround`): the walkers' per-step object
  test (`footprintBlocker`, 3x3 tiles every step) and Set to Next 9 (the four sides) took
  `objectsOnTile` per tile. The test runs an object's "allow intersection" tree on the way,
  which could move something: once a tree has run (`VM::treeRuns`), the next tiles are taken
  again as before.
- **Timers and destinations listed.** `handleTimers` (every tick) walks the objects that may
  have timers (every timer added is followed by `noteTimers`; those left without any leave the
  list) and `flushDestinations` (every tick) the engine's destinations (`noteDestination` after
  `syntheticSlot`/`managedDestination` are set, also when a saved thread state brings them
  back), instead of every object; same order. `OS2_CHECK_CONTAINED=1` checks these lists too.
- **Set to Next 1/4/18 from kept lists.** People and the objects of a GUID were found by a
  walk over every object with a `std::function` test at every call (8.6% of an autonomy run
  by then); they now come from `VM::people()` (kept for the tick and the objects there are)
  and `VM::objectsOfGuid` (by GUID, in ID order, added to when `instantiatePart` sets an
  object's definition, which never changes after, and taken from by `removeObject`), then
  sorted by `appendOrder` as before. A GUID list rebuilt at each tick, or at each change of the
  object table, cost more than the walks it saved (+1 G instructions).
  `OS2_CHECK_CONTAINED=1` checks the GUID lists too.

## O tick sem reconsultar o mapa das threads (Windows, 21/10)

O `VM::tick` copiava os IDs das threads para um vetor e depois procurava cada uma
no `std::map` de novo (`threads.find`): uma busca por thread e por tick. No
autonomy do `N001_Lot11` isso dava ~15% do tempo.

O mapa é ordenado pelo ID e os nós de um `std::map` são estáveis, então o tick
passa a guardar os **iteradores** das threads (num vetor que é membro da VM, sem
alocar a cada tick) e usá-los direto: sem a busca no mapa. O único lugar que
apaga uma thread durante o tick é o `deleteGroup` (o objeto vendido ou
demolido): ele agora **mata** a thread (o status `Killed`, o mesmo que o
`VM::run` dá a uma thread cujo objeto sumiu) e a anota em `VM::deadThreads` (o
ID e o **serial** da thread: o ID vai para um objeto novo nesse meio tempo, e a
thread dele não é a que some), e o tick a apaga no fim — assim os nós do mapa
não somem enquanto o tick os usa. O comportamento é o de antes: as threads
criadas durante o tick só recebem a vez no tick seguinte, e a thread de um
objeto removido não roda.

Medido no Windows (esta máquina, `S2PKG_NO_LIGHT=1`, `autonomy N001_Lot11
--ticks 43200`, a saída comparada linha a linha):

| | tempo | saída |
| --- | --- | --- |
| antes | 22,3 s / 21,3 s | — |
| depois | 19,5 s / 18,2 s (a primeira, 26,7 s, é a do cache frio) | idêntica (as 516 linhas; só o ruído do shell difere) |

`ctest` 39/39.

**O que a mudança também conserta (e por que o sweep do AL muda em 3 árvores).**
O `deleteGroup` (o objeto vendido ou demolido) apagava a thread do objeto
**enquanto ela rodava**: o `std::map::erase` destrói o nó, e o `Thread&` que o
`VM::run` está usando fica pendurado — comportamento indefinido (na prática, a
thread continuava rodando com a memória liberada). Agora a thread é **morta**
(o status `Killed`, o mesmo que o `VM::run` dá a uma thread cujo objeto sumiu,
que é a regra do jogo) e apagada no fim do tick. Com isso o `bhav-sweep` do AL
muda em 3 das 46.880 árvores: 18.625 → 18.622 true, 14.126 → 14.129 falhas, e
as 4 "Slot number out of range" a mais são as árvores cuja thread foi morta
no lugar de continuar com a memória liberada. O `autonomy` do `N001_Lot11` (onde
não há venda) fica **idêntico** — as 516 linhas.

## Onde o tempo do autonomy vai (Windows, 21/10)

Medido com uma instrumentação temporária no `VM::tick` (revertida depois),
no `autonomy N001_Lot11 --ticks 43200` (`S2PKG_NO_LIGHT=1`): o tick inteiro
leva ~13 s dos ~22 s da corrida (o resto é a carga do lote e a preparação).

| parte do tick | tempo |observação |
|---|---|---|
| as threads (as árvores rodando) | **11,5 s** | 30.168.912 instruções → **381 ns por instrução** |
| `tickMotives`/`tickWants`/timers/hurry | 1,05 s | |
| `tickOverlayEvents` (a varredura de todos os objetos) | 0,58 s | |
| `notifyRoomChanges` (a varredura de todos os objetos) | 0,36 s | |

Duas conclusões: (1) o custo do tick é a **simulação** (85%), e o custo por
instrução é o do jogo (a VM do port roda na mesma ordem de grandeza do jogo
original); (2) as duas varreduras por tick que sobram — `notifyRoomChanges` e
`tickOverlayEvents` — somam **0,94 s, 4% da corrida**: fechá-las é ganho
pequeno, o índice por tile (dentro da execução das árvores) é que pesa.

O `bhav-sweep` do AL leva ~10 min para 12,9 M instruções — o custo dele é a
**carga** (cada árvore roda numa VM nova, com o índice de recursos frio), não a
simulação; por isso as comparações de fidelidade usam as contagens de
instruções e não o relógio.

## O índice por tile (22/10)

`objectsOnTile` era a varredura mais cara da simulação: no `N001_Lot11` com os
dados do AL, **35.799 chamadas x ~1.300 objetos = 47,1 M verificações** em
3.600 ticks (e o `objectsAround` mais 4,5 M). O `VM` agora tem um índice
`TilePos -> ObjectId` (`VM::tiles_`), construído na primeira consulta e mantido
pelo `VM::setPosition` — todas as escritas de `Object::position` do VM e do app
passam por ele: o `moveTo` do routing, o `placement`, os teleportes, os
contêineres (`followContainers`, `PlaceInSlot`) e o buy/build — e pelo
`removeObject`. A consulta devolve os IDs da tile **em ordem de ID**, como a
varredura; um passo que não troca de tile (a caminhada) não mexe nos baldes.

Medido no `autonomy N001_Lot11 --ticks 3600` com os dados do AL (os Sims
trabalhando: 24 a 37 buscas/escolhas por Sim) e `S2PKG_NO_LIGHT=1`:
**6,71 s → 4,25-4,68 s** (~35% menos), com a saída idêntica (as mesmas
buscas/escolhas e motivos). O `objectsAround` **continua varrendo**: o raio das
buscas de "o mais próximo" passa de 20 tiles, e andar (2r+1)² baldes custou 3x
mais que a varredura (19,4 s contra 6,7 s, medido e revertido).

`OS2_CHECK_TILE_INDEX=1` compara cada `objectsOnTile` com a varredura completa e
imprime a divergência: rodado no ctest inteiro (39/39) e no `autonomy`, zero
divergências. O `bhav-sweep` do AL não muda (14.025.434 / 19.927 / 10.335 — o número
daquele dia; depois das chamadas 54–84 o sweep vai a 14.093.824 / 23.864 / 6.385).

## A instalação nos comandos de lote (22/10)

O `autonomy` indexava só a pasta do EP dado (`ResourceIndex(resRoot(lot))`), enquanto o
app indexa a instalação inteira (`installationResRoots`, `src/app/win_main.cpp`). Com o
AL o lote rodava **sem os recursos do base e dos outros EPs**: 6.052 plays de
`a-heatstroke-slump-breathe` e `o-door-open-CCW` faltando, os laços de espera que
dependem da duração de uma animação girando até o orçamento de instruções (100.000) e o
Main dos Sims reiniciando — **116 resets** em 29.600 ticks no `N001_Lot11`, e **o ônibus
escolar parado na rua às 09:27** (27 peças). Agora o `autonomy` indexa os mesmos roots do
app: **1** animação faltando (2 plays), **91 resets**, os Sims em "Routing - Goto Head of
Bed" com **17/94/25 buscas** (antes 185/253/229) e **o ônibus vai embora** (0 peças às
09:27). O mesmo sintoma do live-qa da rodada 11 ("o veículo chega e nunca vai embora")
reaparecia no comando de teste por outro motivo (faltavam as animações), o que esconderia
uma regressão de verdade. Os comandos de lote que rodam a VM (`autonomy`, `pie`, `route`,
`wants`, `tutorial`) usam o mesmo `cli/s2pkg/data_index.hpp`; o `res` dado vai primeiro,
então um recurso que ele tem continua ganhando dos outros pacotes e os outros só enchem o
que falta. O custo fixo do comando é o `CodeLibrary` (~35 s tanto com um
root quanto com a instalação; as medições de tick comparam o total com o `--ticks 0`).

## A sala por tick sem varrer os objetos (23/10)

`notifyRoomChanges` varria **todos** os objetos comparando o dado 29 com o último avisado
(0,36 s de uma corrida de 43.200 ticks). Agora o `VM` guarda só os objetos cuja sala foi
escrita desde o último tick (`noteRoomChanged`): os escritores de sala do port (12
sítios), o `Context::set` (uma árvore escrevendo o dado 29 por dono 3/4), o
`createObject` e o `restoreState` do lote (objeto novo/carregado também é avisado uma
vez, como a varredura fazia). `OS2_CHECK_ROOM_DIRTY=1` roda a varredura antiga junto e
acusa um objeto não marcado: **0 divergências** no ctest inteiro (39/39) e no
`autonomy N001_Lot11 --ticks 29600` com a instalação, com a **saída idêntica** (17/94/25
buscas, 9/62/15 resets, os mesmos Mains).

## What is left (not done)

- Por tick: o `tickOverlayEvents` ainda varre todos os objetos. **Remedido em 23/10**
  (instrumentação temporária no `VM::tick` com `OS2_PERF_TICK=1`, já revertida;
  `autonomy N001_Lot11 --ticks 29600`): **0,235 s de 10,25 s de simulação** até os 20.000
  ticks (2,3%; ~0,35 s a 29.600 ticks), e ~0,235 s de 30 s de parede (0,8%). O índice teria
  de marcar os **seis sítios** que somam e apagam overlays (Animate Overlay/Object, o fim de
  cada loop, o `Stop`, a remoção do objeto e o save) e a passagem também mexe nas poses; o
  ganho (~0,8% do relógio) **não paga o risco** — fechado com este critério. A outra
  varredura, `notifyRoomChanges`, foi fechada em 23/10 (acima).
- Carga (do app, não do VM headless): as texturas dos Sims custam **~45 ms cada** para subir
  (0,9 s dos 2,3 s do load de um lote) e o `Catalog::load` relê **47 recursos** depois do lote
  (8% dos bytes lidos, 4,9 de 64 MB) — os números da rodada 11 seguem valendo (o catálogo já
  fica entre cenas; o que resta é o primeiro load a frio). **Fechado com este critério**: o
  custo é do app/render (o host sem janela não o paga), o ganho medido (~0,2 s no load a
  frio) não paga mexer no cache de texturas nem no índice, que é o caminho do render — e o
  `lua_State` do `0x7E` é criado no primeiro script, então o load do VM não mudou.

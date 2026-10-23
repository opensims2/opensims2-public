# Os scripts Lua do jogo (`ObjectScripts`): o formato, a API e o que eles governam (23/10)

O `0x7E` (Run Script) do jogo roda **scripts Lua** presos aos objetos (o RE do PC está no
`re-pc-ghidra.md` §(g)/(h), e o custo da recusa no `data-findings.md` §21). Este doc é o
inventário do que existe nos dados, a API que o port implementou e o que ela já move.

## Onde estão

- `Double Deluxe\EP2\TSData\Res\ObjectScripts\`: **37 arquivos soltos** — a Nightlife (EP2)
  é onde o sistema nasceu. São **25** `<nome>.objLua` (os scripts) e **12**
  `<nome>.globalObjLua` (os prelúdios compartilhados: `Global`, `Neighbor`,
  `NPC_Constants`, `ObjectData`, `ObjectDefinition`, `OutfitData`, `ConnectedColumns`,
  `PersonData`, `DrivewayGlobals`, `GarageGlobals`, …).
- Os EPs seguintes trazem `TSData\Res\ObjectScripts\ObjectScripts.package` (o AL:
  **496** recursos `0x9012468B` e **64** `0x9012468A`) com o TGI **hasheado pelo nome**
  (`nameInstanceHash`/`nameInstanceHiHash` do `resindex`): as versões que substituem as da
  Nightlife.
- **O nome do script**: o `0x7E` pega o id do script **1-based** no STR# 0x130 ("Lua
  Scripts", 7.233 entradas) e a entrada (id − 1) é o nome do arquivo, com o comentário
  depois de `" -- "`. O "NPC - Is Type?" do pool de NPCs usa o id 11 → a entrada `#10` =
  `NPC_IsType` → `NPC_IsType.objLua` (conferido nos dados).

## O formato: bytecode, não texto

Os arquivos começam com `1B 4C 75 61 50` — o chunk **pré-compilado do Lua 5.0**
(`ESC "Lua"`, versão 0x50 = 5.0; formato 1; `int` 4; `size_t` 4; `Instruction` 4;
`Number` 8 = double). Não dá para "ler o script" como texto: ou se porta um
**interpretador de Lua 5.0** (a 5.4 não lê bytecode 5.0) ou se traduz cada script à mão.

**O `size_t` de 32 bits (23/10).** O jogo foi compilado em 32 bits, então o header do chunk
declara `sizeof(size_t) = 4` e cada tamanho de string vem em 4 bytes. O build de 64 bits do
port tem `sizeof(size_t) = 8`, e o `lundump.c` do 5.0.3 recusava todo chunk do jogo com
"virtual machine mismatch … size of size_t is 4 but read 8". O `third_party/lua50` foi
**patcheado** (só isso): `LUA_SIZE_T_SIZE = 4` no `lundump.h`, `LoadSize`/`DumpSize` leem e
escrevem 4 bytes e o header espera 4. Assim os chunks do jogo carregam sem conversão, e um
dump deste build carrega num build de 32 bits (o `test_lua50` cobre o caminho dump → load).

## A API que os scripts chamam

Das constantes dos 627 chunks do AL (os 37 da Nightlife e os prelúdios dos EPs):

- **thread/frame**: `GetTreeParameter`, `SetTreeParameter`, `GetPrimitiveParameter`,
  `GetTemp`/`SetTemp`, `GetLocal`/`SetLocal`, `GetStackObjectId`, `GetRunningObjectId`,
  `GetRunningOnObjectId`, `SetScriptReturnValue`;
- **objetos/pessoas**: `GetPersonData`/`SetPersonData`, `GetNeighborPersonData`,
  `GetObjectData`/`SetObjectData`, `GetObjectAttribute`/`SetObjectAttribute`,
  `GetObjectSemiAttribute`/`SetObjectSemiAttribute`, `GetObjectDefinitionField`,
  `GetObjectIdInSlot`, `GetNumObjectArrays`, `GetObjectArrayCopy`, `SetObjectArray`,
  `GetConstant` (as tabelas de constantes ainda sem uso medido);
- **bits**: `BitwiseAnd`, `BitwiseOr`, `BitwiseXOr`, `BitShiftLeft`, `BitShiftRight`,
  `SetBit`, `ClearBit`, `IsBitSet`;
- **objetos do motor**: `CachedNeighbor` (`new`/`getGUID`/`getPersonData`/`getOutfitProperty`),
  `CachedPerson`, `ObjectManager` (`new`, `getPersonIdFromGUID`, `getObjectIdFromGUID`,
  `getObjectGUIDFromObjectId`, `getObjectIds`, `getNeighborIds`, `getPersonIds`,
  `verifyObjectId`, `verifyNeighborId`), `Neighborhood`, `Inventory`, `nWorld`, `nGameState`;
- **tabelas de nomes**: `PersonData`, `ObjData`, `ObjDef`, `OutfitData`/`OutfitType`/…,
  `NPCTypeToGUID`, `NPCTypeToOutfitGUID`, `Aspiration`, `Scope` — **são prelúdios**, não
  API: os `.globalObjLua` as definem em Lua por cima da API.

**O que é prelúdio e o que é do motor (23/10).** `CachedNeighbor` e `CachedPerson` **não**
estão em nenhum `.globalObjLua` (as strings dos 37 arquivos só os mostram nos scripts): são
**globais do motor**, como `GlobalObjManager`/`ObjectManager`/`nWorld`/`nGameState`. O
binding fornece: `CachedNeighbor` (`.new(nID)` → `:getGUID()`, `:getPersonData(field)`) —
o `nID` é o vizinho da vizinhança; `ObjectManager` (`:getPersonIdFromGUID(guia)` = o `nID`
do Sim vivo com esse GUID, 0 se não está no lote) e as funções puras. Os prelúdios
(`Global`, `Neighbor`, `PersonData`, `ObjDef`, `OutfitData`, …) definem só as
tabelas/classes em cima disso.

**O ponto de virada do `getPersonIdFromGUID` (FUN_00904590).** O engine percorre os objetos
vivos com aquele GUID, fica com o que é pessoa e devolve o **nID** (vfunc 0xBC); sem
nenhum, 0. O script então usa `CachedPerson.new(nID)` (o Sim do lote) ou cai no
`CachedNeighbor` (o vizinho, fora do lote). O port faz o mesmo com
`VM::objectsOfGuid` + `o->person->data[persondata::NeighborId]`.

## O `0x7E` e o binding (23/10)

`src/vm/lua_scripts.cpp` (`LuaRuntime`, um `lua_State` por VM):

- abre a biblioteca padrão que os scripts usam (`base`, `table`, `string`, `math` — o `#`
  não existe no 5.0) e carrega os **627 prelúdios** do índice da instalação
  (`CodeLibrary::luaPreludes()`, `Global` primeiro e depois por nome);
- registra a API acima (cada função é a do engine: `FUN_0090eeb0`, `FUN_00911470`,
  `FUN_0090e1c0`, `FUN_0090faa0`, `FUN_00911180`); o contexto corrente é o do `0x7E` que está
  rodando (`frame.params`, `thread.temps`, `frame.locals`, `frame.stackObject`);
- roda o chunk do nome (`luaL_loadbuffer` + `lua_pcall`) e devolve o byte que
  `SetScriptReturnValue` gravou (o `0x7E` vira `Exit::True`/`False`);
- um erro de script (ou de um prelúdio, ou de um pacote malformado lido pela API) **não
  derruba a thread**: `logOnce` e `false`, como a recusa anterior. Nenhuma função da API
  pode lançar através dos frames C do Lua (a primeira versão lançava em
  `GetPrimitiveParameter` sem stack object e o `bhav-sweep` morria com 0xC0000409).

O `0x7E` em `primitives.cpp`: bytes 0-1 = o STR# (0x130), bytes 2-3 = o id (1-based); o
nome é a entrada (id − 1) até o `" -- "`. Os três pares do operando (bytes 6/9/12) são só o
log do jogo (`GetPrimitiveParameter`), não os parâmetros do script: `GetTreeParameter` lê os
parâmetros da **árvore que chamou** o `0x7E`. O `0x7D` (Follow Sim) continua `false` (1 uso
no AL e o array ainda sem semântica medida).

## O que mudou de comportamento (medido, 23/10)

- **O pool de NPCs do AL funciona.** Antes, "NPC - Is Type?" (`0x7FD46CD0:0x3AD`) devolvia
  sempre false: o array do pool ficava vazio, o `Snap Into` do motorista recebia o stack
  object 0 e o carro da empregada resetava **30 vezes às 07:16**, e a empregada nunca
  chegava (`soak-report.md` round 20). Agora o `NPC_IsType` recebe os `nID`s do
  `Set to Next 17` (os vizinhos de família 0), responde **true para 3 dos 333 candidatos**
  do `N001_Lot68` e a **Kaylynn (#452) chega** (400 ticks: `Interaction - Get Out #3`, no
  lote às 07:22) **sem nenhum reset** e sem nenhum erro de SimAntics.
- **O `bhav-sweep` do AL** vai de **14.093.824 / 23.864 true / 6.385 falhas** para
  **14.509.577 / 23.949 true / 6.381 falhas** (+415.753 instruções: os scripts agora rodam;
  +85 true, −4 falhas) e **nenhuma das falhas que restam é de Lua**. O do **jogo base fica
  igual** (7.234.312 / 11.107 / 2.461): o base não tem `ObjectScripts`, então o `0x7E`
  continua false.
- Os sistemas da Nightlife (atração, pôquer), a garagem e a ordenação de mesas passam a
  rodar; os que ainda falham pedem os globais que faltam (ver abaixo).

## O que ainda falta (por script, do `bhav-sweep` do AL)

Depois da rodada de 23/10 o `bhav-sweep` do AL **não tem nenhuma falha de Lua** (as 6.381
falhas restantes são do VM/dados, como antes). O que ficou como stub, e por quê:

- **`Inventory`/`InventoryToken`**: as classes e as **constantes** (`kType*`, `kFlag*`) estão
  registradas (o prelúdio "Tokens" as lê), mas os métodos sobre o modelo de inventário do
  port (`VM::inventory`: `addToken`, `getTokensByGUID`, `countTokens…`) ainda **não** têm a
  semântica medida — cada um loga e responde 0 pelo fallback da classe. É o que falta para
  os scripts de negócio/loja (ShoppingSetValue…), que hoje degradam como se o token não
  existisse.
- **`Neighborhood`**: `isValidLot`, `getLotType`, `getLotName` e `getLotIds` são reais (vêm de
  `VM::lots`); os de apartamento (`isRoommate`, `isApartmentResident`, `getRoommateId`,
  os `getApartmentLinked…`) são stub — o port não modela apartamento/roommate ainda.
- **`nDataTable`/`ObjectDataTable`/`NeighborDataTable`**: registrados; o port não guarda as
  tabelas genéricas, então `has*` é false, `getTableValue` é nil e as listas vêm vazias (é o
  que o jogo devolve para uma tabela ausente; o `ShoppingGetPrice` testa exatamente isso).
- **`nGameState`**: `IsEPInstalled`/`GetInstalledEps` (máscara 0x3FF = a Ultimate Collection,
  base + 8 EPs + M&G; **inferido** da coleção instalada), `GetCurrentState` 0 e
  `GetProperty` stub.
- **`GetConstant`/`GetNumConstants`** = os BCON do stack object (privado, semi-global, global);
  **`Get*WithGuid`** = o BCON do grupo do OBJD com aquele GUID. O `GetDefinitionField` 204,
  205 e 210 são inferidos dos nomes do `ObjDef` (o 203, o do NPC, é medido).
- **`GetNeighborRelationship`/`SetNeighborRelationship`** sobre `VM::relationships` (o par do
  Relation), **`GetSimulatorGlobal`/`SetSimulatorGlobal`** sobre `VM::globals`,
  `CreateObjectGUID`/`SplitObjectGUID`/`RoundFloat` e `math.round` (o 5.0 não tem `round`,
  e o jogo tem: `ShoppingSetValueByPercent` o usa).
- **`CachedNeighbor:getOutfitProperty`** e o `GetUserDirectory` do prelúdio "LuaLoader" são
  stub (o port não carrega mods nem guarda as propriedades de cada parte do outfit).
- O **`0x7D`** (Follow Sim) segue false (1 uso no AL, o array sem semântica medida).

## O `s2pkg lua` e o `lua-script` (23/10)

`src/formats/lua50.cpp` + `cli/s2pkg/script_cmds.cpp`: o leitor do chunk (o formato do
`lundump.c`/`lopcodes.h` do 5.0.3, com os tamanhos que o header declara: int 4, size_t 4,
Instruction 4, Number 8) e o desmonte de cada instrução com a linha e os constantes
(RK ≥ 250 = `Kst`). `s2pkg lua <file.objLua>` no `NPC_IsType.objLua` (85 instruções, debug
já removido: 0 linhas/locais/upvalues) mostra a lógica do pool:

- `npc = GetTreeParameter(0)`, `tipo = GetTreeParameter(1)`;
- `CachedNeighbor.new(npc)` e `GlobalObjManager:getPersonIdFromGUID(pessoa:getGUID())`;
- com pessoa: `GetPersonData(id, "Person Age") <= 0` → false; `GetPersonData(id, "NPC
  Type") ~= tipo` → false; a família 32767 (sem família) ou ≠ 0 → false; senão
  `SetScriptReturnValue(true)`;
- sem pessoa (id 0): o mesmo pelo `pessoa:getPersonData` e
  `GetObjectDefinitionField(guia, 203) == NPCTypeToGUID[tipo]`.

`s2pkg lua-script <Res-dir> <nome>` acha o script no `ObjectScripts.package` do EP pelo
nome e desmonta (o `NPC_IsType` do AL = 731 bytes, igual ao arquivo solto da Nightlife).
`s2pkg lua-prelude <Res-dir> <nome|*> [texto]` desmonta um `.globalObjLua` (ou todos, com
`*`, filtrando pelo texto) — foi o que mostrou que o `ObjectDataTable`/`NeighborDataTable`
são classes do motor, não prelúdios.

## O recurso empacotado (23/10)

As versões dos EPs não são o chunk puro: `0x9012468B` (os `.objLua`) e `0x9012468A` (os
`.globalObjLua`) trazem `u32 0 + u32 tamanho + o nome` e **o chunk em seguida** (ex.:
`NPC_IsType` + `LuaP...`). O TGI é o hash do nome, mas **não** é o `nameInstanceHash` do
port (`NPC_IsType` = `FF9573E2` no pacote e `FFB38405` na função do port) — por isso o
`CodeLibrary` varre os dois tipos uma vez e monta o mapa nome → chunk
(`CodeLibrary::luaScript`/`scanLuaScripts`).

## O estado (fim de 23/10)

| peça | estado |
|---|---|
| VM (Lua 5.0.3, MIT) | vendorizada, com o `size_t` de 32 bits do jogo (`test_lua50`) |
| leitor/desmontador | `s2pkg lua` e `s2pkg lua-script` |
| prelúdios | 627 chunks carregados por VM; **0 falham** (as classes do motor que eles leem estão registradas) |
| API | as ~45 funções; a cauda (Inventory, apartamento, nDataTable) tem os métodos como stub logado |
| `0x7E` | roda o script e devolve o `SetScriptReturnValue` |
| NPC pool | **medido**: 3/333 true, a empregada chega, 0 resets |
| `bhav-sweep` AL | 14.509.577 / 23.949 true / 6.381 falhas, **0 falhas de Lua** |

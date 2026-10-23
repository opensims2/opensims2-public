# RE do exe PC (`Sims2EP9RPC.exe`) — infra Ghidra + achados

Noite de 16→17/10/2026. Alvo: **o exe que o jogo roda de verdade** — `Sims2EP9RPC.exe`
(46.208.224 B, sha256 `CC4BB4E1909E27422FC191501274FD222C933D18B6573CFA7F95E170FD9F4722`),
o exe do launcher da comunidade **Sims 2 RPC** (v1.15.1; `RPCFilesystem.txt` o lista e o
changelog cita o `CreateProcess` dele). O `Sims2EP9.exe` "puro" (17 MB) está patchado na
pasta local e não é o que o launcher executa. Os dois exes **não têm símbolos**.

Ver também: `docs/goal-ghidra-pc.md` (goal/log da noite), `docs/buy-build-mac.md` (o lado Mac).

## Infra

- **Projeto Ghidra persistente:** `C:\Users\Artur\sims2-re\pcb` (programa `Sims2EP9RPC.exe`).
  Fica fora do workspace porque o Ghidra **rejeita caminho com elemento começando por ponto**
  (`.gemini`) e o workspace vive sob ele. (O projeto `pc` da primeira tentativa da noite ficou
  inútil: o run pendurou sem salvar — ver log do goal.)
- **Env p/ todo run headless** (senão o default é 8G heap e GC capado a 2 threads → GC thrash):
  ```powershell
  $env:GHIDRA_HEADLESS_MAXMEM = '12G'
  $env:GHIDRA_HEADLESS_JAVA_OPTIONS = '-XX:ParallelGCThreads=6'
  $env:JAVA_HOME = 'C:\Program Files\Microsoft\jdk-21.0.12.101-hotspot'
  ```
  (`GHIDRA_HEADLESS_MAXMEM`, não `MAXMEM` — esse só vale via `launch.bat` direto.)
- **Import+análise one-time** (em `C:\Users\Artur\sims2-re\pcb-run.cmd`; lançado destacado via WMI):
  `analyzeHeadless <proj> rpc -import <exe> -scriptPath <scripts>
  -preScript OffAnalyzers.java "PDB Universal" "Decompiler Parameter ID"
  "Decompiler Switch Analysis" "Call Convention ID"
  -analysisTimeoutPerFile 3600 -postScript DumpInventory.java <out> -postScript MemoryInfo.java`
  - `OffAnalyzers` zera o flag do analyzer em `program.getOptions("Analyzers")` (chave = nome)
    e chama `AutoAnalysisManager.initializeOptions()` p/ sincronizar o scheduler (mecanismo
    achado via bytecode; `registerAnalyzerOptions()` sozinho não basta).
  - Por que esses 4: o **PDB Universal** mastigava o debug embutido por ~28 min só de
    "Unprocessed TypeDescriptor" (sem símbolos aproveitáveis — o exe não tem PDB externo); os
    3 de **decompiler** são os suspeitos do loop que pendurou o run da noite.
  - `-analysisTimeoutPerFile` é rede de segurança; **no timeout o programa é salvo parcial**
    (comprovado no s2pkg) — consultas funcionam no que já foi analisado.
- **Consultas (segundos, readonly)** — `FindString` lê o regex de um **arquivo** (1 padrão por
  linha, `#` comenta; alternância/`^$` livres) porque `|`/`^` no argumento quebram o cmd quando
  se passa pelo `.bat`:
  `analyzeHeadless <proj> rpc -process Sims2EP9RPC.exe -noanalysis -readOnly -scriptPath <scripts>
  -postScript FindString.java <out.tsv> <patternFile> [maxHits]`
  (+ `XrefsAt.java <va>`, `DecompAt.java <va> <out.c>`). Lote pronto: `rea-snapshots/ghidra-pc/run-queries.ps1`
  (4 lotes: catalog/bins/paths/ui → `pcb\out\refs-*.tsv`).
- Scripts em `rea-snapshots/ghidra-pc/scripts/`; dump de strings cru em
  `rea-snapshots/ghidra-pc/strings.py` + `out/strings-ascii.txt` (offsets de **arquivo**).
  Pitfall de compilação dos scripts: `Data` é `ghidra.program.model.listing` (não `model.data`).
- **Conserto de regiões "cegas"** (código alcançado só por ponteiro/vtable, que a análise não
  disassembla): `DisasmFix.java <va> <len> [cap]` (varredura linear via
  `DisassembleCommand(set,set,true)`), `FnAt.java <va>…` (cria função), `RedoFns.java <va>…`
  (remove+recria p/ corpo novo), `NoRetOff.java <va>` (limpa a flag `setNoReturn`),
  `WhoRefs.java <ini> <fim>` (quem aponta pra região — slots de vtable).
- **Padrão MSVC** das funções grandes do jogo: `MOV EAX,<handler em .text>; CALL 0xE5C86C`
  (prólogo SEH compartilhado — `0xE5C86C` = `ExceptionList = local; return`, **retorna**,
  apesar do decompiler dizer "does not return") seguido de `SUB ESP,imm; PUSH regs…`.
  O flow de criação de função para no CALL → criar a função **no `SUB ESP`** se necessário
  (perde só o prólogo). Os "stubs" de 10 B espalhados pelo exe são esses prólogos.
- Regiões consertadas nesta sessão: `0xBDF100-0xBE0A77` (módulo de filesystem/segmentos).
  Funções criadas: `FUN_00be0787` (BuildResourceMaps), `FUN_00be0141`, `FUN_00bdfa42`,
  `FUN_00bdf275` (CreateGameDirectoryFileSegment); `FUN_00bdf753` (switch de categoria do
  ResourceManager de áudio); `FUN_00e5c86c`/`0xCF5A70` decompilados (ver seções).

## Âncoras de strings (offset de arquivo; VA a registrar via Ghidra)

| offset | string | uso |
|---|---|---|
| 0xE4AE10 | `TSCatalog` | catálogo (única) |
| 0xE4B498..0xE4B718 | `catalogTextResource`, `catalogTextIndex/Indices`, `buildTipIndex/Indices`, `notInCatalog`, `subsort`, `thumbnailResource`, `deprecated`, `Failed to set/parse catalog text` | parser de props do catálogo |
| 0xE680A8 | `showincatalog` (+ `crapscore`, `nicenessmultiplier`, `routingcost`, `surfacetype`…) | mesmo parser (props de objeto) |
| 0xE358AC..0xE358CC | `FunctionSubsort`, `FunctionFlags`, `RoomFlags`, `CommunitySort`, `Price` | idem (campo 94 = FunctionSubsort) |
| 0xE62535..0xE625D8 | `Bin Prod ID/Type/Sort Idx/Key/Name`, `Frag Prod ID/Sort Idx`, `Obj/Bin/Frag Key:` | debug de bins/fragmentos do catálogo |
| 0xE62602 | `@CcTSDataTables` | tipo embutido (debug) |
| 0xE51638 / 0xE51644 / 0xE516F0 | `/tsdata/`, `tsdata/`, `/tsdata/res/locale/` | normalização de caminhos do res |
| 0xE5173C | `Starting BuildResourceMaps.` | pipeline do índice de recursos |
| 0xE51530 | `CreateGameDirectoryFileSegment : …` | segmentos de diretório (DBSegments) |
| 0xEDA000..0xEDA4E4 | tabela `Catalog/`, `Text/`, `Bins/`, `Downloads/`… + `Directories` | tabela de diretórios do jogo |
| 0xEAA868..0xEAA950 | `AllUsersData/`, `NeighborhoodTemplate/`, `LotCatalog/`, `Collections/`, `Neighborhoods/`, `UserData/` | pastas do user data |
| 0xEAA934 | `Neighborhoods/` | enumeração de bairros |
| 0xED9F4C | `BuildModeThumbnails.package` | pacote de thumbnails do buy/build |
| 0xDF4080 | `UIPieMenu` (+ `kPieMenu*`) | pie menu (contexto) |
| 0xE50DF8 | `Hitlab.ini` | ini base das configs de segmento |
| 0xE50E04..0xE50E80 | `MusicDirectories`, `ResourceFactories`, `DBSegmentDirectoryFiles`, `TrackDefSegments`, `SampleDataSegments`, `PackedDBSegments`, `DBSegments` | seções ini do índice de recursos |
| 0xE50E8C | `%DataDirectory%` | macro do diretório base |
| 0xE51234 | `.\source\TSAudioResourceManager.cpp` | TU de origem dos logs |
| 0xE53F00 | `TSAudio` | módulo de log |

## (a) Catálogo / bins

**Tabela de descritores de props (metadados de reflexão) — VA `0x124B470`.**
Cada registro = **10 ponteiros (40 B) + nome ASCIIZ inline (padded a 4)**. Layout observado:
`{p0=type, p1=?, p2=?, p3=?, p4=setter, p5=getter-do-nome, p6..p9=handlers genéricos}`.
Os 9 registros do objeto de catálogo, na ordem, com getter (`p5`) e setter (`p4`):

| prop | getter-nome (p5) | setter (p4) |
|---|---|---|
| catalogTextResource | 0xBC554E | 0xBC5F02 |
| catalogTextIndex | 0xBC5554 | 0xBC5F63 |
| catalogTextIndices | 0xBC555A | 0xBC6031 |
| buildTipIndex | 0xBC5560 | 0xBC6117 |
| buildTipIndices | 0xBC5566 | 0xBC6173 |
| notInCatalog | 0xBC556C | 0xBC5572 (stub) |
| subsort | 0xBC55D6 | 0xBC55DC (stub) |
| thumbnailResource | 0xBC5650 | 0xBC5656 (stub) |
| deprecated | 0xBC5713 | 0xBC5719 (stub) |

- Getters são `MOV EAX,<ptr do nome>; RET` (daí `DATA from 0x124B4xx` nas xrefs).
- "Stub" = `MOV EAX,<ptr>; CALL 0xE5C86C` (ver Infra: é o prólogo SEH compartilhado, **retorna**)
  ⇒ `notInCatalog`, `subsort`, `thumbnailResource`, `deprecated` são **read-only** (sem setter real).
- p0/p1/p2/p3/p6/p7/p8/p9 compartilhados: `0xCFAC60, 0x85EF10, 0xCFACA0, 0xBCD20F, 0x68F2B0,
  0x68CDF0, 0x1094750, 0xBCD8B1` (handlers genéricos tipo/compare/serialize); a próxima tabela
  (outro tipo) começa em `0x124B698`.

**Texto do catálogo = recursos STR#.** `FUN_00bc57a0` monta um registro `{tipo='STR#' (0x53545223),
hash 0xFB4CB0DE}` e registra via singleton `FUN_00cd42b0` (vtable `+0x28` por nome → `+0x14`
registra); guarda o índice em `this+0x154`. `FUN_00bc5787` = "get get-count/index" (`+0x18`/`+0x3C`).

**Serializer do Exchange (metadados de objeto de compra).** `FUN_009e7060`, tabela de slots em
`.data` `0x140FE60..0x140FED0` (Name/Description/Price/FunctionFlags/FunctionSubsort/RoomFlags/
CommunitySort/Thumbnails/PackageFile/SKU…), escrita por `FUN_009e1370(campo, valor)`. Números de
campo (batem com o formato do port):

| campo | nº |
|---|---|
| Price | 0x12 (18) |
| RoomFlags | 0x27 (39) |
| FunctionFlags | 0x28 (40) |
| **FunctionSubsort** | **0x5E (94)** |
| CommunitySort | 0x64 (100) |

⇒ **Residual do port (53×47) fechado: campo 94 do engine = `FunctionSubsort`** — o campo 94 do
port está certo.

**Parser read/write dos objetos de build (.package).**
- write: `FUN_00d2b0f0`/`FUN_00d2b760` (tipos `floor/wall/roof/fence/canh/terrainPaint`);
- read: `FUN_00d2d1f0`/`FUN_00d2d750` → `deprecated`→`FUN_00d29180` (+0x30),
  `showincatalog`→`FUN_00d29190` (+0x34), categoria→`FUN_00ab9ed0` (+0x8),
  `nicenessmultiplier`→`FUN_00ce1940` (+0x38), `crapscore`→`FUN_00883ab0` (+0x3C);
  strings em +0xC/+0x18/+0x64/+0x7C/+0x88 (`material/subsort/surfacetype/routingcost/brightness/filename`).

**Bins**: os padrões `Bin Prod ID/Type/Sort Idx/Key/Name`, `Frag Prod ID/Sort Idx`
(offsets 0xE62535+) são usados pela **`FUN_00cdf160`** (entrada `0xCDF160`, SEH, criada 17/10 —
era a "região `0xCDF41B`+" sem função; os `PUSH` dos formatos ficam em 0xCDF2F4/0xCDF41A): é o
**dump de Frag/Bin** — recebe a fonte dos dados (vtbl `+0xC/+0x4C/+0x54/+0x64/+0x6C`) e um sink
de impressão (vtbl `+0x20/+0x4C`, uma linha por `[vtbl+0x4C](&string)`):
- cabeçalho (`Frag Key:`) + buffers de 0xB bytes via `FUN_00ec685a`;
- se a propriedade `0x6C4F3599` existe em A (`[vtbl+0x14]`): pega a **STR# do catálogo**
  (hash `0xFB4CB0DE`, `[vtbl+0x40]`) e resolve o texto (`[vtbl+0x1C]`) → `\nBin Name = %s`;
  senão `\nBin Key = 0x%08X 0x%08X 0x%08X` (a chave de 12 bytes);
- `\nFrag Prod ID = %d\nFrag Sort Idx = %d` e depois `\nBin Prod ID = %d\nBin Type = %s\n
  Bin Sort Idx = %d`, com `FUN_00cd42b0`/`FUN_00cdedd0` e `[vtbl+0x30/+0x50/+0x58]`;
- formatação por `FUN_0040cd66` (sprintf-like) / `FUN_0040bc29` (concat).
⇒ fechou a última parte em aberto do goal (a).

## (b) Varredura de `TSData\Res`

**Tabela de segmentos de diretório** — `FUN_01032f20` (o "DBSegments" do jogo): registra cada
diretório/arquivo com ID+parentID (`Neighborhoods/`, `Collections/`, `Downloads/`
(+`Patterns/`,`Skins/`,`Objects/`), `ContentRegistry`, `LotCatalog/`, `UserData/`, `Res/`, `Sys/`,
`UI/`, `3D/`, `BuildModeThumbnails.package` (+DesignMode/CAS/Object/CANHObjects),
`TS2DownloadsDir`/`TS2Downloads`). É a base da resolução multi-pack do port.

**Cadeia de startup** (entradas descobertas nesta sessão; a região `0xBDF100-0xBE0A77` estava
"cega" — disassemblada e com funções criadas, ver Infra):

`FUN_00be0787` = **BuildResourceMaps** (orquestrador):
1. log `Starting BuildResourceMaps.` (0x125173C; módulo de log `TSAudio` 0x1253F00; TU de origem
   `.\\source\\TSAudioResourceManager.cpp` 0x1251234);
2. registra as classes de segmento (`FUN_00bdd89f` ×3, `FUN_00bdd613`) e ids fixos;
3. itera a lista `[vtbl+0x8C](0xADD550A7)` → para cada entrada chama **`FUN_00bdfa42`** (packed segments);
4. itera a lista `FUN_00cd2990(0xEC43275C, …)` → para cada raiz: `FUN_0040ddfd("TSAudio",7,1)`,
   checa `Hitlab.ini` (`FUN_0040cd43`), monta o path e chama **`FUN_00be0141`** (directory files);
5. fecha logando `Audio resource initialization took %d milliseconds.`

`FUN_00bdf275` = **CreateGameDirectoryFileSegment** (prólogo SEH em `0xBDF26B`):
- logs `CreateGameDirectoryFileSegment : Skipping creation of non-existent/empty directory file
  segment %s` (linhas 0x573/0x57A) e `Failed to create directory file segment object.` (0x58E);
- cria o objeto pela fábrica `[vtbl+0xC](0xA079CE1B, 0x856C8CF7, &out)`, seta flags
  (`+0x70`, `+0x74`, `+0x3C`, `+0x40`) e registra os padrões de arquivo;
- parseia a **lista de extensões separada por `,`** (`FUN_0040e487(',')`, padrões tipo `*.<ext>`)
  e registra cada um (`[vtbl+0xC]`, opcional `[vtbl+0xC4]`);
- inicializa/abre (`+0xC`, `+0x14(1,0)`), loga `Failed to initialize/open directory file segment
  at %s` (0x5C6/0x5CC); callback final opcional ([EBP+0x14]/[EBP+0x18]).

`FUN_00be0141` = **registrador dos segmentos de diretório da instalação** (prólogo em `0xBE0137`):
- constrói `"tsdata/"` (0x1251644) e `"/tsdata/res/locale/"` (0x12516F0) sobre a raiz;
  `FUN_0040d883("tsdata/")` = **`std::string::operator+=(const char*)`** (strlen + `FUN_00405f2a`
  = append(first,last): cresce com `FUN_0040e923`, preserva o conteúdo antigo e copia o 1º char
  por último p/ self-append) — o montador de path que anexa cada fragmento;
- lê as seções ini **`DBSegments`**, **`DBSegmentDirectoryFiles`**, `SampleDataSegments`,
  `TrackDefSegments` (×2), `MusicDirectories` (ponteiros em `0x142C7DC..0x142C7F8`; handler de
  linha das duas primeiras = `0xBDF83B`, logo após `FUN_00bdf753`);
- expande o token **`%Language%\\`** com o idioma corrente (`FUN_0040E0D6`/`_strlen`/
  `FUN_0040D2A1`/`FUN_0040E0AB`);
- por entrada chama `FUN_00bdf26b(...)` = CreateGameDirectoryFileSegment (tipo `0x656B8F0C`);
  logs `Segment at %s has no extensions defined in section %s` (0x3B0) / `Failed to create
  segment at %s` (0x3D9).

`FUN_00bdfa42` = **registrador dos packed segments** (prólogo em `0xBDFA38`; chamado pelo passo 3):
- o .ini pode vir **como recurso** (log `Failed to find segment containing .ini resource
  %08x:%08x:%08x%08x`, linha 0x325);
- mesmos paths `"tsdata/"`/`"/tsdata/"`; seções **`PackedDBSegments`**, `SampleDataSegments`,
  `TrackDefSegments` (×2), `MusicDirectories`;
- cria os segmentos via `[vtbl+0x58](0x656B8F0C, &out, 2 /*packed*/, nome,
  FUN_00c02519(1,1))`; log `Failed to create segment for %s` (0x377).

**Chaves/macros**: `%DataDirectory%` (0x1250E8C), `%Language%\`, ini `Hitlab.ini` (0x1250DF8);
`,` = separador de extensões, `;` = comentário ini, `:` = separador. Handlers de linha
(decompilados 17/10; `0xBDF83B`/`FUN_00bde67d` têm prólogo MSVC SEH `MOV EAX,<__ehhandler>;
CALL __SEH_prolog`):
- `FUN_00e5c86c` = **`__SEH_prolog`** (EAX = handler; monta EXCEPTION_REGISTRATION, `FS:[0]=ESP`,
  EBP = framebase; volta com `PUSH EAX/RET`);
- `0xBDF83B` (DBSegments/DBSegmentDirectoryFiles; corpo = `FUN_00bdf845`, 302 B): linha
  `begin_group`/`end_group` → ignora; senão hash do id (`[vtbl+0x14]` → `FUN_00410720`) e
  insere no mapa do 3º param (`FUN_00415585`); id duplicado → log nível 2
  `Ini file section has duplicate id %08x = %s\n` (`TSAudioResourceManager.cpp:0x477`).
  O registro do callback fica em `FUN_00bdf973` (`[vtbl+0x34](…, 0xBDF83B, 0x142C7F4)` + caso
  especial do hash `0x5B945038`), usado por `FUN_00be0141`/`FUN_00bdfa42`;
- `FUN_00bdd8f2` (SampleData/TrackDef): se 3º arg ≠ 0 → hash (`FUN_00410720`) do id em `this+4`
  → `FUN_00bdd887(&hash)`;
- `FUN_00bde67d` (MusicDirectories; corpo = `FUN_00bde687`, 102 B): se 3º arg ≠ 0 → nome do
  1º arg (`[vtbl+0x14]` → `FUN_00425bbc`) + id do 2º (`thunk_FUN_0047b878`) →
  `arg3->[vtbl+0xC](nome, id)`.

**Para o port** (`src\formats\resindex.cpp`, `src\app\user_data.cpp`): ordem *packed → directory
files*; tudo relativo a `tsdata/`, com o ramo de idioma em `/tsdata/res/locale/`; a tabela de
`FUN_01032f20` lista os segmentos de user-data que o port já modela.

## (c) `Neighborhoods` / EPs

- **`Neighborhoods/`** vem da tabela `FUN_01032f20` (junto de `AllUsersData/`,
  `NeighborhoodTemplate/`, `LotCatalog/`, `Collections/`, `UserData/` — âncoras 0xEAA868+).
  A enumeração no jogo passa pelo mesmo sistema de segmentos acima (resolver o segmento →
  varrer `.package`s), não por varredura ad-hoc de diretório.
- **EPs**: o módulo de script **`nGameState`** é registrado por `FUN_0090d3c0` via
  `FUN_00cd3b60(this, "nGameState", &DAT_0148a358, 8)`; tabela `0x148A358`:

  | native | endereço |
  |---|---|
  | GetLotTripInfo | 0x8FC510 |
  | GetTransitionsEnabled | 0x8FC590 |
  | GetSaveLotEnabled | 0x8FC5C0 |
  | GetCurrentState | 0x8FC5F0 |
  | **GetInstalledEps** | **0x8FC630** |
  | **IsEPInstalled** | **0x8FC670** |

  (+ `GetProperty`/`SetProperty` = 8 no total.)
- Implementação: natives da VM (args via helpers `0xCF5A70`/`0xCF5B40`); `IsEPInstalled` faz
  `1 << n` e testa via vtable do VM (`+0xDC`); o backend da máscara é **`FUN_00cf5a70`**, que
  cacheia em **`DAT_01499168`**: para i em 0..31, `FUN_00cd2930(0x4BCFCE5E, …, 1, 0, 1<<i)` →
  OR no bit; no fim força o bit `0x1000`.
- **Conclusão p/ o port**: "EP instalada" = bit do bitmask (com 0x1000 forçado); listar bairros =
  iterar o segmento `Neighborhoods/` do sistema de segmentos.

## (d) Primitivas `0x7D` (Follow Sim) e `0x7E` (Run Script) no exe do PC

**Pergunta.** O Mac base 1.0 e o BV só têm 0x7D/0x7E no *routing aggregate*
(`docs/primitives-mac.md` §2), e o AL usa 0x7E em 2.876 árvores. O que o exe do
PC (o build do AL) faz com elas, e qual é o gate de tipo de objeto?

**Método.** As strings do exe (" Follow Sim in ", "defined in objLua file",
"Semi-Global", "Private", "Global", "Primitive not supported for this type of
object.") e quem as referencia; a tabela de despacho por opcode; e o censo das
árvores do AL (`objects.package`).

**Achados.**

- Strings: VA `0x1227D10` (" Follow Sim in ") → `FUN_008b9bd0` @ `0x8b9bd0`;
  VA `0x1227D3C` ("defined in objLua file") e `0x1227D54` ("Semi-Global") →
  `FUN_008b9e00` @ `0x8b9e00`; VA `0x1213C48` = "Global"; VA `0x1223684`/
  `0x122368B` = "Private"; VA `0x122D348` ("Primitive not supported for this
  type of object.") → `0x8fa8da` (o despacho da classe base).
- **Despacho por opcode**: uma sequência de thunks (35 B cada) a partir de
  `0x8ba900`, com a tabela de saltos em `0x8bab34+`; o índice segue o opcode
  (0x78 RayTrace → `0x8ba93e`, 0x79 Change Outfit → `0x8ba961`, 0x7D Follow Sim
  → `0x8ba9ed`, 0x7E Run Script → `0x8baa10`). Como o RayTrace (0x78) e o Change
  Outfit (0x79) são primitivas da classe base (`nTSSG::cObject::TryRayTrace`,
  `cTSObject::TryChangeOutfit`), **no PC as duas estão na classe base** (no Mac
  base 1.0 elas não existem) — é o "gate de tipo de objeto" que faltava: no PC
  qualquer objeto pode chamá-las.
- **0x7E** (`FUN_008b9e00`, "Run Script"): monta o nome do escopo ("Global" /
  "Semi-Global" / "Private") e chama o pedido `FUN_00cd3fe0` (construtor do
  pedido de execução: `param_1[7] = (param_2 << 16) | (param_3 & 0xffff)`),
  com o id empacotado `(op0-1 << 16) | op2-3`; se a árvore não existir, o jogo
  procura um arquivo `.objLua` (o registro de scripts Lua, `FUN_007fd6c0`, que
  varre `*.globalObjLua` e `*.objLua` e indexa por nome).
- **0x7E em assembly (21/10, `0x8b9e26`–`0x8b9f2c`)** — o escopo não sai dos
  bits da flag como se pensava: `MOV AL, byte ptr [ESI+8]` + `AND AL,0x1` ⇒ só
  o **bit 0** do byte de flags é lido; os bits 1, 2 e 3 são recomputados a
  partir desse valor (⇒ 0) e o teste do bit 1 — o ramo "Private", que chamaria
  o `vfunc 0x108` do objeto — é **código morto** neste build (`SHR BL,1` sobre um
  valor já mascarado). O escopo padrão é o **grupo global** (`0x7FD46CD0`); o
  ramo "Semi-Global" (`vfunc 0x114` do objeto) só entra quando o byte em
  `[ESP+0x12]` — o segundo byte do buffer de 3 B que o **`vfunc 0x1c` do
  objeto** preenche — é ≠ 0. O **nome** do script é montado com `vfunc 0x1c` +
  `vfunc 0x18` do objeto por `FUN_00407f8a` (o nome vem do **objeto**, não de
  um índice). O handler faz **dois pedidos**: o primeiro com o pai
  `0x2026960B`, o segundo com `0x4D45F8E6` ("defined in description" ×
  "defined in objLua file"). O executor `FUN_00cd3d80` procura o recurso de
  tipo **0x89B7DC8** com o pai informado no escopo.
- **0x7D** (`FUN_008b9bd0`, "Follow Sim"): pega o array de objetos (o próprio ou
  o do stack object, conforme o byte 9) e chama o mesmo pedido com a referência
  `0x2026960B` (tipo `0x2026`, instância `0x960B`).
- **O pedido e o executor (21/10)** — `FUN_00cd3fe0` monta o pedido (o id
  empacotado em `param_1[7]`, a referência em `param_1[5]`, o objeto do escopo em
  `param_1[6]`, o byte do escopo em 0x25) e `FUN_00cd3d80` o executa: a busca é
  `(**(code **)(*gerenciador + 0xC))(&pedido[5], 0x89B7DC8, &achado, pedido[0x25], 0)`
  — **o recurso de tipo 0x89B7DC8 com a referência do pedido, no escopo do byte
  0x25**; achado, o pedido marca o byte 0x24 e copia o **texto** do recurso para
  os **argumentos** do pedido (os vfuncs +0x60/+0x6C) e o libera (o vfunc +8).
  O handler do 0x7E faz **dois** pedidos: o de referência `0x2026960B` (o log
  "defined in description") e o de `0x4D45F8E6` ("defined in objLua file"), e o
  segundo só sai quando o byte do objeto em `[ESP+0x12]` é ≠ 0 — o mesmo byte
  que escolhe o escopo global × semi-global.
- **Layout dos parâmetros** (medido no código; o struct do handler é o bloco de
  16 B dos operandos deslocado em -3): op0-1 = id (u16), op2-3 = valor (u16),
  op4 = flags (bit0 semi-global, bit1 privado, bits 2/3 de cache), e os pares
  (tipo u8, valor u16) em op6/op7-8, op9/op10-11 e op12/op13-14.
- **Censo no AL** (`objects.package`): **2.876** instruções 0x7E (em 2.876
  recursos de 567 grupos) e **21** instruções 0x7D; o id (op0-1) é **sempre
  304** e op2-3 varia (1, 2, 3, 4, 22, 36, 38, 116, 120, 122, 181, 207).
  O byte de flags é 0xF9 (1.980×), 0xFB (379), 0xFD (201), 0xF1 (171), 0xF3
  (35), 0x03 (24), 0x01 (24), 0xF5 (12), 0xFA (11), 0xF4 (10); o byte 5 é 0xFF
  em 2.801 e 0x00 em 75.
  Nenhum dos dois é instância de árvore: no grupo do chamador não existe a
  árvore 0x130 (só há a global "Family - Relationships - Reset") e nenhum op2-3
  bate com uma instância de árvore do grupo.

**Experimento descartado (21/10).** Implementar o 0x7E como "a árvore cujo
nome é a entrada (op0-1 − 1) da lista de nomes de árvore do grupo (STR# 0x12F,
a mesma do 'Run Tree by Name')" foi medido no AL: as falhas caíam 2.522 (de
14.126 para 11.604) mas apareciam 232 "tree has no instructions" e 11 recursões —
a instrumentação mostrou que 4.994 das 5.000 primeiras chamadas caem no
semi-global do grupo global (que não existe: `semiGlobalFor(0x7FD46CD0) = 0`) e
as do escopo global não acham a árvore. Como no binário o nome vem do objeto, o
modelo foi revertido (o port segue recusando o 0x7E).

**O que falta.** O id 304 (`0x130`) não é instância de árvore em nenhum escopo
(global, semi-global ou privado): é um *tipo* de 16 bits. Falta saber a que o
pedido é resolvido (o nome montado a partir do objeto em `FUN_00407f8a`, o
`vfunc +0xC` do gerenciador de recursos — a busca do recurso de tipo 0x89B7DC8
—, o registro de tipos do jogo ou o `.objLua`) e o que o jogo faz quando não
acha.

**Conclusão p/ o port.** O port recusa 0x7D/0x7E (erro 0x05) como se fossem
exclusivas do BV; no exe do PC elas são da classe base. Implementar exige
resolver o alvo do pedido (RE de `FUN_00407f8a` e do registro de tipos) — até
lá ficam recusadas, e as 2.876 chamadas do AL continuam dando erro 0x05.

## (e) A tabela de erros do exe do PC e a análise completa (21/10)

**Por que.** O projeto `pcb` estava com a análise parcial (faltavam funções em
várias faixas), o que travava o RE das primitivas. Com o Artur liberando a
máquina, o projeto foi reanalisado **com todos os analisadores**
(`analyzeHeadless <pcb> rpc -process Sims2EP9RPC.exe`, 1.056 s, 12 GB): a análise
criou as funções que faltavam e o decomposificador passou a resolver os
`switch` por tabela de saltos.

**A tabela de erros.** `FUN_008FA650` @ `0x8FA650` é a função que devolve a
mensagem de um código de erro: um `switch` com **147 casos** (0x00–0x8E e os
0x3E9/0x44C/0x44D), cada um com o texto do jogo. Ela é a âncora para achar
quem chama o erro: o código 0x1E é "Search type is invalid. Types are 0: all
motive, 1:mental motives, 2:physical motives." (o erro do Set to Next com um
tipo de busca fora da lista), o 0x2B é "Named tree does not exist." e o 0x2C é
"Bad parameter to call named tree." (o Run Tree by Name).

**Conferência com o port.** Os **55** códigos de erro do `src/vm/errors.hpp`
batem **um a um** com a tabela do jogo (mesmo número, mesmo texto — o 0x57 e o
0x6F só diferem na escape do apóstrofo). O port usa o 0x1E no Set to Next com
um tipo de busca fora da lista e o 0x1C ("Object referred to must be a
person.") no Want Satisfy, como o jogo. Os 92 códigos que o jogo tem e o port
não (0x0B "Gosub??", 0x0C "Reference to target object when none exists.",
0x26 "Divide by zero.", 0x27 "Bad parameter in look towards primitive.", …) são
os que nenhuma árvore implementada usa ainda.

**O despacho da classe base.** O `switch` por opcode que chama os handlers das
primitivas está em `FUN_008CC270` @ `0x8CC270` (módulo 0x008C–0x008F): os
casos 0x1C e 0x1F chamam `FUN_008D3510` (Run Tree by Name) e `FUN_00938140`
(Set to Next). O caso 0x1F do despacho é o *route to* do routing aggregate (o
hash `0x5A69D3D5` = o array de objetos do próprio Sim — o mesmo que o executor do
0x7E usa); o **Set to Next da classe base** é o `FUN_00938140` ✓ (o outro, com o
`switch` do tipo de busca ✓).

**O Set to Next da classe base implementa os tipos 0–51** (o `switch` do
`FUN_00938140` tem um caso para cada número, de 0 a 51 ✓). O port implementa
**todos**: os 31 e 34 já estavam feitos, os **38/39**, os **19/20** e o **13**
entraram nesta rodada (22/10). O **13** (a pessoa mais próxima do temp 0, cujo
único uso no AL é `7F01EC29:20E9` "Reaction - Banter") no PC (caso 0xD,
`0x9393b2`): a âncora é `GetObjectByID(temp 0)` (nula: erro 0x17) e **tem de
ser uma pessoa** — o `QueryInterface` do IID `0x5A69D3D5` (`cITSPerson`) sem
pessoa é o **erro 0x1C** ("Object referred to must be a person."); o laço anda
pelas pessoas da mais perto para a mais longe (a consulta espacial do módulo:
`+0x64` avança o cursor e `FUN_00935a90`/`FUN_008dcf40` são escritores com
conferência de faixa que lançam 0x14), testa cada uma com o **filtro comum**
(flags 0x10000/0x20000, dado 56 & 5, dado 107 e o campo do "where") e, no fim,
grava **temp 4** (`+0x92`, `FUN_00e5c948` = o `float` da distância arredondado)
e **temp 5** (`+0x94`, `FUN_00c3e7b0` = o ângulo em 0–7) e devolve true. O port
usa a mesma distância do `Get Distance To` (tiles inteiros, 10 por andar) e a
mesma direção 0–7 do `Get Direction To`; a âncora fica fora das candidatas e o
alvo 0 começa na mais perto (inferido: com o alvo 0 o binário só grava os
temps zerados e não acha ninguém, e o laço do único usuário não terminaria).

**Os tipos 19 e 20: o grupo multi-tile (22/10).** No `FUN_00938140` o caso
0x13 (19, o próximo) começa em `0x939b00` e o 0x14 (20, o anterior) em
`0x939d81`. Nos dois:

1. `âncora = GetObjectByID(temp 0)` pelo módulo de objetos (`[EBX+0x74]`; o
   mesmo `+0x74` que o `FUN_009350a0` usa depois de `app+0x68`). Âncora nula
   grava **0** no alvo e devolve **false** (`0x939d51`: `SetTarget(0)` em
   `0x0092cbe0`; falhou: erro 0x14). O 19 ainda recusa a âncora **morrendo**
   (`TestFlag` 0x10000 e 0x20000 em `0x939b1f`/`0x939b33`) pelo mesmo caminho;
   o 20 **não** tem essa checagem (só a nula).
2. `chave = âncora->+0xe4()` (`CALL [EDX+0xe4]`); negativa → **false sem tocar
   no alvo** (`0x939b4a` no 19, `0x939dd3` no 20).
3. `lista = módulo->+0x14c(chave)` (`0x939b55`/`0x939dde`, um argumento só,
   `ECX` = o módulo). Lista nula (`0x939b5f`/`0x939de8`) ou vazia (a contagem
   em `+0x20`, `0x939b6e`/`0x939df7`) → **false sem tocar no alvo**.
4. O alvo atual vem de `FUN_008cbe10` (= o `GetTarget`). Com alvo **0**: o 19
   varre a lista da frente para o fim e grava o primeiro objeto válido
   (`GetObjectByID` != nulo e sem as flags 0x10000/0x20000) e devolve true; o
   20 varre de trás para a frente e grava o último. Nada válido → false.
5. Com alvo != 0: procura o alvo na lista (o 19 da frente, `0x939c3c`; o 20 de
   trás, `0x939ea8`). **Não achou** → devolve `alvo != 0` (**true**, alvo
   intacto, `0x939c71`/`0x939ec3`). **Achou** → grava **0** no alvo e continua
   no seguinte (19, `i+1`) ou em **`i-2`** (20: `0x939f04 DEC EBX` e
   `0x939f12 LEA EBP,[EBX-1]`, com o `at` em `+0x1c`) — o 20 **pula o objeto
   imediatamente anterior ao atual**, um off-by-one real do jogo. Nada válido
   adiante → false (alvo 0).

**A chave e a lista: o grupo multi-tile.** A chave sai do objeto (`+0xe4`) e a
lista do módulo de objetos (`+0x14c`), com IDs que vão para `GetObjectByID`.
Duas âncoras independentes dizem que a lista é **um grupo multi-tile** (o dado
de objeto 19, o "lead tile object id" do port):

- os **únicos** usuários dos tipos 19/20 no jogo base e no AL são as árvores
  globais `0x7FFD4F2D:2006` ("Set to Next") e `:2007` ("Set to Prev"), e o grupo
  `0x7FFD4F2D` é o da **lareira** (`Function - Init - Fireplace - Chimney`,
  `Function - Link Objects`, `Update - Model (stack object)`); as peças de uma
  lareira são um grupo multi-tile;
- o handler que apaga uma escada modular (`FUN_008c6bb0` → `FUN_009234f0`,
  `0x9234f0`): se o objeto é do tipo 12 (portal de escada modular), pega
  `chave = obj->+0xe4()`, `lista = (o app do objeto em +0x40)->+0x68 (o
  módulo)->+0x14c(chave)` e apaga **todos** os objetos da lista dos tipos 11
  (escada modular) e 12, menos o de ID `obj->+0xbc()` (o próprio). Uma lista por
  **nível** ou por **sala** levaria junto as outras escadas do andar/sala; só a
  do **grupo** dá a escada inteira, que é o que o jogo faz.
- `+0xe4` negativa existe (o `JL` do passo 2): é o objeto **fora de qualquer
  grupo** (o de um tile só). O port devolve false, sem tocar no alvo, quando o
  grupo tem menos de dois objetos.

O port usa `multiTileGroup()` (o dado 19) e a lista dos objetos com o **mesmo**
dado 19, na ordem da lista do módulo (`VM::objectList()`), como os tipos 0/1/2.
Limite conhecido: o `+0x14c` do jogo pode ser um índice por um **id de grupo**
atribuído no placement, e não pelo ID do lead; o port não guarda esse id e usa o
lead, que dá a mesma lista (o lead nasce com o dado 19 = o próprio ID e as peças
com o do lead).

**O modo 3 do Run Tree by Name (21/10).** O handler `FUN_008D3510` @ `0x8D3510`
(escrito no `primitives-mac.md` §0x1C) resolve o nome (o pedido `FUN_00cd3fe0`
com o id 0x12F) e depois bifurca no **byte 9** (o modo): 0 e 1 rodam a árvore do
*stack object* na hora, 2 empilha, e o **3** (o caso novo) pega o **GUID nos
temps 0/1** (a palavra baixa no temp 0), procura o objeto no mundo, pega a
**árvore do objeto** (o vfunc +0x48) e a roda na hora como o modo 0; um GUID que
nenhum objeto tem é o erro **0x7A** ("You sent me a crappy GUID, please fix
it.") e um modo fora de 0–3 é o **0x2C** ("Bad parameter to call named tree.").
No AL o modo 3 vem logo depois de um **0x7E** (Run Script), que é quem põe o
GUID nos temps.

**Os rótulos dos modos do Run Tree by Name.** O editor guarda os rótulos no
STR# `0xDE` do grupo `0x7FE59FD0` ("how to call named tree"): no **jogo base**
são **três** — "run in my stack" (0), "run in stack object's stack" (1) e "push
onto my stack" (2). O **modo 3 não existe no jogo base**: ele é do Bon Voyage
(o AL usa o modo 3 em 648 árvores) e o erro do jogo para um modo fora da lista
é o **0x2C** ("Bad parameter to call named tree."). No port, o modo 3 continua
recusado com um `VMError` honesto ("mode 3 not implemented") — o comentário do
`primitives.cpp` guarda a semântica que o RE do Mac já tinha: "o modo 3 é a
árvore do objeto cujo GUID está nos temps 0/1".

## (f) A Super Collection do Mac (64 bits) como referência (22/10)

**Por que.** O exe do PC (o build do AL) é x86 de 32 bits e o Mac base 1.0 que
o mantenedor analisou é PowerPC; a "Super Collection" (Aspyr, 1.2.5) é o **jogo em
x86-64**, com a mesma lógica (é o Bon Voyage) e, portanto, **o mesmo 0x7E/0x7D**
que o PC tem — a peça que faltava para fechar a implementação.

**O `.dmg` que o Artur baixou** (`The.Sims.2.Super.Collection.v1.2.5.dmg`, 6,9 GB)
é um UDIF **comprimido com bzip2** (o `koly` no fim diz: data fork no offset 0,
7.266 GB; o plist XML no fim tem só o mapa de partições). O caminho que funcionou,
sem 7-Zip e sem root:

1. ler o `koly` (os últimos 512 bytes) e o plist XML (offset/len em 216/232) — o
   plist é o *mapa* de partições, não o catálogo HFS+;
2. descomprimir o fork de dados com `bz2.BZ2File` em streaming → a imagem do
   disco (9,3 GB), que começa com os boot blocks "ER" (o Apple driver
   descriptor) e tem o mapa de partições no setor 1;
3. procurar o **Mach-O** varrendo a imagem (o `0xCFFAEDFE` de 64 bits e o
   `0xCAFEBABE` do universal): 53 candidatos, e o **executável do jogo** é o
   maior `MH_EXECUTE` (35,8 MB, x86-64, 4 segmentos) — os outros são dylibs e
   helpers;
4. extrair o Mach-O (os segmentos dizem o tamanho todo) e importar no Ghidra
   (o loader Mach-O do Ghidra lê x86-64 ✓).

**A prova de utilidade passou**: o binário do Mac tem **as mesmas strings e as
mesmas constantes** do exe do PC — `Named tree does not exist`, `defined in
objLua`, `defined in description`, `Follow Sim in`, `Semi-Global`, `crappy GUID`,
`Bad parameter to call named tree`, `Set To Next`, `Passing in params`, e os
hashes `0x2026960B` (1076×), `0x89B7DC8` (61×), `0x4D45F8E6` (13×),
`0x5A69D3D5` (214×). Então o mesmo RE do 0x7E é possível lá — e lá o código é
**de 64 bits**, o que deixa a leitura do `vfunc +0xC` do gerenciador de recursos
(mais simples que no x86 de 32 bits) e mostra as *correções* que a Aspyr fez
nessa porta (o 64 bits é uma reimplementação, não o binário original).

**Licença.** O binário da Aspyr é proprietário: ele serve de **referência de
comportamento** (como o Mac PPC do mantenedor), e o código do OpenSims2 é
reimplementado a partir dele, nunca copiado.

## (g) O `0x7E` (Run Script) e o `0x7D` (Follow Sim) no exe do PC: o pedido é o mesmo do `0x1C` e a tabela é "Lua Scripts" (22/10)

**Pergunta.** O que faltava para o item 12 (implementar o `0x7E` e o `0x7D`): o
sentido do byte em `[ESP+0x12]` e a implementação do `vfunc +0xC` do
gerenciador de recursos.

**Método.** As strings do exe ("defined in objLua file", "Semi-Global",
"Private", "Global", "( Follow Sim in ", ", output result to ", "Named tree does
not exist.") e quem as referencia (`FindString`); o handler do `0x7E`
(`FUN_008b9e00` @ 0x8b9e00) lido **inteiro em assembly** (0x8b9e00–0x8ba190) e
o do `0x7D` (`FUN_008b9bd0` @ 0x8b9bd0); o pedido (`FUN_00cd3fe0`) e o executor
(`FUN_00cd3d80`); o handler do `0x1C` da **classe base** (`FUN_008d3510`) para
comparar; e as **tabelas STR#** do `objects.package` do AL (`s2pkg dump`).

**Achados.**

- **Os handlers.** O `0x7E` = `FUN_008b9e00` (938 B) e o `0x7D` = `FUN_008b9bd0`
  (554 B), no *routing aggregate* (a tabela de saltos em 0x8bab34 cobre os
  opcodes 0x6C–0x7F; o `0x7E` é o thunk 0x8baa10 e o `0x7D` o 0x8ba9ed). O
  `0x1C` da **outra** classe (a base) é o `FUN_008d3510`, e o seu layout de
  operandos é o do port (flags em o[2], o nome em o[4], o modo em o[5]).
- **O layout dos operandos do `0x7E`** (o bloco de 16 B dos operandos
  **deslocado em -4**, `arg1 = &operands[-4]`): os bytes 0-1 = o **id do STR#**
  (u16), 2-3 = o **id do script** (u16, 1-based), 4 = o **byte de flags**, 5 não
  é lido, e os três pares (tipo u8, valor u16) em 6/7-8, 9/10-11 e 12/13-14.
  Conferido pelo censo do AL: o id do STR# é sempre 304 (0x0130) em o[0..1] e o
  byte de flags (18 valores) em o[4].
- **O byte de flags (correção do que foi escrito em 21/10).** O byte é
  recarregado **sem máscara** (`MOV AX, word ptr [ESI+8]`) antes dos shifts, então
  os quatro bits valem: **bit 0** = o segundo pedido (o `.objLua`), **bit 1** =
  o escopo **privado** (`vfunc 0x108` do objeto), **bit 2** = o escopo
  **semi-global** (`vfunc 0x114`) e **bit 3** = o log ", Passing in params where
  param 0 = ". Sem bit 1 nem bit 2 o escopo é o **grupo global** (o dono
  `0x7FD46CD0`). A leitura anterior ("só o bit 0 vale, o ramo Private é
  código morto") errou por causa do `MOV AX`.
- **O pedido é o mesmo do `0x1C`.** `FUN_00cd3fe0(this, idDoSTR#,
  idDoScript, dono, 1, tipo)` empacota o id como `(idDoSTR# << 16) | idDoScript`,
  e o executor (`FUN_00cd3d80`) pergunta ao gerenciador
  `(**(code **)(*gerenciador + 0xC))(&pedido[5], 0x89B7DC8, &achado, byte[0x25], 0)`
  — o recurso de tipo 0x89B7DC8 com a referência do pedido, no escopo do dono.
  O `0x1C` da classe base faz o mesmo: `FUN_00cd3fe0(0x12F, idDaárvore, dono,
  1, 0x2026960B)` — o id é o par **(STR# << 16) | id da árvore**, com o STR#
  do `0x1C` fixo (0x12F) e o do `0x7E` vindo dos dados. Os dois pedidos do `0x7E`
  são o mesmo par com o pai diferente: `0x2026960B` (o script "definido na
  descrição") e `0x4D45F8E6` (o "definido num arquivo .objLua").
- **A tabela STR# 0x130 é "Lua Scripts".** No `objects.package` do AL (grupo
  global 0x7FD46CD0): o STR# 0x12F = **"Call Named Tree prim string set"** (2.823
  entradas) e o 0x130 = **"Lua Scripts"** (7.233 entradas) — a lista dos scripts
  Lua do Bon Voyage. O id do AL bate: a árvore global 0x0137 "NPC - Get Outfit
  GUID for Type in T[0,1]" é **uma única instrução** (o `0x7E` com o id
  de script 49) e a entrada 49 da tabela "Lua Scripts" é "NPC - Get Outfit GUID
  for Type in T[0,1]" ✓ (a 0x013A usa o id 50 → "NPC - Get Template GUID for
  Type in T[0,1]" ✓). **Logo o `0x7E` roda um script Lua** (do `.objLua` do
  objeto), e a árvore de descrição com o mesmo nome é o próprio chamador
  — a global 0x0137 chama o próprio nome.
- **O `run`.** O handler termina com `object->vfunc 0x44(<o nome>)` — o mesmo
  "rode a árvore/script com este nome" que o `0x7D` usa (lá com o valor do
  parâmetro 0). Os pedidos servem ao **log** do jogo ("defined in
  description"/"defined in objLua file"), não ao alvo.
- **O `0x7D`.** O handler peçe o array de objetos (o próprio ou o do stack
  object, conforme o byte 5) e faz o pedido com o STR# 0x118; o log diz
  "( Follow Sim in ..., output result to ... my object array: <o nome
  achado> )". No AL ele tem **1 uso** (a árvore 0x1001 "Function - Main" do
  grupo 0x7F649DA1, a instrução #2).

**Conclusão p/ o port.** O RE do `0x7E` está **fechado** e a resposta é
inequívoca: o que ele roda é um **script Lua**, e o port (como o upstream) não
tem interpretador de Lua — e a substituição pela árvore de descrição
com o mesmo nome seria uma recursão (o chamador é essa árvore). **A recusa
(erro 0x05) foi trocada por `false` em 22/10**: o handler do jogo devolve o que
o `run` do script/árvore devolve (`vfunc 0x44`), e o erro derrubava os lotes do
AL — os controladores do AL (LTA Superpowers, Climate, Business, Fish Pond
Effects, Community Lot - Populator...) e a Main dos Sims resetavam a cada
poucos ticks. No `N001_Lot11` com os dados do AL os 4 Sims fizeram **0 buscas e
0 escolhas em 3.600 ticks**; com `false` voltaram a agir (3 a 8 buscas/escolhas
em 600 ticks) e os controladores param de resetar, com os recursos de Lua
inertes. Sweep do AL: 13.170.043 / 19.087 / 13.200 → **14.025.434 / 19.927 /
10.335** (as 3.208 falhas de Run Script somem). O `0x7D` também devolve `false`:
1 uso no AL e o sentido do array de objetos continua sem medida.

## (h) Contraprova no Mac 64 bits (a Super Collection): o pedido do `0x7E` é o mesmo, campo a campo (22/10)

**Método.** O executável x86-64 da Super Collection (o Bon Voyage) importado
no projeto Ghidra `C:\Users\Artur\sims2-re\mac64` (§(f)): as strings do
`0x7E` ("defined in objLua file", "defined in description", "Semi-Global", ",
Passing in params where param 0 = ") e do `0x7D` ("( Follow Sim in ", ", output
result to "), o handler do `0x7E` (`FUN_10075cc60`) e o do `0x7D`
(`FUN_10075c9a0`), e o construtor do pedido (`FUN_10120c5e0`).

**Achados (o Mac 64 confirma o PC 32).**

- **O escopo**: o byte privado → o dono `vfunc 0x220` e a string "Private";
  o byte semi-global → o dono `vfunc 0x238` e "Semi-Global"; sem eles o dono
  é o **grupo global** `0x7FD46CD0` — no PC são os vfuncs 0x108 e 0x114
  (os offsets da vtable mudam entre 32 e 64 bits; o sentido não) ✓.
- **O pedido**: `FUN_10120c670(this, idDoSTR#, idDoScript, dono, 1, 0x2026960B)`
  e, no ramo do objLua, `FUN_10120c670(this, idDoSTR#, idDoScript, dono, 1,
  0x4D45F8E6)` — exatamente o par do PC ✓.
- **O id empacotado**: no construtor do Mac,
  `FUN_1002b30c0(..., param_6, param_4, param_3 & 0xffff | param_2 << 0x10, 0)`
  — o mesmo `(STR# << 16) | id` do PC (`param_1[7] = (int)param_2 << 0x10 |
  param_3 & 0xffff`) ✓✓✓.
- **Os logs**: "defined in description" no ramo sem o bit do objLua e "defined
  in objLua file" no ramo com ele (quando o nome do objLua vem vazio), com o
  nome da descrição e um " - " no meio ✓; e o log dos parâmetros
  (", Passing in params where param 0 = ") atrás do bit 3 ✓, com os três
  pares (byte + u16) lidos pelos acessores do bloco de operandos ✓.
- **O `run`**: nos dois (0x7D e 0x7E) o objeto recebe o vfunc +0x98 (Mac; +0x44
  no PC) com a string do próprio objeto — o mesmo mecanismo nos dois
  binários, e o `0x7E` faz os **dois pedidos antes** dele (para o log).

**Conclusão.** A leitura do PC se sustenta sem correção nova: os pedidos
resolvem o nome do script pela tabela **STR# do id**, e o que o port não tem
é o interpretador de Lua.

## Cross-check REA (fechado em 17/10, ~22:38)

Critério do goal da noite: o REA (ferramenta independente) confirma ≥1 achado do projeto `pcb`.
Fechado com **3/3 âncoras exatas** via `search_strings` (regex
`Bin Prod ID|Bin Sort Idx|TSCatalog|FunctionSubsort`):

| âncora (projeto `pcb`, VA) | REA (rodada final) | match |
|---|---|---|
| `FunctionSubsort` @ `0x012358AC` | `0x12358ac` | sim |
| `TSCatalog` @ `0x0124AE10` | `0x124ae10` | sim |
| `" Bin Prod ID = %d Bin Type = %s Bin Sort Idx = %d"` @ `0x01262534` | `0x1262534` (`\nBin Prod ID = %d\nBin Type = %s\nBin Sort Idx = %d`) | sim |

- **Evidence**: `rea-snapshots/ghidra-pc/rea-crosscheck.success.json` — `evidence_id`
  `ev_6b0910b0a3bef8159244960ac31bf15bb885e21e3fadd7e7a354dfa128b899a5`, provider Ghidra 12.1.4,
  target sha256 `cc4bb4e1…`, `operation: search_strings`, `confidence: observed`.
- **Receita reproduzível** (~2 min de análise; rodar solo — nada de Ghidra em paralelo):
  - `GHIDRA_INSTALL_DIR` + `JAVA_HOME` como em `run-queries.ps1`;
  - `GHIDRA_HEADLESS_MAXMEM=12G` (o default do REA é 2G);
  - `REA_GHIDRA_STARTUP_TIMEOUT_MS=3300000` (o deadline de startup cobre a análise inteira);
  - `REA_GHIDRA_DISABLED_ANALYZERS` = os 19 nomes de `analyzer-list.txt` (todos os pesos-pesados
    do resumo de análise do `pcb`; `ASCII Strings` fica ligado — é o que a busca usa);
  - `npx -y rea-agents@6.5.0 search '<exe>' --kind strings --pattern '...' --mode regex --format json`.
- **Histórico das falhas** (todas por tempo, nenhuma por resultado): a rodada de 1 h caiu no
  default de 3600 s do headless (`analysis_timeout`); as rodadas de 15 e 55 min caíram no deadline
  de startup do REA **ainda no meio da análise** com os analyzers pesados ligados (X86 Function
  Callee Purge 255 s, Data Reference 213 s, Reference 211 s, x86 Constant Reference 90 s,
  Stack 71 s…; o run manual do `pcb` fecha em 17,4 min). Com o pacote pesado desligado a análise
  cai para ~2 min mesmo com as 2 GC threads hardcoded do REA (`GhidraJavaLaunch.js`; há **dois**
  caches do npx — patch com backup `.orig` nos dois, ver log do goal).
- **Limitações do envelope** (relevantes p/ consultas futuras): o inventário cobre só strings
  *definidas* (`defined-data-coverage-v1`); projeto efêmero (`-readOnly`, `-deleteProject` — nada
  persiste); o deadline do REA cobre o ciclo inteiro (import → análise → operação).

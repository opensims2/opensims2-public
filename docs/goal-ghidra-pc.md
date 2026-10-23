# Goal — Noite de RE do lado PC (`Sims2EP9RPC.exe`)

- **Status:** **CONCLUÍDO** — 17/10 22:38: projeto `pcb` analisado e salvo; goals (a)/(b)/(c)
  documentados em `docs/re-pc-ghidra.md`; **cross-check REA fechado (3/3 âncoras exatas**,
  evidence `rea-crosscheck.success.json`); resíduos limpos. Pendências (bins/handlers) seguiram
  para o `goal-port-fidelity.md` (fila item 3).
- **Mandato:** Artur — "Arrocha, use o REA + Ghidra, pode usar mais capacidade, vou dormir, rode por no mínimo 8h, faça um goal." (noite de 16→17/10/2026)
- **Branch:** `windows-x64-ghidra-pc`
- **Cutoff:** ~08:00 de 17/10. Parar limpo: nenhum processo pendurado, nada pela metade sem nota.

## Objetivo

1. **Infra de RE do exe PC permanente**: projeto Ghidra persistente (análise 1×; consultas em
   segundos) + scripts de consulta — o que o REA não dava (ele reimporta a cada chamada com
   teto de 1 h por operação).
2. Usar essa infra, com REA como cross-check, para as perguntas PC-side abertas:
   - **Catálogo/knob de Compra** (doc `buy-build-mac.md` §1.4 "por conferir"; residual 53×47):
     localizar no exe o pipeline da contagem (candidatos + pseudocódigo).
   - **Varredura de `TSData\Res`**: como o exe acha as árvores (valida a resolução multi-pack do port).
   - **Enumeração de `Neighborhoods`**: como as EPs são lidas (pré-requisito do próximo item do port).
3. Documentar tudo em `docs/` com endereços, pseudocódigo e como reproduzir.

## Não-objetivos

- Mexer no código do port nesta noite (foco é RE).
- Symxfer Mac→PC (inviável: PPC vs MSVC x86, sem RTTI no PC).
- Análise dinâmica (exige a máquina em uso; fica para outra sessão).

## Fatos que motivam (verificados 16/10)

- Os dois exes PC **não têm símbolos** (zero nomes mangled/RTTI de verdade; sem PDB na UC):
  no PC não existe "buscar `cTSCatalog::LoadObjects` por nome".
- REA: operações pesadas reimportam a cada chamada (30–60+ min) e estouraram o teto de 1 h;
  busca de strings (~30 min) passa.
- Ghidra 12.1.4 + JDK 21 + Python 3.12 já instalados; máquina nunca suspende em AC.

## Critérios de sucesso

- [x] Projeto `C:\Users\Artur\sims2-re\pcb` (`rpc`) criado, salvo, reabrível (`-process ... -noanalysis`).
- [x] Scripts de consulta funcionando (testados antes no exe pequeno `s2pkg.exe`).
- [x] `functions.tsv` + `strings.tsv` + `symbols.tsv` do exe gerados.
- [x] `TSData\Res`: varredura localizada e documentada (ordem/prioridade).
- [x] `Neighborhoods`: idem.
- [x] Catálogo/knob: candidatos localizados + pseudocódigo (residual 53×47 fechado).
- [x] Cross-check REA de ≥1 achado (evidence JSON em `rea-snapshots`; 3/3 âncoras exatas — 17/10 22:38).
- [x] Docs + commits na branch; log abaixo atualizado.
- [x] Máquina limpa ao final (sem processos pendurados).

## Riscos

- Análise do exe pode ser longa (o "Decompiler Parameter ID" é o suspeito nº 1 — tentativa de
  desabilitar via opções; se não der, aceitar e esperar).
- REA pode estourar o teto de 1 h de novo → perde-se só o cross-check, não o trabalho.
- RAM de 24 GB: rodar REA e Ghidra **em série**, não em paralelo.

## Log

- (16/10 ~23:55) Branch criada; sono/hibernação conferidos (nunca em AC); goal escrito;
  reconhecimento do exe feito (sem símbolos); javap confirmou a API de análise.
- (17/10 ~00:20) Dúvida do Artur sobre o alvo ("não sei se é o exe original do game"):
  investigado e resolvido — `Sims2EP9RPC.exe` é o exe do launcher **Sims 2 RPC** (Voicemxil,
  v1.15.1 na pasta; `RPCFilesystem.txt` o lista como arquivo do launcher e o changelog cita
  "CreateProcess ... on Sims2EP9RPC.exe"). O `Sims2EP9.exe` local foi patchado (hash difere
  dos dois backups idênticos entre si). Artur confirmou: "pode usar o rpc mesmo". Alvo da
  noite = `targets/Sims2EP9RPC.exe` (sha256 cc4bb4e1...).
- (17/10 ~00:45) Smoke dos scripts ok (DumpInventory/MemoryInfo/ListAnalysisOptions compilam
  e rodam). Descobertas: (a) a análise default mastiga PDB/debug embutido por muito tempo
  (s2pkg com `.pdb`: >15 min; no RPC é o flood de "TypeDescriptor" que derrubou o REA);
  (b) opções de análise via `program.getOptions("Analysis")` vieram vazias no headless →
  decisão: rodar a análise completa do RPC com teto de segurança
  `-analysisTimeoutPerFile 10800` em vez de brigar pra desabilitar analisador; MAXMEM default
  = 8G; (c) **Ghidra proíbe diretório com ponto no caminho** — workspace fica sob `.gemini`,
  então os projetos vivem em `C:\Users\Artur\sims2-re\`; (d) REA #1 (strings
  `catalog|subsort|neighborhoods|tsdata`) rodando, teto ~00:55.
- (17/10 ~01:30) Anchors do exe mapeados via dump Python próprio (strings.py): `TSCatalog` 0xE4AE10,
  `subsort` 0xE4B5F4 (cluster do parser de props do catálogo em 0xE4B498: catalogTextResource/Index,
  notInCatalog, buildTipIndex...); `FunctionSubsort`/`FunctionFlags`/`RoomFlags` 0xE358AC+;
  prints de debug dos bins `Bin Prod ID/Type/Sort Idx/Key/Name` + `Frag Prod ID/Sort Idx` 0xE62535+;
  paths `/tsdata/` 0xE51638, `/tsdata/res/locale/` 0xE516F0, `Neighborhoods/` 0xEAA934,
  `Catalog/` 0xEDA304, `UserData/` 0xEAA944, `Starting BuildResourceMaps.` 0xE5173C,
  `CreateGameDirectoryFileSegment` 0xE51530, `BuildModeThumbnails.package` 0xED9F4C.
  Run grande (import+análise, PID 23352) iniciado ~01:02 com `-analysisTimeoutPerFile 10800`
  (cap 3h) e postScripts DumpInventory+MemoryInfo; consultas preparadas em
  `rea-snapshots/ghidra-pc/run-queries.ps1`. REA #1 seguia rodando (2.2 GB WS, 40+ min CPU).
- (17/10 ~02:05) REA #1 terminou em **falha** (`provider_timeout`, "Ghidra startup deadline
  elapsed", 1 h) — terceira falha no exe RPC; hipótese: contenção (rodou junto com os smokes
  Ghidra e com o run grande). Comparação: o `search_strings` "PieMenu" que passou levou ~30 min.
  Plano: ao fim do run grande, tentar **um REA magro sozinho** como cross-check
  (`Bin Prod ID|TSCatalog|subsort`) e comparar endereços; se falhar, documentar como
  infactível neste exe.
- (17/10 ~08:35→09:00) **Correção do registro acima**: o "02:05" foi estimativa errada (sem
  relógio) — os arquivos `pc\console.txt`/`analyze.log` **pararam às 01:22:22** e o processo
  ficou pendurado ~7 h até ser morto na manhã, **sem salvar** (`rpc.rep` = 4 arquivos/0 MB;
  `pc\out` vazio). Causa provável: analyzers do decompiler em loop (mesmos warnings na mesma
  função por horas) agravados por **GC capado a 2 threads** (default do `analyzeHeadless.bat`)
  com heap 8G. Nada a recuperar do run; projeto `pc` ficou inútil (pode apagar no fim).
- (17/10 ~08:40→09:05) **Diagnóstico rendeu correções (todas validadas no `s2pkg.exe`)**:
  - `GHIDRA_HEADLESS_MAXMEM` (não `MAXMEM`) e `GHIDRA_HEADLESS_JAVA_OPTIONS` funcionam →
    12G heap / `-XX:ParallelGCThreads=6` (MemoryInfo confirmou 12288 MB).
  - **Mecanismo p/ desabilitar analyzers**: flags = `program.getOptions("Analyzers")`
    (chave = nome do analyzer), sincronizadas por `AutoAnalysisManager.initializeOptions()`
    (achado via bytecode; `registerAnalyzerOptions()` sozinho não basta e "Analysis" é
    categoria errada). `scripts/OffAnalyzers.java` (preScript) aplica; teste: PDB Universal
    desligado → linhas PDB 154→6.
  - **Bugs dos meus scripts corrigidos**: `Data` vive em `ghidra.program.model.listing`
    (meus `import ...model.data.Data` não compilavam); args com `|` quebram o cmd via `.bat`
    → `FindString.java` agora lê regexes de arquivo (`<outfile> <patternFile> [maxHits]`).
    Validado: `DumpInventory` no s2pkg = 31.816 funções/4.274 strings/203.995 símbolos;
    `FindString` achou `subsort`/`catalog` com refs (`FUN_1402039a0`, `FUN_1407874d0`).
- (17/10 ~09:05) **Retry do RPC lançado em andamento**: projeto novo
  `C:\Users\Artur\sims2-re\pcb` (`rpc`), disables = PDB Universal + Decompiler Parameter ID +
  Decompiler Switch Analysis + Call Convention ID, cap 60 min (timeout salva parcial —
  comprovado no s2pkg), 12G/6 GC, postScripts DumpInventory+MemoryInfo. Lançado **destacado**
  (WMI; imune a fim de sessão) via `C:\Users\Artur\sims2-re\pcb-run.cmd`; watcher avisa no fim.
  Consultas prontas em `run-queries.ps1` (4 lotes: catalog/bins/paths/ui → `pcb\out\refs-*.tsv`).
- (17/10 ~09:40) **Run `pcb` concluído e salvo** (~16-20 min, exit 0): `DumpInventory` =
  101.530 funções / 25.268 strings / 513.110 símbolos → `pcb\out\{functions,strings,symbols}.tsv`.
  Consultas readonly em ~24 s (4 lotes → `refs-catalog/bins/paths/ui.tsv` + `decomp-main.c`,
  `decomp-handlers.c`). Achados iniciais: serializer do Exchange (Price=0x12, RoomFlags=0x27,
  FunctionFlags=0x28, FunctionSubsort=0x5E/**94**, CommunitySort=0x64) e pares read/write dos
  objetos de build; tabela de diretórios em `FUN_01032f20`; módulo nGameState.
- (17/10 tarde) **Regiões cegas consertadas** (`0xBDF100-0xBE0A77` etc. — código só alcançado
  por vtable): scripts novos `DisasmFix`/`WhoRefs`/`RedoFns`/`NoRetOff`/`FnAt` (ver doc);
  descoberto o prólogo SEH `MOV EAX,<handler>; CALL 0xE5C86C` (helper que prepara
  `ExceptionList` e **retorna**) antes das funções grandes; criadas e decompiladas
  `FUN_00bdf275` (CreateGameDirectoryFileSegment), `FUN_00be0141`, `FUN_00bdfa42`,
  `FUN_00be0787` (BuildResourceMaps) + `FUN_00bdf753` (switch do ResourceManager) e
  `FUN_00cf5a70` (bitmask de EPs).
- (17/10 tarde) **`re-pc-ghidra.md` preenchido**: (a) tabela de props de reflexão em 0x124B470
  (9 props, getters/setters; read-only = `notInCatalog`/`subsort`/`thumbnailResource`/
  `deprecated`), STR# do catálogo (hash `0xFB4CB0DE`), **campo 94 = `FunctionSubsort`** (residual
  53×47 do port fechado); (b) cadeia `BuildResourceMaps` → packed → directory files, seções ini
  (`DBSegments`/`DBSegmentDirectoryFiles`/`PackedDBSegments`/`SampleDataSegments`/
  `TrackDefSegments`/`MusicDirectories`), `%DataDirectory%`/`%Language%\\`, paths `tsdata/` +
  `/tsdata/res/locale/`, `Hitlab.ini`; (c) `Neighborhoods/` pela tabela de segmentos + EPs via
  nGameState (`IsEPInstalled` = `1<<n` sobre a máscara `DAT_01499168`).
- Pendências deixadas: (1) cross-check REA solo (tentativa final — 3 falhas anteriores);
  (2) região dos bins `0xCDF41B`+ ainda sem função definida; (3) handlers de linha
  `0xBDF83B`/`FUN_00bdd8f2`/`FUN_00bde67d` não decompilados.
- (17/10 tarde) Resíduos limpos (projeto morto `pc`, testbeds) e docs commitados.
- (17/10 ~21:15) **Cross-check REA — tentativa final solo lançada (Artur autorizou: "força
  total")**. Causa-raiz das falhas achada no próprio pacote `rea-agents@6.5.0` (cache do
  npx): (a) o launcher do REA dá ao Ghidra `-Xmx` default **2G** e ignora nosso
  `GHIDRA_HEADLESS_MAXMEM` só se ausente — `GhidraJavaLaunch.js` lê esse env (e
  `GHIDRA_MAXMEM`) e ainda hardcoda `-XX:ParallelGCThreads=2`/`CICompilerCount=2` **depois**
  dos vmargs; (b) o teto de 1 h não é do REA: é o **default do headless do Ghidra
  (`-analysisTimeoutPerFile` = 3600 s)** que o REA não sobrescreve e não tem knob — a última
  falha (`rea-catalog.json`) é `analysis_timeout` → "Analysis took too long" com
  `timeout_ms: 3600000`. Ou seja: com 2G a análise do RPC afogava em GC e estourava 1 h
  (contenção não era a causa principal). Retry config: `GHIDRA_HEADLESS_MAXMEM=12G`,
  `REA_GHIDRA_STARTUP_TIMEOUT_MS=900000`, `REA_GHIDRA_DISABLED_ANALYZERS` = os mesmos 4 do
  run `pcb` (`PDB Universal`, `Decompiler Parameter ID`, `Decompiler Switch Analysis`,
  `Call Convention ID` — nomes validados por readback; o `ReaGhidraAnalysisOptions.java` do
  REA **falha em nome desconhecido**). `rea-crosscheck.ps1` atualizado; rodando solo
  (sem Ghidra paralelo), snapshot `ghidra-pc\rea-crosscheck.snapshot`, saída
  `rea-crosscheck.json`/`.err`, teto ~1 h. Se passar: comparar endereços com os anchors do
  `pcb` (Bin Prod ID/Bin Sort Idx/TSCatalog/FunctionSubsort) e registrar evidence.
- (17/10 ~21:29) **Retry #1 cortado aos 15 min** — pelo `REA_GHIDRA_STARTUP_TIMEOUT_MS` que eu
  havia setado (900 s): `failure_kind: timeout`/"Ghidra startup deadline elapsed",
  `timeout_ms: 900000`, com o log do Ghidra terminando em plena análise (ruído de
  `TypeDescriptorModel` + `PngDataType`, sem "analysis complete"). **Aprendizado: o deadline de
  startup do REA cobre a análise inteira** (import → análise → pronto p/ servir); o "1 h" das
  falhas antigas era o **deadline por arquivo do headless do Ghidra** (3600 s →
  `timeout_ms: 3600000`), não do REA. Evidência arquivada em
  `rea-crosscheck.attempt15min.json`.
- (17/10 ~21:32) **Retry #2 lançado** com `REA_GHIDRA_STARTUP_TIMEOUT_MS=3300000` (55 min, sob
  o teto de análise do Ghidra); resto da config igual (12G + os 4 analyzers desligados).
  Confirmado **ao vivo** via `Win32_Process`: `java.exe` (PID 18376) com **`-Xmx12G`** — o env
  propaga; só o `-XX:ParallelGCThreads=2`/`CICompilerCount=2` é hardcoded do REA (aplicado
  depois dos vmargs, não dá para subir). Análise esperada: ~20-30 min.
- (17/10 ~22:26) **Retry #2 também cortado — aos 55 min, ainda no meio da análise**
  (`timeout_ms: 3300000`; o stdout do Ghidra termina em `MultEntSubModel` +
  `Applied data type archive: windows_vs12_32`). Comparação com o run manual do `pcb`: lá os
  mesmos marcadores aparecem aos ~8,5 min (ADA às 09:10:48; PNG cedo às 09:02:37) e o resumo de
  análise fecha em **17,4 min** (total 1044 s) — o REA rodou **~5-6× mais lento que o manual**.
  Onde o tempo ia (resumo do `pcb`): X86 Function Callee Purge 255 s, Data Reference 213 s,
  Reference 211 s, x86 Constant Reference 90 s, Stack 71 s, Create Address Tables 43 s,
  Non-Returning 28 s, Function ID 15 s, PE Exception Handling 15 s, Scalar Operand 14 s,
  Subroutine References 12 s, Shared Return Calls 8 s, Demangler 1,4 s (lista completa das 135
  opções validada no projeto e arquivada em `rea-snapshots\ghidra-pc\analyzer-list.txt`).
  Evidência do #2: `rea-crosscheck.attempt55min.json`.
- (17/10 ~22:36) **Tentativa #3 lançada** com três correções independentes: (1) **patch local do
  cache do npx** (`GhidraJavaLaunch.js` com backup `.orig`): `ParallelGCThreads=6`/
  `CICompilerCount=4`; (2) `REA_GHIDRA_DISABLED_ANALYZERS` com **19 nomes validados** na lista das
  135 opções do `pcb` (todos os pesos-pesados acima; mantém `ASCII Strings` p/ a busca);
  (3) **watcher** `rea-watch-r3.log` — amostra CPU/RAM do java a cada 30 s, sobe prioridade p/
  `High`, copia o `ghidra.log` do runtime dir do REA (apagado no fim) p/ `rea-ghidra-r3-live.log`
  e mata se ficar 12 min sem CPU (stall). Deadline segue 55 min. Se passar: comparar endereços
  com os anchors do `pcb` e registrar a evidence.
- (17/10 ~22:37) **O watcher já pagou o preço**: a captura da linha de comando do java mostrou
  que o `npx -y rea-agents@6.5.0` resolve para um **segundo cache** do npx
  (`_npx\ad9a3fdbcad992c6` — não o `af752b90495482d7` que eu tinha patcheado): a #3 está rodando
  com os `-XX:ParallelGCThreads=2`/`CICompilerCount=2` originais mesmo. Patch replicado no cache
  certo (backup `.orig`) p/ qualquer nova tentativa. A linha confirma também: `-Xmx12G` ✓, os
  **19 analyzers desligados** ✓, `-readOnly` + `-deleteProject` (nada persiste) e **sem
  `-analysisTimeoutPerFile`** (default 3600 s do Ghidra segue valendo). Com o pacote pesado já
  desligado, a expectativa é terminar bem antes do deadline mesmo com 2 threads — seguindo.
- (17/10 ~22:38) **CROSS-CHECK FECHADO — exit 0 em ~2 min** (22:36:34→22:38:3x): `search_strings`
  devolveu **3/3 âncoras exatas** — `FunctionSubsort` @ `0x12358ac`, `TSCatalog` @ `0x124ae10`,
  `\nBin Prod ID = %d\nBin Type = %s\nBin Sort Idx = %d` @ `0x1262534` (idênticos ao
  `strings.tsv` do `pcb`). Evidence: `rea-crosscheck.success.json` — `evidence_id`
  `ev_6b0910b0…`, Ghidra 12.1.4, sha256 `cc4bb4e1…`; o próprio envelope grava o preset
  `ghidra-default+disabled:<os 19 nomes>` (reproduzível). Nota: a rodada ainda usou as 2 GC
  threads hardcoded (o patch foi para o outro cache na hora) — mesmo assim ~2 min: os
  pesos-pesados desligados eram o gargalo todo (>55 min → ~2 min). Critério da noite completo;
  seção nova em `re-pc-ghidra.md` com a receita reproduzível.

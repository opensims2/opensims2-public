# Goal — Portar com fidelidade (OpenSims2 · Windows x64 + núcleo compartilhado)

- **Status:** **em andamento** — aberto em 17/10/2026; roda em **loop autônomo** por itens
  verificados; log no fim. Segue até o Artur mandar parar.
- **Mandato:** Artur — "Faça um goal para gente portar com fidelidade, fique rodando em
  looping para gente fazer história, vamos pra cima." (17/10/2026)
- **Branch:** `windows-x64-ghidra-pc` (17 commits à frente de `origin/main`). Um commit por
  item; mensagem única em inglês (padrão do repo para código) / pt-BR sem acentos (padrão dos
  commits de docs), prefixo de área.
- **Fonte de verdade:** docs de RE deste repo (`re-pc-ghidra.md`, `buy-build-mac.md`,
  `inventario-binario.md`, `*-mac.md`, relatórios) — nada se implementa "por memória": cada
  mudança aponta a âncora/medida que a justifica.

## Objetivo

Fechar, um a um e **com verificação**, os itens de fidelidade do port ao original, até:

1. `tools\build_windows.cmd` verde (build + os 39 ctest do Windows) **a cada item** — nada
   entra quebrado.
2. Residuais documentados fechados com evidência reprodutível (número medido, teste, diff
   determinístico, screenshot comparado à medida do doc).
3. Itens que exigem decisão do Artur marcados `[BLOQUEADO: …]` e seguidos adiante — o loop
   **nunca** fica parado esperando resposta.
4. Fila zerada ou bloqueada; cada ciclo deixa 1 bullet no log e (quase sempre) 1 commit.

## Protocolo de loop (o agente trabalha sem pedir licença entre passos)

1. **Escolher** o próximo item da fila. Prioridade: item em curso > verificável no Windows >
   RE de apoio (quando falta dado).
2. **Entender**: comportamento atual no código vs. original (doc/âncora de RE). Se faltar dado,
   virar mini-RE (Ghidra `pcb` ou consulta ao `re-pc-ghidra.md`).
3. **Implementar** a mudança mínima fiel; seguir o estilo do repo (K&R compacto, ~120 col;
   não existe clang-format).
4. **Verificar**: `cmake --build build-win` + `ctest --test-dir build-win --output-on-failure -j 8`;
   quando aplicável, harness (`s2pkg lot-run`/`autonomy` determinístico, `opensims2_win.exe
   --screenshot` + comparação com a medida do doc, soak curto). Anotar comando + resultado.
5. **Commitar**: 1 item = 1 commit (mensagem com os números).
6. **Logar** 1 bullet aqui e **voltar ao passo 1**.

Comandos do loop (Windows):

- build+testes: `tools\build_windows.cmd` (configura + compila + ctest) ou incremental:
  `cmake --build build-win` e `ctest --test-dir build-win --output-on-failure -j 8`.
- rodar: `jogar.cmd` / `jogar-bairro.cmd` (dados em
  `E:\Games\The Sims 2 Ultimate Collection\Double Deluxe\Base\TSData\Res`).
- screenshot: `build-win\opensims2_win.exe "<Res>" --screenshot frame.png` (sidecars
  `frame_*.png`/`frame_vm.txt` ao lado — gerados pelo próprio app).

Regras de convivência: com o Artur na máquina, **nada de Ghidra/RE pesado em paralelo** (ele
avisa quando liberar — "força total"). Untracked pré-existentes (`app_00.png`, `frame*.png`,
`frame_vm.txt`, `save-test/`, `vc140.pdb`) **não se toca**.

## Não-objetivos

- Sistemas que os docs marcam como "precisa de análise dinâmica" (ficam para sessão com o jogo
  aberto/interativo).
- Rodar os testes black-box de macOS (`tests/*.sh`) — só existem no alvo Mac.
- Refatoração de gosto; performance sem medição antes/depois.
- Mudar docs de RE sem achado novo.

## Fila inicial (ordem de ataque; reordenável pelo log)

- [x] **1. Cross-check REA** (herdado do `goal-ghidra-pc.md`): **FECHADO 17/10 22:38** — REA
      magro (12G + 19 analisadores desligados; ~2 min de análise) devolveu **3/3 âncoras exatas**:
      `TSCatalog` @ `0x0124AE10`, `FunctionSubsort` @ `0x012358AC`, print dos bins @ `0x01262534`;
      evidence `rea-crosscheck.success.json` (`ev_6b0910b0…` em `..\..\rea-snapshots\ghidra-pc\`).
- [x] **2. Baseline Windows**: **FECHADO 17/10 22:39** — `tools\build_windows.cmd`: configure ok,
      `ninja: no work to do` (build já em dia) e **39/39 ctest verdes em 3,29 s**. Sem skips.
- [x] **3. RE relâmpago** (Ghidra `pcb`): **FECHADO 17/10 22:57** — bins = **`FUN_00cdf160`**
      (criada 17/10; dump Frag/Bin, nome resolvido pela STR# `0xFB4CB0DE`); handlers:
      `0xBDF83B` (corpo `FUN_00bdf845` — begin/end_group ignorados; hash→mapa; warning de id
      duplicado), `FUN_00bdd8f2` e `FUN_00bde67d` (corpo `FUN_00bde687`); `FUN_0040d883` =
      `std::string::operator+=(const char*)`. Detalhes em `re-pc-ghidra.md` (§a/§b).
- [x] **4. Buy §1.6 — o painel na tela** (fechado 18/10): a sobra de 1 px na costura
      era leitura equívoca — o endcap (`0x4CC48AA4`, 23 px) fica em 532..555 cobrindo a emenda
      532..536 com o cap da janela em 537 (o slot do script +5) e a coluna 1 em
      589 = 537 + 52 (kCellInX 4→0, o canto (52,19) da própria janela) nas duas
      resoluções; a linha 1 desce para `altura − 129`; o trilho vai de 589 ao navy
      em 1207 (619 = colunas×48−5) e a arte do slider virou barra de cantos r=4
      (trilho com 1 px de navy no topo/base e o fill até as pontas; knob com 2 px de
      navy em todas as bordas e 1 px de (157,171,241) por dentro nas pontas);
      fechado, o endcap vai ao fim do sort (588) e o repeat (`0x4CC48AA6`) estica
      até lá (265..503); o pod sobe 3 px (a pílula em 516/396) em Buy, Build e Live.
- [x] **5. Buy §1.4 — "6 de 53"** (cadeiras B120): **FECHADO 21/10, sem depender da memória
      do Artur** — a captura do jogo mostra 53 cadeiras e o port conta 47; os 6 a mais
      **não vêm dos mods desta máquina** (censo medido): o `README_MODS.txt` do backup
      (`_MOD_BACKUP_20261016`) lista o que estava instalado — TS2 Extender (1.0.0 UC),
      Lighting Mod UC v1.4.0, `userStartup.cheat` e o tema Starship UI — e nenhum deles
      acrescenta objetos de catálogo: o `ObjectScripts.package` do Extender (instalado no
      SP9 e o original no backup) tem **0 OBJD** (só scripts: 497 recursos 0x9012468B e 65
      0x9012468A), o mod de luz só mexe em iluminação (os 4 "cadeiras" dos `Overrides` do
      SP9 são os GUID 128AC80D, B28AC9D7, 128AD07E e 128AC991 — os mesmos das 47, ou seja
      *overrides* de textura, não entradas novas), e o tema de UI não tem objetos. Logo os
      6 extras da captura eram conteúdo que esta máquina **não tem mais**: o `Downloads` do
      usuário (o Extender instalava `ld_*.package` em `Downloads\Gameplay` e o Starship UI,
      9 packages, em `Downloads\UI`; o backup guardou só os arquivos substituídos, não o
      `Downloads`). A leitura do knob da barra já estava feita (§1.4: 304 px de 619 numa
      lista de 26 por página → 53 itens). O port conta a instalação: 47 cadeiras e 1666
      compráveis.
- [x] **6. `inventario-binario.md` §5** — itens portáveis e verificáveis no Windows, em ordem
      de facilidade de verificação (ex.: fila — pessoa 164, evento 4 do ActionSkipped, testes
      `0x214`/`0x2e8`; reset do od 7; slots byte 14 do Look At; NGBH u32 `+0x34`/`+0x38`;
      registros `+0x60c` dos objetos da pessoa). Cada um = 1 commit.
      **FECHADO 18/10 (parte no upstream v0.5.0)** — o reset do od 7 (campo 45 do OBJD) e os
      slots byte 14 do Look At saíram no nosso branch; a **rodada n46 do upstream** ("VM,
      resto") levou os registros da pessoa (`cTSPerson +0x60c`), o 109/164, o
      `UpdatePersonDataFields` (37/38/40/55), o `cTSGlobals`/`cTSDataTables` e as sobras da
      fila (evento 4 do `ActionSkipped`, `0x214`/`0x2e8`, a troca da fila cheia `0x1FC`) — o
      núcleo é compartilhado, então o port Windows herda tudo no rebase; a **n47** ("Salvar,
      resto") mediu o `NGBH` (`+0x34`/`+0x38` = tiles, `+0xFD` um bool sem uso, o dono `0x22`
      só de leitura 1–15) e a transação do persist system.
- [ ] **7. `performance.md` "What is left"** — índice por tile, `notifyRoomChanges`/
      `tickOverlayEvents` por tick, "Set to Next" com `std::function`, texturas — com medidor
      antes/depois. **Um sub-item FEITO 21/10** — o `VM::tick` não reconsulta mais o mapa das
      threads: guarda os **iteradores** (os nós de um `std::map` são estáveis) num vetor que
      é membro da VM, e o `deleteGroup` mata a thread do objeto removido e a anota (ID +
      serial) para o tick a apagar no fim. Medido no Windows (`autonomy N001_Lot11 --ticks
      43200`): **22,3 s / 21,3 s → 19,5 s / 18,2 s**, com a saída **idêntica**; ctest 39/39.
      A mudança também conserta um **comportamento indefinido** (o `deleteGroup` apagava a
      thread do objeto removido enquanto ela rodava: agora ela é morta, como o jogo faz, e
      apagada no fim do tick) — por isso o `bhav-sweep` do AL muda em 3 das 46.880 árvores
      (18.625 → 18.622 true; o `autonomy` fica idêntico). O sub-item do "Set to Next sem
      `std::function`" foi **medido sem ganho** e descartado (`performance.md`).
- [ ] **8. Rapas pequenas**: `soak-report` (1 load error recorrente N002_Lot32), `audio.md`
      (SFX que soam como ruído — "sem nome para conferir"), itens corrigíveis do
      `live-qa-report.md`.
- [x] **9. Regressão `1020fd3` — BHAV 288**: **FECHADO 18/10 ~02:30** — não era alvo fora da
      contagem: o cabeçalho **0x8009** (Apartment Life em diante) tem **1 byte a mais** depois
      do *tree version*, e lido como 0x8007 o port desalinhava todas as instruções (o 288 =
      `0x0120` era a primeira palavra lida como alvo). Medido no `objects.package` do AL
      (46.832 BHAVs): **29.014** são 0x8009 e **todas** caem nos alvos com o byte pulado
      (nenhuma com o layout antigo; as 0x8007/0x8008 do mesmo pacote só caem com o layout
      antigo). Conserto em `src/formats/bhav.cpp` (+ teste em `test_formats.cpp`, comentário
      do cabeçalho em `bhav.hpp` e o achado na §1 do `primitives-mac.md`); o `bhav-sweep` do
      AL roda as **46.880 árvores / 11,4 M de instruções** sem nenhum erro de carga e o lote
      com as 17 árvores passa da carga — e mostra o **diálogo de boas-vindas do Pacote de
      Expansão (Vida de Apartamento)**, que antes nem carregava. Os irmãos `JobDataGlobals`
      0x1000/0x1001/0x1002 listam primitivas reais (`Manage Inventory`, `Random Number`,
      `Want Satisfy`). Aberto por isso (fila): 11, 12 e 13.
- [x] **10. Painel do Build vs capturas 30-34** (`orig2/`): **FEITO 21/10 pelo método do item
      4** — as capturas não existem nesta máquina (procuradas em C:, D: e E:), então o painel
      foi auditado contra a tabela do `buy-build-mac.md` §7.8 e contra o censo novo do
      catálogo de construção (`s2pkg catalog --installation --build`, o build type e o
      subsort de cada objeto). A auditoria achou **três lacunas**, corrigidas: a categoria
      **A800 jardinagem** inteira (não havia `case 8`), as **colunas BA20** e os **pincéis
      B751–B753**; o resto da tabela (portas, pisos, revestimentos, escadas, terreno,
      telhados, lareiras, piscinas, cercas, portões) bate com o port.
- [x] **11. Globais dos EPs** (achado do item 9): **FECHADO 18/10 ~03:55** — não era a
      costura das tabelas GLOB: as 17 árvores indexam um só espaço de globais (nada a
      juntar) e o `global 54 out of range` era o **tamanho do conjunto de variáveis** (o port
      usava os 54 do editor do base; os EPs leem além). Fechado pelo sub-item das contagens
      do item 6 — globais **60**, object data **114**, person data **231** — com o
      `bhav-sweep` do AL zerado e o lote das 17 árvores (com o diálogo do Vida de
      Apartamento) sem nenhum deles; números no log de 18/10 ~03:55.
- [x] **12. Primitivas `0x7E` (Run Script) e `0x7D` (Follow Sim)** (achado do item 9): o port
      registra as duas como "sem handler" (erro 0x05), mas o AL usa `0x7E` em controladores —
      no lote resetam `Controller - Fish Pond Effects` (`Sub - Remove All Effects` #6),
      `- Business - Customer Loyalty` e `- Employee Management` (`Function - Main`); 2.213
      casos no sweep. A §2 do `primitives-mac.md` dá os handlers **de routing aggregate**
      (`TryRunScript`/`TryFollowSim`, "só BV") — implementar com o gate de tipo de objeto do
      original. **RE do exe do PC feito (20/10, 21/10 e 22/10)**: as duas estão na **classe base**
      (não só no routing aggregate) e o handler do `0x7E` foi lido **inteiro em assembly**.
      O byte de flags (o byte 4 dos operandos) tem quatro bits: o **bit 0** pede também o
      `.objLua`, o **bit 1** é o escopo **privado** (`vfunc 0x108` do objeto), o **bit 2** o
      **semi-global** (`vfunc 0x114`) e o **bit 3** só o log dos parâmetros; sem eles o
      escopo é o grupo global (`0x7FD46CD0`). O pedido é **o mesmo do `Run Tree by Name`**:
      o id empacotado `(STR# << 16) | id` e a busca
      `(**(code **)(*gerenciador + 0xC))(&pedido[5], 0x89B7DC8, &achado, byte[0x25], 0)`.
      **A peça que faltava**: o STR# do id (304 no AL) é a tabela **"Lua Scripts"**
      (7.233 entradas; o 0x12F é "Call Named Tree prim string set") e o id do script é
      1-based nela — o AL bate ("NPC - Get Outfit GUID for Type in T[0,1]" é a entrada
      49). **Logo o `0x7E` roda um script Lua** (do `.objLua` do objeto), e a árvore de
      descrição com o mesmo nome é o próprio chamador (a global 0x0137 é uma única
      instrução que chama o próprio nome). O port (e o upstream) não tê interpretador
      de Lua, e trocar pela árvore de descrição seria recursão — a recusa (erro 0x05)
      é a resposta honesta. **FECHADO 22/10**: o RE completo está em
      `docs/re-pc-ghidra.md` §(g) e `docs/data-findings.md` §21, e a mensagem do erro
      0x05 agora diz por quê (sem Lua).
- [x] **13. TTAB 0x50/0x51/0x54/0x55 dos EPs** (achado do item 9): `interactions (TTAB) …:
      unsupported version 0x50/0x51/0x54/0x55` — as interações desses objetos ficam de fora
      (20 recursos só no `objects.package` do AL). Estender o leitor de TTAB às versões
      novas. **FECHADO 19/10** — 0x50/0x51 com o layout do 0x4E (276 recursos medidos) e
      0x54/0x55 com o layout do RE do exe do PC (46 bytes fixos + dois arrays de grupos de
      anúncios): o `decode-all` do `objects.package` do AL passa de 1.208 ok / 901 falhas
      para **2.090 ok / 19 falhas**; as 19 restantes são os 7 arquivos 0x53 (11 tabelas,
      layout ainda não fechado) e 12 com o `X` do segundo array ambíguo. 0x53 segue recusado
      com erro claro.
- [x] **14. Vizinhanças e lotes do AL** (achado do sub-item das contagens): **FECHADO 20/10** —
      o lote E001 do AL não carregava - `NGBH: unsupported version 0xCB` (o port conhece 0x6F),
      `FAMI: unsupported version 0x55` (conhece 0x4E), `SDSC: unsupported version 0x36` (conhece
      0x20), `NID: 20 trailing bytes` e `lot info: apartment base version not supported`
      (`lot-run` de 18/10 em `E001_Lot127`); os leitores foram estendidos medindo nos 20
      pacotes do UC: o FAMI aceita 0x4E..0x55, o SDSC 0x20..0x36 (com o person data por
      versão), o NGBH 0x6F/0x70/0xBE/0xC2/0xCB, o NID 3..0x0A e o lot info 18 com a base 11.
      Medido: o `hood-sweep` do E001 passa de 0 Sims / 0 famílias / 0 lotes / 313 erros para
      **234 Sims / 21 famílias / 56 lotes / 0 erros**, e o `lot-run` do `E001_Lot127` carrega
      485 objetos (`data-findings.md` §20).

## Critérios de sucesso

- [ ] Critério do goal anterior fechado (cross-check REA de ≥1 achado, evidence JSON).
- [ ] Build + ctest Windows verdes **ao fim de cada item** (evidência no log).
- [ ] §1.6 fechado com screenshot; §1.4 fechado ou mantido com a resposta do Artur.
- [ ] RE relâmpago (bins/handlers/tsdata) documentado em `re-pc-ghidra.md`.
- [ ] ≥1 item do §6 e ≥1 do §7 fechados com medição.
- [ ] Fila atualizada a cada ciclo; nenhum processo pendurado ao final de cada ciclo.

## Riscos

- Ghidra/REA ocupam a máquina → RE pesado só solo (ou com aval do Artur).
- Um item pode "abrir buraco" (depende de sistema ainda não portado) → marcar bloqueado e seguir.
- Sem os dados do jogo em `E:\…\Base\TSData\Res`, verificação de runtime fica limitada ao
  `s2pkg`/testes (anotar quando isso ocorrer).

## Log

- (17/10 ~21:50) Goal aberto; fila montada a partir de `goal-ghidra-pc.md` (pendências),
  `buy-build-mac.md` §1.4/§1.6, `inventario-binario.md` §5 e `performance.md`. O item 1 já está
  em curso (cross-check REA).
- (17/10 ~22:26) Retry #2 do cross-check cortado aos 55 min ainda no meio da análise — o REA
  rodou ~5-6× mais lento que o run manual do `pcb` (17,4 min); culpados prováveis: GC/JIT
  hardcoded em 2 threads e os pesos-pesados ligados (Callee Purge 255 s, Data Reference/
  Reference ~212 s cada...). Tentativa #3 lançada ~22:36 com 19 analisadores desligados +
  watcher `rea-watch-r3.log` (CPU a cada 30 s + cópia do `ghidra.log` do runtime + kill em
  stall); o patch de threads do cache do npx pegou o cache errado (há dois) — replicado no
  certo, a #3 ainda roda com 2 threads. Item 1 segue em curso.
- (17/10 ~22:38) **Item 1 FECHADO**: cross-check REA deu **3/3 âncoras exatas** em ~2 min de
  análise (bullet no `goal-ghidra-pc.md` + seção nova em `re-pc-ghidra.md`); evidence
  `rea-crosscheck.success.json`. O `goal-ghidra-pc.md` fechou (status CONCLUÍDO).
- (17/10 ~22:39) **Item 2 FECHADO**: baseline Windows — `tools\build_windows.cmd` com configure
  limpo, `ninja: no work to do` e **39/39 ctest verdes (3,29 s)**; sem skips. Próximo: item 3
  (RE relâmpago no `pcb` — consultas curtas; máquina livre).
- (17/10 ~22:40-22:57) **Item 3 FECHADO** (RE relâmpago no `pcb`, 6 rodadas headless curtas):
  - **Bins** (`0xCDF41B`+ → era a última pendência do goal (a)): os formatos `Bin Prod ID/...`
    vêm da **`FUN_00cdf160`** (entrada `0xCDF160`, SEH) — dump de Frag/Bin: chave de 12 bytes
    `0x%08X…` ou nome resolvido pela STR# do catálogo (hash `0xFB4CB0DE`); Frag/Bin Prod
    ID/Sort Idx/Type; fonte = vtbl `+0xC/+0x4C/+0x54/+0x64/+0x6C`, sink = vtbl `+0x20/+0x4C`.
  - **Handlers**: `0xBDF83B` (DBSegments; corpo `FUN_00bdf845` = ignora begin/end_group; hash
    do id → mapa; warning `duplicate id %08x = %s` em `TSAudioResourceManager.cpp:0x477`);
    `FUN_00bdd8f2` (SampleData/TrackDef); `FUN_00bde67d` (MusicDirectories; corpo
    `FUN_00bde687`). `FUN_00e5c86c` = `__SEH_prolog` (prólogo comum `MOV EAX,<handler>; CALL`).
    Registro do callback em `FUN_00bdf973` (`[vtbl+0x34](…, 0xBDF83B, 0x142C7F4)`).
  - `FUN_0040d883` = `std::string::operator+=(const char*)` (strlen + append em
    `FUN_00405f2a`) — monta os paths `"tsdata/"`/`"/tsdata/res/locale/"`.
  - Toolkit novo em `rea-snapshots\ghidra-pc\scripts\`: `RedoFrom.java` (limpa garbage e
    re-disassembla a partir de uma entrada) e `ExtendFn.java` (redefine o body). Lição:
    função com `__SEH_prolog` decompila só até o `CALL` do prólogo — o corpo sai criando
    função nova depois do prólogo (encolher o body do wrapper com `ExtendFn` + `MakeFns`).
  - Edits em `re-pc-ghidra.md` (§a Bins + §b handlers/`FUN_0040d883`); build+ctest
    **39/39 verdes (2,19 s)**.
- (17/10 22:57) Próximo da fila: **item 4** — §1.6 do `buy-build-mac.md`, o pixel da costura
  do catálogo (532×533); medir com `--screenshot` no Windows.
- (18/10 ~01:30) **Item 4 FECHADO** — §1.6 medido e corrigido no app e no doc. As capturas
  do jogo (800x600 e 720) saem com o cliente em +(3,29) (o port, em (0,0)): aplicando o
  offset, o catálogo está 5 px à direita do slot do script (cap em 537, não ≈533), a coluna
  1 em 589 = 537 + 52 (kCellInX 4→0 — o "+4+8" antigo era o canto da janela da coleções), a
  linha 1 em `altura − 129` (não −126) e o trilho de 589 ao navy em 1207 (619 = colunas×48−5,
  não 591..1211/620). O endcap (`0x4CC48AA4`) fica em 532..555 cobrindo a emenda 532..536 (o
  joint `0x4CC48AA8` não desenha em estado nenhum) e, fechado, vai ao fim do sort (588) com o
  repeat (`0x4CC48AA6`) esticado do 265 até lá; o pod sobe 3 px em Buy, Build e Live (a
  pílula em 516/396 sobre os 519/399 do script). A arte do slider (achada na verificação) não
  era um estádio: é uma barra de cantos r≈4 (a coluna extrema abrange 8 das 12 linhas) —
  trilho com 1 px de navy no topo/base e o fill até as pontas, knob com 2 px de navy em todas
  as bordas e 1 px de (157,171,241) por dentro nas pontas. Verificação: capturas do port em
  720 (aberto — `cmp5` com o slider novo — e fechado) contra o jogo e o script; o botão do
  grid bate linha a linha (coluna da cadeira 591..616 = 500..525 do jogo) e a barra fecha no
  mesmo navy (605 no cliente) nos dois; diferenças de tom pequenas (arte Mac × Win). Falsos
  positivos derrubados: o "verde × azul" dos subsorts do jogo (é o painel 0xB100 do Seating
  com o B120 selecionado vs o port forçando Decoração — estados diferentes) e o "frame 0" do
  Comfort (o jogo desenha o frame 1 azul como o port). Fica de polimento: as bevels do knob e
  o raio das placas (as artes do jogo divergem ±1 px nas colunas extremas). build+ctest
  39/39. Próximo: item 9 (regressão `1020fd3`), depois 5 (bloqueado) / 6 / 7 / 8 / 10.
- (18/10 ~02:30) **Item 9 FECHADO** — a regressão da carga com as 17 árvores não era alvo
  fora da contagem: o cabeçalho **0x8009** tem 1 byte a mais depois do *tree version* e o
  leitor (que já lia 0x8007) desalinhava todas as instruções. Varredura do `objects.package`
  do AL: 29.014 de 46.832 árvores são 0x8009, todas caem nos alvos com o byte pulado e
  nenhuma com o layout antigo; conserto em `bhav.cpp` + teste em `test_formats.cpp`
  (build+ctest **39/39**). Verificação: `bhav-sweep` do AL = 46.880 árvores / 11.379.667
  instruções, zero erro de carga (17.589 verdadeiros, 15.531 falhas de semântica, 8.733
  falsos, 5.027 yields); o lote com o Double Deluxe + as 17 árvores (sem junction) passa da
  carga e mostra o **diálogo de boas-vindas do Vida de Apartamento**. Achados enfileirados:
  11 (globais dos EPs), 12 (`0x7D`/`0x7E` em routing aggregates), 13 (TTAB 0x50+). Próximo:
  6 (seguido de 7 / 8 / 10; 5 bloqueado).
- (18/10 ~02:38) **Item 6, 1º sub-item** — "reset do od 7": já estava feito (o Reset após um
  erro grava `data[7]` = campo 45 do OBJD, `GetDefaultNicenessMultiplier` vtable `0x578`, desde
  a rodada 14), mas o comentário do `resetAfterError` ainda dizia que o 7 "fica" e a linha dos
  Erros do `inventario-binario.md` ainda dava como faltando. Pinado:
  `testResetAfterError` agora tem campo 45 = 250, semeia `data[7]` = 999 e confere 250 depois do
  reset (build+ctest **39/39**); comentário e nota corrigidos. Resto do item 6: os slots do
  Look At (byte 14), depois 164 / evento 4 / `0x214`/`0x2e8` (precisam de RE do binário Mac),
  NGBH `+0x34`/`+0x38` e os registros `+0x60c`; depois 7 / 8 / 10 (5 bloqueado).
- (18/10 ~03:55) **Item 6, sub-item das contagens do interpretador (fecha o item 11)** — o
  "global 54 out of range" dos EPs não era a costura das tabelas GLOB: as árvores indexam um
  só espaço de globais e o que faltava era o **tamanho dos conjuntos de variáveis**, que o
  port tinha pelos rótulos do editor do base (88/54/170). Com esses tamanhos o `bhav-sweep`
  do AL acusava **460 globais** (456 + 4 em árvores de um tick), **1.882 de person data**
  (1.642 + 141 + 98 + 1; + 15 de person data de vizinho) e **264 de object data** (157 + 59 +
  47 + 1) fora do intervalo; o BV compilado (0x71/0x3A/0xCC = 113/58/204, §4j do
  `primitives-mac.md`) também não bastava. Medido subindo os tamanhos até a mesma varredura
  (`S2PKG_SWEEP_REASON=out of range`) zerar: os **maiores acessos** das árvores do AL são
  object data **113**, global **59** e person data **230** → arrays **114 / 60 / 231**
  (`vm.hpp`), e o `OpenTS2` dimensiona object data e globais em **114 e 60** também. Depois:
  sweep do AL com 46.880 árvores / **13.054.928 instruções** (contra 11.379.667 — +14,7%
  rodadas a mais em vez de falhar) e **zero** "out of range" desses donos; N001_Lot11 com 627
  objetos e 0 falhas em 60 ticks; e o lote 30x20 com as **17 árvores** no app (17 resource
  trees, o diálogo do Vida de Apartamento) com **3** erros de SimAntics — só os três `0x7E`
  do item 12 (os três "global 54" sumiram). `testVariableSets` pina 0x71/0x3B/0xE6 válidos e
  0x72/0x3C/0xE7 fora (build+ctest **39/39**). Achado enfileirado: **14** (NGBH 0xCB / FAMI
  0x55 / SDSC 0x36 / NID +20 / lot info de apartamento). Próximo: os slots do Look At (byte
  14); depois 7 / 8 / 10 (5 bloqueado).
- (18/10 ~07:45) **Item 6, sub-item dos slots do Look At (byte 14)** — o byte 14 escolhe a
  família de slots do alvo e o byte 8 o índice dentro dela: **1 → tipo 4, 2 → tipo 3, 3 →
  tipo 0** (o `SlotID` do pedido `cAnimRequest` empacota os dois, `slot | tipo << 12`); passou
  da contagem daquele tipo, erro **0x1B** (antes o port mirava a caixa do alvo nos três
  casos). Medido com a sonda nas varreduras do AL (8 Look Ats), do base (7) e no N001_Lot11
  (2): **nenhum índice fora da contagem** — e o censo dos **17 `objects.package`** do UC
  (**4.121 SLOTs distintos**, 35.426 leituras — os pacotes dos EPs/SPs repetem: um objeto só
  usa os tipos 0/3/4; o 1 só aparece em **quatro objetos** — Baby Bottle 7FBA6191, Baby
  Bottle - Smart Milk 7F228D60, Toy - Paper Airplane 7F385F33 e Newspaper 7F08E23C — e o 2 em
  nenhum). O censo estático (a passada do sweep, sem executar) conta **316 leituras com tipo**
  no AL (157 de tipo 1, 124 de tipo 3, **nenhuma de tipo 2**) e **200** no base: o maior
  índice de tipo 3 é **37** e só os dois tabuleiros de xadrez (**67** entradas de tipo 0) o
  cabem. O avião de papel tem Look Ats próprios com índice 0 nos dois tipos, dentro das
  contagens; o tipo 1 (o "location" do nome Sims 1) fica **fora** da mira — aberto: se a mira o
  incluísse, o ponto do avião seria o `(0,0,1)` do entry em vez do osso. Verificação com o
  código final: AL sweep 46.880 árvores /
  13.054.928 instruções com o resumo de falhas **idêntico** ao run das contagens (nenhuma
  árvore de Look At nas falhas; a sonda removida), N001_Lot11 627 objetos / 0 falhas em 60
  ticks, `testLookAtSlots` (6 casos: os três tipos, os dois erros 0x1B e o byte 14 = 0) —
  build+ctest **39/39**. No app o ponto do slot vai por `objectSlotPoint` (osso por nome, STR#
  `0x90`, os desenhados de um Sim inclusive; senão o deslocamento). Achado enfileirado: o
  **`RayTrace` (0x78)** usa a mesma convenção de slot — **FECHADO 20/10** (o port resolvia o
  ponto pelo tile do objeto): agora o 0x78 procura o slot do mesmo jeito que o Look At
  (byte 4 = 1/2/3 → os SLOT de tipo 4/3/0, byte 5 = o índice dentro deles, pasto a
  contagem erro 0x1B; o alvo nos bytes 11/12), o teste `testRayTraceSlots` cobre o slot, o
  caso sem slot e o 0x1B, e o sweep do AL passa a dar os 4 erros 0x1B que o jogo dá (as
  árvores globais que pedem um slot que o objeto não tem). Próximo: 164 / evento 4 /
  `0x214`/`0x2e8` (RE do binário Mac), NGBH u32 `+0x34`/`+0x38` e os registros `+0x60c`;
  depois 7 / 8 / 10 (5 bloqueado).
- (18/10) **Fila de fechamento aberta em `goal-fechar-pendencias.md`**: item 13 (TTAB;
  0x4F..0x51 prontos para portar e o modelo 0x53+ medido até `X×(4+6Y)`), depois 8 / 14 /
  7 / 10 / RayTrace / 12; 5 e 6-resto seguem bloqueados.

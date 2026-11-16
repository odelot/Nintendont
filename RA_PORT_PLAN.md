# RetroAchievements para GameCube via Nintendont — Análise de viabilidade e plano

> Status: **análise / plano** (2026-06-13). Nenhum código escrito ainda.
> Linha de base: instalar Nintendont **stock** e testar jogo via disco ANTES de começar,
> para ter uma versão-controle funcional para comparar.

## TL;DR — Veredito

**Viável, e em vários aspectos mais simples que o port do Wii.** A arquitetura existente
(Starlet lê a RAM do jogo → fala com o ESP32 por EXI → ESP roda rcheevos) mapeia ~1:1 para o
Nintendont. O Nintendont é essencialmente "o seu d2x-cios" para GameCube: um kernel ARM
rodando no Starlet, ao lado do jogo, com acesso total ao hardware.

**O protocolo EXI e o firmware do ESP NÃO precisam mudar estruturalmente.** O ESP32 carrega
jogos via `rc_client_begin_load_game(hash)` — é **agnóstico de console**. Se o Nintendont
mandar o hash RA correto do jogo GC, o servidor RA resolve o jogo e devolve os achievements
com endereços já no espaço de memória do GC. Mesmo ESP, mesmo protocolo.

## 1. Como o Wii funciona hoje (mapeamento)

- **Loader PPC**: WiiFlow + `ra_exi.c` + `ra_hash.c` — calcula o hash RA do disco, faz
  IDENTIFY + LOAD_GAME + poll até GAME_LOADED **antes** do IOS reload (enquanto detém o EXI).
- **Servidor de memória ARM**: `ra-module` dentro do d2x-cios (Starlet, **modo USER**) — a cada
  frame: lê MEM1/MEM2, manda SNAPSHOT, responde ADDR_QUERY, processa WATCHLIST_*.
- **Cérebro**: ESP32 `wii-ra-adapter` (escravo SPI no Slot B / EXI canal 1) — roda rc_client,
  mantém watchlist, dispara achievements.

Dores caras no Wii que vêm de "módulo IOS roda em modo usuário":
- Todo acesso a registro Hollywood teve que passar por `Swi_MLoad` (SVC) senão crash.
- `.rodata` não carregava (literais viravam zero; workaround stack-init).
- Coexistência de barramento EXI com d2x/IOS (EXI_Lock, contenção de slot mainIOS).

## 2. Mapeamento Wii → Nintendont

| Conceito (Wii) | Equivalente no Nintendont | Mudança |
|---|---|---|
| ra-module (módulo IOS, USER mode) | Código novo no **kernel ARM** (`kernel/`) | Roda em **SVC/kernel** → `read32`/`write32` direto. **Elimina `Swi_MLoad`** |
| `exi.c` via SWI | Driver EXI trivial em `0xd806800` | EXI real (Hollywood) está **livre** no Nintendont |
| `.rodata não carrega` | Linker normal (`kernel.ld`) carrega `.rodata` | Pode usar strings/const normalmente |
| `ra_read_ppc_byte()` lê MEM1 direto | Ler MEM1 do GC com `sync_before_read` (Starlet só acessa alinhado 32B) | **Coerência de cache = risco #1** |
| Pacing HW_TIMER | Mesmo `HW_TIMER` (0xd800010) + `udelay`/`mdelay` | Idêntico |
| Hash `rc_hash_wii_disc` | Hash GC `rc_hash_gamecube` da ISO | Algoritmo **bem mais simples** (sem cripto) |
| LOAD_GAME antes do IOS reload | LOAD_GAME em **thread de background**, jogo já rodando | **Não há reload** → handshake "preguiçoso", não trava o boot |
| Phase B INT (latch `EXI_CSR` bit1) | Polling de `EXI1_CSR` bit 1 direto | Idêntico |

O Nintendont resolve "de graça" três das maiores dores do Wii: modo kernel, .rodata, EXI livre.

## 3. As 4 diferenças críticas (onde mora o trabalho)

### 3.1 Coerência de cache de MEM1 — RISCO #1
O jogo (PPC) escreve em MEM1 pela própria cache. Para o ARM ver valores atuais:
- ARM: `sync_before_read()` antes de cada leitura. O Nintendont já usa isso e comenta em
  `kernel/common.c` que *"Starlet só acessa MEM1 em blocos de 32 bytes; leituras 8/16-bit dão
  resultado imprevisível"* (helpers `R32`/`R16`).
- PPC: idealmente um flush no VBlank.

No Wii você leu MEM1/MEM2 "direto" e funcionou empiricamente (milestone Kirby). Aposta: o mesmo
vale no GC lendo via `R32` + `sync_before_read` em vez de `*(volatile u8*)`. **Validar em HW na
Fase 1.** Mitigação: codehandler Gecko (ver 3.3).

### 3.2 Barramento EXI — confirmar que está livre
- Nintendont **emula** memory cards por software: faz *patch* das instruções do jogo para
  redirecionar acessos EXI para um bloco falso em `0x13026800`. **Nunca** dirige o controlador
  EXI real (`0xd806800`). Refs: `kernel/EXI.c`, `kernel/Patch.c` (DisableEXIPatch em ~1659).
- Logo, o Slot B físico (EXI canal 1) — onde o ESP está plugado — fica ocioso; o kernel pode
  dirigi-lo como o ra-module faz. O jogo nunca "vê" o ESP; o ESP nunca "vê" o jogo.
- VERIFICAR em HW: nada (Nintendont nem IOS subjacente) sonda o EXI real do canal 1 no boot.
- CUIDADO com `HW_EXICTRL` (0xd800070): no Wii escrever nesse bit travava o kernel (compartilhado
  com o DI, que o Nintendont usa muito). **Ler primeiro; só escrever se necessário, com teste.**

### 3.3 Sincronização de frame
Nintendont não tem VBlank natural no ARM (loop principal roda livre com `udelay(20)`). Opções:
- **(A) Pacing por HW_TIMER ~60Hz** — modo `USE_TIMER_PACING` já em produção no ra-module.
  Mais simples, zero código PPC. **Começar por aqui.**
- **(B) Hook VBI via Gecko code** — Nintendont **tem engine Gecko** (`codehandler/`, hooks em
  `OSSleepThread`/`PADHook`). Injetar C0 code que incrementa contador em MEM1 no VBlank (igual
  ao PPC VBI hook do Wii); ARM faz polling. Bônus: ponto de flush de cache (ataca 3.1).

### 3.4 Hash RA do GameCube
`rc_hash_gamecube` (`rcheevos/.../src/rhash/hash_disc.c:512`) é muito mais simples que o do Wii:
1. Confere magic word `0xC2339F3D` em 0x1c.
2. Hasheia o cabeçalho da partição (`0x2440` + apploader header/body/trailer).
3. Lê offset do `main.dol` em 0x420; hasheia os 18 segmentos do DOL (7 code + 11 data).

Sem cripto, sem tabela de partições; `part_offset=0`, `wii_shift=0`. Port direto lendo a ISO
pelas funções que o Nintendont já tem (`ISO.c`/`FST.c`/`DI.c`).

## 4. A única mexida no ESP (NÃO é mudança de protocolo)

Hoje o handler `RA_CMD_LOAD_GAME` (`wii-ra-adapter.ino:~1288`) **ignora** o `md5_hash` que já
existe no protocolo e usa tabela estática `wii_lookup_game_hash(gid)`. O campo
`has_hash` + `md5_hash[32]` **já está** no `gc_ra_protocol.h` e o WiiFlow já transmite.

Mudança recomendada: **quando `has_hash==1`, usar o hash da wire**; senão, tabela.
- NÃO é mudança de protocolo (campo já existe).
- Firmware-only, ~5 linhas.
- Melhora **também** o Wii (jogos fora da tabela passam a funcionar).
- ESP segue agnóstico de console — `rc_client_begin_load_game(hash)` faz o resto.

Verificar de passagem se `rx_len` no LOAD_GAME chega com os 40 bytes (comentário do .ino diz
"WiiFlow só manda 8 bytes" — parece desatualizado; `ra_exi.c` escreve a struct inteira).

## 5. Plano por fases

**Fase 0 — Provas de vida (de-risk)**
1. Add `kernel/ra_module.c` ao `OBJECTS` do `kernel/Makefile`; thread (`do_thread_create`,
   padrão do `DI_Thread` em `main.c`) que pisca LED/GPIO. Confirma build + thread viva.
2. Driver EXI mínimo (`read32`/`write32` em `0xd806814`/`...820`/`...824`) → `IDENTIFY` contra o
   ESP no Slot B, confirmar `RA_DEVICE_ID`. Valida barramento real livre e dirigível.

**Fase 1 — Leitura de MEM1 + SNAPSHOT (risco #1)**
3. `ra_read_gc_byte(addr)` = `sync_before_read` + `R32(addr & 0x7FFFFFFF)` + extrair byte
   (endereços do rcheevos para GC vêm base-0 em MEM1, como no Wii).
4. Portar `ra_send_snapshot`/`ra_send_addr_response`/`ra_parse_response` ~verbatim do `main.c`
   do ra-module (máquina de estados, WATCHLIST_APPEND/REMOVE, Phase B). **Validar em HW que os
   valores lidos batem com a RAM real** (ler contador/score conhecido e ver mudar).

**Fase 2 — Hash + handshake**
5. Portar `rc_hash_gamecube` para `kernel/ra_hash.c` lendo a ISO via funções internas.
6. Thread de background no boot: hash → IDENTIFY → LOAD_GAME (`has_hash=1`) → poll GAME_LOADED →
   GET_WATCHLIST_CHUNK. Em paralelo ao jogo (sem travar boot).
7. Mexida ~5 linhas no ESP para honrar `md5_hash`.

**Fase 3 — Pacing e robustez**
8. Loop SNAPSHOT 60Hz via `HW_TIMER` (modo A). Achievement end-to-end num jogo simples.
9. Se cache instável: hook VBI Gecko (modo B) via codehandler p/ sync de frame + flush.

**Fase 4 — Integração/UX**
10. Config flag no Nintendont (lig/deslig RA), exibição de unlock, conviver com memcard emu
    (garantir que ESP no Slot B não conflite com a config do usuário).

## 6. Questões em aberto / decisões

1. **Onde computar o hash**: kernel ARM (auto-contido, recomendado) vs loader PPC (UI). → kernel ARM em background.
2. **Slot B com memcard**: emulação é software, não conflita com ESP físico no Slot B.
3. **Validar HW_EXICTRL** sem perturbar o DI do Nintendont.
4. **Cobertura de jogos**: escolher jogo-alvo GC com set RA ativo para o teste inicial (análogo ao Kirby do Wii).

## Referências de código (arquivos-chave)

### Nintendont (C:\dev\gamecube\Nintendont)
- `kernel/main.c` — loop principal (~334-536), criação de thread (`DI_Thread` ~242), HW_TIMER
- `kernel/common.c` — `R32`/`R16`, `udelay`/`mdelay`, acesso MEM1 alinhado 32B
- `kernel/global.h` — `read32`/`write32`, tipos u8/u16/u32, `sync_*_align32`, `P2C` (& 0x7FFFFFFF)
- `kernel/EXI.c` / `kernel/Patch.c` — emulação EXI (bloco falso 0x13026800), DisableEXIPatch
- `kernel/Makefile` — lista explícita `OBJECTS` (add .o aqui); toolchain devkitARM, gnu89, -O3 BE
- `kernel/kernel.ld` — code@0x12F00000(384K), data@0x12F60000(512K) — espaço de sobra
- `codehandler/` — engine Gecko (hooks OSSleepThread/PADHook) para o modo VBI (B)
- `loader/source/main.c` (~688-697) — carrega kernel.zip em 0x92F00000

### Wii (a portar)
- d2x ra-module: `C:\dev\wii\d2x-cios\source\ra-module\{main.c,exi.c,gc_ra_protocol.h}`
- EXI real Hollywood: `EXI_REG_BASE 0xd806800`, canal 1 = +0x14 (CSR/MAR/LEN/CR/DATA em +0/4/8/c/10)
- WiiFlow: `source/retroachievements/{ra_exi.c,ra_hash.c}`, `source/menu/menu_game_boot.cpp` (~1103-1129)
- ESP: `C:\dev\wii\wii-ra-adapter\wii-ra-adapter.ino` (LOAD_GAME ~1288, `loadGame`/`rc_client_begin_load_game` ~2235/2258)
- rcheevos GC hash: `C:\Users\ftole\Documents\Arduino\libraries\rcheevos\src\src\rhash\hash_disc.c` (`rc_hash_gamecube` 512, `rc_hash_nintendo_disc_partition` 415)

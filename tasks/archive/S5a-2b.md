# Arşiv — S5a-2b: CLI, Ctrl+C, gerçek süreç testleri

> `tasks/todo.md`'den **olduğu gibi** taşındı (2026-11-22). Bu dosya tarihsel kayıttır;
> güncel durum ve sıradaki iş için `tasks/todo.md` indeksine bak.

##### S5a-2b — CLI, Ctrl+C, gerçek süreç testleri

**Amaç:** Kullanıcı yüzeyi: `--no-resume`, resume bildirimi, Ctrl+C ile iptal; gerçek süreç öldürme ve
Windows Ctrl+C sonrası devam.

**CLI**

- [x] `get --no-resume` (değersiz bayrak) → `options.resume=false`; yardım metni.
- [x] `on_resume` bağlanır: `Resuming from %.1f MiB` (bayt / 1 MiB) `stderr`'e, ilk progress'ten önce.
- [x] Ctrl+C: `run_get` artık `cancel` jetonu geçirir.
  - Windows: `SetConsoleCtrlHandler`; `CTRL_C_EVENT` + `CTRL_BREAK_EVENT`; ilk olayda yalnız
    `std::atomic<bool>::store(true)` (ayrı thread; dosya/log/kilit yok) ve `TRUE`; ikinci olayda
    `FALSE` → varsayılan davranış.
  - POSIX: `sigaction(SIGINT)`; handler yalnız `std::atomic<bool>` (lock-free, `static_assert`) ve
    `volatile sig_atomic_t` sayaç kullanır; ikinci sinyalde `SIG_DFL` + `raise(SIGINT)`. Dosya/log/kilit yok.
  - İptal → `aborted`, checkpoint yazılır, çıkış kodu **130**; handler indirme sonunda kaldırılır.
- [x] `summary.http.error == aborted` → 130; diğer hatalar 1; kullanım hatası 2 (mevcut).

**Süreç testleri (`tests/integration/process_resume_test.cpp`, ayrı CTest girdisi)**

- [x] CLI yolu `FLOWGET_CLI_PATH` derleme tanımı / CTest env ile verilir; test hedefi `flowget-cli`'ye
      bağımlıdır.
- [x] **Öldürme testi (Windows `TerminateProcess`, POSIX `SIGKILL`):** yerel throttle'lı sunucu;
      çocuk süreç başlatılır; **sabit sleep yok** — sidecar her 50 ms yoklanır, `bytes_done()` belirli
      eşiğe (ör. toplamın %20'si) ulaşınca öldürülür (zaman aşımı 60 s). Sonra aynı komut yeniden
      çalıştırılır; `exit=0`, kaynak/hedef SHA-256 eşit, sidecar silinmiş.
- [x] **Windows Ctrl+C:** çocuk `CREATE_NEW_PROCESS_GROUP` ile başlatılır; sidecar eşiğe ulaşınca
      `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid)` gönderilir; çıkış 130 beklenir; ardından
      resume ile SHA-256 eşit. **Gerekçe:** `CTRL_C_EVENT` belirli bir süreç grubuna gönderilemez
      (grup 0 tüm konsolu etkiler); `CTRL_BREAK_EVENT` gönderilebilir ve CLI iki olayı aynı handler'da
      işler. Test süreci konsolsuzsa (`GetConsoleWindow()==nullptr`) `AllocConsole()` denenir;
      `GenerateConsoleCtrlEvent` yine başarısız olursa test `GTEST_SKIP()` ile gerekçeli atlanır —
      alternatif kapsam: handler birim testi + `TerminateProcess` öldürme testi (OS teslimi hariç).
      **Atlama görünür olmalı:** `GTEST_SKIP() << "reason"` (logda `SKIPPED` satırı) ve rapor/kanıt
      tablosunda "Windows Ctrl+C otomatik kanıtsız, elle doğrulanacak" notu; sessiz skip yasak.
- [x] **POSIX Ctrl+C (opsiyonel ama ucuz):** `SIGINT` gönder → 130 → resume → SHA-256 eşit.
- [x] **`--no-resume` CLI:** geçerli sidecar + kısmi dosya varken `--no-resume` → sidecar silinmiş,
      taze indirme, hash doğru.
- [x] **`Resuming from ...` çıktısı:** kısmi indirme sonrası ikinci çalıştırmanın `stderr`'inde satır
      var; MiB değeri `resumed_from_bytes` ile tutarlı.
- [x] Kanıt: Windows `ctest` (process testleri dahil), `check_linux.ps1` (POSIX SIGINT/SIGKILL),
      pre-commit, CLI e2e çıktısı, clang-tidy baseline.

**Riskler (S5a-2)**

- 2 s checkpoint + fsync 100 MB/3,66 s hedefinde maliyetli olabilir; ölçüm %5'i aşarsa önce gereksiz
  flush'lar kaldırılır, sonra interval kararı kullanıcıya sorulur (şart 2 s olduğu için sessizce
  değiştirilmez).
- Windows'ta `FlushFileBuffers` + antivirüs sidecar rename'ini geciktirebilir; checkpoint hatası
  ölümcül değil, sonraki tetikte yeniden denenir.
- `Last-Modified` 1 s çözünürlüklü; aynı saniye içinde değişen dosyada eski 206 gelebilir. Güçlü ETag
  tercih edilir; bu bilinen sınır S5b/S8 hash doğrulamasıyla kapatılır.
- Süreç testleri CI'da konsol/zamanlama nedeniyle kırılgan olabilir; durum tabanlı tetik + zaman aşımı
  + gerekçeli skip.
- İkinci Ctrl+C checkpoint yazmadan süreci sonlandırır (bilinçli; varsayılan davranış).

**DoD (S5a-2)**

- S5a-2a: yukarıdaki API/cancel testleri yeşil; flush invariantı kanıtlı; 100 MB performans ≤ ~%5;
  `check_linux.ps1` üç preset `exit 0`; clang-tidy baseline 6.
- S5a-2b: `--no-resume`, resume satırı, Ctrl+C 130, gerçek öldürme + Windows Ctrl+C sonrası SHA-256
  eşitliği; process testleri yeşil; `check_linux.ps1` yeşil.
- Her iki dilim ayrı commit; push kullanıcıda.

**S5a-2b kanıtı (2026-11-22):** commit'ler `5956c89` (CLI bayrağı + resume satırı), `73afc9c`
(Ctrl+C + 130), `6bcb0ac` (gerçek süreç testleri).

- **Windows:** `cmake --build --preset windows-x64` + `ctest --preset windows-x64` → **3/3 passed**
  (unit 0,18 s; integration 26,66 s; process 15,94 s). Süreç testleri:
  `KillAndResumeMatchesTheSourceHash` (sidecar %20'de `TerminateProcess` → aynı komut → hash eşit),
  `InterruptExitsWith130AndResumes` (`CTRL_BREAK_EVENT` → **130** → resume → hash eşit),
  `NoResumeDeletesTheSidecarAndStartsOver` (geçerli sidecar + kısmi dosya → `--no-resume` → sidecar
  silinmiş, taze indirme, hash doğru). Bu makinede konsol `CTRL_BREAK`'i teslim etti; konsol
  reddederse test gerekçeli `GTEST_SKIP()` satırı basar (sessiz atlama yok) ve öldürme testi
  resume'u kanıtlamaya devam eder — o durumda Windows Ctrl+C **otomatik kanıtsız, elle
  doğrulanacak**.
- **Linux:** `scripts/check_linux.ps1` → **PASSED (7,4 dk)**; `linux-x64`, `linux-x64-asan`,
  `linux-x64-tsan` üçünde de **3/3 passed** (POSIX `SIGKILL` + `SIGINT` dahil); clang-tidy
  **6 = baseline**. `pre_commit run --all-files` temiz.
- **CLI e2e (gerçek süreç, WSL `linux-x64`):** 8 MiB dosya, 4 bağlantı, yerel throttle'lı sunucu;
  2,4 s sonra `SIGINT`:

```text
=== first run: exit 130 ===
--- first stderr (last 4 lines, CR shown as LF) ---
4718592 / 8388608 bytes (56.2%)
4980736 / 8388608 bytes (59.4%)
flowget-cli: error: download cancelled
note: the partial file and its resume sidecar were kept; rerun the same command to continue
--- files after the interrupt ---
-rw-r--r-- 1 kouizer kouizer 8388608 Nov 22 16:59 /tmp/flowget-e2e/out.bin
-rw-r--r-- 1 kouizer kouizer     512 Nov 22 16:59 /tmp/flowget-e2e/out.bin.flowget
=== second run: same command ===
exit 0
--- second stderr (CR shown as LF) ---
Resuming from 4.8 MiB
saved 8388608 bytes to /tmp/flowget-e2e/out.bin using 4 connection(s)
--- SHA-256 ---
0aeda0cb939c30ca58e1be264852f347372e17418ee3c84c465901b0faf58c8f  /tmp/flowget-e2e/source.bin
0aeda0cb939c30ca58e1be264852f347372e17418ee3c84c465901b0faf58c8f  /tmp/flowget-e2e/out.bin
--- sidecar after the resume (should be gone) ---
ls: cannot access '/tmp/flowget-e2e/out.bin.flowget': No such file or directory
```

**Kanıt (Windows, ELLE DOĞRULAMA, 2026-11-22):** Gerçek PowerShell, gerçek ağ:
`download.thinkbroadband.com/512MB.zip`, 536870912 bayt, `--connections 8`.

- İndirme ortasında Ctrl+C → **%24,7** (132628480 bayt) noktasında durdu; çıkış kodu **130**;
  `.bin` ve `.flowget` (896 bayt) diskte kaldı.
- Aynı komut yeniden çalıştırıldı → `Resuming from 126.5 MiB` (kesilen noktayla aynı), çıkış **0**;
  indirme bitince `.flowget` silindi.
- SHA-256: `884A240A272FF12BADBCBB8A9657AFF1C9A9DC2B516C1FEC0B1DEEBA63F17526` — `curl.exe`
  referans indirmesiyle aynı.
- `--no-resume` ile: `Resuming` satırı **yok**, baştan indirdi, hash aynı.

Bu, S5a-2b'nin "Windows Ctrl+C otomatik kanıtsız kalırsa elle doğrulanacak" notunu kapatır.

**Kanıt (CI, 2026-11-22):** GitHub Actions **run #19** — Windows (MSVC), Linux (Clang,
ASan/UBSan) ve `policy` job'larının üçü de yeşil. Windows Ctrl+C süreç testi CI konsolunda
atlanırsa logda gerekçeli bir `SKIPPED` satırı görünür (sessiz atlama yok). Bu satır yerelde
doğrulanamadı: `gh` CLI bu makinede kurulu değil; bu yüzden Windows Ctrl+C kanıtı yukarıdaki
elle doğrulamaya dayanır.

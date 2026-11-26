# Dersler (lessons) — kural özeti

Her hata düzeltmesinden sonra **tam kayıt** [`lessons-archive.md`](lessons-archive.md) dosyasına eklenir
(belirti, kök neden, düzeltme, tekrarını önleyen kontrol). Burada her ders için yalnız **kural +
kontrol** tutulur; başlık arşivdekiyle birebir aynıdır. Kural: kök neden yazılır, belirti değil.

## Kayıt akışı

1. `tasks/lessons-archive.md` → `### YYYY-MM-DD — Kısa başlık` altında dört alanı doldur.
2. Buraya aynı tarih + başlıkla 1-2 satırlık kural ve kontrol ekle.
3. Aynı hata ikinci kez olursa test/CI kontrolünü güçlendir; yalnız yeni kayıt eklemekle kalma.

## Kurallar

- **2026-11-14 — GTest test keşfi, ASCII olmayan dizin yolunda build'i kırıyor:** `gtest_discover_tests`
  mutlak JSON yolunu ANSI kod sayfasıyla açar; `add_test` kullan. Kontrol: ASCII dışı dizinde build.
- **2026-11-14 — Visual Studio'nun getirdiği `cmake.exe`, `VCPKG_ROOT`'ı sessizce eziyor:** önce
  `FLOWGET_VCPKG_ROOT` oku. Kontrol: `CMakeCache.txt` içindeki `Z_VCPKG_ROOT_DIR` beklenen kökü göstermeli.
- **2026-11-14 — ASCII olmayan dizinde vcpkg port derlemesi (curl) kırılıyor:** vcpkg'ın ürettiği mutlak
  yollar ASCII olmalı; `VCPKG_INSTALLED_DIR` ASCII konuma taşınır. Kontrol: ASCII dışı dizinde temiz build.
- **2026-11-14 — Manifest okuyucu: BOM ve pubspec girinti derinliği:** `utf-8-sig` ile oku; bağımlılık
  yalnız tam 2 boşluk girintide sayılır. Kontrol: GPL fixture `exit=1`, temiz fixture `exit=0`.
- **2026-11-14 — `windows.h` makroları test sunucusunun derlemesini bozdu:** `NOMINMAX` +
  `WIN32_LEAN_AND_MEAN` Windows başlıklarından önce tanımlanır. Kontrol: MSVC `/W4 /WX`.
- **2026-11-14 — CLI bayrak döngüsünün gövdesi her yolda `return` ediyordu (MSVC C4702):** gövdesi
  koşulsuz dönen döngü yazma; uyarıyı susturma, ölü kodu sil. Kontrol: MSVC CI (`/W4 /WX`).
- **2026-11-14 — Test HTTP sunucusu soket tiplerini `int` varsayıyordu; POSIX'te SIGPIPE:** platform
  typedef'leri (`SocketLength`/`SocketIoSize`/`IoResult`) kullan; POSIX `send`'de `MSG_NOSIGNAL`.
  Kontrol: Clang `-Wconversion -Wsign-conversion -Werror` + ASan/UBSan.
- **2026-11-14 — Dosya sonundaki fazla boş satır pre-commit `end-of-file-fixer`'ı kırdı:** dosya tek
  satır sonuyla biter. Kontrol: commit öncesi `pre-commit run --all-files`.
- **2026-11-14 — `curl.h` üzerinden gelen `windows.h`, `numeric_limits<long>::max()`'i bozdu:** tanım
  dosyada değil, `core/CMakeLists.txt` içinde hedef düzeyinde verilir. Kontrol: MSVC `C4003/C2589`.
- **2026-11-14 — Probe'un HEAD zaman aşımı hiç uygulanmadı (test 5 s bekledi):** `head_timeout_ms`
  istek nesnesine kopyalanır. Kontrol: test hem `head_used == false` hem süre < 5 s iddia eder.
- **2026-11-14 — Transport 416'yı "HTTP hatası" saydığı için probe boş dosyada başarısız göründü:**
  probe `416`'yı anlamlı sonuç sayar; diğer 4xx hata kalır. Kontrol: `ReportsUnsatisfiableRange…` testi.
- **2026-11-15 — `pre-commit` yerelde hiç çalışmadı: kullanıcı önbellek dizinlerinde ACL hatası:**
  `PRE_COMMIT_HOME` + `VIRTUALENV_OVERRIDE_APP_DATA` proje içindeki `build/` altına verilir (geçici
  betikler de `build/`'de). Kontrol: "manifest is not a file" / `WinError 5` görünce depoda değil,
  bu iki değişkende ara.
- **2026-11-15 — CI kırıldı: vcpkg sığ klonunda `builtin-baseline` commit'i yok:** pinned baseline
  commit'i `git fetch --depth=1` ile çekilir; cache anahtarı baseline içerir. Kontrol: fetch script'i
  her iki job'da bootstrap'ten önce koşar, hata job'u kırmızı yapar.
- **2026-11-15 — "CMAKE_MAKE_PROGRAM is not set" vcpkg hatasının sonucu değil, ayrı bir koşul:** Linux
  toolchain adımında `ninja --version` çalıştır. Kontrol: üreteç eksiği configure ortasında değil,
  kurulum adımında açık mesajla çıkar.
- **2026-11-15 — `reopen()` yarım kalmış dosyayı siliyordu (truncate modu yeniden uygulanıyordu):**
  truncate yalnız oluşturma anında geçerli; `reopen()` veriyi koruyan modla açar. Kontrol:
  `ReopenAfterTruncateModeKeepsData`.
- **2026-11-15 — PowerShell `Set-Content -NoNewline` dosyayı tek satıra indirdi:** satır bazlı toplu
  değişiklikte `write`/`edit` kullan; `-NoNewline` verme. Kontrol: `git diff --stat` + pre-commit.
- **2026-11-15 — Kullanılmayan lambda yakalaması yalnız Clang'de hata oldu:** açık yakalama listesine
  yalnız gövdede kullanılan değişken girer. Kontrol: Linux Clang build'i; yerel MSVC tek başına yetmez.
- **2026-11-16 — clang-format her koşuda dosyayı yeniden yazıyordu (satır sonu türevi):**
  `.clang-format` içinde `LineEnding: LF`; yeni dosyalar LF yazılır. Kontrol: pre-commit üst üste iki
  kez koşunca hiçbir dosya değişmemeli.
- **2026-11-16 — WSL doğrulama betiği iki katmanda sahte yeşil üretiyordu:** rsync kaynağı sonda `/`
  ile verilir ve script dosyadan `bash script.sh` ile koşar. Kontrol: çıktıda `Building CXX` satırı
  görünmeli; "ninja: no work to do" tek başına yeşil değildir.
- **2026-11-15 — Linux derleme hataları ancak push sonrası CI'da ortaya çıkıyordu:** commit öncesi
  `scripts/check_linux.ps1` zorunlu. Kontrol: betik `exit 0` vermeden Linux doğrulanmış sayılmaz.
- **2026-11-16 — Test sunucusunda TSan yarışı ve yıkıcı sırası:** paylaşılan soket alanı atomik;
  yıkıcı `exchange → shutdown + close → join` sırasını izler. Kontrol: `linux-x64-tsan` preset'i.
- **2026-11-16 — `pre_commit run --all-files` değişiklikleri sahnelemeden yeşil diyebilir:** önce
  `git add -A`, sonra hook. Kontrol: hook yeşilse `git status` temiz olmalı.
- **2026-11-16 — Push'lanmış commit amend edildi; geçmiş ayrıştı, merge commit'i gerekti:** push
  sonrası amend/force push yok; düzeltme yeni commit. Kontrol: `git status -sb` `ahead/behind`
  göstermiyorsa geçmiş değiştirilmez.
- **2026-11-17 — clang-tidy `IgnoredRegexp` tam (qualified) adla eşleşir:** desenleri `.*` ile sar
  (`.*FLOWGET_.*`). Kontrol: tek dosyalık örnekle çalıştır ve toplam bulgu sayısının düştüğünü ölç.
- **2026-11-19 — Kullanılmayan `constexpr` sabit yalnız Linux/Clang'de build'i kırdı:** ad alanı
  kapsamlı sabit yalnız gerçekten okunuyorsa tanımlanır. Kontrol: commit öncesi `check_linux.ps1`.
- **2026-11-19 — clang-tidy: `get_ref` tipini tekrar eden değişken (`modernize-use-auto`):** şablon
  dönüşümünde `const auto&` kullan. Kontrol: baseline üstü `check_linux.ps1` çıktısında "EXCEEDED".
- **2026-11-19 — Sidecar okuyucu `ifstream` motorun kendi rename'ini kilitler (Windows):** çalışma
  anında rename edilebilecek dosya `IStorage::open(read_only)` ile açılır; Windows'ta açık hedefi
  değiştirmek için `ReplaceFileW` kullanılır. Kontrol: `ReplacesTheSidecarWhileAReaderHasItOpen` ve
  `DeletesTheSidecarWhileAReaderHasItOpen`.
- **2026-11-21 — A/B ölçümü: aynı sunucudan ardışık indirme 429/403 yer, loopback gürültüye teslim:**
  WAN'da çiftler arası ~10 s bekle, `exit≠0` koşuyu tabloya alma; loopback'te ≥20 çift, eşleşmiş oran
  medyanı + bootstrap %95 GA raporla. Kontrol: benchmark betiği `exit` kodunu ve yazılan baytı kaydeder;
  tek batch medyanıyla "regresyon var" denmez.
- **2026-11-25 — Checkpoint testi geçici "tam" checkpoint'i yarış olarak görüyordu:** Monotonic
  maksimum `done` yerine tamamlanmamış checkpoint (`0 < done < total`) görüldüğünü iddia et; tam
  checkpoint'in görülmesi zamanlamaya bağlı. Kontrol: düzeltilmiş test ASan'da 50 tekrar yeşil;
  S5b öncesi HEAD'de aynı test 300 tekrarda 3 kez düştü.
- **2026-11-26 — Commit bölme betiği pre-commit'ten sonra dosya üretti; CI clang-format düştü:**
  Hook, commit'lenecek **son** içerikte ve üretimden SONRA koşar. Kontrol: push öncesi temiz ağaçta
  `py -m pre_commit run --all-files`; çıktıda bütün satırlar "Passed".

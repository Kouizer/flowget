# Dersler (lessons)

Her hata düzeltmesinden sonra buraya bir kayıt eklenir. Kural: **kök neden** yazılır, belirti değil.
Aynı hata ikinci kez olursa ilgili test/CI kontrolü eklenir.

## Şablon

```
### YYYY-MM-DD — Kısa başlık
- Belirti:
- Kök neden:
- Düzeltme:
- Tekrarını önleyen test/kontrol:
```

---

### 2026-11-14 — GTest test keşfi, ASCII olmayan dizin yolunda build'i kırıyor
- Belirti: `gtest_discover_tests` link sonrası `Unable to open file "...Varsay�lan Proje..."` ile
  build'i durdurdu; CI'da (ASCII yol) sorun görünmüyordu.
- Kök neden: CMake'in GoogleTest keşfi, testi `--gtest_output=json:<mutlak yol>` ile çalıştırıyor;
  GoogleTest bu yolu Windows ANSI kod sayfasıyla açıyor. Türkçe "ı" karakteri kaybolunca dosya
  oluşturulamıyor.
- Düzeltme: `gtest_discover_tests` kaldırıldı; süit tek CTest girdisi olarak kaydedildi
  (`add_test`). Test bazlı filtreleme `--gtest_filter` ile yapılıyor.
- Tekrarını önleyen kontrol: Build, ASCII olmayan bir dizinde de çalıştırılmalı (yerel geliştirme
  yolu); CI tek başına bunu yakalamaz.

### 2026-11-14 — Visual Studio'nun getirdiği `cmake.exe`, `VCPKG_ROOT`'ı sessizce eziyor
- Belirti: Kabukta `VCPKG_ROOT=C:\Users\Enes\vcpkg` olmasına rağmen configure, VS'in gömülü
  `C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg` toolchain'ini kullandı;
  `CMakeCache.txt` içinde `Z_VCPKG_ROOT_DIR` VS yolunu gösterdi.
- Kök neden: VS ile gelen `cmake.exe`, kendi süreç ortamında `VCPKG_ROOT` değişkenini VS'in vcpkg
  kopyasına yazıyor. `cmd` içinde `echo %VCPKG_ROOT%` doğru değeri gösterdiği için sorun ilk bakışta
  görünmüyor; fark yalnızca `cmake -E environment` çıktısında ortaya çıkıyor.
- Düzeltme: `CMakeLists.txt` önce `FLOWGET_VCPKG_ROOT`, sonra `VCPKG_ROOT` kullanıyor; yerelde
  `FLOWGET_VCPKG_ROOT=C:\Users\Enes\vcpkg` ayarlandı, README'de not edildi.
- Tekrarını önleyen kontrol: Configure logunda `vcpkg install` satırları ve `CMakeCache.txt` içindeki
  `Z_VCPKG_ROOT_DIR` beklenen kökü göstermeli; şüphede `cmake -E environment | findstr VCPKG_ROOT`.

### 2026-11-14 — ASCII olmayan dizinde vcpkg port derlemesi (curl) kırılıyor
- Belirti: `curl` portu configure aşamasında `Imported target "CURL::nghttp2" includes non-existent
  path "C:/Users/Enes/Documents/Varsay�lan"` ile `BUILD_FAILED` verdi; aynı dizinde `nghttp2` ve
  `zlib` sorunsuz derlendiği için ilk bakışta "curl portu bozuk" görünüyordu.
- Kök neden: vcpkg, ürettiği `*Config.cmake` dosyalarına mutlak kurulum yollarını yazıyor; Windows
  bu dosyaları ANSI kod sayfasında okuduğu için ASCII olmayan karakter ("ı") yolu kesiyor. M0'daki
  GTest keşfi hatasıyla aynı sınıf: **araç zincirinin ürettiği dosyalara giden yol ASCII olmalı.**
- Düzeltme: Kaynak yolu ASCII değilse `VCPKG_INSTALLED_DIR` otomatik olarak
  `%LOCALAPPDATA%\FlowGet\vcpkg-installed` altına taşınıyor (vcpkg'ın ürettiği tüm yollar ASCII kalır).
- Tekrarını önleyen kontrol: Temiz `build/` ile configure + build, ASCII olmayan bir dizinde
  çalıştırılmalı; CI (ASCII yol) bu hatayı yakalamaz.

### 2026-11-14 — Manifest okuyucu: BOM ve pubspec girinti derinliği
- Belirti: `check_dependencies.py`, PowerShell'in yazdığı `vcpkg.json`'ı `Unexpected UTF-8 BOM` ile
  reddetti; ayrıca pubspec'te 3 bağımlılık varken 4 saydı.
- Kök neden: (a) dosya `utf-8` ile okunuyordu, Windows editörleri/`Set-Content` BOM ekliyor;
  (b) girinti kontrolü `indent < 2` idi, `sdk: flutter` gibi alt alanlar da paket adı sayıldı.
- Düzeltme: okuma `utf-8-sig`; bağımlılık satırı yalnızca tam olarak 2 boşluk girintide kabul ediliyor.
- Tekrarını önleyen kontrol: Betik iki manifestli bir fixture üzerinde çalıştırılıyor (GPL örneği
  `exit=1`, temiz örnek `exit=0`) — S1a kanıtı `tasks/todo.md`'de.

### 2026-11-14 — `windows.h` makroları test sunucusunun derlemesini bozdu
- Belirti: `http_test_server.cpp` içinde `std::min(...)` çağrıları MSVC'de
  `error C2589: '(': '::' sağ tarafında geçersiz belirteç` verdi; aynı dosyada `support/...` başlığı
  da bulunamadı (`C1083`).
- Kök neden: `winsock2.h` → `windows.h` zinciri `min`/`max` makrolarını tanımlıyor ve
  `std::min(a, b)` metni makro tarafından bozuluyor; ayrıca integration test hedefinin include
  dizinleri `tests/` kökünü içermiyordu, oysa test `"support/http_test_server.h"` diye ekliyor.
- Düzeltme: Windows başlıklarından **önce** `NOMINMAX` (+ `WIN32_LEAN_AND_MEAN`) tanımlandı;
  `flowget_integration_tests` hedefine `target_include_directories(... ${CMAKE_CURRENT_SOURCE_DIR})`
  eklendi.
- Tekrarını önleyen kontrol: `windows.h` kullanan her .cpp `NOMINMAX` ile başlar; ayrıca MSVC build'i
  (`-W4 /WX`) bu sınıfı CI'da zaten yakalıyor.

### 2026-11-14 — CLI bayrak döngüsünün gövdesi her yolda `return` ediyordu (MSVC C4702)
- Belirti: Windows CI (MSVC, `/W4 /WX`) `cli/src/main.cpp:145` için `error C2220` + `warning C4702:
  unreachable code` verdi; Clang bu uyarıyı üretmediği için Linux işi yeşil kaldı.
- Kök neden: `for (int index = 1; index < argc; ++index)` döngüsünün gövdesi **her** yolda
  dönüyordu (`handle_flag` sonucu >= 0 ise `return result`, değilse hata yazıp `return 2`). Bu yüzden
  döngünün artış ifadesi (`++index`) ve döngüden sonraki `print_usage/return 2` hiç yürütülemiyordu;
  sözde döngü aslında yalnızca `argv[1]`'i işliyordu. MSVC erişilemeyen artış satırını işaretledi.
- Düzeltme: döngü kaldırıldı; `argv[1]` doğrudan `handle_flag(command)`'a verildi ve davranış
  korundu (bayraklar birbirini dışlar ve süreci sonlandırır). Uyarı susturulmadı, erişilemeyen kod
  silindi.
- Tekrarını önleyen kontrol: MSVC `/W4 /WX` CI build'i; gövdesi koşulsuz `return` eden döngü
  yazılmaz. İleride birden çok argüman işlenecekse gövdede gerçek bir `continue`/tamamlama yolu
  bırakılmalı.

### 2026-11-14 — Test HTTP sunucusu soket tiplerini `int` varsayıyordu; POSIX'te SIGPIPE
- Belirti: Linux CI (Clang, `-Wconversion -Wsign-conversion -Werror`) `tests/support/http_test_server.cpp`
  içinde `send()` sonucunun `const int`'e atanmasını (+ `int` → `size_t` işaret dönüşümünü) ve
  `recv()` sonucunun `const int`'e atanmasını hata verdi. Windows'ta aynı satırlar temiz derlendi.
- Kök neden: Winsock `send`/`recv` **int** döndürür ve uzunluğu **int** alır; POSIX `ssize_t`
  döndürür ve **size_t** uzunluk alır. Kod tek bir `int` varsayımıyla yazılmıştı: Linux'ta dönüş
  değeri daralıyor (ssize_t → int, sessiz kesme riski) ve uzunluk argümanı işaret dönüşümüne
  uğruyordu. Aynı sınıftaki ikinci sorun: POSIX `send()`, peer bağlantıyı kapatmışsa SIGPIPE üretir;
  iptal/düşen bağlantı testlerinde bu, tek bir `send` hatası yerine tüm test ikilisini öldürebilirdi.
- Düzeltme: Platforma göre tek yerde tanımlı `SocketLength` / `SocketIoSize` / `IoResult`
  typedef'leri (Windows: `int`/`int`/`int`; POSIX: `socklen_t`/`std::size_t`/`ssize_t`). `send`/`recv`
  uzunluk ve dönüş değerleri bu tiplerle, açık cast'lerle kullanılıyor; `accept`/`getsockname`/`bind`
  uzunlukları `SocketLength`; POSIX'te `send` bayrakları `MSG_NOSIGNAL`, Windows'ta `0` (SIGPIPE yok).
  `close`/`closesocket` farkı zaten `close_socket` sarmalayıcısında.
- Tekrarını önleyen kontrol: Linux CI'da Clang `-Wconversion -Wsign-conversion -Werror` build'i ve
  ASan/UBSan testleri. Soket sarmalayıcıları dışında çıplak `send`/`recv`/`close` çağrısı yazılmaz;
  dönüş tipleri platform typedef'lerinden alınır.

### 2026-11-14 — Dosya sonundaki fazla boş satır pre-commit `end-of-file-fixer`'ı kırdı
- Belirti: Linux CI'da yalnızca pre-commit adımı kırıldı; `fix end of files` hook'u
  `tasks/lessons.md` sonundaki fazladan boş satırları sildi ve CI "files were modified by this hook"
  ile durdu.
- Kök neden: dosya sonuna birden fazla boş satır bırakılmıştı; hook dosyayı yeniden yazdığı için
  kontrol adımı değişiklik tespit etti. Sorun derleme değil, depo hijyeniydi.
- Düzeltme: `tasks/lessons.md` tam olarak tek satır sonuyla bitecek şekilde temizlendi; depodaki
  diğer izlenen dosyalar da tarandı (başka fazla boş satır veya satır sonu boşluğu yok).
- Tekrarını önleyen kontrol: commit'ten önce `pre-commit run --all-files` çalıştır; pre-commit yoksa
  her dosyanın sonunda yalnızca bir yeni satır kaldığını doğrula.

### 2026-11-14 — `curl.h` üzerinden gelen `windows.h`, `numeric_limits<long>::max()`'i bozdu
- Belirti: MSVC `/W4 /WX` build'i `curl_transport.cpp` içinde `C4003: 'max' ... yeterli bağımsız
  değişken yok`, `C2589: '(': '::' sağ tarafında geçersiz belirteç` ve `C2737: 'kMaxTimeout'
  başlatılması gerekiyor` verdi; aynı dosya Linux'ta sorunsuz derlenir.
- Kök neden: `curl.h` Windows'ta `windows.h`'ı çekiyor ve `min`/`max` **fonksiyon benzeri makro**
  oluyor; `std::numeric_limits<long>::max()` metni `max()` (sıfır argüman) olarak genişleyip
  patlıyor. Kural (`windows.h` kullanan her .cpp `NOMINMAX` ile başlar) `tasks/lessons.md`'de vardı
  ama yeni dosya yazılırken hatırlanması gereken bir kuraldı — yani koruma geliştiricinin belleğine
  bağlıydı.
- Düzeltme: `core/CMakeLists.txt` içinde Windows'ta `NOMINMAX` + `WIN32_LEAN_AND_MEAN` **hedef
  düzeyinde** (`target_compile_definitions(flowget_core PUBLIC ...)`) tanımlandı; dosya başındaki
  yerel blok kaldırıldı. Artık `curl.h`'ı hangi çeviri birimi çekerse çeksin makrolar kapalı.
- Tekrarını önleyen kontrol: MSVC CI build'i (aynı hata anında `C4003/C2589` ile düşer). Kural:
  `windows.h`'ı dolaylı çeken üçüncü parti başlıklar için tanım CMake'te, dosyada değil.

### 2026-11-14 — Probe'un HEAD zaman aşımı hiç uygulanmadı (test 5 s bekledi)
- Belirti: `Probe.FallsBackWhenHeadStalls` testi 200 ms beklemek yerine 5011 ms ölçtü; probe yine de
  doğru sonucu döndürdüğü için hata yalnızca süre iddiasında ortaya çıktı.
- Kök neden: `ProbeOptions.head_timeout_ms` yalnızca "HEAD atılsın mı" kararında kullanılıyordu;
  `HttpRequest::timeout_ms` alanına **kopyalanmıyordu**. Bu yüzden HEAD, transport varsayılanlarına
  (60 s low-speed) kaldı ve süreyi test sunucusunun 5 s soket zaman aşımı belirledi. Sessiz bir
  davranış farkı: sonuç doğru, gecikme yanlış.
- Düzeltme: HEAD isteği oluşturulurken `head_request.timeout_ms = options.head_timeout_ms` atanıyor.
- Tekrarını önleyen kontrol: test hem `head_used == false` hem de **geçen süre < 5 s** iddia ediyor;
  alan yeniden kopyalanmazsa test kırmızıya döner (senaryonun kendisi 5 s'lik varsayılanı aşar).

### 2026-11-14 — Transport 416'yı "HTTP hatası" saydığı için probe boş dosyada başarısız göründü
- Belirti: 0 baytlık dosyada probe `result.ok() == false`, mesaj `HTTP status 416`; beklenen
  `unsatisfiable` + `content_length == 0`.
- Kök neden: `curl_transport` her `status >= 400` yanıtını `HttpError::http_status` yapıyor (genel ve
  doğru kural). Probe ise "taşıma hatası varsa erken dön" diyordu; `416`'nın 4xx olması ama anlamlı
  bir cevap olması bu iki katmanın arasında kayboldu.
- Düzeltme: probe, `http_status` + `status == 416` kombinasyonunu kabul edilen sonuç olarak ayıklıyor;
  gerçek hata durumları (404 vb.) değişmedi.
- Tekrarını önleyen kontrol: `Probe.ReportsUnsatisfiableRangeForEmptyResource`; S5'te sidecar
  doğrulaması aynı ayrımı kullanacağı için karar `tasks/todo.md`'de not edildi.

### 2026-11-15 — `pre-commit` yerelde hiç çalışmadı: kullanıcı önbellek dizinlerinde ACL hatası
- Belirti: `py -m pre_commit run --all-files` önce `InvalidManifestError: ...repoutztkbt_\
  .pre-commit-hooks.yaml is not a file`, sonra `PermissionError: [WinError 5] ...\.cache\pre-commit\
  repoq346t_fr` verdi. `Get-Acl` aynı dizinde "Yetkisiz işlem" döndü; yani sorun pre-commit'te değil,
  dizinin erişim denetimindeydi.
- Kök neden: `%USERPROFILE%\.cache\pre-commit` (yarım kalmış klon) ve `%LOCALAPPDATA%\pypa\
  virtualenv\Cache` dizinleri yazılamaz durumda (bozuk ACL / antivirüs kilidi). pre-commit bunları
  varsayılan konumdan kullandığı için hook ortamı kurulamadı; hata mesajı manifest hatası gibi
  göründüğü için ilk bakışta yanıltıcı.
- Düzeltme: kullanıcı dizinlerine dokunmadan, proje içindeki `build/` altında geçici konumlar verildi:
  `PRE_COMMIT_HOME=C:\dev\flowget\build\pre-commit-home`,
  `VIRTUALENV_OVERRIDE_APP_DATA=C:\dev\flowget\build\virtualenv-app-data`. Bu iki değişkenle
  `run --all-files` tüm hook'larda `Passed` verdi. Geçici betikler de proje dışına değil `build/`
  altına konur ve commit'lenmez.
- Tekrarını önleyen kontrol: pre-commit yine "manifest is not a file" veya `WinError 5` derse, sorunu
  depo/config içinde aramadan önce bu iki ortam değişkeniyle `build/` altında temiz bir önbellek
  kullan. CI etkilenmez (temiz runner).

### 2026-11-15 — CI kırıldı: vcpkg sığ klonunda `builtin-baseline` commit'i yok
- Belirti: GitHub Actions run #5 (commit `51e7699`), Linux job'u `Configure (debug)` adımında 1 s'de
  düştü: `error: while checking out baseline from commit '4702e381...', failed to \`git show\`
  versions/baseline.json` + `fatal: path 'versions/baseline.json' exists on disk, but not in
  '4702e381...'`. Windows job'u aynı run'da yeşildi. `ci.yml` ve `vcpkg.json` en son `24fe335`'te
  değişmişti; run #3 (`82cbf88`) ve run #4 (`ab9be99`) yeşildi. Yani kod değişmeden kırıldı.
- Kök neden: `actions/checkout@v4` yabancı depoyu (microsoft/vcpkg) varsayılan `fetch-depth: 1` ile
  klonluyor → `vcpkg/` içinde **tek** commit var (o gün master ucu `8f363bd`). `vcpkg.json` ise
  `builtin-baseline` olarak `4702e38`'i pinliyor; vcpkg bu commit'in ağacından
  `versions/baseline.json`'ı okuyor. Baseline seçildiği gün (14 Kasım 07:07 UTC) `4702e38` master'ın
  ucu olduğu için run #3/#4'te klonda hazırdı; master ilerleyince commit sığ klondan düştü. Hata
  kodda değil, **dış bağımlılığın ucunun kaymasındaydı** — bu yüzden "dün yeşildi bugün kırmızı"
  göründü. `ci.yml`'deki "Full clone, not shallow" yorumu yanlıştı; klon sığdı. Yerel tekrar: sığ
  klon (9.4 MB, 1 commit) + `git cat-file -e 4702e38:versions/baseline.json` → aynı
  "exists on disk, but not in" hatası.
- Düzeltme: `scripts/fetch_vcpkg_baseline.py` eklendi. SHA'yı `vcpkg.json`'dan okur (tek doğruluk
  kaynağı), commit yoksa `git fetch --depth=1 origin <sha>` ile yalnız o commit'i çeker,
  `versions/baseline.json`'ın gerçekten okunabildiğini doğrular ve `GITHUB_OUTPUT`'a `baseline=`
  yazar. İki job'a da "Fetch pinned vcpkg baseline" adımı bootstrap'ten **önce** eklendi; binary
  cache anahtarı `vcpkg-<os>-<baseline>-<manifest>` oldu (eski baseline'ın arşivleri geri
  yüklenmiyor). Tam klon (yüzlerce MB) bilinçli olarak seçilmedi.
- Tekrarını önleyen kontrol: adım her iki job'da koşuyor ve başarısızsa job kırmızı; baseline
  bump'ında script kendiliğinden yeni SHA'yı çeker (workflow düzenlemesi gerekmez). Cache anahtarı
  baseline içerdiği için eski arşivler sessizce kullanılmaz. Yerel doğrulama: sığ klonda aynı hata
  üretildi, script sonrası `git show 4702e38:versions/baseline.json` okunabildi; ikinci koşu
  "already present" ile 0 döndü; uydurma SHA / eksik manifest / olmayan dizin → exit 1 + net mesaj.

### 2026-11-15 — "CMAKE_MAKE_PROGRAM is not set" vcpkg hatasının sonucu değil, ayrı bir koşul
- Belirti: Linux logunda iki mesaj birlikte: vcpkg baseline hatası ve "CMake was unable to find a
  build program corresponding to Ninja. CMAKE_MAKE_PROGRAM is not set." Soru: ikincisi ilkinin
  sonucu mu? Yanıt: **hayır**, ayrı bir koşul — ama ikisi aynı configure çıktısında görünebilir.
- Kök neden (tespit): CMake sırası ölçüldü — vcpkg install (`project()` içindeki toolchain) **önce**
  çalışır, üretecin make programını bulma **sonra**. vcpkg hatası configure'ı hemen bitirmiyor;
  CMake üretim adımına geçip ninja'yı arıyor ve o da yoksa ikinci hatayı yazıyor. Yerel deney:
  bozuk baseline + PATH'te ninja yok → aynı çıktıda **iki mesaj birlikte**; bozuk baseline + ninja
  var → yalnız vcpkg hatası. Yani Ninja satırı vcpkg'ın türevi değil, o configure'da CMake'in
  ninja'yı çözemediğini gösteren bağımsız bir koşul. CI'da `ninja-build` apt adımında kurulu ve o
  adım run #5'te `success`; run #3/#4 aynı `apt-get` satırı ve aynı presetlerle Ninja kullanarak
  derledi. Run #5'in anonim logu kapalı ("Sign in to view logs") olduğu için o koşuda ninja'nın
  neden çözülemediği kanıtlanamadı; en olası kaynak, aynı mesajı **yerelde** üreten durum: düz
  PowerShell'de `cmake`/`ninja` PATH'te yok (VS'in CMake'i yalnız kendi dizininde), `cmake --preset`
  çağrısı vcvars olmadan aynı satırı veriyor.
- Düzeltme: Linux toolchain adımına `ninja --version` eklendi; üreteç eksikse hata configure'ın
  ortasında değil, kurulum adımında ve açık mesajla çıkar. Yerelde build için VS geliştirici
  ortamı (`vcvars64.bat`) + VS'in Ninja dizini PATH'e eklenmeli.
- Tekrarını önleyen kontrol: `ninja --version` çıktısı her koşuda logda; configure'daki
  "CMAKE_MAKE_PROGRAM" metni bundan sonra doğrudan üreteç/ortam eksiğine işaret eder, vcpkg
  hatasıyla karıştırılmaz.

### 2026-11-15 — `reopen()` yarım kalmış dosyayı siliyordu (truncate modu yeniden uygulanıyordu)
- Belirti: `StorageIoTest.NonAsciiPathRoundTrip` 4096 bayt yazıp `close()` + `reopen()` sonrası
  `read_at` ile `read` hatası verdi; dosya beklenen boyutta değildi.
- Kök neden: `open()` çağrıldığı modu (`read_write_truncate` dahil) `mode_` alanına yazıyordu ve
  `reopen()` aynı modu yeniden uyguluyordu. Truncate bir **oluşturma** kararıdır, yeniden açma kararı
  değil; S5'te "çökme sonrası dosyayı yeniden doğrula" adımı veriyi silecekti. Hata sessiz: API
  "ok" dönüyor, yalnız dosyanın içeriği kayboluyor.
- Düzeltme: `open()` başarılı olduktan sonra `mode_ = (mode == read_write_truncate) ? read_write :
  mode`; `reopen()` her zaman veriyi koruyan modla açar. `storage.h` yorumunda sözleşme yazılı.
- Tekrarını önleyen kontrol: `ReopenAfterTruncateModeKeepsData` testi; `NonAsciiPathRoundTrip` de
  yazma → close → reopen → okuma zincirini doğruluyor.

### 2026-11-15 — PowerShell `Set-Content -NoNewline` dosyayı tek satıra indirdi
- Belirti: `(Get-Content f) -replace ... | Set-Content -NoNewline f` sonrası dosya 0 satır sonu, 0 CR
  içeriyordu; tüm içerik tek satıra yapışmıştı.
- Kök neden: `-NoNewline` verildiğinde `Set-Content` dizi elemanlarını **ayırıcısız** birleştirir
  (satır sonlarını da eklemez). `Get-Content` satır sonlarını zaten atmıştı.
- Düzeltme: dosya `write` aracıyla bütünüyle yeniden yazıldı; satır bazlı toplu değişiklik yapılacaksa
  `edit`/`write` aracı ya da `Set-Content` **`-NoNewline` olmadan** kullanılmalı.
- Tekrarını önleyen kontrol: toplu değişiklikten sonra `git diff --stat` + dosyada CR/LF sayımı;
  `pre-commit` (`end-of-file-fixer`) tek satıra inmiş dosyayı hemen yakalar.

### 2026-11-15 — Kullanılmayan lambda yakalaması yalnız Clang'de hata oldu
- Belirti: Linux CI (Clang, `-Werror`) `tests/unit/storage_test.cpp:59` için
  `error: lambda capture 'buffer' is not used [-Wunused-lambda-capture]` verdi; Windows (MSVC `/W4
  /WX`) aynı satırı uyarısız derledi ve S3 diliminin yerel kanıtı yeşil kaldı. Üretim kodu
  (`storage_posix.cpp`) temizdi; hata yalnız testteydi.
- Kök neden: `io_all(0, buffer.size(), ...)` çağrısında `buffer.size()` **lambda dışında** hesaplanıyor,
  ama lambda yine de `[&buffer]` ile yakalanmıştı; gövde `buffer`'a hiç dokunmuyor. MSVC'nin `/W4`
  setinde `-Wunused-lambda-capture`'ın karşılığı yok (C4100 yalnız *fonksiyon* parametresini
  kapsar), bu yüzden uyarı yalnız Clang'de görünür. 2026-11-14'teki C4702 kaydının tersi yön:
  orada yalnız MSVC, burada yalnız Clang yakalıyor.
- Düzeltme: yakalama listesi `[]` yapıldı; uyarı susturulmadı, gereksiz yakalama silindi. Aynı sınıf
  için `tests/` ve `core/` altındaki tüm lambda'lar, fonksiyon parametreleri ve yerel değişkenler
  elle tarandı: başka kullanılmayan açık yakalama/parametre/değişken yok (`[&]` varsayılan
  yakalamaları Clang yalnız gerçekten kullanılanı yakaladığı için bu sınıfa girmez).
- Tekrarını önleyen kontrol: açık yakalama listesine yalnız gövdede okunan/yazılan değişkenler girer;
  bir değeri sadece **argüman olarak hesaplamak** için yakalama gerekmez. Bu sınıfı yalnız Linux CI
  (Clang) yakalar; yerel makinede Clang yok, bu yüzden yerel MSVC build'i tek başına yeterli kanıt
  sayılmaz.

### 2026-11-16 — clang-format her koşuda dosyayı yeniden yazıyordu (satır sonu türevi)
- Belirti: `pre_commit run --all-files` aynı dosyayı (`tests/unit/segment_planner_test.cpp`)
  art arda iki koşuda da "files were modified" ile değiştirdi; ilk koşunun çıktısı commit'lendiği
  hâlde ikinci koşu yeni bir satır kaydırma üretti.
- Kök neden: `.clang-format`ta `LineEnding` yoktu; LLVM varsayılanı satır sonunu dosyadan türetir.
  Windows'ta Python `Path.write_text` ile üretilen çalışma kopyası CRLF'ti; clang-format CRLF yazarken
  tam 100 sütunluk satırları 101 sayıp yeniden kırdı, sonraki koşuda girdi yine CRLF olduğu için
  farklı bir sonuç üretti. Yani biçimlendirme çıktısı checkout'un satır sonuna bağlıydı.
- Düzeltme: `.clang-format`a `LineEnding: LF` eklendi; böylece çıktı satır sonundan bağımsız ve
  deterministik.
- Tekrarını önleyen kontrol: pre-commit üst üste iki kez koşulur; ikinci koşu hiçbir dosyayı
  değiştirmemeli. Yeni dosyalar `write` aracıyla (LF) yazılır; Python ile yazarken `newline="\n"`
  verilir.

### 2026-11-16 — WSL doğrulama betiği iki katmanda sahte yeşil üretiyordu
- Belirti: `scripts/check_linux.ps1` her koşuda "PASSED" veriyordu ama WSL tarafında yeni eklenen
  `core/src/download.cpp` hiç görünmüyordu; `ninja: no work to do` ve `ctest 2/2` çıktısı 14 Kasım
  kaynaklarına aitti. Ayrıca her aşamada bash `set` komutunun ortam dökümünü basıyordu
  (`BASH_EXECUTION_STRING=set`) ve betik yine de exit 0 dönüyordu.
- Kök neden (a): rsync kaynağı `'$source'` (sonda `/` yok) veriliyordu. Bu durumda rsync dizini
  hedefin **içine** kopyalar (`~/flowget/flowget/...`); `--delete` de bu ağacı meşru sandığı için
  hiçbir şey silinmiyor, gerçek `~/flowget` ağacı hiç güncellenmiyordu. Yani build, aylık eski
  kaynakları test ediyordu.
- Kök neden (b): çok satırlı script `bash -lc "<script>"` biçiminde tek argüman olarak geçiriliyordu.
  PowerShell 5.1 native komutlarda gömülü çift tırnakları kaçırmıyor; wsl.exe de komut satırını
  yeniden bölüyor. Sonuç: bash'in `-c` argümanı yalnızca ilk kelime (`set`) oluyor, ortam dökümü
  basılıyor ve süreç 0 ile dönüyor. Yani "yeşil" çıktı hiçbir iş yapılmadan üretiliyordu.
- Düzeltme: rsync kaynağının sonuna `/` eklendi. Her aşama LF'e normalize edilip geçici bir dosyaya
  yazılıyor ve `wsl -d <distro> -- bash /mnt/.../script.sh` ile koşuyor; böylece tırnak/argüman
  katmanı tamamen kalktı. Windows→WSL yol çevrimi `wslpath` yerine PowerShell'de yapılıyor, çünkü
  wsl.exe argümandaki ters bölüleri yiyor (`C:\Users\...` → `C:Users...`).
- Tekrarını önleyen kontrol: betik çıktısında gerçek derleme satırları (`Building CXX ... .o`)
  görünmeli; "ninja: no work to do" tek başına yeşil sayılmaz. Yeni dosya eklenen her dilimde WSL
  tarafında `ls ~/flowget/<yeni dosya>` ile senkronizasyon ayrıca doğrulanır.

### 2026-11-15 — Linux derleme hataları ancak push sonrası CI'da ortaya çıkıyordu
- Belirti: Yerelde MSVC build + `ctest` yeşil ve `pre-commit` temiz olduğu hâlde push sonrası Linux
  job'u kırmızı döndü (örnek: `-Wunused-lambda-capture`). Her tur push → CI bekleme → düzeltme
  maliyetindeydi; dilim "bitti" denildiğinde Linux tarafı henüz hiç doğrulanmamış oluyordu.
- Kök neden: Yerel doğrulama yalnız Windows araç zincirini (MSVC + `windows.h` tuzakları) kapsıyordu;
  CI'ın Linux job'u (Clang `-Wconversion -Wsign-conversion -Werror`, ASan/UBSan) yerelde **hiç
  koşmuyordu**. Clang'in MSVC'de karşılığı olmayan tanılamaları ve sanitizer davranışı bu yüzden
  geri bildirim döngüsünün en pahalı yerinde (CI'da) görünüyordu. Yani sorun "Linux bozuk" değil,
  doğrulama kapsamının platform eksik olmasıydı.
- Düzeltme: `scripts/check_linux.ps1` eklendi — depoyu WSL2'deki `~/flowget` klonuna rsync'ler
  (`build/`, `.git/` hariç), sonra `linux-x64` (debug) ve `linux-x64-asan` (ASan/UBSan) presetlerini
  configure + build + `ctest` eder ve CI'daki adımların aynısını yerelde koşturur. `AGENTS.md`'de
  commit öncesi zorunlu adım yapıldı; "Linux derlemesi yerelde doğrulanamaz, CI doğrular" kuralı
  kaldırıldı.
- Betik yazılırken çıkan üç tuzak (üçü de sessiz kırılma üretiyordu):
  (a) `bash -lc` **login** shell olduğu için `~/.bashrc` okunmaz → orada tanımlı `FLOWGET_VCPKG_ROOT`
  boş geliyor ve CMake "No vcpkg root found" ile duruyor; betik değişkeni kendisi export ediyor.
  (b) `.gitattributes` `*.ps1` dosyalarını CRLF checkout ediyor; CRLF'li uzak script bash'e
  `cmake --preset linux-x64\r` olarak gider ve "unknown preset" gibi yanıltıcı hata verir; betik
  metni bash'e vermeden önce LF'e normalize ediyor.
  (c) Klasik `rsync -a` "değişmedi" kararını boyut + mtime ile veriyor; WSL'in drvfs/9p katmanı yeni
  yazılmış bir zaman damgasını birkaç saniye eski gösterebildiği için senkronizasyon dosyayı atladı
  ve `ninja: no work to do` + yeşil `ctest` çıktısı **eski kaynak** için "başarılı" dedi. Yani
  doğrulama betiği yanlış yeşil üretebiliyordu. Düzeltme: `--checksum` — dosyanın kopyalanıp
  kopyalanmayacağına içerik karar verir, damga yanılsaması kararı etkilemez. Denenip **yanlış çıkan**
  ara çözüm: `--no-times`; içeriği güncelliyor ama hedefte eski mtime'ı bırakıyor, ninja yine
  "no work to do" diyor. Doğrusu `-a --checksum`: mtime kaynaktan gelir (`-a` → `-t`), böylece ninja
  tam olarak kopyalanan dosyaları yeniden derler.
- Tekrarını önleyen kontrol: commit öncesi `.\scripts\check_linux.ps1` **exit 0** vermeden Linux
  doğrulaması yapılmış sayılmaz; CI'daki Linux job'u artık yalnızca son kapı (push sonrası sürpriz
  değil).

### 2026-11-16 — Test sunucusunda TSan yarışı ve yıkıcı sırası
- Belirti: `linux-x64-tsan` derleyip tüm testleri koşturdu; 44 entegrasyon testi "PASSED" görünürken
  TSan 24 data race raporladı ve ctest `exit 8` verdi. Rapor `HttpTestServer::Impl` bloğunu
  (offset 0x20 = `listener`) işaret ediyordu: yıkıcıda ana thread yazıyor, accept thread okuyor.
- Kök neden: `listener` alanı atomik değildi; yıkıcı `close` ederken accept thread aynı baytları
  okuyordu. Üretim kodu değil, test altyapısı yarışıydı — ama TSan haklıydı.
- Düzeltmenin ilk denemesi **iki tuzak** çıkardı:
  (a) yıkıcıdaki yerel `listener` değişkeni `Impl::listener` üyesini gölgeledi → MSVC `C4458`,
  `-Werror` yüzünden Windows derlemesi düştü (yerel adlar `listening_socket` oldu);
  (b) `shutdown()` sonrası `join()` Windows'ta **kilitlemiyor**: `shutdown` dinleme soketinde
  `WSAENOTCONN` döner ve bloklu `accept`'i uyandırmaz; uyandıran `closesocket`'tir. Sonuç: ilk
  entegrasyon testi 300 s zaman aşımına düştü. Doğrusu: `exchange(kInvalidSocket)` →
  `shutdown` **ve** `close` → `join` (POSIX'te `shutdown`, Windows'ta `close` uyandırır).
- Tekrarını önleyen kontrol: TSan/ASan preset'leri `check_linux.ps1` içinde kaldığı sürece yarış
  sessizce geçemez; soket kapatan yıkıcılarda uyandırma yolu platforma göre test edilir
  (uyandırmıyorsa test zaman aşımına düşer, yanlış yeşil üretmez).

### 2026-11-16 — `pre_commit run --all-files` değişiklikleri sahnelemeden yeşil diyebilir
- Belirti: `py -m pre_commit run --all-files` "clang-format Passed" dedi, commit atıldı; aynı hook
  bir sonraki turda **commit edilmiş** kodu yeniden biçimlendirdi ve `git diff` gerçek format
  farkları gösterdi (satır kırılmaları, include sırası).
- Kök neden: pre-commit, hook'ları çalıştırmadan önce **sahnelenmemiş (unstaged) izlenen
  dosyaları stash'ler** ve hook'lar HEAD içeriğini denetler. HEAD zaten biçimli olduğu için kontrol
  "Passed" döner, sonra stash geri yüklenir; benim düzenlemelerim hiç denetlenmemiş olur. Yeni
  (untracked) dosyalar stash'lenmediği için ilk turda gerçekten denetlenmişti; karışık durum kafa
  karıştırdı.
- Düzeltme: commit öncesi `git add -A` yapıp hook'u **sahnelenmiş** içerik üzerinde koşturmak.
  Alternatif doğrulama: hook ortamındaki `clang-format.exe` ile dosyaları doğrudan biçimlendirip
  `git diff` boş olduğunu görmek.
- Tekrarını önleyen kontrol: `pre-commit` yeşilse `git status` temiz olmalı (denetlenmeyen değişiklik
  kalmamalı); commit sonrası `git status --short` boş çıkmıyorsa hook gerçekten koşmamıştır.

### 2026-11-16 — Push'lanmış commit amend edildi; geçmiş ayrıştı, merge commit'i gerekti
- Belirti: `git log --all --oneline` aynı işi iki SHA ile gösterdi: `627fa48` (`origin/main` üzerinde)
  ve `f38267e` (yerelde) "add streaming SHA-256 helper". `main` ile `origin/main` ayrıştı ve
  `6437b8a merge: reconcile amended SHA-256 commit with origin` merge commit'i oluştu.
- Kök neden: commit **push edildikten sonra** `git commit --amend` ile değiştirildi. Amend aynı
  içeriğe yeni bir SHA üretir; `origin/main` eski SHA'yı tuttuğu için iki geçmiş ortak atadan sonra
  ayrışır. Force push yapılmadığı için uzak dal bozulmadı ve sorun CI'da kırmızı üretmedi; bu yüzden
  hata geç fark edildi. Yerelde `git log` "düzgün" görünüyordu, çünkü yerel dal yeni SHA'yı taşıyordu.
- Düzeltme: force push yerine yerel geçmiş korunarak `origin/main` merge edildi (`6437b8a`); kural
  `AGENTS.md` Git bölümüne yazıldı: push'lanmış commit amend edilmez, düzeltme yeni commit olarak
  atılır.
- Tekrarını önleyen kontrol: commit öncesi `git status -sb` çıktısında `ahead/behind` yoksa
  (HEAD == origin/main) amend/`rebase -i` yapılmaz. Push sonrası `git log --oneline origin/main..HEAD`
  boş olmalı; doluysa yeni commit'ler push edilmemiş demektir, geçmiş değiştirilmez.

### 2026-11-17 — clang-tidy `IgnoredRegexp` tam (qualified) adla eşleşir
- Belirti: `.clang-tidy` içinde `EnumConstantIgnoredRegexp: ^FLOWGET_` ve
  `LocalConstantIgnoredRegexp: ^k[A-Z]` ayarlıydı, ama aynı bulgular (C ABI enum'ları, `kMaxTimeout`)
  üretilmeye devam etti; toplam yalnız 51'e düştü.
- Kök neden: `readability-identifier-naming` seçeneklerindeki `IgnoredRegexp` desenleri **tam adla**
  (`std::regex_match`) karşılaştırılır, `regex_search` ile değil. `^FLOWGET_` yalnız başlangıcı
  sabitler; adın kalanı eşleşmediği için kural uygulanır. clang-tidy 18'de constexpr'e özel naming
  ayarı da yok.
- Düzeltme: desenler `.*FLOWGET_.*`, `.*flowget_.*`, `k[A-Z].*` olarak sarıldı; 51 -> 36.
- Tekrarını önleyen kontrol: yeni naming seçeneği eklerken tek dosyalık bir örnekle
  `clang-tidy --config-file=...` çalıştırıp desenin gerçekten bastırdığını gör; "ayar var" demekle
  yetinme, toplam sayının düştüğünü ölç.

### 2026-11-19 — Kullanılmayan `constexpr` sabit yalnız Linux/Clang'de build'i kırdı
- Belirti: `scripts/check_linux.ps1` `resume_state.cpp` içinde
  `unused variable 'kStateFieldCount' [-Werror,-Wunused-const-variable]` ve aynı sınıftan ikinci bir
  hata verdi; aynı dosya MSVC `/W4 /WX` ile uyarısız derlenmişti ve yerel `ctest` yeşildi.
- Kök neden: `-Wunused-const-variable` Clang'in `-Wall` setinde var; MSVC'nin `/W4` setinde ad alanı
  kapsamlı `constexpr` için karşılığı yok (C4189 yalnız yerel değişkenleri kapsar). Sabitler
  `has_exact_fields(...)` çağrıları `.size()` kullandığı için gerçekten ölüydü — uyarı doğruydu,
  susturulacak bir gürültü değildi.
- Düzeltme: kullanılmayan iki sabit silindi (sayı `initializer_list::size()`'dan geliyor). MSVC tarafı
  hiç değişmedi.
- Tekrarını önleyen kontrol: ad alanı kapsamlı sabit yalnız gerçekten okunuyorsa tanımlanır; `ctest`
  yeşili tek başına yeterli kanıt değil, commit öncesi `check_linux.ps1` (Clang `-Werror`) zorunlu.
  Bu sınıf MSVC'de görünmez.

### 2026-11-19 — clang-tidy: `get_ref` tipini tekrar eden değişken (`modernize-use-auto`)
- Belirti: clang-tidy 6 → 9 bulgu; biri `resume_state.cpp` içinde "use auto when initializing with a
  template cast to avoid duplicating the type name".
- Kök neden: `nlohmann::json::get_ref<const std::string&>()` dönüş tipi zaten `const std::string&`;
  açık tip tekrarı, şablon argümanı değişince ayrışabilecek bir kopya bilgi taşıyor.
- Düzeltme: `const auto& text = value.get_ref<const std::string&>();`. Aynı turda iki bulgu daha
  düzeltildi: `has_exact_fields` döngüsü `std::ranges::all_of` oldu ve `parse_resume_state`
  (`cognitive complexity 30`) `parse_header` + `parse_segments` olarak bölündü.
- Tekrarını önleyen kontrol: baseline 6'nın üstü `check_linux.ps1` çıktısında "EXCEEDED" olarak
  görünür; yeni kod eklerken tidy advisory olsa da sayı okunur.

### 2026-11-19 — Sidecar okuyucu `ifstream` motorun kendi rename'ini kilitler (Windows)
- Belirti (önceden önlenen sınıf): Sidecar'ı checkpoint sırasında okuyan bir yoklayıcı veya test
  yardımcısı `std::ifstream`/paylaşımsız `CreateFileW` ile açtığında, motorun atomik
  `MoveFileExW(REPLACE_EXISTING)` çağrısı `ERROR_SHARING_VIOLATION` (32) alır. Checkpoint "sidecar
  yazılamadı" diye sessizce atlanır; indirme sürer ama resume verisi ilerlemez.
- Kök neden: Windows'ta paylaşım modu açan tarafın niyetidir. `FILE_SHARE_DELETE` verilmezse açık
  handle, hedef dosyanın rename ile değiştirilmesini engeller. `IStorage::open(read_only)` bunu
  doğru yapıyor: `CreateFileW(..., FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, ...)`
  (S3'ten beri). Sorun storage'da değil, **yanlış API seçiminde**: kısa bir JSON okumak için
  `ifstream` kullanmak doğal görünüyor ama paylaşım bayraklarını devre dışı bırakıyor.
- Düzeltme: Sidecar okumaları `detail::read_file_utf8` üzerinden `IStorage` (read_only) kullanıyor;
  boyut sınırı ayrıştırmadan önce uygulanıyor. Böylece UTF-8→UTF-16 dönüşümü de tek yerde kalıyor.
  Testteki yoklayıcılar da aynı yolu kullanır. **Ek bulgu:** paylaşım bayrakları tek başına yeterli
  değil — Windows'ta `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` hedef dosya açıkken, okuyucu
  `FILE_SHARE_DELETE` verse bile `ERROR_ACCESS_DENIED` (5) veriyor (yerel ölçüm: MoveFileEx 5,
  `ReplaceFileW` 0). Bu yüzden `replace_file` Windows'ta hedef varsa `ReplaceFileW(...,
  REPLACEFILE_WRITE_THROUGH)`, hedef yoksa `MoveFileExW(MOVEFILE_WRITE_THROUGH)` kullanıyor.
- Tekrarını önleyen kontrol: `ResumeStoreTest.ReplacesTheSidecarWhileAReaderHasItOpen` bir okuma
  handle'ı açıkken atomik yazma yapıyor; paylaşım bayrağı düşerse **veya** `ReplaceFileW` yerine
  `MoveFileExW`'ye dönülürse test Windows'ta kırmızıya döner. Kural: çalışma anında başka bir
  yazarın rename edebileceği dosyalar `ifstream` ile açılmaz; Windows'ta açık hedefi değiştirmek
  için `ReplaceFileW` kullanılır. Silme tarafı da aynı bayrağa bağlı:
  `ResumeStoreTest.DeletesTheSidecarWhileAReaderHasItOpen` açık handle varken silmeyi doğrular.

### 2026-11-21 — A/B ölçümü: aynı sunucudan ardışık indirme 429/403 yer, loopback gürültüye teslim
- Belirti: checkpoint'li sürümü S5a-1 CLI'sine karşı ölçerken thinkbroadband ardışık koşularda
  `exit=1` döndü (gövde yok, dosya oluşmadı); loopback 200 MB koşularında aynı varyant 0,45 s ile
  8,3 s arasında salındı, batch medyanları −%7…+%23 arasında değişti.
- Kök neden: (1) sunucu hız sınırı — aynı istemciden aralıksız büyük dosya istekleri 429/403 ile
  reddediliyor; ölçülen şey indirme süresi değil hata oluyor. (2) 200 MB loopback indirmesi ~0,5 s
  sürdüğü için toplam süre makine yüküne teslim (Defender, disk yazımı, Python test sunucusu);
  tek batch'in medyanı tekrarlanabilir değil.
- Düzeltme/ölçüm kuralı: WAN'da çiftler arasında ~10 s bekle ve `exit≠0` koşuyu tabloya alma;
  loopback'te en az 20 çift topla, **eşleşmiş oranın** (B/A) medyanını ve bootstrap %95 GA'sını
  raporla, ham medyan farkını yanına yaz. Sonuç: WAN −%2,7 (5 çift), loopback +%1,9 (61 çift,
  eşleşmiş medyan 0,988, GA [0,926; 1,066]).
- Tekrarını önleyen kontrol: benchmark betiği `exit` kodunu ve yazılan baytı kaydeder; sıfır olmayan
  çıkış satırı istatistiğe girmez. Tek batch'lik medyan farkına bakıp "regresyon var" denmez.

# FlowGet — Yol Haritası (todo)

> Onay durumu: **ONAYLANDI** — plan onaylandı; M0 tamamlandı, M1 dilimler halinde yürütülüyor.
> İlgili analiz: `docs/architecture-review.md`
> Kural: Bir madde ancak **kanıtıyla** (test çıktısı, ölçüm, ekran görüntüsü, CI linki) kapatılır.

---

## Süreç kuralları (her milestone için geçerli)

- [ ] Kod + test + dokümantasyon aynı PR'da
- [ ] `-Werror`, clang-tidy, clang-format temiz
- [ ] CI yeşil (Windows + Linux; Android etkileniyorsa Android job'u)
- [ ] Yeni davranış için test yazıldı (birim veya entegrasyon)
- [ ] Hata düzeltmesi yapıldıysa `tasks/lessons.md`'ye kök neden + ders eklendi
- [ ] Milestone sonunda kısa özet + ölçüm tablosu

---

## M0 — Karar, iskele, CI (≈2-3 gün) — **TAMAMLANDI**

**Hedef:** Boş ama *çalışan* monorepo; kararlar dondurulmuş; CI ilk kez yeşil.

- [x] ADR-0001: HTTP yığını = libcurl multi (gerekçe + sonuçlar)
- [x] ADR-0002: C ABI sözleşmesi (sürümleme, hata kodları, olay modeli)
- [x] ADR-0003: DB sahipliği = C++ core, WAL, checkpoint politikası
- [x] ADR-0004: Lisans + üçüncü parti politikası (GPL yasağı CI'da)
- [x] ADR-0005: Lisans seçimi (MIT) + isim kararı (FlowGet)
- [x] ADR-0006: Android medya hattı (Media3 Transformer, yt-dlp yok)
- [x] Dizin iskeleti: `core/ cli/ bindings/ app/ extension/ native_host/ tests/ docs/ tasks/`
- [x] `CMakePresets.json`: `windows-x64`, `windows-x64-release`, `linux-x64`, `linux-x64-asan`
- [x] `.clang-format`, `.clang-tidy`, `.editorconfig`, `.gitignore`, `.gitattributes`
- [x] CI: Windows (MSVC) + Ubuntu (Clang, ASan/UBSan) build & ctest; pre-commit ile sabitlenmiş
      clang-format; clang-tidy (M1'e kadar uyarı modunda)
- [x] `flowget-cli --version` çalışıyor → `flowget-cli 0.1.0 (C ABI v1)`
- [x] README iskeleti: proje nedir, ne değildir (DRM/kapsam), yol haritası
- [x] GTest 1.17.0 pinli `FetchContent`; 3 birim testi yeşil (`ctest` 1/1 geçti)
- [x] **M1/S1a'da tamamlandı:** `vcpkg.json` — ilk gerçek bağımlılıklar (curl, openssl, sqlite3, spdlog)
      + pinlenmiş registry baseline; `FLOWGET_VCPKG_ROOT` ile toolchain çözümü, CI vcpkg adımları.
- [x] **M1/S1a'da tamamlandı:** `scripts/check_dependencies.py` (GPL/AGPL denylist, ADR-0004) + CI `policy` job'u.

**DoD:** Temiz makinede `cmake --preset windows-x64 && cmake --build --preset windows-x64 && ctest` → yeşil; CI'da aynı adımlar yeşil.

**Kanıt (CI, 2026-11-14):** GitHub Actions run #3, commit `82cbf88` — Windows (MSVC), Linux
(Clang, ASan/UBSan) ve lisans/politika (`policy`) job'larının üçü de yeşil. Böylece "CI doğrulaması
push sonrası" notu kapandı.

**Kanıt (yerel, MSVC 19.52 / CMake 4.3.1):** configure 9.1 s · build 11 hedef · `ctest` 1/1 passed ·
ASCII olmayan dizin yolunda GTest keşfi kırıldı → `add_test`'e geçildi (`tasks/lessons.md`).

---

## M1 — Çekirdek motor: tek dosya, çok parçalı, resume, CLI (≈2-3 hafta)

**Hedef:** `flowget-cli get <URL> -o out.bin --connections 8` ile 100 MB dosyayı doğru, hızlı, kesintiden
sonra devam ettirilebilir şekilde indirmek. UI yok.

### Dilim planı ve ilerleme

| Dilim | Kapsam | Durum |
|---|---|---|
| S1a | vcpkg manifesti (curl, openssl, sqlite3, spdlog) + toolchain/CI + GPL denylist betiği | **TAMAMLANDI** |
| S1b | `HttpClient` + tek bağlantılı indirme + yerel test HTTP sunucusu | **TAMAMLANDI** |
| S2 | Probe (HEAD / `Range: bytes=0-0`, 206/200/416, metadata) | **TAMAMLANDI** |
| S3 | `IStorage` + preallocate + `pwrite` (Win32/POSIX) | **TAMAMLANDI** |
| S4 | `SegmentPlanner` + curl multi, 8 bağlantı, tek akışa düşüş | **TAMAMLANDI** |
| S4b | S4b-lite: clang-tidy dosya listesi + gerçek bulgular + baseline (`--warnings-as-errors` S8'e ertelendi) | **TAMAMLANDI** |
| S5 | Resume (`.flowget` sidecar, checkpoint, ETag değişimi) | **ONAYLANDI** — S5a-1 yürütülüyor |
| S6 | `RetryPolicy` + `RateLimiter` | |
| S7 | `DynamicSplitter` + benchmark tablosu | |
| S8 | `HashVerifier` + hata modeli + CLI bayrakları + RSS ölçümü | |

**S1a kanıtı (2026-11-14):** vcpkg `2026-10-29` / baseline `4702e38`; `curl 8.23.0 (schannel+nghttp2)`,
`OpenSSL 3.6.5`, `SQLite 3.54.1`, `spdlog 1.17.0` `flowget-cli --version` çıktısında;
`ctest` 1/1 passed; `check_dependencies.py` temiz manifestte `exit=0`, GPL fixture'da `exit=1`
(`libx264`, `some-agpl-package` yakalandı).

**S1a/CI düzeltmesi (2026-11-15):** Run #5'te (commit `51e7699`) Linux `Configure (debug)` düştü:
`actions/checkout` vcpkg'ı sığ (depth 1) klonladığı için `builtin-baseline` commit'i (`4702e38`)
klonda yoktu; master ucu `8f363bd`'ye ilerlemişti. Düzeltme: `scripts/fetch_vcpkg_baseline.py`
(vcpkg.json'dan SHA okur, `git fetch --depth=1 origin <sha>`, baseline dosyasının okunabildiğini
doğrular, `GITHUB_OUTPUT`'a `baseline=` yazar) iki job'da bootstrap'ten önce koşuyor; binary cache
anahtarı `vcpkg-<os>-<baseline>-<manifest>`; Linux toolchain adımı `ninja --version` ile üreteci
doğruluyor. Windows job'u run #5'te yeşildi ama aynı yapılandırma hatasını taşıyordu (aynı adım
ondaki de). Yerel kanıt: sığ klonda hata yeniden üretildi, script sonrası baseline okunabildi;
ikinci koşu idempotent (`already present`), üç hatalı girdi `exit=1`. Yerel build/`ctest`: configure +
build (24 hedef) + `ctest` 2/2 passed, `pre_commit run --all-files` temiz. CI yeşili push sonrası
doğrulandı: run #6, commit `8aaf255` — üç job da yeşil.

**S1b kanıtı (2026-11-14):** `flowget-cli get <URL> -o <dosya>` tek bağlantıyla indiriyor; yerel test
sunucusuna karşı 6 entegrasyon testi yeşil (`ctest` 2/2): gövde birebir yazılıyor (1 MiB), 404'te
dosya bırakmıyor, `chunked` (Content-Length yok) doğru, 302 takip ediliyor, iptal `aborted` +
kısmi dosya siliniyor, kopan bağlantı tespit ediliyor. CLI e2e: 5 MiB dosya indirildi, kaynak/hedef
SHA-256 aynı (`B26D7E22…`), 404 → `exit=1`, kısmi dosya yok.

**S2 kanıtı (2026-11-15):** `flowget-cli probe <URL>` çalışıyor; birim + entegrasyon toplam **42 test**
yeşil (`ctest` 2/2: 26 birim + 16 entegrasyon). Yerel Range sunucusuna karşı: 206 → `range_support:
supported`, `size: 1024000`, ETag/Last-Modified/Content-Disposition/filename doğru, `exit=0`; 0 baytlık
dosya → `416` + `size: 0` + `unsatisfiable`, `exit=0`; 404 → `exit=1`. Gerçek HTTPS
(`https://example.com/`, `curl 8.23.0 schannel`) → `206`, `size: 577`, ETag okundu.

### S2 — Probe — **TAMAMLANDI**

**Hedef:** `probe(url)` → tek çağrıda güvenilir sunucu metadata'sı; S4 segment planlayıcısının tek
girdisi.

- [x] `HttpRequest`'e `method` alanı (`get`/`head`); curl transport'ta `CURLOPT_NOBODY`. Ayrıca
      `HttpRequest::timeout_ms` (`CURLOPT_TIMEOUT_MS`) — probe'un HEAD zaman aşımı bunun üstünde.
- [x] `core/include/flowget/http_range.h` + `.cpp` — `ContentRange` ve `parse_content_range()`:
      `bytes a-b/N`, `bytes */N` (416), `*` (belirsiz toplam); boşluk/büyük-küçük harf toleransı; ters
      aralık, taşma, `last >= total` ve çöp girdi reddi.
- [x] `core/include/flowget/http_filename.h` + `.cpp` — `filename*` (RFC 5987, UTF-8) önceliği,
      `filename=` düşüşü, percent-decode, `/ \ : <>|"?*`, kontrol karakteri ve `..` temizliği; sonuç
      boşsa URL'in son segmenti, o da yoksa `download.bin`. (Plan adı `content_disposition.h` idi;
      iki kaynağı da kapsadığı için `http_filename.h` seçildi.)
- [x] `core/include/flowget/probe.h` + `core/src/probe.cpp` — **HEAD tamamen opsiyonel:** 403/405/501,
      zaman aşımı veya herhangi bir taşıma hatasında sessizce atlanır (`head_used=false`); otorite
      `GET Range: bytes=0-0`. Gövde 64 KiB'te kesilir (206'da 1 bayt), kesme kaynaklı
      `CURLE_WRITE_ERROR` beklenen durum sayılır → `206 supported`, `200 ignored`, `416 unsatisfiable`.
- [x] `ProbeResult`: `HttpError`, `status`, `content_length`, `range_support`, `etag`, `last_modified`,
      `accept_ranges`, `content_type`, `content_disposition`, `suggested_filename`, `effective_url`,
      `head_used`.
- [x] CLI: `flowget-cli probe <url> [--user-agent UA] [--header "Ad: değer"]…` → `anahtar: değer`
      satırları; `0` = başarı (Range yoksa da), `1` = taşıma/HTTP hatası, `2` = kullanım hatası.
- [x] Test sunucusu: `Response::honor_range` → `bytes=a-b`/`bytes=a-` için `206` + `Content-Range`,
      aralık dışı → `416` + `bytes */N`, `honor_range=false` → `200` + tam gövde; `Response::stall` →
      isteği okuyup hiç yanıt vermeme (HEAD takılması); HEAD yanıtlarında gövde gönderilmiyor.
- [x] Birim testleri (26): `Content-Range` parser (mutlu yol, `*/N`, 16 bozuk girdi) + dosya adı
      çıkarımı (tırnaklı/tırnaksız/RFC 5987/yol kaçışı/Windows karakterleri).
- [x] Entegrasyon testleri (`Probe.*`, 10): 206 + tam metadata; URL'den dosya adı; Range'i yok sayan
      sunucu (200, 64 KiB'te kesme); 416 (0 bayt); 404; **HEAD 403**; **HEAD takılması** (200 ms
      zaman aşımı, < 5 s'de dönüyor); `head_timeout_ms=0` ile hiç HEAD atmama; 302; ETag değişimi.
- [x] Yerel kanıt: `ctest` 2/2 (42 test), `flowget-cli probe` çıktıları, pre-commit temiz.
- [x] Push sonrası CI yeşil — **Kanıt (CI, 2026-11-15):** GitHub Actions run #6, commit `8aaf255` —
      Windows (MSVC), Linux (Clang, ASan/UBSan) ve `policy` job'larının üçü de yeşil. Linux derlemesi
      (`-Wconversion -Wsign-conversion -Werror`) böylece CI'da doğrulandı.

**Kararlar (onaylandı):**
1. **Asıl kaynak `Range` GET**, `HEAD` yalnızca metadata zenginleştirmesi: herhangi bir HEAD hatası
   (403/405/501, zaman aşımı, taşıma) probe'u başarısız saymaz; imzalı URL'ler yalnızca GET kabul
   ettiği için bu şart. HEAD `head_timeout_ms=0` ile tamamen kapatılabilir.
2. **`Accept-Ranges: bytes` tek başına kanıt sayılmaz** (yalan söyleyen sunucu var); karar 206/416'dan
   verilir, `Accept-Ranges` yalnızca bilgi olarak taşınır.
3. Multi-range (`multipart/byteranges`) desteklenmez: yalnızca `bytes=0-0` gönderilir.
4. `416` taşıma katmanında `http_status` olarak görünür (4xx genel kuralı); probe bu tek durumu
   anlamlı sonuç sayar — S5'te sidecar doğrulaması da aynı ayrımı kullanacak.

### S3 — IStorage + preallocate + pwrite — **TAMAMLANDI**

**Hedef:** S4'ün ihtiyacı olan dosya katmanı: aynı dosyaya 8 işçi farklı offset'lerden yazar; yol UTF-8
(Windows'ta UTF-16'ya çevrilir); ön tahsis isteğe bağlıdır ve başarısızlığı indirmeyi sessizce bozmaz.

**Onaylanan kararlar (2026-11-15):**
1. Arayüzde yol tipi **UTF-8 `std::string`** (`std::filesystem::path` değil).
2. Preallocate desteklenmiyorsa **`preallocate_unsupported` + devam**; `ENOSPC`/`EIO` **fatal**.
3. `SingleStreamDownloader` bu dilimde değişmez; taşıma işi **S4'ün ilk maddesi**.

**Dosyalar**
- [x] `core/include/flowget/storage.h` — `IStorage`, `StorageError`, `StorageResult`, `OpenMode`,
      `FileOffset`, `make_platform_storage()`
- [x] `core/include/flowget/storage_detail.h` — yalnız test seam'i: kısmi yazma/EINTR döngüsü
      (`flowget::detail::io_all`); C ABI'nin parçası değil
- [x] `core/src/storage.cpp` — `storage_error_name`, `storage_error_is_fatal`, platform seçimi
- [x] `core/src/storage_win32.cpp`, `core/src/storage_posix.cpp` — tek arayüz, iki uygulama
- [x] `core/CMakeLists.txt` — yeni kaynaklar; POSIX'te `_FILE_OFFSET_BITS=64` **hedef düzeyinde** +
      `static_assert(sizeof(off_t) >= sizeof(std::int64_t))`
- [x] `tests/unit/storage_test.cpp`, `tests/integration/storage_io_test.cpp` + `tests/CMakeLists.txt`

**Arayüz**
- `open(const std::string& utf8_path, OpenMode)`; `OpenMode = { read_only, read_write, read_write_truncate }`
  (`read_write`: varsa açar, veriyi korur — resume; `read_write_truncate`: oluşturur + 0'a indirir)
- `reopen()` (aynı yol/mod ile kapat+aç), `close()` (idempotent), `is_open()`
- `preallocate(int64 size)`, `write_at(offset, data, size, out written)`, `read_at(...)`, `flush()`,
  `size(out)`
- **Yol neden `std::filesystem::path` değil:** Windows'ta `path`'i `std::string`'den kurmak ANSI kod
  sayfası kullanır (`tasks/lessons.md`: "Varsayılan Proje"). UTF-8 `std::string` alıp içeride
  `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS)` ile `wstring`'e çevirmek **doğru davranışı
  varsayılan** yapar; kural geliştiricinin belleğine kalmaz. C ABI de UTF-8 `char*` taşıyor.
- Geçersiz UTF-8 → `invalid_argument` (Windows). POSIX'te yol baytları opak → `open` hatası.

**Windows**
- `CreateFileW` + `FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE`
- Offset'e yazma: `FILE_FLAG_OVERLAPPED` **kullanılmaz**. Senkron handle + her çağrıda yığ üzerinde
  sıfırlanmış `OVERLAPPED{Offset, OffsetHigh}` ile `WriteFile(..., &written, &ov)`. MS dokümanı:
  senkron handle'da `lpOverlapped` non-NULL ise yazma verilen offset'ten başlar ve çağrı bitene kadar
  döner. Paylaşılan dosya konumu kullanılmaz → **mutex yok**, 8 işçi paralel yazar. (Yedek plan:
  `SetFilePointerEx` + iç mutex; profilde çekişme görülürse.)
- `preallocate`: `GetFileSizeEx`; istenen ≤ mevcut → hiçbir şey yapma (asla küçültme).
  `SetFileInformationByHandle(FileAllocationInfo)` kümeleri rezerve eder (EOF'u uzatması garanti
  değil), ardından `SetFilePointerEx` + `SetEndOfFile` ile EOF = size (posix_fallocate ile aynı
  gözlenebilir davranış). Rezervasyon `ERROR_INVALID_PARAMETER/NOT_SUPPORTED/CALL_NOT_IMPLEMENTED`
  ile düşerse → `preallocate_unsupported` (fatal değil). `SetEndOfFile` hatası (disk dolu) →
  `preallocate` (fatal).
- `flush` = `FlushFileBuffers`. Tek çağrı ≤ 64 MiB parçalara bölünür; `size_t`→`DWORD` açık cast +
  aralık kontrolü (`-Wconversion` tuzağı).

**POSIX**
- `open(..., O_CLOEXEC)`, `pread`/`pwrite`, `ftruncate`, `fsync` (Android'de yt-dlp `fork/exec`
  edecek; fd sızmasın diye `O_CLOEXEC` şart)
- Tür dönüşümü: `ssize_t n = ::pwrite(fd, data, size, static_cast<off_t>(offset))`; `n < 0` → `errno`;
  `n == 0` → hata (ilerleme yok); `written += static_cast<std::size_t>(n)` yalnız `n > 0` sonrası.
  Offset negatif veya `off_t`'ye sığmıyorsa `invalid_argument`. `size_t` uzunluk değişmez.
- `posix_fallocate` **hata kodunu döndürür** (errno'ya yazmaz!). `EOPNOTSUPP/ENOSYS/EINVAL` →
  `ftruncate(size)` ile EOF uzat + `preallocate_unsupported`; `ENOSPC/EIO` → `preallocate` (fatal).

**Kısmi yazma / EINTR**
- Tek döngü (`flowget::detail::io_all`), iki platform da onu kullanır: dönen bayt < istenen → offset
  ilerlet, devam; `EINTR` (POSIX'te ayrıca `EAGAIN`) → aynı offset'ten tekrar; `n == 0` → hata;
  ardışık 100 ilerlemesiz tekrar → hata (sonsuz döngü koruması).
- Testler sahte syscall ile deterministik: 1 baytlık kısmi yazmalar, iki kez `EINTR`, aralıklı 0
  dönüşü, 100+ `EINTR` → hata.

**Hata modeli**
- Metotlar `StorageResult{error, platform_code, message}` döner (`platform_code` = `GetLastError()` /
  `errno`). Exception sınırı aşmaz (ADR-0002); `noexcept` işaretlenmez (message `std::string` tahsisi).
- `storage_error_is_fatal()`: `none` ve `preallocate_unsupported` fatal değil; diğerleri fatal.
- `write_at` hatasında `written` o ana kadar yazılan baytı verir (S6 retry aynı offset'ten devam eder).

**Eşzamanlılık sözleşmesi**
- `write_at` / `read_at`: aynı instance'ta çok iş parçacığından güvenli (paylaşılan dosya konumu yok:
  Windows'ta yığ `OVERLAPPED`, POSIX'te `pwrite`/`pread`).
- `open/reopen/close/preallocate/flush/size`: çağıran dışlar (S4 işçiler durmuşken çağırır); başlıkta
  açıkça yazılır.

**Testler**
- Birim: `storage_error_name`, `storage_error_is_fatal`, `io_all` (kısmi/EINTR/0/100 tekrar), kapalıyken
  I/O → `not_open`, ikinci `open` → `already_open`, negatif offset/size → `invalid_argument`.
- Entegrasyon (gerçek dosya, build dizini): preallocate 8 MiB → `size() == 8 MiB`; **8 iş parçacığı aynı
  dosyanın ayrık aralıklarına aynı anda `write_at`** → okunan içerik birebir; sıra dışı offset + üzerine
  yazma; EOF ötesi `read_at` → hata; `reopen` sonrası veri yerinde; `read_write_truncate` 5 MiB dosyayı
  0'a indirir; `read_only` dosyaya yazma → hata; iki kez `close` güvenli; preallocate mevcut veriyi
  küçültmez; ASCII olmayan yol (`İndirilen_çğış.bin`, ayrıca `İndirilenler/şğü-中文-🚀.bin`) ile
  open/write/read round-trip (hem Windows hem POSIX dalı).
- Yerel kanıt: `ctest` 2/2 (mevcut 42 + ~15 yeni), `pre_commit run --all-files` temiz, build ASCII
  olmayan yolda da tekrar edilir.

**Kapsam dışı (bilinçli)**
- `SingleStreamDownloader` bu dilimde `IStorage`'a taşınmaz. **Bilinen bug:** bugünkü `ofstream` +
  `path.string()` ANSI yolu ASCII olmayan yolda kırabilir; S4'te IStorage'a geçişle kapanacak.
- Windows long-path (`\\?\`, > 260 karakter), sparse/`SetFileValidData`, sektör hizalı I/O yok.

**Riskler / tuzaklar**
- `SetFileInformationByHandle` `GENERIC_WRITE` ister (bizde var); FAT/exFAT/ağ paylaşımında
  desteklenmez → downgrade yolu sınıflandırma birim testiyle doğrulanır.
- `posix_fallocate` bazı dosya sistemlerinde yavaş; 1 GB ölçümü S7 benchmark'ına not.
- **Linux derlemesi yerelde doğrulanamaz** (Windows makine): `-Wconversion/-Wsign-conversion` ve
  ASan/UBSan sonucu push sonrası CI'da doğrulanacak; bu açıkça raporlanacak.

**DoD:** yukarıdaki testler yeşil, `-Werror` temiz, clang-format temiz, ASCII olmayan yolda round-trip
kanıtlı, Linux CI yeşili (run #8).

**Kanıt (yerel, MSVC 19.52 / CMake 4.3.1, 2026-11-15):** commit `f7170b5`. `/W4 /WX` build temiz
(uyarı yok); `ctest` 2/2: **39 birim + 30 entegrasyon = 69 test** (S3 öncesi 42).
- 8 iş parçacığı aynı dosyanın ayrık aralıklarına eşzamanlı `write_at` → tüm dosya ve dilim dilim
  içerik birebir (`EightThreadsWriteDisjointRangesConcurrently`).
- ASCII olmayan dizin+ad (`İndirilenler/İndirilen_çğış.bin`, ayrıca `şğü-中文-🚀.bin`) ile
  open/write/flush/close/reopen/read round-trip; dosya sistemi aynı adı görüyor.
- Preallocate 64 KiB / 1 MiB / 5 MiB sonrası `size()` doğru, önceden yazılmış veri korunuyor, küçültme
  yok; `read_write_truncate` 5 MiB dosyayı 0'a indirir; `read_only` yazmayı reddeder; EOF ötesi okuma
  hata döner; `close` idempotent; geçersiz argümanlar (`-1` offset, null buffer, offset+size taşması)
  `invalid_argument`.
- `io_all` kısmi yazma / iki `EINTR` / 0 bayt / 100+ `EINTR` senaryoları sahte syscall ile; 0 bayt
  tekrar denemez, 100 tekrardan sonra hata, sınırda (100) başarı.
- `pre_commit run --all-files` temiz.

**S3 sırasında bulunan tasarım hatası:** `reopen()` ilk açılış modunu (truncate dahil) yeniden
uyguluyordu ve yarım kalmış dosyayı siliyordu. Düzeltme: truncate yalnız oluşturma anında geçerli,
`reopen()` her zaman `read_write`/`read_only` ile açar; `ReopenAfterTruncateModeKeepsData` testi bunu
kilitliyor (`tasks/lessons.md`).

**Yerelde doğrulanamayan:** Linux dalı (Clang `-Wconversion -Wsign-conversion -Werror`, ASan/UBSan) —
`PosixStorage` bu makinede hiç derlenmedi; push sonrası CI doğrulayacak. `_FILE_OFFSET_BITS=64` ve
`off_t` `static_assert`'leri bilinçli olarak build zamanında yakalar.

> **2026-11-15 güncellemesi — bu not geçersiz:** Linux presetleri artık yerelde koşuluyor.
> `scripts/check_linux.ps1` depoyu WSL2'deki `~/flowget` klonuna senkronize edip `linux-x64` (debug) ve
> `linux-x64-asan` (ASan/UBSan) için configure + build + `ctest` çalıştırıyor; iki preset için de
> `2/2 tests passed` (Clang 21.1.8, cmake 4.2.3). `AGENTS.md`'deki "Linux derlemesi yerelde
> doğrulanamaz" kuralı bu nedenle güncellendi; arka plan `tasks/lessons.md` (2026-11-15).

**Kanıt (CI, 2026-11-15):** GitHub Actions run #8, commit `b180dc0` — Windows (MSVC), Linux
(Clang, ASan/UBSan) ve `policy` job'larının üçü de yeşil. Linux job'u `PosixStorage`'ı
`-Wconversion -Wsign-conversion -Werror` ile derledi ve ASan/UBSan `ctest`'ini koşturdu; böylece
"Linux CI push sonrası doğrulanacak" notu kapandı.

### S4 — SegmentPlanner + curl multi, çok parçalı indirme, tek akışa düşüş — **TAMAMLANDI (2026-11-16)**

**Hedef:** `flowget-cli get <url> -o out.bin --connections 8` tek dosyayı N aralığa bölüp aynı anda
indirir; sunucu/aralık koşulları sağlanmazsa **bir kez** tek akışa düşer. Motor tek iş parçacığında
`curl_multi` sürer (thread-per-segment yok, ADR-0001).

**Onaylanan kararlar (kullanıcı, 2026-11-16)**
1. `--connections` varsayılan **8**, geçerli aralık **1..64**. N=1: **probe yok**, doğrudan tek akış.
   (architecture-review §2.1'deki "üst sınır 16" notu bu kararla geçersiz; 64 üst sınırı bilinçli,
   S6/S7 adaptif düşürmeyi getirecek.)
2. `DownloadOptions::output_path` **UTF-8 `std::string`** (`std::filesystem::path` değil); Windows
   CLI'da `wmain` + UTF-8 dönüşümü bu dilime dahil.
3. **Tek akışa düşüş** (multi iptal → kısmi dosya sil → baştan tek akış, yalnız bir kez):
   probe `ignored`; `Content-Length` yok/bilinmiyor; dosya küçük (plan < 2 segment); segment yanıtı
   `200`; `Content-Range` uyuşmazlığı; `Content-Length` uyuşmazlığı; probe `ETag` ile segment `ETag`
   uyuşmazlığı; yazılan bayt sayısı segment uzunluğundan farklı.
4. **Taşıma hatası** (`CURLE_PARTIAL_FILE`, `recv`, `connect`, timeout) → **hata**: kısmi dosya silinir,
   tek akışa düşülmez (yeniden deneme S6). Disk/`IStorage` hatası da hatadır, fallback değil.
5. Keep-alive testi S5'e ertelendi; ayarlar (`CURLOPT_TCP_KEEPALIVE`) kodda uygulanır.

**Ek istekler (onaylandı) ve çözüm**
- **a) Ayrı iptal watcher thread'i yok.** Motor döngüsü çağıran thread'de döner; her turda iptal
  jetonunu okur ve `curl_multi_poll`u **50 ms** zaman aşımıyla bekler → en kötü iptal gecikmesi ~50 ms
  (S1b taşımasındaki sınırla aynı). `curl_multi_wakeup` yalnız *başka* bir thread'den multi'yi
  uyandırmak içindir; tek sürücü thread varken ek thread açmak gereksiz karmaşıklık olur.
  `CURLOPT_XFERINFOFUNCTION` de jetonu okur (tek bir yazma aralığı uzun sürse bile).
- **b)** `linux-x64-tsan` configure/build/test preset'i (ThreadSanitizer) eklendi;
  `scripts/check_linux.ps1` `linux-x64`, `linux-x64-asan`, `linux-x64-tsan` üçünü koşar. CI'a ekleme
  ayrı iş (bu dilimde değil).
- **c)** SHA-256 yardımcısı `core/` içinde: `flowget/sha256.h` + `core/src/sha256.cpp`, OpenSSL EVP,
  akışlı (`update`/`hex_digest`). S8 `HashVerifier`'ın tabanı; `tests/support`'a yazılmaz.
- **d)** `CURLOPT_BUFFERSIZE = 256 KiB` (ortak curl ayarlarında). Disk yazımı motor thread'inde kalır;
  S7 benchmark'ına not: 256 KiB tamponla ağ hızı vs `pwrite` gecikmesi ölçülecek, darboğaz çıkarsa
  tek yazıcı thread + segment kuyruğu (architecture-review §2.3) değerlendirilecek.
- **e) İlk madde:** `SingleStreamDownloader` `IStorage`'a taşınır ve ASCII olmayan hedef yolla test
  edilir (`ifstream(path)` ANSI tuzağına düşmemek için doğrulama `IStorage::read_at` ile yapılır).
- **f)** clang-tidy temizliği S4'e **dahil değil**; aşağıdaki **S4b** dilimi.

**Mimari**
- **`SegmentPlanner`** (saf fonksiyon): `minimum_segment_size(total) = max(1 MiB, total/64)`;
  segment sayısı `clamp(connections, 1, total / min_segment_size)`; ilk `total % count` segment bir
  bayt uzun. Aralıklar bitişik, boşluksuz, toplam == boyut; `total == 0` → boş plan.
- **Motor** (`core/src/segmented_download.cpp`) libcurl multi'yi **doğrudan** sürer. Gerekçe:
  uyuşmazlık başlık aşamasında görülünce gövdeyi tüketmeden tüm multi iptal edilmeli; `HttpTransport::
  get()` tek istek, senkron arayüzü bunu ifade edemez. Easy-handle ortak ayarları
  `core/src/curl_common.h`'a taşınır; `curl_transport.cpp` da onu kullanır (iki kopya drift etmez).
- **`HttpRequest::range_start` / `range_length`** (curl `CURLOPT_RANGE`). Aralık isteği varsa
  HTTP/1.1 zorlanır ve her segment **yeni bağlantı** açar (`CURLOPT_FRESH_CONNECT`); HTTP/2
  multiplexing tüm segmentleri tek TCP'ye bindirir, yeniden kullanılan boş bağlantı da segmentleri
  sıraya sokar. `CURLMOPT_MAX_HOST_CONNECTIONS = CURLMOPT_MAX_TOTAL_CONNECTIONS = plan boyutu`
  (bağlantı havuzu varsayılanının 8 segmenti kırmaması için).
- **Başlık doğrulaması başlık callback'inde** (boş satır = başlık sonu): durum `206`; `Content-Range`
  `first/last/total` planlanan aralık ve probe boyutuyla aynı; `Content-Length` **varsa** segment
  uzunluğuna eşit (yoksa gövde sonu bayt sayacı belirler — RFC 9110 eksikliğe izin verir); iki tarafta
  da doluysa `ETag` probe ile aynı. Uyuşmazlık → o transfer `CURLE_WRITE_ERROR` ile iptal edilir,
  nedeni kaydedilir, motor kalanları da durdurur → **fallback**.
- **Gövde:** `write_at(segment.offset + written, data, size)`. Bitişte `written == length` değilse
  bayt sayısı uyuşmazlığı → fallback. `IStorage` hatası → fatal (fallback yok).
- **İstek başlıkları:** `If-Range: <ETag>` yalnız probe ETag'i doluysa; `Accept-Encoding: identity`;
  `CURLOPT_BUFFERSIZE` 256 KiB; TCP keepalive; user-agent + `DownloadOptions::headers`.
- **Dosya akışı:** probe → plan → üst dizini oluştur → `open(read_write_truncate)` →
  `preallocate(size)` (`preallocate_unsupported` → uyarı, devam; fatal → hata) → motor → `flush` →
  `close`. Fallback/hata yolunda `close` + dosya silme. `open` UTF-8 yolu
  `std::filesystem::path(u8string)` ile kurar (ANSI tuzağı yok).
- **Özet:** `DownloadSummary{http, bytes_written, message, connections, fallback, fallback_reason}`.
  Segmentli başarıda `http.response` probe metadata'sından sentezlenir (`status = 206`,
  `content_length = boyut`, etag/effective_url probe'dan). Boş kaynak (`bytes */0`): HTTP isteği yok,
  boş dosya oluşturulur (`bytes_written = 0`, `connections = 0`).
- **`Downloader`** tek giriş noktası: `connections == 1` → probe'suz tek akış; aksi halde probe +
  segmentli deneme. CLI `Downloader` kullanır; `SingleStreamDownloader` fallback olarak kalır.

**Dosyalar**
- [x] `core/include/flowget/segment_planner.h`, `core/src/segment_planner.cpp`
- [x] `core/include/flowget/download.h` — ortak `DownloadOptions` / `DownloadProgress` /
      `DownloadSummary` / `ProgressCallback` tipleri (UTF-8 yol, `connections`, `headers`)
- [x] `core/include/flowget/downloader.h`, `core/src/downloader.cpp` — probe → plan → motor → fallback
- [x] `core/src/curl_common.h` — RAII handle'lar, global init, hata eşlemesi, ortak easy ayarları,
      başlık toplayıcı (256 KiB tampon, HTTP/1.1 + no pipelining aralıkta)
- [x] `core/src/segmented_download.h/.cpp` — çok segmentli motor (başlık doğrulaması, iptal, ilerleme)
- [x] `core/include/flowget/sha256.h`, `core/src/sha256.cpp` — akışlı SHA-256 (OpenSSL EVP)
- [x] `core/src/download_util.h` — UTF-8 yol → `std::filesystem::path`, üst dizin oluşturma
- [x] `core/src/single_stream_download.cpp` — `IStorage`'a geçiş (S4'ün ilk maddesi)
- [x] `core/src/curl_transport.cpp` — ortak `curl_common.h` kullanımı + `range_start/length`
- [x] `cli/src/main.cpp` — `wmain` + UTF-8 argv dönüşümü, konsol çıkışı UTF-8, `--connections`,
      `--header`, `Downloader` kullanımı, fallback notu
- [x] `CMakePresets.json` + `scripts/check_linux.ps1` — `linux-x64-tsan` preset'i ve üç preset koşusu
- [x] `tests/support/http_test_server.{h,cpp}` — `omit_content_length`, `content_length_override`,
      `content_range_override` (uyuşmazlık senaryoları)
- [x] `tests/unit/segment_planner_test.cpp`, `tests/unit/sha256_test.cpp`
- [x] `tests/integration/downloader_test.cpp` (`SegmentedDownload` süiti); `single_stream_download_test.cpp` güncellemesi
      (UTF-8 `std::string` yol + ASCII olmayan hedef)
- [x] `tests/CMakeLists.txt`, `core/CMakeLists.txt` — yeni kaynaklar/hedefler

**Testler**
- Birim (planner): 100 MiB/8 → bitişik, boşluksuz, toplam == boyut; boyutlar farkı ≤ 1; `0` → boş;
  `connections < 1` / `> 64` kırpma; `connections` > olası segment sayısı → daha az segment; tam
  bölünen/bölünmeyen/asal boyutlar; 1 GiB/64; `int64` sınırına yakın boyutta taşma yok.
- Birim (SHA-256): boş giriş, `"abc"`, 1 MiB tek seferde vs rastgele parçalarla akışlı (aynı özet);
  bilinen NIST vektörleri.
- Entegrasyon (yerel sunucu, gerçek dosya):
  - 8 MiB, `connections=4` → içerik birebir, boyut doğru, `fallback == false`, sunucuya ≥ 5 istek
    (HEAD + probe + 4 segment).
  - `connections=1` → **probe yok**: sunucuya tek GET (request_count == 1), içerik doğru.
  - Range'i yok sayan sunucu → tek akışa düşer (`fallback == true`), içerik doğru.
  - Segment yanıtı `200` (yalnız probe aralığına 206) → fallback, içerik doğru; gövde tüketilmeden
    iptal edildiği için indirilen bayt toplamı dosya boyutundan küçük kalır.
  - `Content-Range` uyuşmazlığı → fallback, içerik doğru.
  - `Content-Length` uyuşmazlığı → fallback, içerik doğru.
  - `Content-Length` yok + kısa gövde → bayt sayısı uyuşmazlığı → fallback, içerik doğru.
  - probe sonrası `ETag` değişimi → fallback, içerik doğru (sessiz bozulma yok).
  - Segmentte kopan bağlantı → **hata**, kısmi dosya yok, fallback yok.
  - Throttle'lı sunucuda iptal → `aborted`, kısmi dosya yok, < 5 s'de dönüş.
  - Boş kaynak (0 bayt) → boş dosya, `bytes_written == 0`.
  - Mevcut büyük dosyanın üzerine indirme → truncate, içerik birebir.
  - ASCII olmayan hedef (`İndirilenler/İndirilen_çğış.bin`) → tek akış ve segmentli yollar; içerik
    `IStorage::read_at` ile doğrulanır.
- Yerel kanıt: `ctest` (birim + entegrasyon) yeşil, `pre_commit run --all-files` temiz,
  `scripts/check_linux.ps1` üç preset için `exit 0`.
- CLI e2e: yerel sunucudan 8 MiB indirme, kaynak/hedef SHA-256 aynı; `--connections 0` / `65` → `exit=2`;
  ASCII olmayan hedef yolla indirme.

**Riskler / tuzaklar**
- `CURLMOPT_MAX_HOST_CONNECTIONS` varsayılanı 6 → açıkça yükseltilmezse 8 segment 6 bağlantıya biner.
- HTTP/2 multiplexing → aralık isteklerinde HTTP/1.1 + pipelining kapalı.
- `CURLOPT_RANGE` + `Content-Encoding` → `identity` şart (gzip offsetleri kaydırır).
- libcurl `curl_multi_poll` 50 ms: iptal gecikmesi kabul edilebilir; CPU boşta ~0.
- Eşik altı dosyalar (≈2 MiB'dan küçük) hep tek akış — segmentasyon kazancı yok, doğru davranış.
- Fallback yalnız **bir kez**; ikinci denemede de uyuşmazlık olursa hata (sonsuz döngü yok).
- TSan, libcurl/OpenSSL'in kendi içindeki yarışları göremez (instrument edilmemiş); bizim kod
  (depolama, iptal, motor durumu) taranır.

**DoD:** Yukarıdaki testler yeşil; `-Werror` + clang-format temiz; `check_linux.ps1` (debug, ASan/UBSan,
TSan) `exit 0`; CLI e2e hash eşleşmesi; fallback/hata sınıfları testlerle kilitli.

**Kanıt (2026-11-16):**
- Commit'ler: `1cffbf6` (tek akış `IStorage`), `abd8f0e` (planner), `f38267e` (SHA-256),
  `a1da161` (motor + fallback + testler), `31ed6ee` (CLI), `152c6db` (TSan preset'i).
- Windows x64 (MSVC, `-Werror`) `ctest`: **2/2 passed** — `flowget_unit_tests` 59 test / 10 süit
  (planner 11, SHA-256 9), `flowget_integration_tests` 44 test / 4 süit (13'ü `SegmentedDownload`),
  entegrasyon 4.5 s.
- `scripts/check_linux.ps1` (WSL2 Ubuntu, Clang 21.1.8): `linux-x64` (debug), `linux-x64-asan`
  (ASan+UBSan), `linux-x64-tsan` (TSan) — üçünde de `ctest` **2/2 passed**, `exit 0` (~1 dk).
- TSan ilk koşuda test altyapısında `HttpTestServer::Impl::listener` yarışı raporladı (üretim kodu
  değil): alan `std::atomic` yapıldı, yıkıcı `exchange → shutdown + close → join` sırasına alındı
  (Windows'ta `accept`'i uyandıran `closesocket`, POSIX'te `shutdown`). Sonraki koşu temiz.
- CLI e2e (9 MiB, range destekli yerel sunucu, `--connections 4`): aynı anda 4 bağlantı; kaynak ve
  hedef SHA-256 `9E6B4366044E90A16BE4A5651807609F32F873F1877E686012022668BD041D24`;
  `--connections 0` / `65` → `exit=2`; range'siz sunucuda fallback ve ASCII olmayan hedef
  (`İndirilenler/…`) yolla indirme doğrulandı.
- `py -m pre_commit run --all-files`: temiz (clang-format, trailing whitespace, yaml/json).
- **Kanıt (CI, 2026-11-16):** GitHub Actions **run #12**, commit `6437b8a` (S4'ün merge commit'i;
  push sonrası amend uzlaştırması dahil) — Windows (MSVC), Linux (Clang, ASan/UBSan) ve `policy`
  job'larının üçü de yeşil. Böylece S4'ün "CI doğrulaması push sonrası" notu kapandı.

### S4b-lite — clang-tidy temizliği (S4 sonrası, ayrı dilim)

**Neden ayrı:** S4 boyunca clang-tidy uyarı modunda kalır; kodun şekli otursun, sonra tek geçişte
bulgular düzeltilsin. Karışık commit (davranış + stil) review'u zorlaştırır.

**Kapsam kararı:** Tam S4b (tüm bulguların sıfırlanması + `--warnings-as-errors`) ertelendi. Bu dilim
yalnız "gürültüyü kes + gerçek bulguları düzelt" işini yapar; kalan bulgular baseline'da kayıtlıdır.

- [x] Dosya listesi `compile_commands.json`'dan üretilir (elle liste yok): `scripts/clang_tidy.py`
      `core/` + `cli/` TU'larını seçer. Eski `git ls-files 'core/*.cpp'` komutu Linux'ta
      `storage_win32.cpp` gibi derlenmeyen dosyaları istiyordu ("not found in compilation database").
- [x] `tests/` **bilinçli olarak kapsam dışı**: test kodu farklı stil profiline sahip, baseline'ı domine
      ederdi. Tidy yalnız üretim kodunu (`core/`, `cli/`) ölçer.
- [x] Yerel `const` değişkenlerde **k-prefix zorunlu değil** (`LocalConstantCase: lower_case`,
      prefix boş). clang-tidy 18'de constexpr'e özel ayar yok; yerel `k...` sabitleri
      `LocalConstantIgnoredRegexp: k[A-Z].*` ile muaf. Global/static sabitler `kCamelCase` kalır.
- [x] Enum adlandırması projedeki gerçek stile hizalandı: C++ değerleri `lower_case`, C ABI
      (`FLOWGET_*`, `flowget_status`) `.*FLOWGET_.*` / `.*flowget_.*` ile muaf. `IgnoredRegexp` tam
      (qualified) adla eşleştiği için desenler `.*` ile sarıldı.
- [x] `misc-include-cleaner` ve `performance-enum-size` **kapatıldı**; gerekçe `.clang-tidy` içinde
      yorum olarak duruyor. curl callback `reinterpret_cast`'leri için gerekçeli NOLINT eklendi.
- [x] Gerçek bulgular düzeltildi (davranış değişmedi): widening çarpımları hedef tipte, `branch-clone`,
      `misc-const-correctness`, `readability-math-missing-parentheses`, `missing-std-forward`,
      `modernize-use-auto`, `starts_with`, `std::array`, `std::string_view`, storage sınıflarında
      special-member delete'leri, `no-automatic-move`. Sınır kaynaklı 4 bulgu (maskelenmiş digest
      indeksi, sınırı aynı satırda kontrol edilen status lookup, libcurl `CURLMSG` union'ı) gerekçeli
      NOLINT ile kapandı.
- [x] CI ile aynı sürüm: **clang-tidy-18** (CI `apt-get install clang-tidy-18`, yerel WSL 18.1.8).
- [x] Baseline: **6 bulgu** (`scripts/clang_tidy_baseline.txt`). Kural: yeni kod yeni bulgu eklemez;
      sayı yalnız bulgu düzeltilince düşürülür. Kalan 6 = 4 yüksek bilişsel karmaşıklık fonksiyonu
      (`run_get`, `run_probe`, `run_segmented`, `download`) + 2 referans üye (`client_`); ikisi de
      gerçek refactor, sonraki milestone'lara bırakıldı.
- [x] `scripts/check_linux.ps1` tidy adımı **advisory**: bulgu sayısı baseline'ı aşarsa uyarı yazıp
      devam eder (exit 2 → advisory); yalnız araç hatasında (clang-tidy yok, DB yok, liste boş →
      exit 1) durur. CI'da `continue-on-error: true` korunuyor.
- [x] `--extra-arg` **gerekmedi**: clang-tidy 18 `compile_commands.json` bayraklarını olduğu gibi
      okudu; gerçek derleme hataları da susturulmadı.
- [ ] `--warnings-as-errors` ve CI'daki `continue-on-error` kaldırma **M1 sonuna (S8) ertelendi**;
      o zamana kadar clang-tidy advisory kalır.
- [x] Kanıt (2026-11-17): Windows MSVC build + ctest 2/2 yeşil; `scripts/check_linux.ps1` 3 preset
      (linux-x64 debug, ASan/UBSan, TSan) PASSED; aynı çalıştırmada `clang-tidy: 6 finding(s) in
      18 file(s); baseline 6 -> OK`. Baseline gate negatif testi: baseline 5 yapıldığında script
      exit 2 + "EXCEEDED by 1" verdi, `check_linux` yine PASSED (advisory) — beklenen davranış.
- [x] Kanıt (CI, 2026-11-17): GitHub Actions **run #13**, commit `9a5b417` — Windows (MSVC), Linux
      (Clang, ASan/UBSan) ve `policy` job'larının üçü de yeşil. CI'daki clang-tidy adımı **6 bulgu**
      verdi = baseline 6, yani gate "yeni bulgu yok" kuralını tutuyor; `continue-on-error` S8'e kadar
      advisory kalıyor.

### S5 — Resume (`.flowget` sidecar, checkpoint, ETag/Last-Modified) — **PLAN, ONAYLANDI (2026-11-17)**

**Hedef:** Kesilen indirme, dosya ve sunucu değişmediyse kaldığı yerden devam eder; değiştiyse sessizce
bozuk dosya üretmez, baştan indirir. M1'de JSON sidecar; M2'de SQLite'a taşınır.

**Dilim bölünmesi (her dilim ayrı commit + ayrı kanıt):**

- **S5a-1** — `ResumeState` modeli + atomik yazma + katı doğrulama. Ağ yok; yalnız birim ve dosya testleri.
- **S5a-2** — motor + CLI entegrasyonu: checkpoint, kalan aralıklar, `If-Range`, Ctrl+C, `--no-resume`.
- **S5b** — tek akış devamı + kayıp ve performans ölçümleri.

#### Onaylanan kararlar (2026-11-17)

1. **JSON: `nlohmann/json`** (MIT, header-only). `vcpkg.json`'a eklenir; ayrıştırma
   `json::parse(..., allow_exceptions=false)` ile yapılır (ADR-0002: exception sınırı geçmez).
   `check_dependencies.py` temiz olmalı; build süresi etkisi ölçülüp kanıta yazılır.
2. **Resume varsayılan açık:** geçerli sidecar varsa devam edilir ve `Resuming from X MiB` yazılır.
   Zorla sıfırdan başlatmak için `--no-resume`.
3. **Sidecar'daki plan kazanır:** segment sayısı/offsetleri sidecar'da durur; resume'da `--connections`
   yok sayılır (yeni indirmede geçerlidir).
4. **Şüphede sıfırdan:** doğrulama geçmezse kısmi dosya + sidecar silinir, indirme tek denemede sıfırdan.
5. **Validator yoksa resume yok.** **Zayıf ETag (`W/...`) validator sayılmaz** ve `If-Range`'de
   kullanılmaz: Last-Modified'a düşülür, o da yoksa resume yapılmaz.

#### Sidecar şeması (sürüm 1)

```json
{
  "schema_version": 1,
  "url": "https://example.com/big.bin",
  "total_bytes": 104857600,
  "etag": "\"abc123\"",
  "last_modified": "Tue, 17 Nov 2026 10:00:00 GMT",
  "segments": [
    {"start": 0, "end": 13107200, "done": 13107200},
    {"start": 13107200, "end": 26214400, "done": 4194304}
  ]
}
```

- `start`/`end` mutlak bayt offsetleri (`end` hariç); `done` = o segmentin diskte doğrulanmış bayt sayısı
  (`0..end-start`). Sıradaki istek `start + done`'dan başlar.
- Adlar: `<çıktı>.flowget` (asıl), `<çıktı>.flowget.tmp` (geçici). UTF-8 yol, IStorage üzerinden yazılır.
- Okuma katı: eksik/fazla alan, bilinmeyen `schema_version`, 1 MiB üstü dosya, bozuk JSON, aralık
  geometrisi tutmuyor → geçersiz.

#### Checkpoint: ne zaman ve nasıl

- **Yalnız zaman tabanlı: ~2 s'de bir.** Bayt eşiği yok; 1 MiB'da bir `fsync` throughput'u öldürür.
- Ayrıca: segment bitince, iptal, hata ve normal bitiş.
- Sıra (çökme tutarlılığı): (1) `storage->flush()` (fsync) → o ana kadar yazılan her bayt diskte;
  (2) sidecar `.tmp`'ye yaz + flush; (3) atomik yeniden adlandırma (`rename` /
  `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`). Böylece sidecar ya eski ya yeni sürümdür.
- Checkpoint anı güvenli: tüm ağ + disk I/O sürücü thread'inde; turlar arasında uçuşta `write_at` yok →
  sayaç anlık görüntüsü kilit gerektirmez.
- Bayt sayımı: `done` yalnız `write_at` başarı döndürdükten sonra artar; curl'ün 256 KiB iç tamponu ve
  callback'e gelmemiş veri sayılmaz. Sidecar ancak `flush` başarılıysa yayınlanır → "indirildi" denen
  bayt kaybolmaz. `flush` hatası indirmeyi `io` hatasıyla bitirir.
- Sidecar yazma hatası: checkpoint atlanır (uyarı), indirme sürer; sonraki turda yeniden denenir.
- Kayıp üst sınırı ≈ 2 s'lik veri (20 MB/s'de ~40 MB). S5b hedefi bu yüzden "yeniden indirilen veri ≈
  checkpoint aralığı"; eski "< 1 MB" hedefi geçersiz.
- Açılışta artık `.flowget.tmp` silinir; başarılı bitişte sidecar silinir (önce veri `flush`).

#### Sunucu dosyası değişti mi?

- Karar probe ile verilir: `total_bytes` farklı → değişti.
- **Güçlü** ETag iki tarafta da varsa ve farklıysa → değişti. ETag yok veya zayıf (`W/...`) ise
  Last-Modified karşılaştırılır; o da yoksa resume yok.
- Devam istekleri: `Range: bytes=(start+done)-(end-1)` + `If-Range: <güçlü ETag veya Last-Modified>` +
  `Accept-Encoding: identity`. `If-Range` uyuşmazsa sunucu 200 döner; **200 = değişti** → sıfırdan.
- Segment sırasındaki ETag değişimi zaten S4 başlık doğrulamasında yakalanıyor.

#### Yarım dosya / sidecar yok, bozuk, elle değiştirilmiş

| Durum | Davranış |
|---|---|
| Sidecar yok | Yeni indirme; dosya truncate |
| Sidecar > 1 MiB, JSON değil, okunamıyor, `schema_version` bilinmiyor | Geçersiz say, sil, sıfırdan |
| Alan eksik/fazla, `total_bytes` ≤ 0, segmentler bitişik değil/boşluk var/toplam ≠ `total_bytes` | Geçersiz say, sil, sıfırdan |
| `done` aralığı `0..end-start` dışında veya `done` toplamı > `total_bytes` | Geçersiz say, sil, sıfırdan |
| ETag ve Last-Modified ikisi de yok (veya ETag zayıf + Last-Modified yok) | Geçersiz say, sil, sıfırdan |
| Dosya yok veya boyutu ≠ `total_bytes` | Sıfırdan (preallocate EOF'u `total_bytes` yapar, kontrol yine anlamlı) |
| URL / güçlü ETag / Last-Modified uyuşmuyor | Sıfırdan, `resume_note` ile neden |
| Tüm segmentlerde `done == end-start` | Tamam kabul et (ağ isteği yok), sidecar'ı sil |
| Elle "done büyütülmüş" sidecar | **Bilinçli sınır:** yapısal doğrulama geçer, içerik doğrulanmaz; yalnız `--sha256` (S8) yakalar. Sidecar kullanıcı verisidir, imzalanmaz |

#### S5a-1 — model, atomik yazma, doğrulama (ağ yok)

- [ ] `vcpkg.json` + `core/CMakeLists.txt`: `nlohmann-json` (`nlohmann_json::nlohmann_json`, PRIVATE).
- [ ] `core/include/flowget/resume_state.h` + `core/src/resume_state.cpp`: `ResumeState`, `ResumeSegment`,
      `serialize_resume_state` (deterministik), `parse_resume_state` (`allow_exceptions=false`), katı
      doğrulama, `is_strong_etag`.
- [ ] `core/src/file_replace.h/.cpp`: `replace_file` (atomik), `write_file_atomically` (tmp → IStorage
      yaz + flush → rename; hata halinde tmp sil), `sidecar_path` / `sidecar_temp_path`,
      `remove_stale_sidecar_temp`.
- [ ] `tests/unit/resume_state_test.cpp` (round-trip + her reddetme sınıfı) ve
      `tests/integration/resume_store_test.cpp` (gerçek dosya, ağ yok: atomik yazma, üzerine yazma, tmp
      temizliği, ASCII olmayan yol).
- [ ] Kanıt: Windows build + `ctest`, `check_linux.ps1` (3 preset), pre-commit, `check_dependencies.py`,
      clean build süresi öncesi/sonrası.

#### S5a-2 — motor + CLI entegrasyonu

- [ ] `DownloadOptions::resume` (varsayılan `true`) + `DownloadSummary` alanları: `resumed`,
      `resumed_from_bytes`, `resume_note`.
- [ ] Motor: `done`'dan kalan aralık, `If-Range`, checkpoint (2 s + olaylar), sidecar silme.
- [ ] `Downloader`: sidecar oku → doğrula → probe ile karşılaştır → devam/baştan; `.tmp` temizliği;
      fallback/hata yolunda sidecar silme.
- [ ] CLI: `--no-resume`; `Resuming from X MiB` satırı.
- [ ] **Ctrl+C:** bugün `run_get` `download(...)`'a cancel jetonu geçmiyor (`cancel = nullptr`). Windows
      `SetConsoleCtrlHandler` (`CTRL_C_EVENT` + `CTRL_BREAK_EVENT`), POSIX `SIGINT` → `std::atomic<bool>`;
      iptal `aborted` döner, checkpoint yazılır, çıkış kodu 130. İkinci Ctrl+C varsayılan davranışa döner.
- [ ] Testler (entegrasyon): %40'ta iptal → sidecar → devam → içerik birebir; gerçek süreç öldürme
      (Windows `TerminateProcess`, POSIX `SIGKILL`) → devam; **Windows'ta Ctrl+C (`CTRL_BREAK_EVENT` ile
      ayrı süreç grubuna) → sonrasında `--resume` → SHA-256 eşit**; ETag/Last-Modified değişimi; zayıf
      ETag → Last-Modified'a düşme; validator yok → resume yok; 200 dönüşü → sıfırdan; bozuk sidecar 5
      varyantı; dosya silinmiş/boyut değişmiş; yanlış `Content-Length`.
- [ ] Kanıt: build + `ctest`, `check_linux.ps1`, pre-commit, CLI e2e çıktısı (`Resuming from ...`).

#### S5b — tek akış devamı + ölçümler

- [ ] `SingleStreamDownloader` resume: dosya boyutu > 0 ise `Range: bytes=N-` + `If-Range`; 206 → ekle,
      200 → truncate + baştan.
- [ ] Ölçüm 1 (kayıp): throttle'lı yerel sunucu, 20 MB/s, %40'ta öldür → yeniden çalıştır; yeniden
      indirilen veri **≈ checkpoint aralığı** (~2 s'lik veri), SHA-256 eşit. Tabloya işlenir.
- [ ] Ölçüm 2 (performans): 100 MB, 8 bağlantı, checkpoint **açık** → S7 baseline'ı 3,66 s'nin en fazla
      **~%5** üstü (≤ ~3,84 s). Aynı makine/sunucu, birkaç tekrar; sonuç tabloya.
- [ ] Doküman: resume varsayılan açık, `--no-resume` kaçış yolu; sidecar M2'de SQLite'a taşınır.

**Riskler**

- 2 s'lik checkpoint 20 MB/s'de ~40 MB kayıp demek; DoD tablosu buna göre güncellendi.
- `preallocate` EOF'u `total_bytes` yapar; resume `open(read_write)` kullanır, asla `truncate` (S3 dersi).
- Kısmi dosya başka bir dosyaya aitse ve validator yoksa fark edilmez → validator yoksa resume yok.
- Sidecar salt-okunur dizinde yazılamazsa uyarı + indirme devam (resume verisi kaybolur).
- Zayıf ETag tek başına yetmez; sunucu `If-Range`'i yok sayıyorsa 200 → sıfırdan (sessiz ekleme yok).

**DoD:** S5a-1/S5a-2/S5b testleri yeşil; Ctrl+C ve kill senaryolarında devam çalışıyor; 100 MB performans
şartı (±%5) sağlanıyor; `check_linux.ps1` üç preset `exit 0`; clang-tidy baseline temiz.

### S7 — DynamicSplitter + benchmark tablosu

#### S7 karşılaştırma tabanı (2026-11-16, Windows, 100MB.zip, download.thinkbroadband.com, tek ölçüm)
- `curl.exe`: 6,29 sn
- FlowGet `--connections 1`: 5,70 sn
- FlowGet `--connections 8`: 3,66 sn
- SHA-256 hash'ler 1 ve 8 bağlantıda aynı, indirilen boyut 104857600 bayt.

S7'de aynı komutlarla, **birkaç tekrarla ve ölçüm sırasını değiştirerek** yeniden ölç (tek ölçüm ve sabit
sıra ısınma/önbellek etkisini ayırmaz); sonuçlar `## Ölçüm ve kanıt tablosu`na işlenir.

### S8 — HashVerifier + hata modeli + CLI bayrakları + RSS ölçümü

- [ ] Windows'a özel dosyalar (`storage_win32.cpp` vb.) Linux CI clang-tidy kapsamı dışında.
- [ ] CI action sürümleri: `actions/checkout`, `actions/cache`, `ilammy/msvc-dev-cmd` için Node 24
      destekleyen güncel sürümleri her action'ın Releases sayfasından kontrol edip güncelle
      (Node 20 deprecation uyarısı, run #13).

### Tasarım
- [ ] **S4'ün ilk maddesi:** `SingleStreamDownloader`'ı `IStorage`'a taşı, ASCII olmayan hedef yolla test
      et (bugün `ofstream` + `path.string()` ANSI kod sayfası kullanır; `tasks/todo.md` S3 kararı).
- [x] `IStorage` arayüzü + `Win32Storage` (preallocate: `SetFileInformationByHandle(FileAllocationInfo)`),
      `PosixStorage` (`posix_fallocate`), `pwrite`/`pread` — **S3'te tamamlandı**
- [x] `HttpClient` — **S1b'de tamamlandı:** curl multi üzerinde senkron `get()`, tek uçuşta tek transfer;
      iptal `CURLOPT_XFERINFOFUNCTION` ile 50 ms'de bir yoklanıyor (ölçülen sınır ~50 ms). Ayrı engine
      thread + `curl_multi_wakeup` S4'te (aynı anda çok transfer) devreye alınacak; bugün gereksiz.
      Kanıt: `core/src/curl_transport.cpp`, `tests/integration/single_stream_download_test.cpp`
      (6 test yeşil, iptal testi `aborted` + kısmi dosya silinmiş).
- [x] `Probe`: HEAD + `Range: bytes=0-0` ile `206/200`, `Content-Length`, `Accept-Ranges`, `ETag`,
      `Last-Modified`, `Content-Disposition`, `Content-Type` çıkarımı — **S2'de tamamlandı** (416 ayrımı
      ve opsiyonel HEAD dahil; kanıt: `tasks/todo.md` → S2 kanıtı)
- [ ] `SegmentPlanner`: boyut/bağlantı → aralıklar; `min_segment_size = max(1 MiB, size/64)`
- [ ] `SegmentWorker`: aralık başına easy handle; `CURLOPT_RANGE`, `If-Range`, `identity` encoding,
      HTTP/1.1 (multiplexing kapalı), keep-alive
- [ ] `DynamicSplitter`: EWMA hız ölçümü; en yavaş segmentin kalanı ≥ 2×min ise ikiye böl, boştaki işçiye ver
- [ ] `RetryPolicy`: exponential backoff + jitter, `Retry-After` desteği, 429/503'te bağlantı sayısını azalt
- [ ] `RateLimiter`: token bucket (global), ağ okuma callback'inde uygula
- [ ] `ResumeStore`: `.flowget` sidecar (M1'de JSON; M2'de SQLite'a taşınır) — segment bitmap + ETag
- [ ] `HashVerifier`: streaming SHA-256 (`EVP`), MD5; beklenen hash verilmişse erken uyuşmazlık tespiti
- [ ] Hata modeli: enum + mesaj; C++ exception sınırı geçmez
- [ ] CLI: `get`, `--connections`, `--rate-limit`, `--user-agent`, `--referer`, `--header`, `--proxy`,
      `--cookie`, `--sha256`, `--resume`, `--verbose`

### Test altyapısı (asıl işin yarısı)
- [x] Yerel test HTTP sunucusunun çekirdeği — **S1b'de tamamlandı:** loopback + OS'in seçtiği port,
      bağlantı başına iş parçacığı, `chunked` çerçeveleme, per-connection throttle, kopan bağlantı
      üretimi, 5 s soket zaman aşımı. Kanıt: `tests/support/http_test_server.{h,cpp}`;
      `chunked`/throttle/drop senaryoları 6 entegrasyon testinde fiilen koşuyor (`ctest` 2/2).
- [x] Yerel test HTTP sunucusuna S2 senaryoları: Range/206, Range'i yok sayıp 200 dönme, 416, ETag
      değişimi, HEAD 403 / HEAD takılması. Kanıt: `Response::honor_range` + `apply_range()`,
      `Response::stall`, `Probe.*` testleri (10 test).
- [ ] Yerel test HTTP sunucusuna yanlış `Content-Length` senaryosu — S5'te (resume/kurtarma bozulma
      testi) gerekiyor, probe için gerekli değil.
- [ ] 1 MB / 100 MB / 1 GB fixture üretimi (deterministik PRNG) + SHA-256 manifest
- [ ] Birim: planner aralık matematiği (bitişik, boşluksuz, toplam=boyut), `Content-Range` parser,
      backoff hesabı, token bucket doğruluğu, bitmap serileştirme
- [ ] Entegrasyon: 8 bağlantı ile indir → hash eşleşiyor
- [ ] Entegrasyon: indirmeyi %40'ta `SIGKILL` → yeniden çalıştır → hash eşleşiyor, yeniden indirilen
      byte < %3
- [ ] Entegrasyon: ETag değişti → uyarı + baştan indirme (sessiz bozulma yok)
- [ ] Entegrasyon: sunucu Range'i yok sayıyor → tek akışa düşüyor, hash doğru
- [ ] Entegrasyon: 429 fırtınası → backoff, bağlantı sayısı düşüyor, indirme tamamlanıyor
- [ ] Yarış testi: TSan (Linux CI) + 100 iterasyon rastgele pause/resume
- [ ] Benchmark harness: 1/2/4/8/16 bağlantı, yerel throttle'lı sunucu ve gerçek URL'ler → tablo

**DoD:** Tüm testler yeşil; TSan temiz; benchmark tablosu README'ye; 1 GB dosyada bellek kullanımı
< 64 MB (sabit).

### Bilinen tuzaklar (kontrol listesi)
- [ ] `CURLMOPT_MAX_HOST_CONNECTIONS` varsayılan 6 → yükseltildi
- [ ] HTTP/2 multiplexing segmentlerde kapalı (yoksa tek bağlantıya biner)
- [ ] `Accept-Encoding: identity` (gzip offset kayması)
- [ ] Sunucu `Content-Length` göndermiyorsa segmentasyon yok
- [ ] Aynı anda iki işçi aynı aralığa yazmıyor (generation jetonu)

---

## M2 — Kuyruk, SQLite, limitler, kurtarma (≈2 hafta)

- [ ] Şema v1: `downloads`, `segments`, `settings`, `media_probe`, `log_events`; `PRAGMA user_version`
- [ ] Migration çerçevesi + ileri/geri test (v1→v2 simülasyonu)
- [ ] `DownloadRepository`: tek yazar, WAL, `busy_timeout=5000`, transaction batch
- [ ] Checkpoint: 250 ms'de bir + pause/cancel/exit'te `fsync`; çökme sonrası kurtarma
- [ ] Scheduler: global eşzamanlı indirme limiti, toplam bağlantı bütçesi, öncelik (FIFO + öncelikli)
- [ ] Hız limiti: global + indirme başına; doğruluk ±%5 (test)
- [ ] Zamanlayıcı: "şu saatte başlat / durdur", kuyruk duraklatma
- [ ] Proxy (HTTP/SOCKS5), cookie jar, özel header, Referer, UA — kalıcı ayarlar
- [ ] Duraklat/devam/iptal/sil (dosya ile birlikte), yeniden deneme politikası ayarı
- [ ] Log: seviye, rotasyon, UI'da görüntüleme için olay tablosu
- [ ] Testler: 500 indirme simülasyonu (sahte transport), scheduler adaleti, limit doğruluğu,
      çökme kurtarma property testi (rastgele kesme noktaları)
**DoD:** 200 indirme içeren kuyrukla 8 saatlik soak testi (hız limitli) → bellek sızıntısı yok
(RSS artışı < %10), DB tutarlı, tüm hash'ler doğru.

---

## M3 — Flutter Windows UI + FFI (ince dikey dilim, ≈3 hafta)

- [ ] C ABI: `flowget_create/destroy`, `enqueue`, `pause`, `resume`, `cancel`, `remove`, `snapshot`,
      `subscribe(events)`, `settings_get/set`, `version`
- [ ] `bindings/dart`: `ffigen` config, güvenli Dart API (handle yaşam döngüsü, `Finalizer`)
- [ ] Olay akışı: 100 ms coalesce, `NativeCallable.listener` → `Stream<EngineEvent>` → Riverpod
- [ ] UI iskeleti: sol kategori ağacı (Tümü, Devam eden, Biten, Video, Müzik, Belgeler, Sıkıştırılmış,
      Hatalı), sağ liste (ad, boyut, ilerleme, hız, kalan süre, durum)
- [ ] Segment bazlı ilerleme çubuğu bileşeni (bitmap görselleştirme, 8-16 dilim)
- [ ] Araç çubuğu: Ekle, Duraklat, Devam, Sil, Ayarlar; çoklu seçim + klavye kısayolları
- [ ] "Ekle" diyaloğu: URL, kayıt yeri, bağlantı sayısı, hash, başlıklar (gelişmiş sekme)
- [ ] Ayarlar: tema, dil, varsayılan bağlantı, hız limiti, kuyruk, proxy, FFmpeg/yt-dlp yolları
- [ ] Koyu/açık tema + sistem teması; `tr`/`en` ARB ile tam çeviri
- [ ] Tray: simge, menü (göster/gizle, duraklat tümü, çıkış), kapatınca tepsiye küçülme
- [ ] Bildirimler: indirme bitti/hata; "klasörü aç"
- [ ] Tek örnek (single instance) + ikinci çağrıda URL'yi mevcut örneğe iletme
- [ ] Performans: 1000 satırlık liste, 20 MB/s'de 60 FPS; UI thread'inde >16 ms blok yok (DevTools kanıtı)
- [ ] Erişilebilirlik: klavye ile tam gezinme, ekran okuyucu etiketleri (temel)
**DoD:** UI'dan 1 GB indirme: duraklat/devam, uygulamayı kapat-aç (devam ediyor), tray, bildirim,
tema/dil değişimi — hepsi ekran kaydıyla kanıtlı. `flutter test` + widget testleri yeşil.

---

## M4 — Tarayıcı eklentisi + Native Messaging (≈2 hafta)

- [ ] `native_host`: stdio binary framing (4-byte LE uzunluk + JSON), komutlar: `ping`, `enqueue`,
      `probe`, `status`; uygulama kapalıysa başlatma (tek örnek argümanı ile)
- [ ] Host installer: Chrome/Edge/Firefox registry girdileri + manifest yazımı + kaldırma; yönetici
      hakkı gerektirmeyen HKCU; "kurulumu doğrula" komutu
- [ ] Manifest üretimi: tek kaynak → `manifest.chromium.json` ve `manifest.firefox.json`
      (`background.service_worker` vs `background.scripts`, `browser_specific_settings.gecko.id`)
- [ ] Sabit extension ID (`key` alanı) — native host `allowed_origins` eşleşmesi için
- [ ] Medya yakalama: `webRequest.onBeforeRequest` gözlemi (mp4/webm/m3u8/mpd/mkv/mp3, boyut filtresi),
      content script `<video>/<source>` + `performance` taraması, sezgisel skorlama
- [ ] UI: popup (yakalanan medya listesi, hızlı indir), sağ tık menüsü, "Download with FlowGet",
      bağlantı üzerine gelince buton (content script)
- [ ] Host yok/kapalı durumunda net hata + kurulum yönlendirmesi
- [ ] Testler: framing/JSON birim testleri, sahte host ile e2e (Node/Python harness), Chrome + Edge +
      Firefox manuel kontrol listesi (imzalı ekran kaydı)
- [ ] Güvenlik: URL'ler loglanmaz (token maskeleme), host yalnızca izinli origin'den mesaj kabul eder,
      enqueue komutunda path traversal/`file://` reddi
**DoD:** Üç tarayıcıda kurulum → tık → indirme başlıyor; YouTube sayfasında video yakalanıp yt-dlp
yoluna yönlendiriliyor (M5 sonrası tam), HLS sayfasında m3u8 yakalanıyor.

---

## M5 — yt-dlp + HLS/DASH + FFmpeg (≈3 hafta)

### M5 ön koşul: Üçüncü taraf araçlar ve lisans ADR'si (docs/adr/0007)

Karar: bu araçlar koda gömülmez, ayrı program (sidecar) olarak paketlenir. `docs/adr/0007-*.md` henüz
yok; M5'in ilk işi bu ADR'yi yazmak (araç başına karar + gerekçe + lisans).

- [ ] yt-dlp (Unlicense): video sitesi indirme, ayrı süreç. Windows `.exe` içindeki bileşen
      lisanslarını kontrol et.
- [ ] FFmpeg (LGPL veya GPL, derlemeye göre): yalnızca LGPL derlemesi, ayrı program. x264 vb. içeren
      GPL derlemesi yasak.
- [ ] QuickJS-NG (MIT): yt-dlp bazı sitelerde JS çalıştırıcıya ihtiyaç duyabilir; M5 başında gerekip
      gerekmediğini doğrula.
- [ ] N_m3u8DL-RE (MIT): HLS/DASH için hazır araç. ADR'de "kendi motorumuzu yaz" ile "sidecar kullan"
      seçeneklerini kıyasla; Android'de çalışıp çalışmadığına dikkat et.
- [ ] Her sidecar için sürüm sabitleme ve SHA-256 doğrulama.
- [ ] `docs/THIRD_PARTY_NOTICES`: ad, sürüm, lisans, kaynak.
- [ ] Paketlenen sidecar'larda GPL/AGPL denylist kontrolü (ADR-0004).

**Kapsam dışı:** Ghost Downloader 3 ve cat-catch GPL-3.0; koddan hiçbir şey alınmaz. Torrent ve eD2k
v1 kapsamında değil.

- [ ] `YtDlpSupervisor`: süreç başlatma, `-J` ile metadata, ilerleme parse (`--newline --progress`),
      iptal, Windows Job Object ile çocuk süreç temizliği, sürüm kontrolü + güncelleme
- [ ] Format seçim diyaloğu: video+ses, çözünürlük, codec, dosya boyutu tahmini, "sadece ses"
- [ ] HLS: master/media playlist parser, `EXT-X-MAP` init segment, `EXT-X-BYTERANGE`, AES-128
      (anahtar URL'si; DRM yok → tespit edilirse net hata), paralel segment indirme, sıralı birleştirme
- [ ] DASH: MPD parser (`SegmentTemplate`, `SegmentTimeline`, `SegmentBase`), adaptasyon seçimi,
      ayrı video/ses akışları, FFmpeg ile mux
- [ ] FFmpeg pipeline: mp4/mp3/m4a/webm presetleri, bitrate, "indirme bitince dönüştür", ilerleme,
      hata yönetimi, LGPL build doğrulaması (`ffmpeg -L`)
- [ ] Test fixture'ları: yerel HLS (6 segment + init) ve DASH (video+ses) sunucu; birleşik çıktının
      süre/çözünürlük/kare sayısı `ffprobe` ile doğrulanır
- [ ] Testler: playlist parser birim testleri (varyantlı, byterange, şifreli, bozuk), kesintiye uğrayan
      HLS indirmesinin devamı, format seçimi mantığı
- [ ] Doküman: "desteklenen siteler yt-dlp'nin desteklediği kadardır", DRM kapsam dışı, telif uyarısı
**DoD:** Gerçek bir açık lisanslı HLS test akışı ve yt-dlp destekli bir site üzerinden uçtan uca indirme +
mp3 dönüştürme; `ffprobe` çıktısı kanıt olarak.

---

## M6 — Android portu (≈3 hafta)

- [ ] NDK build: `arm64-v8a`, `armeabi-v7a`, `x86_64`; 16 KB page hizalama (`-Wl,-z,max-page-size=16384`)
- [ ] `AndroidStorage`: app-specific external files + `pwrite`; MediaStore/SAF dışa aktarma (kopyalama
      ilerlemesi ile); seekable olmayan SAF hedefini reddet
- [ ] Foreground service (`dataSync`): başlat/durdur, bildirim (ilerleme, duraklat/iptal), `onTimeout`
      (6 saat) → checkpoint + durdur + kullanıcıya bilgi, `BootCompleted`'dan başlatma yok
- [ ] Motor sahipliği: service süreci motoru sahiplenir; UI attach/detach (Binder/AIDL veya
      `FlutterEngineGroup`); uygulama kapanınca indirme sürer
- [ ] Wake lock (partial) + pil optimizasyonu muafiyeti istemi (açıklayıcı ekran)
- [ ] Share intent: `ACTION_SEND` (text/plain URL) → Add dialog ön dolu; `ACTION_VIEW` (http/https)
- [ ] Mobil UI: dar ekran düzeni, alt navigasyon, dokunma hedefleri, geri tuşu davranışı
- [ ] Ağ geçişi: Wi-Fi ↔ hücresel kesintisinde devam; "yalnızca Wi-Fi" ayarı; metered uyarısı
- [ ] Testler: gerçek cihazda 1 GB indirme (ekran kapalı, uçak modu aç/kapa, süreç öldürme),
      battery historian ile pil ölçümü, `logcat` kanıtı
**DoD:** Fiziksel cihazda: paylaş → indir → bildirimden duraklat → devam → uçak modu → devam → tamam;
hash doğru. F-Droid uyumluluğu (`fdroid lint`).

---

## M7 — Paketleme, dokümantasyon, sürüm (≈1-2 hafta)

- [ ] Windows installer (Inno Setup): FFmpeg + yt-dlp paketleme, native host kaydı, kaldırma temizliği
- [ ] Taşınabilir zip + otomatik güncelleme kontrolü (imza/hash doğrulamalı)
- [ ] İmzalama kararı uygulaması (varsa) + SmartScreen notu
- [ ] Android APK/AAB + F-Droid metadata (`fastlane`), sürüm kodları
- [ ] README: kurulum, derleme (Windows/Android), ekran görüntüleri, SSS, bilinen sınırlar, yasal uyarı
- [ ] `docs/` İngilizce sürüm: `architecture-review.md` ve `tasks/todo.md` yayın öncesi İngilizceye çevrilir
      (repo dili İngilizce; Türkçe kopya `docs/tr/` altında saklanır)
- [ ] `docs/`: mimari, ADR'ler, katkı rehberi, güvenlik politikası, sorun şablonları
- [ ] `THIRD_PARTY_LICENSES.md` (CI üretimi + gözden geçirme), `LICENSE`
- [ ] CI: release workflow (tag → build → test → artifacts), sürüm notları, checksum
- [ ] Temiz VM'de kurulum testi (Windows 10/11), temiz cihazda APK testi
**DoD:** `v1.0.0` etiketiyle yayınlanmış artifact'lar; temiz makinede kurulum çalışıyor; README'deki
tüm komutlar sıfırdan çalışıyor.

---

## Ölçüm ve kanıt tablosu (doldurulacak)

| Metrik | Hedef | M1 | M2 | M3 | M6 |
|---|---|---|---|---|---|
| 1 GB indirme süresi (8 bağlantı, yerel) | referans | | | | |
| 1 bağlantıya göre hız kazancı (throttle'lı sunucu) | ≥ 4× | | | | |
| CPU kullanımı @ 20 MB/s | < %10 (1 çekirdek) | | | | |
| RSS @ 1 GB indirme | < 64 MB | | | | |
| Resume kaybı (kill -9) | ≈ checkpoint aralığı (~2 s) | | | | |
| UI kare süresi @ 20 MB/s | < 16 ms | | | | |
| Pil tüketimi (30 dk, Wi-Fi) | ölçülür | | | | |

---

## Kapsam dışı (bilinçli)

DRM bypass · torrent/magnet · canlı yayın kaydı · tarayıcı proxy'si · macOS/Linux masaüstü (v1.1) ·
bulut senkronizasyonu · hesap sistemi · telemetri (varsayılan kapalı, v1'de yok)

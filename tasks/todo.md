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
| S3 | `IStorage` + preallocate + `pwrite` (Win32/POSIX) | |
| S4 | `SegmentPlanner` + curl multi, 8 bağlantı, tek akışa düşüş | |
| S5 | Resume (`.flowget` sidecar, checkpoint, ETag değişimi) | |
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
doğrulanacak (Linux yerelde koşulamıyor).

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

### S2 — Probe — **TAMAMLANDI** (push sonrası CI hariç)

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
- [ ] Push sonrası CI yeşil — Linux (Clang, `-Wconversion -Wsign-conversion -Werror`) derlemesi
      yerelde doğrulanamıyor; push kullanıcıda, sonuç buraya yazılacak.

**Kararlar (onaylandı):**
1. **Asıl kaynak `Range` GET**, `HEAD` yalnızca metadata zenginleştirmesi: herhangi bir HEAD hatası
   (403/405/501, zaman aşımı, taşıma) probe'u başarısız saymaz; imzalı URL'ler yalnızca GET kabul
   ettiği için bu şart. HEAD `head_timeout_ms=0` ile tamamen kapatılabilir.
2. **`Accept-Ranges: bytes` tek başına kanıt sayılmaz** (yalan söyleyen sunucu var); karar 206/416'dan
   verilir, `Accept-Ranges` yalnızca bilgi olarak taşınır.
3. Multi-range (`multipart/byteranges`) desteklenmez: yalnızca `bytes=0-0` gönderilir.
4. `416` taşıma katmanında `http_status` olarak görünür (4xx genel kuralı); probe bu tek durumu
   anlamlı sonuç sayar — S5'te sidecar doğrulaması da aynı ayrımı kullanacak.

### Tasarım
- [ ] `IStorage` arayüzü + `Win32Storage` (preallocate: `SetFileInformationByHandle(FileAllocationInfo)`),
      `PosixStorage` (`posix_fallocate`), `pwrite`/`pread`
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
| Resume kaybı (kill -9) | < 1 MB | | | | |
| UI kare süresi @ 20 MB/s | < 16 ms | | | | |
| Pil tüketimi (30 dk, Wi-Fi) | ölçülür | | | | |

---

## Kapsam dışı (bilinçli)

DRM bypass · torrent/magnet · canlı yayın kaydı · tarayıcı proxy'si · macOS/Linux masaüstü (v1.1) ·
bulut senkronizasyonu · hesap sistemi · telemetri (varsayılan kapalı, v1'de yok)

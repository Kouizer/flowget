# FlowGet — Mimari İncelemesi ve Teknoloji Kararları

> Durum: **Taslak / onay bekliyor** · Tarih: 2026-11-14
> Bu belge kod yazılmadan önceki analizdir. Onaylanan kararlar `docs/adr/` altında ADR olarak dondurulur.

---

## 1. Özet değerlendirme

Önerilen katmanlama (`core` / `bindings` / `app` / `extension` / `native_host`) **doğru ve korunmalı**.
Ancak üç noktada planı olduğu gibi uygulamak projeyi riske atıyor:

1. **Android'de yt-dlp'yi gömülü kütüphane olarak kullanmak lisansı bozar.** `youtubedl-android`
   GPL-3.0 ve uygulamaya *link* ediliyor → tüm Android uygulaması GPL-3.0 olur. MIT/Apache hedefiyle
   çelişir. Windows'ta yt-dlp'yi **ayrı süreç** (subprocess) olarak çalıştırmak GPL bulaşması yaratmaz;
   Android'de ise v1'de yt-dlp'yi **kapsam dışı** bırakmak gerekir (alternatifler §3.3).
2. **"8-16 bağlantı = 8-16 kat hız" varsayımı çoğu sunucuda yanlış.** YouTube, Cloudflare, Akamai gibi
   CDN'ler tek bağlantıya zaten yüksek throughput verir; bazıları IP başına toplam hızı kısar. Çok
   parçalı indirme kazancı esas olarak **yavaş tek akışlı / per-connection throttled** sunucularda
   görülür. Bunu bir pazarlama vaadi değil, **ölçülen bir metrik** yapmak gerekir (M1'de benchmark
   harness + README'de ölçüm tablosu).
3. **Flutter + FFI + Android foreground service birlikte, süreç yaşam döngüsü problemidir.** Android'de
   UI süreci öldürüldüğünde indirmelerin devam etmesi bekleniyorsa, motorun sahibi **service** olmalı;
   Flutter UI motora *attach/detach* olmalı. Bu kararı M6'da vermek çok geç olur — `bindings`
   sözleşmesi M3'te **transport-agnostik** (komut/olay) tasarlanmalı.

Ayrıca iki "sessiz" tuzak var: (a) DB'yi hem Dart hem C++ tarafından yazmak, (b) MV3'te
`webRequest` blocking'in artık sadece kurumsal politika ile mümkün olması. İkisi de §2'de.

**Tahmini efor (tek kıdemli geliştirici, tam zaman):** M0-M7 toplam **11-15 hafta**.
Yarı zamanlı (haftada ~15 saat) gerçekçi takvim **7-9 ay**. Bu, "bir hafta sonu projesi" değil.

---

## 2. Mimari eleştirisi (risk + alternatif)

### 2.1 Çok parçalı indirme: protokol gerçekleri
- `Range` desteği **opsiyoneldir**. Sunucu `Accept-Ranges: bytes` demese bile `206` dönebilir; `Range`
  gönderip `200` dönen sunucu da vardır. Üç durum ayrı ayrı ele alınmalı: 206 (kullan), 200 (tek akışa
  düş, kullanıcıya bildir), 416 (`Range`/boyut tutarsız → yeniden planla).
- **Doğrulama anahtarı**: resume sırasında `ETag` veya `Last-Modified` + `Content-Length` değişirse
  parçalar geçersizdir. `If-Range` ile koşullu istek + yerel `ETag` saklama zorunlu.
- `Content-Encoding: gzip` yanıtında byte offset'ler sıkıştırılmış gövdeye göre olur → `Accept-Encoding:
  identity` gönderilmeli, aksi halde birleştirme bozulur.
- Sunucu `Content-Length` yerine `chunked` dönerse toplam boyut bilinmez → segmentasyon imkânsız,
  tek akış moduna düş.
- **Aşırı bağlantı cezalandırılır**: 16 bağlantı bazı WAF'lerde rate-limit/ban tetikler. Varsayılan **8**,
  üst sınır 16, sunucu bazlı adaptif düşürme (429/503 görülünce bağlantı sayısını yarıya indir,
  exponential backoff) önerilir.

### 2.2 Dinamik segment bölme (IDM'in asıl püf noktası)
Basit "biten işçi en yavaş parçayı devralır" yaklaşımı hatalı: parça *sahipliği* devredilirken iki işçi
aynı byte aralığına yazarsa dosya bozulur. Doğru model:

- Her segment bir **aralık + sahiplik jetonu (generation)** taşır. Bölme, sadece sahibi işçinin
  `curl_multi_remove_handle` ile durdurulduğu **ve** yazma tamponunun flush edildiği anda yapılır
  (tek thread'li motor döngüsünde atomik).
- Bölme kararı: EWMA hızı en düşük segment, kalanı `min_segment_size`'dan büyükse ikiye bölünür.
  `min_segment_size = max(1 MiB, toplam/64)`. Maksimum segment sayısı = bağlantı limiti (bölme,
  sayıyı artırmaz; sadece iş yükünü dengeler). Yeni işçi ancak boşta varsa devreye girer.
- Her segmentin yazması `pwrite` ile **kendi offset'ine** yapılır → aynı dosyaya eşzamanlı yazma
  güvenli, kilit gerekmez. `ftruncate`/preallocate ile dosya boyutu baştan ayrılır.
- Birleştirme adımı yok (IDM'in `.part` parçaları gibi); tek dosya + segment bitmap. Bu, disk alanını
  yarıya indirir ve resume'u basitleştirir.

### 2.3 Thread modeli
Öneri: **tek I/O thread'i + `curl_multi`** (thread-per-segment değil).

```
UI (Dart isolate)  ──FFI komut──▶  Engine thread (event loop)
                                      ├── curl_multi (8-16 easy handle)
                                      ├── token bucket (hız limiti)
                                      ├── writer (pwrite + 256 KiB buffer)
                                      └── checkpoint timer (250 ms)
UI  ◀──NativeCallable.listener──  olaylar (100 ms'de coalesce)
```

- `curl_multi_poll` + `curl_multi_wakeup` ile komutlar (pause/cancel) anında işlenir; polling gecikmesi yok.
- Hash hesaplama (SHA-256) ayrı tek bir yardımcı thread'de, indirme bittikten sonra tek geçişte.
- Disk yazımı engine thread'inde; ölçümde disk throughput'u ağdan yavaş çıkarsa **tek yazıcı thread +
  segment başına kuyruk** eklenir (M1'de ölçülüp karar verilir, spekülatif karmaşıklık yok).

### 2.4 FFI sınırı
- `extern "C"`, opak handle, C++ exception sınırı geçmez (hepsi hata koduna çevrilir).
- Sürümlenmiş `flowget_api_version()`; struct'lar boyut/`struct_size` alanı ile genişletilebilir.
- Progress için **callback (NativeCallable.listener)**; Dart tarafında polling yok. Callback engine
  thread'inden çağrılır → listener `Dart_PostCObject` üzerinden UI isolate'ine düşer, UI thread'i bloklanmaz.
- Pointer ömrü: engine sahipli, UI sadece `const` snapshot okur. Snapshot `free` çağrısı ile serbest
  bırakılır (tek tahsis, arena).
- **Kaçınılacak**: FFI'dan Dart `String` döndürmek, struct içine `std::string` koymak, callback içinde
  FFI'ye geri çağrı yapmak (deadlock).

### 2.5 Veritabanı sahipliği (kritik)
İki yazar (Dart `drift/sqflite` + C++ `sqlite3`) = şema kayması + `SQLITE_BUSY` + mantık iki yerde.
**Karar: DB'nin tek sahibi C++ core.** UI, ihtiyaç duyduğu listeyi FFI snapshot/olay ile alır;
UI ayarları (tema, dil, pencere geometrisi) ayrı ve küçük bir `settings` tablosunda yine core üzerinden
veya `shared_preferences`-benzeri platform deposunda tutulur. WAL modu, `busy_timeout`, tek yazar.
Checkpoint: segment bitmap'i **250 ms'de bir** ve pause/cancel/exit'te `fsync` ile yazılır → elektrik
kesintisinde en fazla 250 ms'lik ilerleme kaybı, bozulma yok.

### 2.6 Android yaşam döngüsü
- İndirme motoru **foreground service** içinde yaşar (`dataSync`). Flutter UI ayrı süreç/activity olarak
  bağlanır. Böylece kullanıcı uygulamayı kapatsa da indirme sürer.
- **Android 15+ (`targetSdk 35`) `dataSync` FGS toplam 6 saat / 24 saat** ile sınırlı; süre dolunca
  `Service.onTimeout()` çağrılır ve sistem servisi durdurur. Bu yüzden: süre dolmadan checkpoint +
  `stopSelf()`, kuyruğu "duraklatıldı" durumuna al, kullanıcıya bildirim. Uzun indirmeler için
  WorkManager + `setForeground` kombinasyonu tek başına çözüm değil; tasarım bu bütçeyi **kabul eder**.
- `BOOT_COMPLETED`'dan `dataSync` FGS başlatılamaz (Android 15).
- Scoped storage: SAF ile alınan `ParcelFileDescriptor` **her zaman seekable değildir** (bulut
  sağlayıcıları). Strateji: uygulamanın kendi external files dizinine indir (tam `pwrite`), bitince
  MediaStore/SAF ile `Downloads/`'a kopyala; kopyalama da ilerleme gösterir. Doğrudan SAF'a indirme
  sadece "seekable" doğrulanırsa denenir.
- 16 KB page size: Android 15+ cihazlar ve Play, NDK ile üretilen `.so`'ların 16 KB hizalı olmasını
  istiyor → **NDK r28+**, `-Wl,-z,max-page-size=16384`.

### 2.7 Tarayıcı eklentisi
- MV3'te **`webRequest` sadece gözlem** yapabilir (blocking, kurumsal politika hariç) — medya yakalama
  için bu yeterli, ama "indirmeyi eklenti üzerinden proxy'le" gibi bir tasarım kurulamaz. İyi ki
  gerekmiyor: eklenti sadece URL + cookie + referer bilgisini host'a iletir, indirmeyi core yapar.
- Medya yakalama tek yönteme bağlı kalmamalı: (1) `webRequest.onBeforeRequest` gözlemi (mp4/webm/m3u8/mpd),
  (2) content script'te `<video>/<source>` ve `performance.getEntriesByType('resource')` taraması,
  (3) sağ tık → "Download with FlowGet". Üçü birleşince kapsama tatmin edici olur.
- Native messaging mesajları **4 byte native-endian uzunluk + UTF-8 JSON**; büyük payload yok (sadece
  metadata + URL). Host tarafı `stdin/stdout`'u **binary** açmalı (Windows'ta text modu CRLF bozar).
- Kurulum: `HKCU\Software\Google\Chrome\NativeMessagingHosts\<ad>`, `...\Microsoft\Edge\...`,
  `HKCU\Software\Mozilla\NativeMessagingHosts\<ad>` (Firefox manifestinde `allowed_extensions`,
  Chrome/Edge'de `allowed_origins` + extension ID). Chrome Web Store dışı kurulumda extension ID sabit
  olmalı → `key` alanı manifestte.
- Android'de native messaging yok → Firefox Android eklentisi host ile konuşamaz. Çözüm: **Share intent**
  birincil yol; eklenti ikincil olarak "linki kopyala/paylaş" üretir. (Alternatif: uygulama içinde
  `127.0.0.1` token'lı HTTP köprüsü — M6'da değerlendirilecek, varsayılan değil.)

### 2.8 Kapsam ve dağıtım riskleri
- **Google Play**: yt-dlp tabanlı indiriciler politika ihlali gerekçesiyle kaldırılıyor. Plan: **GitHub
  Releases + F-Droid** birincil, Play opsiyonel ve yt-dlp'siz varyant. Bunu baştan kabul etmek gerekir.
- **Windows SmartScreen**: imzasız installer uyarı verir. Kod imzalama sertifikası (OV ~$200-400/yıl,
  EV daha pahalı) bütçe sorusu. İmzasız başlanacaksa README'de hash + "More info → Run anyway" notu.
- **Marka/hukuk**: isim ve logo için temel arama (GitHub, npm, PyPI, marka tescil) yapılmalı; README'de
  DRM kapsamı ve telif sorumluluğu net yazılmalı. "Her videoyu indirir" ifadesi yasak.
- **Kapsam dışı (net)**: DRM (Widevine/FairPlay/PlayReady) bypass, torrent/magnet, canlı yayın kaydı,
  ücretli içerik erişim kontrolü aşma, tarayıcıda indirme hızlandırma (proxy).

---

## 3. Teknoloji karşılaştırmaları

### 3.1 HTTP yığını: libcurl (multi) vs Boost.Asio + OpenSSL

| Kriter | libcurl multi | Boost.Asio + OpenSSL (+nghttp2) |
|---|---|---|
| HTTP/2 | Hazır, olgun (nghttp2) | nghttp2'yi elle bağlarsın |
| Range/resume/proxy/cookie | Hazır (`CURLOPT_RANGE`, `CURLOPT_RESUME_FROM_LARGE`, `CURLOPT_PROXY`, cookie engine) | Hepsi elle |
| TLS/CA/sertifika deposu | Windows schannel / OpenSSL seçilebilir | OpenSSL yapılandırması sana ait |
| Olay döngüsü | `curl_multi_poll` (tek thread, 16 bağlantı rahat) | Asio io_context (esnek, ama kod 3-5× fazla) |
| Android NDK | vcpkg ile sorunsuz | OpenSSL + Boost derlemesi zahmetli |
| Lisans | curl (MIT benzeri) | Boost (BSL-1.0), OpenSSL (Apache-2.0) |
| Risk | API'si "eski usul" C; hata ayıklama alışkanlık ister | Süre ve hata yüzeyi çok daha büyük |

**Karar: libcurl multi.** Gerekçe: projenin farklılaştırıcısı HTTP katmanı değil, segmentasyon/kuyruk/UX.
Asio'yu yeniden yazmak 3-6 hafta ve sürekli bakım yükü demek. `HttpClient` arayüzü arkasında tutulur;
ileride HTTP/3 (curl'de `CURL_HTTP_VERSION_3`) veya farklı bir yığın takılabilir.

**Uygulama detayları (bunlar atlanırsa "çok bağlantı" çalışmaz):**
- `CURLMOPT_MAX_HOST_CONNECTIONS` varsayılanı **6**'dır → 8-16 segment için yükseltilmeli.
- libcurl HTTP/2 multiplexing'i açıksa tüm segmentler **tek TCP bağlantısına** biner ve paralellik
  kaybolur. Segment indirmelerinde `CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_1_1` (veya
  `MAX_HOST_CONNECTIONS` + `CURLMOPT_PIPELINING = 0`) kullanılmalı. Tek akış/medya probe isteklerinde
  HTTP/2 açık kalır.
- `CURLOPT_ACCEPT_ENCODING = "identity"`, `CURLOPT_TCP_KEEPALIVE`, `CURLOPT_LOW_SPEED_LIMIT/TIME`
  (sessiz kopmaları yakalamak için), `CURLOPT_NOSIGNAL = 1`, `CURLOPT_XFERINFOFUNCTION` ile iptal.
- DNS: `CURLOPT_RESOLVE` veya `CURLOPT_DNS_CACHE_TIMEOUT` ile 16 bağlantıda tek DNS çözümü.

### 3.2 FFI yaklaşımı

| Yöntem | Artı | Eksi |
|---|---|---|
| `dart:ffi` doğrudan (öneri) | Sıfır serileştirme, en düşük gecikme, Windows+Android aynı | ABI disiplini, hata ayıklama elle |
| Platform channel / MethodChannel | Basit, Dart-native | Her progress mesajı mesaj kuyruğuna girer; 20 MB/s'de darboğaz |
| Yerel WebSocket/gRPC köprüsü | Süreçler arası ayrık, UI yeniden başlatılabilir | Ek süreç, port/token yönetimi, paket boyutu, Android'de pil |
| `ffi` + `NativeCallable.listener` | Yukarıdaki + olay tabanlı, UI bloklanmaz | Callback ömrü yönetimi |

**Karar: `dart:ffi` + `NativeCallable.listener`.** Android'de service ayrı süreç olursa aynı komut/olay
sözleşmesi Binder/AIDL'e taşınır; sözleşme bu yüzden M3'te "transport-agnostik" tanımlanır.

### 3.3 Android'de yt-dlp

| Seçenek | Lisans | Paket boyutu | Bakım | Değerlendirme |
|---|---|---|---|---|
| `youtubedl-android` (Python gömülü) | **GPL-3.0** (link) | +30-60 MB | Topluluk, alpha sürümler | MIT/Apache projede **kullanılamaz** (aksi halde tüm uygulama GPL-3.0) |
| Termux'ta yt-dlp + intent | — | 0 | Kırılgan UX | Reddedildi |
| Android'de yt-dlp yok; native HLS/DASH + share intent | Temiz | 0 | Bizim | **v1 kararı** |
| Rust/Python-free alternatif (ör. `rusty-dl` türü) | Değişken | Küçük | Deneysel | Değerlendirilmedi, takip edilir |

**Karar:** Windows'ta yt-dlp **ayrı süreç** (subprocess, GPL bulaşması yok, kullanıcı güncelleyebilir).
Android'de v1: doğrudan medya URL'leri + HLS/DASH + share intent. Android'de yt-dlp istenirse ayrı bir
"GPL varyant" APK olarak, izole şekilde değerlendirilir (M6 sonrası, opsiyonel).

### 3.4 Android'de FFmpeg

| Seçenek | Lisans | Durum |
|---|---|---|
| `ffmpeg-kit` | LGPL/GPL varyantları | **Ocak 2025'te emekliye ayrıldı**, arşivli |
| Topluluk fork'ları (ffmpeg-kit rebuild, FFmpegKitNext) | Değişken | Bakım/lisans belirsizliği |
| Kendi LGPL FFmpeg derlemen (NDK) | LGPL-2.1+ (dinamik link + notice) | Kontrol bizde, CI maliyeti var |
| **AndroidX Media3 `Transformer`** | Apache-2.0 | Remux/transcode temel senaryolar, donanım hızlandırma |

**Karar:** Android'de **Media3 Transformer** (mp4/webm remux, AAC/MP3 çıktı, ses ayırma). Yetmeyen
formatlar için M6'da "kendi LGPL FFmpeg derlemesi" opsiyonu. Windows'ta **paketlenmiş FFmpeg** (LGPL
build, dinamik DLL + `THIRD_PARTY_LICENSES` + kaynak teklifi). GPL özellikli encoder'lar (x264 vb.)
kullanılmaz.

### 3.5 Diğer seçimler
- **Dil standardı**: C++20 (`std::jthread`, `std::span`, `std::format` yok — MSVC/NDK uyumu için
  `fmt` kullanılabilir). Derleyiciler: MSVC 2022, Clang 18 (Android NDK r28), GCC 13 (CI Linux).
- **Bağımlılık yönetimi**: vcpkg manifest modu (`vcpkg.json`), binary cache ile CI süresi kısalır.
- **Build**: CMake Presets (`windows-x64`, `android-arm64`, `linux-x64`), `-Werror`, clang-tidy +
  clang-format CI'da zorunlu.
- **SQLite**: core içinde `sqlite3` C API + ince RAII sarmalayıcı; şema versiyonlama (`PRAGMA
  user_version`), ileri/geri migrasyon testleri.
- **Flutter**: Riverpod (test edilebilir, global state yok), `go_router` gerekmez (tek pencere),
  `intl`/`flutter_localizations` (tr/en), `tray_manager`, `local_notifier`, `windows_single_instance`.
  Paket seçiminde "son 12 ayda commit + platform desteği" kriteri uygulanır.
- **Loglama**: `spdlog` (core), dosya rotasyonu + UI'da "logları aç". Kullanıcı verisi/log'da URL
  token'ları maskelenir.

---

## 4. Revize mimari

```
flowget/
├─ core/                 # C++20, platformdan bağımsız
│  ├─ http/              # curl multi sarmalayıcı, retry, proxy, cookie
│  ├─ download/          # SegmentPlanner, SegmentWorker, DynamicSplitter, ResumeStore
│  ├─ storage/           # IStorage (Win32 / POSIX / Android), preallocate, pwrite
│  ├─ queue/             # scheduler, limits, priority
│  ├─ media/             # HLS/DASH parser + segment indirici, yt-dlp supervisor
│  ├─ convert/           # FFmpeg süreç yönetimi
│  ├─ db/                # SQLite şema + repository
│  └─ api/               # C ABI (flowget_*.h) — tek giriş noktası
├─ cli/                  # flowget-cli (M1 doğrulaması, CI'da kullanılır)
├─ bindings/dart/        # ffigen ile üretilen bağlar + Dart API katmanı
├─ app/                  # Flutter (windows/, android/)
├─ extension/            # MV3, chromium + firefox manifest üretimi
├─ native_host/          # stdio JSON host + registry installer
├─ tests/                # birim + entegrasyon (yerel HTTP sunucusu) + fixture medya
├─ docs/                 # ADR, mimari, yasal notlar
└─ .github/workflows/    # windows / linux / android / lint
```

**Katman kuralı:** `core` UI ve platform API'lerine bağımlı değil; platform farkları `storage/` ve
`platform/` altındaki arayüz implementasyonlarında. `bindings` sadece C ABI'yi sarar, iş mantığı içermez.

---

## 5. Risk kaydı

| # | Risk | Olasılık | Etki | Azaltma |
|---|---|---|---|---|
| R1 | Sunucuların Range'i reddetmesi / boyut değişimi | Yüksek | Orta | 200/416/ETag senaryoları M1 testlerinde; tek akışa zarif düşüş |
| R2 | Android 15 FGS 6 saat limiti | Kesin | Orta | `onTimeout` + checkpoint + kullanıcı bildirimi; tasarımda kabul |
| R3 | youtubedl-android GPL-3.0 | Kesin | Yüksek | Android'de kullanılmaz; Windows'ta subprocess |
| R4 | ffmpeg-kit emekliliği | Kesin | Orta | Media3 Transformer; gerekirse kendi LGPL build |
| R5 | Play Store politikası | Yüksek | Yüksek | GitHub/F-Droid birincil dağıtım |
| R6 | MV3 native messaging kurulum sürtünmesi | Orta | Orta | Tek tık installer + "bağlantıyı test et" ekranı + net hata mesajları |
| R7 | Disk I/O'nun ağdan yavaş kalması (mobil) | Orta | Orta | M1'de ölçüm; gerekirse yazıcı thread + daha büyük tampon |
| R8 | Kapsam büyümesi (7 milestone, 2 platform) | Yüksek | Yüksek | M3'te ince dikey dilim; her milestone'da çalışan çıktı; M5/M6 kapsamı kesilebilir |
| R9 | Windows kod imzalama maliyeti | Orta | Orta | İmzasız başla, hash yayınla; bütçe netleşince imzala |
| R10 | `webRequest` gözleminin kaçırdığı medya | Orta | Düşük | 3 katmanlı yakalama + manuel "URL ekle" |

---

## 6. Lisans stratejisi

- **Kendi kodumuz: MIT** (basit, ekosistemle uyumlu, katkı bariyeri düşük). Apache-2.0'ın patent
  maddesi kurumsal katkı için avantaj; ancak proje tek geliştiriciyle başladığı için MIT yeterli.
  Karar senin — ADR-0005'te dondurulur.
- `libcurl` (MIT benzeri), `SQLite` (public domain), `nghttp2` (MIT), `OpenSSL` (Apache-2.0),
  `spdlog` (MIT), `FFmpeg` (LGPL-2.1+, **dinamik link + notice**), `yt-dlp` (Unlicense, **ayrı süreç**),
  `Media3` (Apache-2.0), `Flutter` (BSD-3-Clause).
- `THIRD_PARTY_LICENSES.md` CI'da üretilir (lisans tarayıcı + elle gözden geçirme). GPL/AGPL bağımlılık
  CI'da **fail** eder.

---

## 7. İsim önerileri

| İsim | Artı | Eksi |
|---|---|---|
| **FlowGet** | Anlaşılır, akılda kalıcı | Jenerik; arama sonuçlarında kaybolur, marka riski |
| **Rivulet** | Akış + küçük dere; teknik çağrışım güzel | İngilizcede telaffuz zor; başka projelerde kullanılmış olabilir |
| **Sel** (Türkçe "flood") | Kısa, güçlü, Türkçe kimlik | İngilizcede anlamsız; arama zorluğu |
| **Cascade** | Çok parçalı akış metaforu | Aşırı kullanılmış (CSS'te de terim) |
| **Aktar** | Türkçe "transfer et" | Uluslararası pazarda zor |

**Öneri:** Geliştirme kod adı `flowget` kalsın (repo adı), kullanıcıya görünen isim için **Rivulet**
veya **Sel** arasında seçim yap; yayına çıkmadan önce GitHub/npm/PyPI/AMO ve marka araması yapılır.
Bu, M0'ın bir maddesi (24 saatlik bir kontrol, kod yazmayı engellemez).

---

## 8. Onay gerektiren kararlar

1. Lisans: **MIT** (öneri) / Apache-2.0
2. İsim: FlowGet (kod adı) + kullanıcı adı önerisi
3. Android v1'de yt-dlp: **yok** (öneri) / GPL varyant APK
4. Dağıtım: **GitHub Releases + F-Droid** (öneri) / Play Store da hedeflensin
5. `minSdk`: **26** (Android 8.0, öneri) / 24
6. M3'te ince dikey dilim önceliği (UI'ı M5/M6'dan önce çalıştırmak) — öneri: evet
7. Windows imzalama: şimdilik imzasız (öneri) / sertifika alınacak

# FlowGet — Yol Haritası (todo)

> Onay durumu: **ONAYLANDI** — M0 ve M1'in S5a-2b'ye kadarki dilimleri tamamlandı; sırada **S5b**.
> Kural: Bir madde ancak **kanıtıyla** (test çıktısı, ölçüm, CI linki) kapatılır.
> **Arşiv:** biten dilimlerin tam planı ve kanıtı [`tasks/archive/`](archive/) altında; ölçüm tabloları
> [`tasks/measurements.md`](measurements.md). Bu dosya yalnız indekstir; ayrıntı için arşive bak.

## Süreç kuralları (her milestone için geçerli)

- [ ] Kod + test + dokümantasyon aynı PR'da
- [ ] `-Werror`, clang-tidy, clang-format temiz
- [ ] CI yeşil (Windows + Linux; Android etkileniyorsa Android job'u)
- [ ] Yeni davranış için test yazıldı (birim veya entegrasyon)
- [ ] Hata düzeltmesi yapıldıysa `tasks/lessons.md`'ye kök neden + ders eklendi
- [ ] Milestone sonunda kısa özet + ölçüm tablosu
- [ ] Dilim bitince plan + kanıt `tasks/archive/`ye taşınır; burada 2-3 satırlık özet kalır

## Durum tablosu

| Dilim | Kapsam | Durum | Kayıt |
|---|---|---|---|
| M0 | Karar, iskele, CI | **TAMAMLANDI** | [M0.md](archive/M0.md) |
| S1a | vcpkg manifesti, toolchain/CI, GPL denylist | **TAMAMLANDI** | [S1a.md](archive/S1a.md) |
| S1b | `HttpClient`, tek akış indirme, test HTTP sunucusu | **TAMAMLANDI** | [S1b.md](archive/S1b.md) |
| S2 | Probe (HEAD / `Range: bytes=0-0`, 206/200/416) | **TAMAMLANDI** | [S2.md](archive/S2.md) |
| S3 | `IStorage` + preallocate + `pwrite` | **TAMAMLANDI** | [S3.md](archive/S3.md) |
| S4 | `SegmentPlanner` + curl multi + tek akışa düşüş | **TAMAMLANDI** | [S4.md](archive/S4.md) |
| S4b-lite | clang-tidy temizliği + baseline | **TAMAMLANDI** | [S4b-lite.md](archive/S4b-lite.md) |
| S5 tasarım | Sidecar şeması, checkpoint, doğrulama kararları | **ONAYLANDI** | [S5-design.md](archive/S5-design.md) |
| S5a-1 | `ResumeState`, atomik yazma, doğrulama | **TAMAMLANDI** | [S5a-1.md](archive/S5a-1.md) |
| S5a-2a | Motor + `Downloader` (resume, checkpoint) | **TAMAMLANDI** | [S5a-2a.md](archive/S5a-2a.md) |
| S5a-2b | CLI, Ctrl+C, gerçek süreç testleri | **TAMAMLANDI** | [S5a-2b.md](archive/S5a-2b.md) |
| S5b | Tek akış devamı + kayıp/performans ölçümleri | **SIRADA** | bu dosya |
| S6 | `RetryPolicy` + `RateLimiter` | | [M1-design.md](archive/M1-design.md) |
| S7 | `DynamicSplitter` + benchmark tablosu | | bu dosya |
| S8 | `HashVerifier` + hata modeli + CLI bayrakları + RSS | | bu dosya |
| M1 kalan | Test altyapısı + bilinen tuzaklar | kısmi | [M1-design.md](archive/M1-design.md) |
| M2–M7 | Kuyruk, UI, eklenti, medya, Android, sürüm | | [roadmap-M2-M7.md](archive/roadmap-M2-M7.md) |

**M1 hedefi:** `flowget-cli get <URL> -o out.bin --connections 8` ile 100 MB dosyayı doğru, hızlı ve
kesintiden sonra devam ettirilebilir indirmek; UI yok.

## Biten dilimler — tek satır kanıt

- **M0** — Monorepo + ADR-0001…0006 + CI; run #3 üç job yeşil, `ctest` 1/1. → [M0.md](archive/M0.md)
- **S1a** — vcpkg `2026-10-29` / baseline `4702e38`, GPL denylist betiği; run #6 üç job yeşil
  (sığ klon baseline düzeltmesi sonrası). → [S1a.md](archive/S1a.md)
- **S1b** — Tek bağlantılı indirme + yerel test sunucusu; 6 entegrasyon testi, `ctest` 2/2,
  CLI e2e SHA-256 eşit. → [S1b.md](archive/S1b.md)
- **S2** — Probe; 42 test, 206/416/404/HEAD 403/HEAD takılması kapsandı; run #6 yeşil.
  → [S2.md](archive/S2.md)
- **S3** — `IStorage` + preallocate + `pwrite`; 69 test, ASCII olmayan yol round-trip, run #8 yeşil.
  → [S3.md](archive/S3.md)
- **S4** — Segmentli indirme + fallback; 59 birim + 44 entegrasyon, üç Linux preset'i PASSED,
  run #12 yeşil. → [S4.md](archive/S4.md)
- **S4b-lite** — clang-tidy gerçek bulguları düzeltildi; baseline **6**, run #13 yeşil.
  → [S4b-lite.md](archive/S4b-lite.md)
- **S5a-1** — Sidecar modeli + atomik yazma; 74 birim + 51 entegrasyon, üç preset PASSED,
  build maliyeti ~1,2 s/TU. → [S5a-1.md](archive/S5a-1.md)
- **S5a-2a** — Motor + `Downloader`; 79 birim + 75 entegrasyon, checkpoint maliyeti WAN −%2,7 /
  loopback +%1,9 (GA +%6,6'ya kadar farkı dışlamıyor), run #17 yeşil. → [S5a-2a.md](archive/S5a-2a.md)
- **S5a-2b** — `--no-resume`, `Resuming from`, Ctrl+C → 130; 3/3 süreç testi, run #19 yeşil.
  Windows elle doğrulama: 512 MB dosyada Ctrl+C %24,7 → 130, aynı komut `Resuming from 126.5 MiB`,
  SHA-256 `884A240A…F17526` curl ile aynı, `--no-resume` baştan indirip aynı hash'i verdi.
  → [S5a-2b.md](archive/S5a-2b.md)

## S5b — tek akış devamı + ölçümler (sıradaki dilim)

- [ ] `SingleStreamDownloader` resume: dosya boyutu > 0 ise `Range: bytes=N-` + `If-Range`; 206 → ekle,
      200 → truncate + baştan.
- [ ] Ölçüm 1 (kayıp): throttle'lı yerel sunucu, 20 MB/s, %40'ta öldür → yeniden çalıştır; yeniden
      indirilen veri **≈ checkpoint aralığı** (~2 s'lik veri), SHA-256 eşit. Tabloya işlenir.
- [ ] Ölçüm 2 (performans): S5a-2a'da ölçülen 100 MB / 8 bağlantı / checkpoint açık sonucunu S5b
      sonunda **tekrar doğrula** → S7 baseline'ı 3,66 s'nin en fazla **~%5** üstü (≤ ~3,84 s).
      Aynı makine/sunucu, birkaç tekrar; sonuç tabloya.
- [ ] Doküman: resume varsayılan açık, `--no-resume` kaçış yolu; sidecar M2'de SQLite'a taşınır.

**Riskler**

- 2 s'lik checkpoint 20 MB/s'de ~40 MB kayıp demek; DoD tablosu buna göre güncellendi.
- `preallocate` EOF'u `total_bytes` yapar; resume `open(read_write)` kullanır, asla `truncate` (S3 dersi).
- Kısmi dosya başka bir dosyaya aitse ve validator yoksa fark edilmez → validator yoksa resume yok.
- Sidecar salt-okunur dizinde yazılamazsa uyarı + indirme devam (resume verisi kaybolur).
- Zayıf ETag tek başına yetmez; sunucu `If-Range`'i yok sayıyorsa 200 → sıfırdan (sessiz ekleme yok).

**DoD:** S5a-1/S5a-2/S5b testleri yeşil; Ctrl+C ve kill senaryolarında devam çalışıyor; 100 MB performans
şartı (±%5) sağlanıyor; `check_linux.ps1` üç preset `exit 0`; clang-tidy baseline temiz.

## Sıradaki adımlar

1. **S5b** — yukarıdaki dört madde; ayrı commit + ölçüm kanıtı.
2. **S6** — `RetryPolicy` (exponential backoff + jitter, `Retry-After`, 429/503'te bağlantı azaltma) ve
   `RateLimiter` (global token bucket); testler ve `--rate-limit` bayrağı.
3. **S7** — `DynamicSplitter` (EWMA) + benchmark tablosu; aşağıdaki tabanla yeniden ölçüm.

## M1 kalan işler (S6–S8, test altyapısı, tuzaklar)

Ayrıntılı plan ve gerekçeler: [M1-design.md](archive/M1-design.md).

- **S6:** `RetryPolicy`, `RateLimiter`; CLI `--rate-limit`.
- **S7:** `DynamicSplitter` (en yavaş segmentin kalanı ≥ 2×min ise böl), benchmark harness
  (1/2/4/8/16 bağlantı) → tablo.
- **S8:** `HashVerifier` (akışlı SHA-256/MD5, erken uyuşmazlık), hata modeli (enum + mesaj, exception
  sınırı geçmez), CLI bayrakları (`--rate-limit`, `--user-agent`, `--referer`, `--header`, `--proxy`,
  `--cookie`, `--sha256`, `--resume`, `--verbose`), RSS ölçümü.
- **Test altyapısı:** yanlış `Content-Length` senaryosu; 1 MB/100 MB/1 GB fixture + SHA-256 manifest;
  planner aralık matematiği, `Content-Range` parser, backoff, token bucket, bitmap serileştirme
  birim testleri; 8 bağlantı hash, %40 `SIGKILL`, ETag değişimi, Range'i yok sayan sunucu, 429
  fırtınası entegrasyonları; TSan ile 100 iterasyon; benchmark tablosu README'ye.
- **Bilinen tuzaklar:** `CURLMOPT_MAX_HOST_CONNECTIONS` (varsayılan 6 → yükselt); HTTP/2
  multiplexing segmentlerde kapalı; `Accept-Encoding: identity`; `Content-Length` yoksa segmentasyon
  yok; aynı aralığa iki işçi yazmasın (generation jetonu).

## S7 karşılaştırma tabanı

2026-11-16, Windows, `100MB.zip`, download.thinkbroadband.com, tek ölçüm: `curl.exe` **6,29 sn**;
FlowGet `--connections 1` **5,70 sn**, `--connections 8` **3,66 sn**; 1 ve 8 bağlantıda SHA-256 aynı,
indirilen boyut 104857600 bayt.

S7'de aynı komutlarla, **birkaç tekrarla ve ölçüm sırasını değiştirerek** yeniden ölç (tek ölçüm ve sabit
sıra ısınma/önbellek etkisini ayırmaz). Ayrıntı ve metrik tablosu:
[measurements.md](measurements.md).

## S8 — ertelenenler

- [ ] Windows'a özel dosyalar (`storage_win32.cpp` vb.) Linux CI clang-tidy kapsamı dışında.
- [ ] CI action sürümleri: `actions/checkout`, `actions/cache`, `ilammy/msvc-dev-cmd` için Node 24
      destekleyen güncel sürümleri her action'ın Releases sayfasından kontrol edip güncelle
      (Node 20 deprecation uyarısı, run #13).
- [ ] `get --help` / alt komut yardımı yok (yalnız ana `--help` var).

## M2–M7 özet

Tam metin: [roadmap-M2-M7.md](archive/roadmap-M2-M7.md).

- **M2 — Kuyruk, SQLite, limitler, kurtarma (≈2 hafta):** şema v1 + migration, tek yazar/WAL repository,
  250 ms checkpoint + çökme kurtarma, scheduler (global limit/öncelik), hız limiti ±%5, proxy/cookie/
  header, duraklat/devam/iptal/sil, log rotasyonu. DoD: 200 indirmelik 8 saatlik soak, RSS artışı <%10.
- **M3 — Flutter Windows UI + FFI (≈3 hafta):** C ABI, `ffigen`, olay akışı (100 ms coalesce), liste +
  segment ilerleme çubuğu, ekle/ayarlar/tray/bildirim/tek örnek, 60 FPS ve erişilebilirlik.
  DoD: UI'dan 1 GB indirme, kapat-aç sonrası devam; `flutter test` yeşil.
- **M4 — Tarayıcı eklentisi + Native Messaging (≈2 hafta):** stdio framing host, HKCU installer,
  Chromium/Firefox manifestleri, medya yakalama, popup/sağ tık, origin ve path traversal güvenliği.
  DoD: üç tarayıcıda kurulum → tık → indirme; HLS yakalama.
- **M5 — yt-dlp + HLS/DASH + FFmpeg (≈3 hafta):**
  **Ön koşul:** araçlar koda gömülmez, sidecar olarak paketlenir; `docs/adr/0007-*.md` henüz yok —
  M5'in ilk işi bu ADR (araç başına karar + gerekçe + lisans). yt-dlp (Unlicense), FFmpeg **yalnız
  LGPL** derlemesi (GPL/x264 yasak), QuickJS-NG (MIT) gerekip gerekmediği, N_m3u8DL-RE (MIT) ile
  "kendi motor" kıyası, sürüm sabitleme + SHA-256, `THIRD_PARTY_NOTICES`, GPL/AGPL denylist.
  Kapsam dışı: Ghost Downloader 3 / cat-catch (GPL-3.0), torrent/eD2k.
  İşler: `YtDlpSupervisor`, format seçimi, HLS/DASH parser + mux, FFmpeg presetleri, yerel fixture'lar,
  DRM tespitinde net hata. DoD: gerçek HLS + yt-dlp sitesi uçtan uca, `ffprobe` kanıtı.
- **M6 — Android portu (≈3 hafta):** NDK (16 KB hizalama), `AndroidStorage` + SAF dışa aktarma,
  foreground service + checkpoint, motor sahipliği, wake lock, share intent, ağ geçişi.
  DoD: cihazda paylaş → indir → duraklat/devam → uçak modu → tamam, hash doğru; F-Droid uyumlu.
- **M7 — Paketleme, dokümantasyon, sürüm (≈1-2 hafta):** Inno Setup + taşınabilir zip, imza/SmartScreen,
  APK/AAB + F-Droid metadata, README/SSS, `docs/` İngilizce sürüm, lisans dosyaları, release workflow,
  temiz VM/cihaz kurulum testi. DoD: `v1.0.0` artifact'ları ve sıfırdan çalışan README komutları.

## Ölçümler

Metrik hedef tablosu, S7 tabanı ve S5a-2a checkpoint maliyeti: [measurements.md](measurements.md).
Yeni ölçümler oraya eklenir; bu dosyada yalnız bağlantı tutulur.

## Kapsam dışı (bilinçli)

DRM bypass · torrent/magnet · canlı yayın kaydı · tarayıcı proxy'si · macOS/Linux masaüstü (v1.1) ·
bulut senkronizasyonu · hesap sistemi · telemetri (varsayılan kapalı, v1'de yok)

# Arşiv — M2…M7 tam yol haritası metni

> `tasks/todo.md`'den **olduğu gibi** taşındı (2026-11-22). Bu dosya tarihsel kayıttır;
> güncel durum ve sıradaki iş için `tasks/todo.md` indeksine bak.

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

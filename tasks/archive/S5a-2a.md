# Arşiv — S5a-2a: motor + Downloader

> `tasks/todo.md`'den **olduğu gibi** taşındı (2026-11-22). Bu dosya tarihsel kayıttır;
> güncel durum ve sıradaki iş için `tasks/todo.md` indeksine bak.

#### S5a-2 — motor + CLI entegrasyonu (iki dilime bölündü, **PLAN — ONAY BEKLİYOR**)

> S5a-2a çekirdeği (motor + `Downloader`) bitirir; S5a-2b yalnız CLI/Ctrl+C ve gerçek süreç testlerini
> ekler. Her dilim ayrı commit + ayrı kanıt. S5a-2a'da süreç öldürme yok; iptal yalnız API'deki
> `std::atomic<bool>` cancel jetonuyla tetiklenir.

##### S5a-2a — motor + Downloader (ağ + sidecar), süreç öldürme yok

**Amaç:** Geçerli sidecar + doğrulanmış kısmi dosya varsa yalnız eksik bayt aralıklarını indir; her
checkpoint'te sidecar'ı çökme tutarlı biçimde güncelle; sunucu değiştiyse tek kez sıfırdan başlat.

**Kararlar (uygulama ayrıntısı)**

1. `DownloadOptions::resume = true` (varsayılan). `false` → sidecar + `.tmp` silinir, dosya truncate.
2. `DownloadSummary`: `bool resumed`, `int64_t resumed_from_bytes`, `std::string resume_note`.
3. Yeni `DownloadOptions::on_resume` (`std::function<void(int64_t)>`, fırlatmaz): resume kararı verilince
   bir kez, ilk istekten önce çağrılır. CLI satırı S5a-2b'de bağlanır.
4. Motor `SegmentTask { Segment range; int64_t done; }` alır; `done == length` olan aralık istek
   açmadan tamam sayılır. İstek `start+done .. end-1`, uzunluk `length-done`.
5. `If-Range`: güçlü ETag varsa ETag; yoksa `Last-Modified`; ikisi de yoksa başlık yok (ve resume
   yapılmaz). Zayıf `W/` ETag `If-Range`'de kullanılmaz.
6. Sidecar'ın `url` alanı **kullanıcının verdiği özgün URL** olarak yazılır, probe'un effective URL'i
   değil. Resume'da özgün URL + `total_bytes` + güçlü ETag/Last-Modified karşılaştırılır; **effective
   URL karşılaştırılmaz.** Gerekçe: CDN yönlendirmeleri imzalı ve her çalıştırmada farklı olabilir;
   effective URL'i karşılaştırmak gerçek sitelerde resume'u hiç çalıştırmaz. Segment istekleri yeni
   probe'un **güncel effective URL'ine** gider (yönlendirme hedefi değişse bile aynı temsil).
7. Checkpoint sırası değişmez: `storage->flush()` → başarılıysa `.flowget.tmp` yaz + flush →
   atomik rename. **Veri flush'ı başarısızsa sidecar yayınlanmaz**; indirme `io` hatasıyla biter,
   önceki sidecar olduğu gibi kalır.
8. `done` yalnız `write_at` başarı döndürdükten sonra artar; checkpoint anı motor thread'inde,
   uçuşta yazma yokken alınır (kilit gerekmez).
9. Checkpoint tetikleri: 2 s periyot (sabit `2000 ms`), segment bitişi, iptal, hata, normal bitiş.
   Yeni bayt yoksa periyodik checkpoint atlanır. Sidecar yazma hatası ölümcül değil: sonraki tetikte
   yeniden denenir; iptal/hata anındaki son hata `resume_note`'a yazılır.
10. Çıkış yolları:
    - başarı: veri flush (zaten) → sidecar sil (silinemezse tamam sayılır, sonraki çalıştırma temizler);
    - iptal: checkpoint yayınla → `aborted`; dosya + sidecar korunur;
    - taşıma hatası: son checkpoint'i yayınlamayı dene → hata; dosya + sidecar korunur (S6 yeniden
      denemesi ve S5b ölçümü buna dayanır);
    - semantik fallback (range/ETag/Content-Range uyuşmazlığı): dosya + sidecar silinir, tek akış;
    - resume geçersiz (`If-Range`'e 200): dosya + sidecar silinir, **bir kez** yeniden probe + taze
      plan; taze deneme de 200 alırsa tek akışa düşer (döngü yok).
    > Not: S5a-2 maddesindeki "fallback/hata yolunda sidecar silme" burada "dosyanın silindiği
    > yollar" olarak netleştirildi; iptal/taşıma hatasında dosya korunduğu için sidecar da korunur.
11. Resume yalnız segmentli yolda (`connections >= 2`) uygulanır; tek akış resume'u S5b. `resume=false`
    her iki yolda da sidecar'ı siler.
12. Sidecar okuma UTF-8 güvenli olmalı: `std::ifstream(path)` yerine `IStorage::open(read_only)` +
    `read_at` kullanan `detail::read_file_utf8` (ASCII olmayan yol testi).
13. `run_segmented` içine test seam'i: `StorageFactory` (varsayılan `make_platform_storage`) ve
    `checkpoint_interval` (varsayılan 2000 ms, yalnız iç kullanım). Üretimde varsayılan değişmez.

**Dosyalar**

- [x] `core/include/flowget/download.h` — `resume`, `on_resume`, summary alanları.
- [x] `core/src/segmented_download.{h,cpp}` — `SegmentTask`, kalan aralık, `If-Range` (güçlü ETag
      yoksa Last-Modified), checkpoint, `resume_invalidated` sonucu, `StorageFactory`/interval
      seam'i; segment istekleri probe'un güncel effective URL'ine gider.
- [x] `tests/integration/segmented_engine_test.cpp` — motor: kalan aralıktan devam, bitmiş aralığa
      istek yok, `If-Range` seçimi (güçlü/zayıf/yok), iptalde checkpoint, flush hatasında `io` +
      eski sidecar korunur.
- [x] `core/src/resume_checkpoint.{h,cpp}` — `publish_resume_checkpoint(IStorage&, sidecar_path,
      const ResumeState&, std::string& error)`: veri flush → sidecar atomik yaz; flush hatasında
      sidecar'a dokunmaz.
- [x] `core/src/file_replace.{h,cpp}` (veya `file_io.{h,cpp}`) — `read_file_utf8` (boyut sınırı +
      UTF-8 yol); `resume_sidecar_path` / `remove_stale_sidecar_temp` kullanımı.
- [x] `core/src/downloader.cpp` — sidecar oku → doğrula → probe karşılaştır → devam/baştan; tamam
      kontrolü; `.tmp` temizliği; fallback/hata/başarı yollarında sidecar kararı.
- [x] `tests/unit/resume_state_test.cpp` — truncation/mutation fuzz (aşağıda): 2 test, her bayt
      kesme + 1000 sabit tohumlu mutasyon.
- [x] `tests/unit/resume_checkpoint_test.cpp` — flush hatası invariantı: veri flush'ı başarısızsa
      sidecar baytları değişmez, `.tmp` kalmaz; boş sidecar yolunda veri hiç flush edilmez. Motor
      seviyesinde enjekte edilen hata commit (b)'de.
- [x] `tests/integration/downloader_test.cpp` (veya yeni `resume_download_test.cpp`) — API + cancel
      jetonu senaryoları (`resume_download_test.cpp`, 12 test).
- [x] `tests/CMakeLists.txt`, `core/CMakeLists.txt`.

**Resume doğrulama sırası (`Downloader`)**

1. `resume == false` → sidecar + `.tmp` sil; taze.
2. Her durumda `.flowget.tmp` temizle.
3. Sidecar yoksa taze.
4. Boyut > 1 MiB / okunamıyor / parse hatası → sidecar sil, taze, `resume_note`.
5. Tüm segmentler `done == length` ise: çıktı boyutu `total_bytes` ise **tamam** (ağ isteği yok,
   sidecar sil, `resumed=true`, `resumed_from_bytes=total`, note `"already complete"`); değilse sil, taze.
6. Probe. Sidecar `url` **kullanıcının özgün URL'i** ile, `total_bytes` probe boyutuyla, güçlü ETag
   (yoksa Last-Modified) probe değeriyle karşılaştırılır. Effective URL karşılaştırılmaz (CDN
   yönlendirmeleri değişebilir). Uyuşmazsa → sil, taze, note (neden).
7. Çıktı dosyası yok veya boyutu `total_bytes` değil → sil, taze.
8. `bytes_done == 0` → resume'un kazancı yok: sil, taze.
9. Aksi halde `SegmentTask`'ları kur, motoru `read_write` (asla truncate) + `preallocate(total)` ile
   çalıştır; `on_resume(bytes_done)` çağır.

**Testler (S5a-2a; süreç öldürme yok, API + cancel jetonu)**

- [x] **%40'ta iptal → resume:** throttle'lı yerel sunucu; progress callback bayt ≥ %40 olunca
      `cancel=true`; `aborted`, sidecar var; ikinci `download` → `resumed=true`,
      `resumed_from_bytes>0`, içerik SHA-256 birebir; sunucuya giden `Range` `done`'dan başlıyor,
      `If-Range` doğru validator.
- [x] **Periyodik checkpoint:** >2 s süren throttle'lı indirme; iptalden önce sidecar en az bir kez
      güncellenmiş (`done > 0`) ve içerik geçerli. (`ResumeDownload.PublishesPeriodicCheckpoints…`:
      ayrı iş parçacığı 50 ms'de sidecar okur, arada `0 < done < total` görür.)
- [x] **Flush hatası sidecar'ı ilerletmez:** `publish_resume_checkpoint` sahte storage'ın `flush`'ı
      hata döndürünce `false`; sidecar baytları **değişmez**, `.tmp` kalmaz (birim). Motor seviyesinde
      `StorageFactory` ile enjekte edilen hata → `io` hatası, sidecar ilerlemez (entegrasyon).
- [x] **`done` yalnız başarılı yazmadan sonra:** sahte storage N. yazmada hata; iptal/checkpoint
      sonrası sidecar'daki `done`, başarıyla yazılan baytları aşmaz.
      (`SegmentedEngineTest.CheckpointNeverClaimsBytesThatFailedToWrite`.)
- [x] **ETag değişimi:** ikinci çalıştırmada sunucu yeni ETag/gövde → `resumed=false`, sıfırdan,
      yeni içerik doğru, note nedeni taşır.
- [x] **Yönlendirme hedefi değişir, ETag aynı (özgün URL kuralı):** ilk çalıştırma A → B/v1; kısmi
      indirme sonrası ikinci çalıştırmada A → B/v2 yönlendirir, ETag aynı → `resumed=true`, içerik
      SHA-256 birebir; segment istekleri **B/v2**'ye gider, `If-Range` taşır. Sidecar `url` alanı
      A (özgün kullanıcı URL'i) kalır; effective URL karşılaştırılsaydı test kırmızı olurdu.
- [x] **Zayıf ETag → Last-Modified:** `ETag: W/"..."` + `Last-Modified`; resume çalışır, `If-Range`
      Last-Modified taşır.
- [x] **Validator yok:** resume yok; sidecar silinir, sıfırdan. (Motor da validator yokken hiç
      checkpoint yazmaz; birim/entegrasyon testi mevcut.)
- [x] **`If-Range`'e 200:** sunucu validator uyuşmayınca 200 döner → dosya + sidecar silinir, bir kez
      yeniden probe + taze plan; içerik doğru; ikinci 200'de tek akışa düşer.
- [x] **Bozuk sidecar 5 varyantı:** geçersiz JSON, bilinmeyen `schema_version`, eksik alan,
      bitişik olmayan segment, `done` aralık dışı → sidecar silinir, taze indirme, içerik doğru.
- [x] **Dosya silinmiş / boyut değişmiş:** taze indirme; eski sidecar silinir.
- [x] **Kenar 1 — sidecar yok + çıktı var:** çöp içerikli dosya truncate edilir, taze indirme,
      içerik doğru. (`SegmentedDownload.OverwritesExistingFile` aynı yolu kapsar.)
- [x] **Kenar 2 — tüm segmentler done, sidecar silinmemiş:** boyut doğrulanır, ağ isteği yok,
      tamam sayılır, sidecar silinir.
- [x] **Kenar 3 — `resume=false` mevcut sidecar'ı siler:** geçerli sidecar + kısmi dosya varken
      `resume=false` → sidecar yok, dosya truncate, taze içerik.
- [x] **Yanlış `Content-Length`:** resume isteğinde segment `Content-Length` uyuşmazlığı → fallback
      (tek akış) ve içerik doğru; sidecar silinmiş.
- [x] **Fuzz/truncation (S5a-1 parser'ı):** `ResumeState.EveryTruncationEitherParsesOrFailsCleanly`
      geçerli sidecar metnini 0..N **her bayt konumunda** keser (N+1 girdi);
      `ResumeState.FixedSeedMutationsNeverProduceAnInvalidState` sabit tohumla (`0xC0FFEE`) 1000
      mutasyon üretir (1–3 bayt değişimi). Her sonuç ya geçerli invariant'larla `ok()` (şema,
      `total_bytes > 0`, boş olmayan URL, validator, bitişik segmentler, `0 ≤ done ≤ length`,
      toplam = `total_bytes`) ya da boş olmayan hata. `linux-x64-asan`/UBSan ve TSan temiz.
- [x] **Performans (S5a-2a şartı):** A = S5a-1 CLI (`d23ea1f` worktree build), B = checkpoint'li yeni
      CLI; A,B / B,A sıralı çiftler, aynı sunucu, 8 bağlantı. Eşik: medyan ≤ ~%5 yavaş.
      - thinkbroadband 100 MB, 5 çift (aralarda 10 s): A medyan **4,680 s**, B **4,553 s** →
        **−%2,7** (B daha hızlı); eşleşmiş medyan oran 0,891. Aralıksız koşuda sunucu 429/403
        döndürdü; `exit≠0` koşular tabloya alınmadı (`tasks/lessons.md`, 2026-11-21).
      - loopback 200 MB, 61 çift (3 batch): A medyan **0,574 s**, B **0,585 s** → **+%1,9**;
        eşleşmiş medyan oran **0,988**, %95 bootstrap GA **[0,926; 1,066]** → sıfır fark ayırt
        edilemiyor. Batch medyanları makine yükü yüzünden −%7…+%23 salındı (tek koşu 0,35–5,1 s).
      - Ayrıntılı tablo: `## Ölçüm ve kanıt tablosu` → "S5a-2a checkpoint maliyeti".
- [x] Kanıt: MSVC build + `ctest` (79 birim + 75 entegrasyon), `check_linux.ps1`
      (debug/ASan/TSan hepsi PASSED, 4,9 dk), `pre_commit`, clang-tidy baseline 6 (yeni bulgu yok),
      performans ölçümü; **CI run #17, commit `8d0aaec` — üç job yeşil** (Windows MSVC, Linux Clang +
      ASan/UBSan, GPL denylist; 2026-11-21,
      [run #17](https://github.com/Kouizer/flowget/actions/runs/44918120502)).

> **Dilim commit'i (b) kanıtı (2026-11-20):** MSVC 19.52 `/W4 /WX` build temiz; `ctest` 2/2 —
> **77 birim + 61 entegrasyon** (5 yeni `SegmentedEngineTest`: kalan aralık, `If-Range` seçimi,
> iptalde checkpoint, flush hatası). `scripts/check_linux.ps1`: linux-x64, ASan/UBSan ve TSan
> üçünde `ctest` 2/2 **PASSED** (3,2 dk); clang-tidy **6 = baseline**. `pre_commit run --all-files`
> temiz.

> **Dilim commit'i (c) kanıtı (2026-11-20):** `resume`, `on_resume`, `resumed`,
> `resumed_from_bytes`, `resume_note` API'de; `Downloader` karar ağacı (özgün URL doğrulaması,
> tamam kontrolü, `resume=false` katı sidecar silme, `.tmp` temizliği, tek yeniden probe/plan,
> fallback/hata yollarında sidecar kararı) tamamlandı. MSVC 19.52 `/W4 /WX` build temiz;
> `ctest` 2/2 — **77 birim + 74 entegrasyon** (12 yeni `ResumeDownload` senaryosu + `done`
> değişmezi için `SegmentedEngineTest.CheckpointNeverClaimsBytesThatFailedToWrite`).
> `scripts/check_linux.ps1`: linux-x64, ASan/UBSan ve TSan `ctest` 2/2 **PASSED** (5,2 dk);
> clang-tidy **6 = baseline** (yeni bulgu yok). `pre_commit run --all-files` temiz. Sidecar
> okumaları `IStorage::open(read_only)` üzerinden `FILE_SHARE_READ|WRITE|DELETE` ile yapılıyor
> (paylaşım dersi `tasks/lessons.md`, 2026-11-20). **CI doğrulaması:** run #16, commit `137b664` —
> üç job da yeşil (Windows MSVC, Linux Clang + ASan/UBSan, GPL denylist).

> **Dilim commit'i (d) kanıtı (2026-11-21):** Parser fuzz'ı (her bayt kesme + 1000 sabit tohumlu
> mutasyon), sidecar **silme** paylaşım testi ve performans ölçümü eklendi. Plan-test eşlemesi:
> özgün URL → `ResumeDownload.RedirectTargetMayChangeWhileTheOriginalUrlResumes`; Windows paylaşım
> bayrakları → `ResumeStoreTest.ReplacesTheSidecarWhileAReaderHasItOpen` +
> `ResumeStoreTest.DeletesTheSidecarWhileAReaderHasItOpen`; flush invariantı →
> `ResumeCheckpoint.FlushFailureLeavesThePreviousSidecarUntouched` +
> `SegmentedEngineTest.FlushFailureEndsWithIoAndKeepsTheOldSidecar`; `done` değişmezi →
> `SegmentedEngineTest.CheckpointNeverClaimsBytesThatFailedToWrite`. MSVC 19.52 `/W4 /WX` build
> temiz; `ctest` 2/2 — **79 birim + 75 entegrasyon**. `scripts/check_linux.ps1`: linux-x64,
> ASan/UBSan ve TSan `ctest` 2/2 **PASSED** (4,9 dk); clang-tidy **6 = baseline**.
> `pre_commit run --all-files` temiz. Performans: WAN 5 çiftte −%2,7; loopback 61 çiftte +%1,9
> (eşleşmiş medyan 0,988, GA [0,926; 1,066]) — ölçülebilir yavaşlama yok; GA +%6,6'ya kadar bir farkı
> dışlamıyor, S7'de sessiz makinede yeniden doğrulanacak.

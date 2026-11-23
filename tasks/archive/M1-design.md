# Arşiv — M1 tasarımı, test altyapısı, bilinen tuzaklar

> `tasks/todo.md`'den **olduğu gibi** taşındı (2026-11-22). Bu dosya tarihsel kayıttır;
> güncel durum ve sıradaki iş için `tasks/todo.md` indeksine bak.

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

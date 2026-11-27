# Arşiv — S5b: tek akış devamı + kayıp/performans ölçümleri

> `tasks/todo.md`'den **olduğu gibi** taşındı (2026-11-27). Bu dosya tarihsel kayıttır;
> güncel durum ve sıradaki iş için `tasks/todo.md` indeksine bak. Ölçüm tabloları
> [`tasks/measurements.md`](../measurements.md) içinde.

##### S5b — tek akış devamı + kayıp/performans ölçümleri

- [x] `SingleStreamDownloader` resume: sidecar'daki **flush edilmiş `done`** değeri N ise
      `Range: bytes=N-` + `If-Range`; 206 → ekle, 200 → truncate + baştan. **N dosya boyutu değildir:**
      dosya alanı baştan `total_bytes` olarak ayrılır (preallocate), flush edilmemiş baytlar güvenilir
      değildir; N yalnız checkpoint'in yayınladığı `done`'dan gelir.
- [x] Resume yalnız `Content-Length` biliniyorsa yapılır; bilinmiyorsa tek akışta boyut doğrulanamaz →
      sıfırdan indir.
- [x] Sidecar (tek akış): tek segment `[0, total)` + `done`. **`done` < dosyadaki yazılmış bayt** iken
      (flush edilmemiş kuyruk) resume testi: aynı komut `done`'dan devam eder, SHA-256 eşit.
- [x] Ölçüm 1 (kayıp): throttle'lı yerel sunucu, 20 MB/s, %40'ta öldür → yeniden çalıştır; yeniden
      indirilen veri **≈ checkpoint aralığı** (~2 s'lik veri), SHA-256 eşit. Tabloya işlenir.
- [x] Ölçüm 2 (performans, `--connections 1`): A = S5a-2b'nin son commit'indeki (`6bcb0ac`)
      checkpoint'siz tek akışlı CLI, B = yeni CLI. Loopback ~200 MB, A,B sırayla **en az 20 çift**;
      eşleşmiş oran (B/A) medyanı + bootstrap %95 GA; sonuç `measurements.md`'ye işlenir.
      → A gerçekte `f749d83` oldu (S5b öncesi HEAD) ve ölçüm 2026-11-27'de 40 çiftle yenilendi;
      ikisi de `--connections 1`'de probe yapmıyor (`measurements.md`).
- [x] Doküman: resume varsayılan açık, `--no-resume` kaçış yolu; sidecar M2'de SQLite'a taşınır.

**Riskler**

- 2 s'lik checkpoint 20 MB/s'de ~40 MB kayıp demek; DoD tablosu buna göre güncellendi.
- `preallocate` EOF'u `total_bytes` yapar; resume `open(read_write)` kullanır, asla `truncate` (S3 dersi).
- Tek akışta resume başlangıcı dosya boyutundan alınırsa preallocate edilmiş sıfır baytlar atlanır/tekrar
  yazılır → bozuk dosya; başlangıç yalnız sidecar `done`.
- Kısmi dosya başka bir dosyaya aitse ve validator yoksa fark edilmez → validator yoksa resume yok.
- Sidecar salt-okunur dizinde yazılamazsa uyarı + indirme devam (resume verisi kaybolur).
- Zayıf ETag tek başına yetmez; sunucu `If-Range`'i yok sayıyorsa 200 → sıfırdan (sessiz ekleme yok).

**DoD:** S5a-1/S5a-2/S5b testleri yeşil; Ctrl+C ve kill senaryolarında devam çalışıyor; tek akış A/B
ölçümü tamamlanıp eşleşmiş oran + GA `measurements.md`'ye işlendi; `check_linux.ps1` üç preset `exit 0`;
clang-tidy baseline temiz.

**S5b kanıtı (2026-11-27):**

Commit'ler: `9e35fc4` (tek akış checkpoint), `9e5857e` (checkpoint'ten resume + `If-Range`),
`a7a3f4f` (yarışlı test düzeltmesi), `b539da7` (clang-format), `bc03de3` (tek akış denemesini tek
amaçlı adımlara bölme), `245732f` (tidy baseline **5**), `da43565`/`011cc49` (dersler),
`674ef35`/`f1e1a50` (ölçüm + ders özeti düzeltmesi).

- **Windows:** `ctest --test-dir build/windows-x64` → **3/3 passed** (51,94 s). GTest sayıları:
  **79 birim + 84 entegrasyon + 3 süreç = 166 test**. S5a-2b'de entegrasyon 75 idi; S5b 9 test ekledi
  (resume, `If-Range`/200 düşüşü, uyumsuz 206/416, `done == total`, validatorsüz sidecar yok).
- **Linux:** `scripts/check_linux.ps1` üç preset (`linux-x64`, `linux-x64-asan`, `linux-x64-tsan`)
  **PASSED**; clang-tidy **5 = baseline** (`scripts/clang_tidy_baseline.txt`).
- **Yarış testi:** düzeltilmiş test HEAD'de ASan'da **300/300 yeşil**; "hiç checkpoint görmedi" türü
  bir kez görüldü, yeniden üretilemedi (`tasks/lessons.md`, 2026-11-25).
- **Ölçüm 1 (kill -9 kaybı, 250 MiB @ 20 MiB/s):** üç denemede de kayıp **~21,1 MB = 1,01 s** veri
  (2 s'lik aralığın beklenen yarısı); resume isteği tam `boyut − done`, SHA-256 eşit.
- **Ölçüm 2 (`--connections 1` A/B, loopback, Release):** 200 MiB / 40 çift → B/A **1,058**
  GA **[1,039; 1,071]** (1,0 dışında, ≈6 ms); 512 MiB / 20 çift → **0,992** [0,969; 1,024];
  1 GiB / 20 çift → **0,943** [0,777; 1,123]. A ve B `--connections 1`'de probe yapmıyor; kalan sabit
  ek yükün checkpoint işinden geldiği **doğrulanmadı**. Ayrıntı: `tasks/measurements.md`.
- **Push:** kullanıcıda (AGENTS kuralı); bu arşiv commit'i push edilmedi.

**S5b'den ertelenen:** A/B'nin sessiz makinede, WAN ve gerçek disk üzerinde yeniden ölçümü ve 200 MiB'deki
sabit ek yükün kaynağının doğrulanması → `tasks/todo.md` **S7 — ertelenenler**.

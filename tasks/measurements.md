# Ölçümler ve kanıt tabloları

> `tasks/todo.md`'den **olduğu gibi** taşındı (2026-11-22). Yeni ölçümler buraya eklenir;
> `tasks/todo.md` indeksi yalnız bağlantı verir.

#### S7 karşılaştırma tabanı (2026-11-16, Windows, 100MB.zip, download.thinkbroadband.com, tek ölçüm)
- `curl.exe`: 6,29 sn
- FlowGet `--connections 1`: 5,70 sn
- FlowGet `--connections 8`: 3,66 sn
- SHA-256 hash'ler 1 ve 8 bağlantıda aynı, indirilen boyut 104857600 bayt.

S7'de aynı komutlarla, **birkaç tekrarla ve ölçüm sırasını değiştirerek** yeniden ölç (tek ölçüm ve sabit
sıra ısınma/önbellek etkisini ayırmaz); sonuçlar `## Ölçüm ve kanıt tablosu`na işlenir.

## Ölçüm ve kanıt tablosu (doldurulacak)

| Metrik | Hedef | M1 | M2 | M3 | M6 |
|---|---|---|---|---|---|
| 1 GB indirme süresi (8 bağlantı, yerel) | referans | | | | |
| 1 bağlantıya göre hız kazancı (throttle'lı sunucu) | ≥ 4× | | | | |
| CPU kullanımı @ 20 MB/s | < %10 (1 çekirdek) | | | | |
| RSS @ 1 GB indirme | < 64 MB | | | | |
| Resume kaybı (kill -9) | ≈ checkpoint aralığı (~2 s) | ≈1,0 s (3/3) | | | |
| UI kare süresi @ 20 MB/s | < 16 ms | | | | |
| Pil tüketimi (30 dk, Wi-Fi) | ölçülür | | | | |

### S5a-2a checkpoint maliyeti (2026-11-21)

A = S5a-1 CLI (`d23ea1f`, checkpoint yok); B = checkpoint'li yeni CLI. Aynı makine, aynı sunucu,
`--connections 8`, A,B / B,A sıralı çiftler. Ölçülen: duvar saati; `exit≠0` koşular alınmadı.

| Ölçüm | A | B | Fark (medyan) | Değerlendirme |
|---|---|---|---|---|
| thinkbroadband 100 MB, 5 çift | 4,680 s | 4,553 s | −%2,7 | ölçülebilir yavaşlama yok |
| loopback 200 MB, 61 çift | 0,574 s | 0,585 s | +%1,9 | GA üst ucu +%6,6 → fark dışlanamıyor |

**Değerlendirme:** ölçülebilir yavaşlama yok; loopback GA [0,926; 1,066] +%6,6'ya kadar bir farkı
dışlamıyor; S7'de sessiz makinede yeniden doğrula.

Eşleşmiş oran (B/A) medyanı: WAN 0,891; loopback **0,988** (%95 bootstrap GA [0,926; 1,066]).
Loopback'te ham süreler 0,35–5,1 s arasında salındı (makine yükü); batch medyanları −%7…+%23
arasında değiştiği için tek batch'e bakılmadı, eşleşmiş oran ve bootstrap GA raporlandı
(`tasks/lessons.md`, 2026-11-21).

### S5b tek akış resume: kill kaybı ve `--connections 1` A/B (2026-11-25; A/B 2026-11-27'de yeniden ölçüldü)

Ortam: WSL2 Ubuntu, loopback `bench_server.py` (Range + If-Range destekli, 256 KiB parça), Release
derlemeler. A = S5b öncesi HEAD `f749d83`, B = S5b (`9e5857e` + refactor `bc03de3`). Sıra çift
başına A,B / B,A değişir; her koşuda `exit=0`, tam boyut ve **aynı SHA-256** doğrulanır
(`build/measure_s5b.py`).

**İkisi de `--connections 1`'de probe yapmıyor:** `Downloader::download` bu değeri doğrudan
`SingleStreamDownloader`'a yönlendirir ve A'da da aynı dal vardır (`f749d83:core/src/downloader.cpp`).
Bu yüzden A/B farkı probe ya da istek sayısı değildir. Olası neden S5b'nin eklediği checkpoint işidir
(preallocate, sidecar yazma/fsync, final flush/silme); **doğrulanmadı**.

**kill -9 kaybı** (250 MiB dosya, 20 MiB/s throttle, 3 deneme, ~%40'ta SIGKILL):

| Deneme | İlk koşuda sunucudan giden | Sidecar `done` | Kayıp | Kayıp (veri saniyesi) | Resume isteği |
|---|---|---|---|---|---|
| 1 | 105.119.744 | 84.017.152 | 21.102.592 | 1,01 s | 178.126.848 |
| 2 | 105.119.744 | 83.902.464 | 21.217.280 | 1,01 s | 178.241.536 |
| 3 | 105.119.744 | 84.033.536 | 21.086.208 | 1,01 s | 178.110.464 |

`kayıp = giden − done`. 2 s'lik checkpoint aralığında kesme anı düzgün dağıldığı için beklenen değer
~1 s; üç deneme de bunu verdi. Resume isteği üç denemede de tam olarak `boyut − done` bayt istedi,
yani dosya baştan indirilmedi; SHA-256 sonuç aynı.

**`--connections 1` A/B** (eşleşmiş oran B/A; bootstrap %95 GA, 10.000 yeniden örnekleme):

| Dosya | Çift | A medyan | B medyan | B/A medyan | %95 GA | Değerlendirme |
|---|---|---|---|---|---|---|
| 200 MiB | 40 | 0,1701 s | 0,1764 s | 1,058 | [1,039; 1,071] | GA 1,0'ı **içermiyor**: +%5,8 (≈6 ms) |
| 512 MiB | 20 | 0,3566 s | 0,3540 s | 0,992 | [0,969; 1,024] | ölçülebilir fark yok (2026-11-25) |
| 1 GiB | 20 | 1,1886 s | 1,1192 s | 0,943 | [0,777; 1,123] | GA 1,0'ı içeriyor; ham süreler 0,85–1,86 s |

**Değerlendirme:** 200 MiB'de ölçülen +%5,8 (≈6 ms) istatistiksel olarak anlamlı: GA 1,0'ı
içermiyor. Fark baytla ölçeklenmiyor: 512 MiB ve 1 GiB'de GA 1,0'ı içeriyor. 1 GiB koşusunda ham
süreler 0,85–1,86 s arasında salındı (page-cache writeback gürültüsü), bu yüzden GA geniş. Sonuç:
indirme hızında bayt başına ölçülebilir yavaşlama görülmedi; 200 MiB gibi kısa koşuda sabit ek yük
(≈6 ms) orana yansıyor, olası kaynağı S5b checkpoint işi ama doğrulanmadı. Hedef hız olan 20 MB/s'de
1 GiB indirme ~50 s sürer, 6 ms bunun %0,02'sinin altında. S7'de sessiz makinede, WAN ve gerçek disk
üzerinde yeniden doğrulanacak.

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
| Resume kaybı (kill -9) | ≈ checkpoint aralığı (~2 s) | | | | |
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

# Arşiv — S5 tasarımı (onaylanmış kararlar, sidecar şeması, checkpoint)

> `tasks/todo.md`'den **olduğu gibi** taşındı (2026-11-22). Bu dosya tarihsel kayıttır;
> güncel durum ve sıradaki iş için `tasks/todo.md` indeksine bak.

### S5 — Resume (`.flowget` sidecar, checkpoint, ETag/Last-Modified) — **PLAN, ONAYLANDI (2026-11-17); S5a-2 bölünmesi ONAY BEKLİYOR (2026-11-19)**

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

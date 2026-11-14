# Dersler (lessons)

Her hata düzeltmesinden sonra buraya bir kayıt eklenir. Kural: **kök neden** yazılır, belirti değil.
Aynı hata ikinci kez olursa ilgili test/CI kontrolü eklenir.

## Şablon

```
### YYYY-MM-DD — Kısa başlık
- Belirti:
- Kök neden:
- Düzeltme:
- Tekrarını önleyen test/kontrol:
```

---

### 2026-11-14 — GTest test keşfi, ASCII olmayan dizin yolunda build'i kırıyor
- Belirti: `gtest_discover_tests` link sonrası `Unable to open file "...Varsay�lan Proje..."` ile
  build'i durdurdu; CI'da (ASCII yol) sorun görünmüyordu.
- Kök neden: CMake'in GoogleTest keşfi, testi `--gtest_output=json:<mutlak yol>` ile çalıştırıyor;
  GoogleTest bu yolu Windows ANSI kod sayfasıyla açıyor. Türkçe "ı" karakteri kaybolunca dosya
  oluşturulamıyor.
- Düzeltme: `gtest_discover_tests` kaldırıldı; süit tek CTest girdisi olarak kaydedildi
  (`add_test`). Test bazlı filtreleme `--gtest_filter` ile yapılıyor.
- Tekrarını önleyen kontrol: Build, ASCII olmayan bir dizinde de çalıştırılmalı (yerel geliştirme
  yolu); CI tek başına bunu yakalamaz.


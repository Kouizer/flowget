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

### 2026-11-14 — Visual Studio'nun getirdiği `cmake.exe`, `VCPKG_ROOT`'ı sessizce eziyor
- Belirti: Kabukta `VCPKG_ROOT=C:\Users\Enes\vcpkg` olmasına rağmen configure, VS'in gömülü
  `C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg` toolchain'ini kullandı;
  `CMakeCache.txt` içinde `Z_VCPKG_ROOT_DIR` VS yolunu gösterdi.
- Kök neden: VS ile gelen `cmake.exe`, kendi süreç ortamında `VCPKG_ROOT` değişkenini VS'in vcpkg
  kopyasına yazıyor. `cmd` içinde `echo %VCPKG_ROOT%` doğru değeri gösterdiği için sorun ilk bakışta
  görünmüyor; fark yalnızca `cmake -E environment` çıktısında ortaya çıkıyor.
- Düzeltme: `CMakeLists.txt` önce `FLOWGET_VCPKG_ROOT`, sonra `VCPKG_ROOT` kullanıyor; yerelde
  `FLOWGET_VCPKG_ROOT=C:\Users\Enes\vcpkg` ayarlandı, README'de not edildi.
- Tekrarını önleyen kontrol: Configure logunda `vcpkg install` satırları ve `CMakeCache.txt` içindeki
  `Z_VCPKG_ROOT_DIR` beklenen kökü göstermeli; şüphede `cmake -E environment | findstr VCPKG_ROOT`.

### 2026-11-14 — ASCII olmayan dizinde vcpkg port derlemesi (curl) kırılıyor
- Belirti: `curl` portu configure aşamasında `Imported target "CURL::nghttp2" includes non-existent
  path "C:/Users/Enes/Documents/Varsay�lan"` ile `BUILD_FAILED` verdi; aynı dizinde `nghttp2` ve
  `zlib` sorunsuz derlendiği için ilk bakışta "curl portu bozuk" görünüyordu.
- Kök neden: vcpkg, ürettiği `*Config.cmake` dosyalarına mutlak kurulum yollarını yazıyor; Windows
  bu dosyaları ANSI kod sayfasında okuduğu için ASCII olmayan karakter ("ı") yolu kesiyor. M0'daki
  GTest keşfi hatasıyla aynı sınıf: **araç zincirinin ürettiği dosyalara giden yol ASCII olmalı.**
- Düzeltme: Kaynak yolu ASCII değilse `VCPKG_INSTALLED_DIR` otomatik olarak
  `%LOCALAPPDATA%\FlowGet\vcpkg-installed` altına taşınıyor (vcpkg'ın ürettiği tüm yollar ASCII kalır).
- Tekrarını önleyen kontrol: Temiz `build/` ile configure + build, ASCII olmayan bir dizinde
  çalıştırılmalı; CI (ASCII yol) bu hatayı yakalamaz.

### 2026-11-14 — Manifest okuyucu: BOM ve pubspec girinti derinliği
- Belirti: `check_dependencies.py`, PowerShell'in yazdığı `vcpkg.json`'ı `Unexpected UTF-8 BOM` ile
  reddetti; ayrıca pubspec'te 3 bağımlılık varken 4 saydı.
- Kök neden: (a) dosya `utf-8` ile okunuyordu, Windows editörleri/`Set-Content` BOM ekliyor;
  (b) girinti kontrolü `indent < 2` idi, `sdk: flutter` gibi alt alanlar da paket adı sayıldı.
- Düzeltme: okuma `utf-8-sig`; bağımlılık satırı yalnızca tam olarak 2 boşluk girintide kabul ediliyor.
- Tekrarını önleyen kontrol: Betik iki manifestli bir fixture üzerinde çalıştırılıyor (GPL örneği
  `exit=1`, temiz örnek `exit=0`) — S1a kanıtı `tasks/todo.md`'de.

### 2026-11-14 — `windows.h` makroları test sunucusunun derlemesini bozdu
- Belirti: `http_test_server.cpp` içinde `std::min(...)` çağrıları MSVC'de
  `error C2589: '(': '::' sağ tarafında geçersiz belirteç` verdi; aynı dosyada `support/...` başlığı
  da bulunamadı (`C1083`).
- Kök neden: `winsock2.h` → `windows.h` zinciri `min`/`max` makrolarını tanımlıyor ve
  `std::min(a, b)` metni makro tarafından bozuluyor; ayrıca integration test hedefinin include
  dizinleri `tests/` kökünü içermiyordu, oysa test `"support/http_test_server.h"` diye ekliyor.
- Düzeltme: Windows başlıklarından **önce** `NOMINMAX` (+ `WIN32_LEAN_AND_MEAN`) tanımlandı;
  `flowget_integration_tests` hedefine `target_include_directories(... ${CMAKE_CURRENT_SOURCE_DIR})`
  eklendi.
- Tekrarını önleyen kontrol: `windows.h` kullanan her .cpp `NOMINMAX` ile başlar; ayrıca MSVC build'i
  (`-W4 /WX`) bu sınıfı CI'da zaten yakalıyor.




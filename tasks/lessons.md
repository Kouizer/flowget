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

### 2026-11-14 — CLI bayrak döngüsünün gövdesi her yolda `return` ediyordu (MSVC C4702)
- Belirti: Windows CI (MSVC, `/W4 /WX`) `cli/src/main.cpp:145` için `error C2220` + `warning C4702:
  unreachable code` verdi; Clang bu uyarıyı üretmediği için Linux işi yeşil kaldı.
- Kök neden: `for (int index = 1; index < argc; ++index)` döngüsünün gövdesi **her** yolda
  dönüyordu (`handle_flag` sonucu >= 0 ise `return result`, değilse hata yazıp `return 2`). Bu yüzden
  döngünün artış ifadesi (`++index`) ve döngüden sonraki `print_usage/return 2` hiç yürütülemiyordu;
  sözde döngü aslında yalnızca `argv[1]`'i işliyordu. MSVC erişilemeyen artış satırını işaretledi.
- Düzeltme: döngü kaldırıldı; `argv[1]` doğrudan `handle_flag(command)`'a verildi ve davranış
  korundu (bayraklar birbirini dışlar ve süreci sonlandırır). Uyarı susturulmadı, erişilemeyen kod
  silindi.
- Tekrarını önleyen kontrol: MSVC `/W4 /WX` CI build'i; gövdesi koşulsuz `return` eden döngü
  yazılmaz. İleride birden çok argüman işlenecekse gövdede gerçek bir `continue`/tamamlama yolu
  bırakılmalı.

### 2026-11-14 — Test HTTP sunucusu soket tiplerini `int` varsayıyordu; POSIX'te SIGPIPE
- Belirti: Linux CI (Clang, `-Wconversion -Wsign-conversion -Werror`) `tests/support/http_test_server.cpp`
  içinde `send()` sonucunun `const int`'e atanmasını (+ `int` → `size_t` işaret dönüşümünü) ve
  `recv()` sonucunun `const int`'e atanmasını hata verdi. Windows'ta aynı satırlar temiz derlendi.
- Kök neden: Winsock `send`/`recv` **int** döndürür ve uzunluğu **int** alır; POSIX `ssize_t`
  döndürür ve **size_t** uzunluk alır. Kod tek bir `int` varsayımıyla yazılmıştı: Linux'ta dönüş
  değeri daralıyor (ssize_t → int, sessiz kesme riski) ve uzunluk argümanı işaret dönüşümüne
  uğruyordu. Aynı sınıftaki ikinci sorun: POSIX `send()`, peer bağlantıyı kapatmışsa SIGPIPE üretir;
  iptal/düşen bağlantı testlerinde bu, tek bir `send` hatası yerine tüm test ikilisini öldürebilirdi.
- Düzeltme: Platforma göre tek yerde tanımlı `SocketLength` / `SocketIoSize` / `IoResult`
  typedef'leri (Windows: `int`/`int`/`int`; POSIX: `socklen_t`/`std::size_t`/`ssize_t`). `send`/`recv`
  uzunluk ve dönüş değerleri bu tiplerle, açık cast'lerle kullanılıyor; `accept`/`getsockname`/`bind`
  uzunlukları `SocketLength`; POSIX'te `send` bayrakları `MSG_NOSIGNAL`, Windows'ta `0` (SIGPIPE yok).
  `close`/`closesocket` farkı zaten `close_socket` sarmalayıcısında.
- Tekrarını önleyen kontrol: Linux CI'da Clang `-Wconversion -Wsign-conversion -Werror` build'i ve
  ASan/UBSan testleri. Soket sarmalayıcıları dışında çıplak `send`/`recv`/`close` çağrısı yazılmaz;
  dönüş tipleri platform typedef'lerinden alınır.

### 2026-11-14 — Dosya sonundaki fazla boş satır pre-commit `end-of-file-fixer`'ı kırdı
- Belirti: Linux CI'da yalnızca pre-commit adımı kırıldı; `fix end of files` hook'u
  `tasks/lessons.md` sonundaki fazladan boş satırları sildi ve CI "files were modified by this hook"
  ile durdu.
- Kök neden: dosya sonuna birden fazla boş satır bırakılmıştı; hook dosyayı yeniden yazdığı için
  kontrol adımı değişiklik tespit etti. Sorun derleme değil, depo hijyeniydi.
- Düzeltme: `tasks/lessons.md` tam olarak tek satır sonuyla bitecek şekilde temizlendi; depodaki
  diğer izlenen dosyalar da tarandı (başka fazla boş satır veya satır sonu boşluğu yok).
- Tekrarını önleyen kontrol: commit'ten önce `pre-commit run --all-files` çalıştır; pre-commit yoksa
  her dosyanın sonunda yalnızca bir yeni satır kaldığını doğrula.

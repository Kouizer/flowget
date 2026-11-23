# Arşiv — S4b-lite: clang-tidy temizliği

> `tasks/todo.md`'den **olduğu gibi** taşındı (2026-11-22). Bu dosya tarihsel kayıttır;
> güncel durum ve sıradaki iş için `tasks/todo.md` indeksine bak.

### S4b-lite — clang-tidy temizliği (S4 sonrası, ayrı dilim)

**Neden ayrı:** S4 boyunca clang-tidy uyarı modunda kalır; kodun şekli otursun, sonra tek geçişte
bulgular düzeltilsin. Karışık commit (davranış + stil) review'u zorlaştırır.

**Kapsam kararı:** Tam S4b (tüm bulguların sıfırlanması + `--warnings-as-errors`) ertelendi. Bu dilim
yalnız "gürültüyü kes + gerçek bulguları düzelt" işini yapar; kalan bulgular baseline'da kayıtlıdır.

- [x] Dosya listesi `compile_commands.json`'dan üretilir (elle liste yok): `scripts/clang_tidy.py`
      `core/` + `cli/` TU'larını seçer. Eski `git ls-files 'core/*.cpp'` komutu Linux'ta
      `storage_win32.cpp` gibi derlenmeyen dosyaları istiyordu ("not found in compilation database").
- [x] `tests/` **bilinçli olarak kapsam dışı**: test kodu farklı stil profiline sahip, baseline'ı domine
      ederdi. Tidy yalnız üretim kodunu (`core/`, `cli/`) ölçer.
- [x] Yerel `const` değişkenlerde **k-prefix zorunlu değil** (`LocalConstantCase: lower_case`,
      prefix boş). clang-tidy 18'de constexpr'e özel ayar yok; yerel `k...` sabitleri
      `LocalConstantIgnoredRegexp: k[A-Z].*` ile muaf. Global/static sabitler `kCamelCase` kalır.
- [x] Enum adlandırması projedeki gerçek stile hizalandı: C++ değerleri `lower_case`, C ABI
      (`FLOWGET_*`, `flowget_status`) `.*FLOWGET_.*` / `.*flowget_.*` ile muaf. `IgnoredRegexp` tam
      (qualified) adla eşleştiği için desenler `.*` ile sarıldı.
- [x] `misc-include-cleaner` ve `performance-enum-size` **kapatıldı**; gerekçe `.clang-tidy` içinde
      yorum olarak duruyor. curl callback `reinterpret_cast`'leri için gerekçeli NOLINT eklendi.
- [x] Gerçek bulgular düzeltildi (davranış değişmedi): widening çarpımları hedef tipte, `branch-clone`,
      `misc-const-correctness`, `readability-math-missing-parentheses`, `missing-std-forward`,
      `modernize-use-auto`, `starts_with`, `std::array`, `std::string_view`, storage sınıflarında
      special-member delete'leri, `no-automatic-move`. Sınır kaynaklı 4 bulgu (maskelenmiş digest
      indeksi, sınırı aynı satırda kontrol edilen status lookup, libcurl `CURLMSG` union'ı) gerekçeli
      NOLINT ile kapandı.
- [x] CI ile aynı sürüm: **clang-tidy-18** (CI `apt-get install clang-tidy-18`, yerel WSL 18.1.8).
- [x] Baseline: **6 bulgu** (`scripts/clang_tidy_baseline.txt`). Kural: yeni kod yeni bulgu eklemez;
      sayı yalnız bulgu düzeltilince düşürülür. Kalan 6 = 4 yüksek bilişsel karmaşıklık fonksiyonu
      (`run_get`, `run_probe`, `run_segmented`, `download`) + 2 referans üye (`client_`); ikisi de
      gerçek refactor, sonraki milestone'lara bırakıldı.
- [x] `scripts/check_linux.ps1` tidy adımı **advisory**: bulgu sayısı baseline'ı aşarsa uyarı yazıp
      devam eder (exit 2 → advisory); yalnız araç hatasında (clang-tidy yok, DB yok, liste boş →
      exit 1) durur. CI'da `continue-on-error: true` korunuyor.
- [x] `--extra-arg` **gerekmedi**: clang-tidy 18 `compile_commands.json` bayraklarını olduğu gibi
      okudu; gerçek derleme hataları da susturulmadı.
- [ ] `--warnings-as-errors` ve CI'daki `continue-on-error` kaldırma **M1 sonuna (S8) ertelendi**;
      o zamana kadar clang-tidy advisory kalır.
- [x] Kanıt (2026-11-17): Windows MSVC build + ctest 2/2 yeşil; `scripts/check_linux.ps1` 3 preset
      (linux-x64 debug, ASan/UBSan, TSan) PASSED; aynı çalıştırmada `clang-tidy: 6 finding(s) in
      18 file(s); baseline 6 -> OK`. Baseline gate negatif testi: baseline 5 yapıldığında script
      exit 2 + "EXCEEDED by 1" verdi, `check_linux` yine PASSED (advisory) — beklenen davranış.
- [x] Kanıt (CI, 2026-11-17): GitHub Actions **run #13**, commit `9a5b417` — Windows (MSVC), Linux
      (Clang, ASan/UBSan) ve `policy` job'larının üçü de yeşil. CI'daki clang-tidy adımı **6 bulgu**
      verdi = baseline 6, yani gate "yeni bulgu yok" kuralını tutuyor; `continue-on-error` S8'e kadar
      advisory kalıyor.

# FlowGet: Çalışma Kuralları (AGENTS.md)

C++ çekirdekli, Flutter arayüzlü, Windows + Android hedefli download manager. MIT lisanslı, açık kaynak.
Bu dosya her oturumun başında okunur. Kısa tutulur; ayrıntı `tasks/` ve `docs/` altındadır.

## Oturum başında
1. `tasks/todo.md` oku: hangi milestone/dilimdeyiz, hangi madde açık.
2. `tasks/lessons.md` oku: aynı hatayı tekrar etme.
3. Göreve ilgili `docs/adr/*` dosyalarına bak. Kararlarla çelişen bir şey gerekiyorsa dur ve sor.

## Plan ve onay
- Kapsamlı iş (3+ adım veya mimari karar) için önce plan yaz: `tasks/todo.md`'ye işaretlenebilir maddelerle.
- Planı göster, **onay bekle**, sonra kodla.
- İşler ters giderse (beklenmedik hata, varsayım çöktü) körlemesine devam etme: dur, yeniden planla.
- Basit ve net düzeltmelerde plan gerekmez.

## Bitti demeden önce kanıt
- Build + `ctest` çalıştır, çıktıyı göster. Kanıtsız "tamam" deme.
- Maddeyi `todo.md`'de ancak kanıtıyla (test çıktısı, ölçüm) işaretle.
- Linux derlemesi de yerelde doğrulanır: `scripts/check_linux.ps1` (WSL2 Ubuntu; debug + ASan/UBSan).
  "Doğrulanamadı" yazmak yerine betiğin sonucunu raporla. Yalnızca betiği çalıştıramadığın bir
  durum (WSL yok, klon yok) varsa bunu gerekçesiyle açıkça yaz; CI doğrulayacak.
- Kendine sor: kıdemli bir mühendis bu değişikliği onaylar mıydı?

## Kod kuralları
- `-Werror` açık. **Uyarıyı susturma, kök nedeni düzelt.**
- Kod hem **Windows (MSVC)** hem **Linux (Clang, ASan/UBSan)** üzerinde derlenmeli. Sık tuzaklar:
  `send/recv` dönüş ve uzunluk tipleri, `closesocket`/`close`, `NOMINMAX`, `MSG_NOSIGNAL`, işaretli/işaretsiz dönüşümler.
- Proje yolu ve araç zincirinin ürettiği yollar **ASCII** olmalı (`C:\dev\flowget`).
- `clang-format` temiz olmalı. Değişiklik küçük ve odaklı olsun; gereksiz yeniden yazma yok.
- GPL/AGPL bağımlılık yasak (ADR-0004). DRM kırma kapsam dışı.
- Zarafet dengesi: büyük değişiklikte "daha sade yol var mı?" diye dur; basit düzeltmede aşırı mühendislik yapma.
- Geçici çözüm yok; kök nedeni bul.

## Hata ve ders
- Bir hata düzeltince veya kullanıcı seni düzeltince `tasks/lessons.md`'ye ekle: belirti, **kök neden**, düzeltme, tekrarını önleyen kontrol.
- Hata bildirimi (log, CI çıktısı, başarısız test) geldiğinde el tutulmasını bekleme: logdan kök nedeni bul ve düzelt.

## Windows araçları
- Python için **`py`** komutunu kullan. `python` Windows Store kısayoluna takılabilir.
  Örnekler: `py -m pip install ...`, `py scripts\check_dependencies.py`.
- pre-commit: **`git add -A` yaptıktan SONRA** `py -m pre_commit run --all-files` çalıştır (CI aynı
  kontrolü yapar). Sıra önemli: pre-commit sahnelenmemiş değişiklikleri stash'leyip HEAD'i denetler;
  önce koşarsa kendi değişikliklerini görmeden "Passed" der (`tasks/lessons.md`, 2026-11-16).
  `pre-commit.exe` PATH'te olmayabilir; modül olarak çalıştır.
- Linux doğrulaması: **commit'ten önce** `.\scripts\check_linux.ps1` çalıştır. Betik `C:\dev\flowget`
  içeriğini WSL2'deki `~/flowget` klonuna rsync'ler (`build/`, `.git/` hariç) ve `linux-x64` (debug)
  ile `linux-x64-asan` (ASan/UBSan) presetlerinde configure + build + `ctest` koşar. Linux derlemesi
  yerelde doğrulanır; "doğrulanamadı" demek yerine sonucu raporla.
  Gereksinim: WSL2 Ubuntu, `~/vcpkg` klonu (`FLOWGET_VCPKG_ROOT`) ve `cmake/clang/ninja/rsync`.
- vcpkg: `FLOWGET_VCPKG_ROOT=C:\Users\Enes\vcpkg` tanımlıdır. Visual Studio'nun gömülü vcpkg'ını kullanma.
- Yeni terminal/OpenCode oturumu açmadan ortam değişkeni değişiklikleri görünmez.

## Git
- Conventional commits (`feat(core): ...`, `fix(build): ...`, `docs: ...`). Küçük, mantıklı commit'ler.
- **Commit'le ama `git push` yapma.** Push'u kullanıcı yapar.
- **Push'lanmış commit'i asla amend etme / force push yapma.** Düzeltme gerekiyorsa **yeni commit** at.
  (2026-11-16: push sonrası amend, `origin/main` ile ayrık geçmiş üretti; gereksiz merge commit'i
  `6437b8a` doğdu.)
- Git kimliğini ve yapılandırmasını değiştirme.
- Proje klasörü dışındaki dosyalara dokunma.

## Alt ajanlar ve kaynak kullanımı
- Araştırma/keşif gerçekten gerekiyorsa alt ajan kullan (görev başına bir tane). Gereksiz yere çoğaltma: model limitleri ve bağlam sınırlı.
- Büyük çıktıları (log, uzun dosya) bağlama olduğu gibi yığma; özetle veya filtrele.

## İletişim
- Kullanıcıya **Türkçe** yanıt ver. Kullanıcı yeni başlayan biri: komutları tam yaz, terimleri kısaca açıkla.
- Her adımın sonunda kısa özet ver: ne yapıldı, kanıt, sıradaki adım.

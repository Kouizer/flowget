# Arşiv — S1a: vcpkg manifesti, toolchain, CI

> `tasks/todo.md`'den **olduğu gibi** taşındı (2026-11-22). Bu dosya tarihsel kayıttır;
> güncel durum ve sıradaki iş için `tasks/todo.md` indeksine bak.

**S1a kanıtı (2026-11-14):** vcpkg `2026-10-29` / baseline `4702e38`; `curl 8.23.0 (schannel+nghttp2)`,
`OpenSSL 3.6.5`, `SQLite 3.54.1`, `spdlog 1.17.0` `flowget-cli --version` çıktısında;
`ctest` 1/1 passed; `check_dependencies.py` temiz manifestte `exit=0`, GPL fixture'da `exit=1`
(`libx264`, `some-agpl-package` yakalandı).

**S1a/CI düzeltmesi (2026-11-15):** Run #5'te (commit `51e7699`) Linux `Configure (debug)` düştü:
`actions/checkout` vcpkg'ı sığ (depth 1) klonladığı için `builtin-baseline` commit'i (`4702e38`)
klonda yoktu; master ucu `8f363bd`'ye ilerlemişti. Düzeltme: `scripts/fetch_vcpkg_baseline.py`
(vcpkg.json'dan SHA okur, `git fetch --depth=1 origin <sha>`, baseline dosyasının okunabildiğini
doğrular, `GITHUB_OUTPUT`'a `baseline=` yazar) iki job'da bootstrap'ten önce koşuyor; binary cache
anahtarı `vcpkg-<os>-<baseline>-<manifest>`; Linux toolchain adımı `ninja --version` ile üreteci
doğruluyor. Windows job'u run #5'te yeşildi ama aynı yapılandırma hatasını taşıyordu (aynı adım
ondaki de). Yerel kanıt: sığ klonda hata yeniden üretildi, script sonrası baseline okunabildi;
ikinci koşu idempotent (`already present`), üç hatalı girdi `exit=1`. Yerel build/`ctest`: configure +
build (24 hedef) + `ctest` 2/2 passed, `pre_commit run --all-files` temiz. CI yeşili push sonrası
doğrulandı: run #6, commit `8aaf255` — üç job da yeşil.

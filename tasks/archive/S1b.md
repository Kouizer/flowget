# Arşiv — S1b: HttpClient, tek bağlantılı indirme

> `tasks/todo.md`'den **olduğu gibi** taşındı (2026-11-22). Bu dosya tarihsel kayıttır;
> güncel durum ve sıradaki iş için `tasks/todo.md` indeksine bak.

**S1b kanıtı (2026-11-14):** `flowget-cli get <URL> -o <dosya>` tek bağlantıyla indiriyor; yerel test
sunucusuna karşı 6 entegrasyon testi yeşil (`ctest` 2/2): gövde birebir yazılıyor (1 MiB), 404'te
dosya bırakmıyor, `chunked` (Content-Length yok) doğru, 302 takip ediliyor, iptal `aborted` +
kısmi dosya siliniyor, kopan bağlantı tespit ediliyor. CLI e2e: 5 MiB dosya indirildi, kaynak/hedef
SHA-256 aynı (`B26D7E22…`), 404 → `exit=1`, kısmi dosya yok.

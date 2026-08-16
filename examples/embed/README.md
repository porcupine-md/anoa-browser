# Menyematkan live view anoa

Halaman contoh yang menaruh browser anoa di dalam `<iframe>` dan mengemudikannya
dari aplikasi lain.

## Menjalankannya

Dua proses: anoa, dan sebuah server statis untuk halaman contoh ini. Keduanya
harus punya origin yang berbeda — itulah kasus yang sebenarnya ingin diuji.

```bash
# 1. anoa, dengan origin halaman contoh diizinkan mem-frame live view
./anoa --headless --port 9222 --embed-origin http://127.0.0.1:8080

# 2. sajikan halaman contoh (terminal lain)
cd examples/embed && python3 -m http.server 8080 --bind 127.0.0.1
```

Lalu buka <http://127.0.0.1:8080/>.

Tanpa `--embed-origin`, frame-nya ditolak dan browser menampilkan ikon dokumen
rusak — bukan karena rusak, tapi karena defaultnya memang same-origin saja. Lihat
"Siapa yang boleh menyematkannya" di bawah.

## Yang ditunjukkan

- **`?embed=1`** membuang bilah tab dan bilah URL bawaan viewer, menyisakan
  tampilannya saja, supaya aplikasi Anda menggambar UI-nya sendiri.
- **`?tab=<id>`** memilih tab mana yang dikemudikan frame itu. Dua frame dengan
  dua id mengemudikan dua tab sekaligus.
- **`/json/list`** memberi daftar tab beserta `anoaTabId`, `anoaTabName`,
  `anoaActive`, judul dan URL-nya — cukup untuk menggambar bilah tab sendiri.
- **`POST /render/tab/new`** membuka tab dan mengembalikan `{"id":"t3"}`.

Interaksi tetikus dan papan ketik ditangani viewer di dalam frame; aplikasi
tuan rumah tidak perlu meneruskan apa pun.

## Beberapa hal yang mudah terlewat

**Klik sekali sebelum mengetik.** Peristiwa papan ketik hanya sampai ke frame
yang sedang fokus, dan sebuah iframe selalu mulai tanpa fokus. Viewer
menampilkan pil kecil di pojok selama itu belum terjadi.

**Token.** Kalau anoa dijalankan dengan `--auth-token`, isi konstanta `TOKEN` di
`index.html`. Token itu tinggal di URL frame; halaman viewer disajikan apa
adanya dan tidak pernah memuat token milik server di dalam badannya.

**Siapa yang boleh menyematkannya.** Live view meneruskan ketikan, bukan sekadar
menampilkan piksel — halaman yang bisa mem-frame-nya bisa menonton sesi yang
sudah login dan bertindak di dalamnya. Karena itu defaultnya
`Content-Security-Policy: frame-ancestors 'self'`, dan `--embed-origin` dipakai
untuk melebarkannya (boleh diulang; `'*'` mencabutnya sama sekali).

Perlu dicatat bahwa endpoint `/render/*` sendiri tidak punya pemeriksaan origin,
dan `--auth-token` mati secara bawaan. Di mesin bersama, pasang token.

#!/usr/bin/env bash
# Regenerates the archive fixtures used by tests/archives.rs. Needs: 7z, rar, zstd, lz4, gcab,
# ar (binutils), python3, gzip, bzip2, xz. The test tree is tiny and deterministic.
set -euo pipefail
cd "$(dirname "$0")"
rm -rf tree out && mkdir -p tree/src/lib tree/data tree/empty out
printf 'hello archive\n' > tree/readme.txt
printf 'fn main() {}\n' > tree/src/main.rs
printf 'pub fn util() {}\n' > tree/src/lib/util.rs
python3 -c "open('tree/data/blob.bin','wb').write(bytes(i%251 for i in range(1000)))"
find tree -exec touch -d '2021-03-04 05:06:08' {} +
printf 'hello single\n' > hello.txt; touch -d '2021-03-04 05:06:08' hello.txt
cp hello.txt out/ >/dev/null 2>&1 || true
(cd tree
 python3 - <<'PY'
import zipfile, os
with zipfile.ZipFile('../sample.zip','w',zipfile.ZIP_DEFLATED) as z:
    for root,dirs,files in os.walk('.'):
        for d in dirs:
            i=zipfile.ZipInfo(os.path.join(root,d)[2:]+'/',(2021,3,4,5,6,8)); z.writestr(i,b'')
        for f in files:
            p=os.path.join(root,f); i=zipfile.ZipInfo(p[2:],(2021,3,4,5,6,8)); i.compress_type=zipfile.ZIP_DEFLATED; z.writestr(i,open(p,'rb').read())
PY
 tar --sort=name --mtime='2021-03-04 05:06:08' -cf ../sample.tar .
 7z a -bd -y ../sample.7z . >/dev/null
 rar a -r -inul ../sample.rar . >/dev/null
)
gzip -kf -n sample.tar
cp sample.tar.gz sample.tgz
bzip2 -kf sample.tar && xz -kf sample.tar && zstd -qf sample.tar -o sample.tar.zst && lz4 -qf sample.tar sample.tar.lz4
gzip -kf -n hello.txt; bzip2 -kf hello.txt; xz -kf hello.txt; zstd -qf hello.txt -o hello.txt.zst
(cd tree && gcab -c -z "$PWD/../sample.cab" $(find . -type f | sed "s|^\./||"))
# .deb is an ar of tarballs
mkdir -p deb && (cd deb && printf '2.0\n' > debian-binary && tar -czf control.tar.gz --files-from /dev/null && cp ../sample.tar.gz data.tar.gz && ar rc ../sample.deb debian-binary control.tar.gz data.tar.gz)
# nested: outer.zip contains inner.7z (which holds the sample tree)
mkdir -p nest && cp sample.7z nest/inner.7z && (cd nest && python3 -c "
import zipfile; z=zipfile.ZipFile('../outer.zip','w'); z.write('inner.7z'); z.writestr('top.txt','top\n')")
# hostile names
python3 - <<'PY'
import zipfile
with zipfile.ZipFile('evil.zip','w') as z:
    for n in ['../evil.txt','/abs.txt','a/../../b.txt','ok.txt','sub\\\\win.txt']:
        z.writestr(n,'x')
PY
rm -rf tree nest deb out hello.txt
ls -la

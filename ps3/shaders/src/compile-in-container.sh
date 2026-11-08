set -e
apt-get update -qq >/dev/null && apt-get install -y -qq g++ make >/dev/null
cp libCg.so /usr/lib/ && ldconfig
cd cgcomp && make >/dev/null 2>&1 || make 2>&1 | tail -20
cd ..
for f in *.vcg; do ./cgcomp/cgcomp -v "$f" "${f%.vcg}.vpo"; done
for f in *.fcg; do ./cgcomp/cgcomp -f "$f" "${f%.fcg}.fpo"; done
ls -la *.vpo *.fpo

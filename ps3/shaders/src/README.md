Cg sources for the precompiled shaders in `..`. To rebuild after editing (needs Docker, and
NVIDIA's Cg 3.1 `libCg.so` from https://developer.download.nvidia.com/cg/Cg_3.1/Cg-3.1_April2012_x86_64.tgz):
put `libCg.so`, a copy of PSL1GHT's `tools/cgcomp` and these files in one folder, run
`docker run --rm -v "$PWD":/w -w /w debian:12 sh compile-in-container.sh`, and copy the
resulting `.vpo`/`.fpo` files into `ps3/shaders/`.

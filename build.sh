#!/bin/bash
set -e

mkdir -p build/

# Toolchain defaults (override by exporting before running)
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"
export PICO_SDK_PATH=${PICO_SDK_PATH:-$HOME/.pico-sdk/sdk/1.5.0}
export PICO_TOOLCHAIN_PATH=${PICO_TOOLCHAIN_PATH:-$HOME/.pico-sdk/toolchain/12_3_Rel1}
# pico-sdk 1.5.0 sub-projects (pioasm) predate CMake 4's policy floor
export CMAKE_POLICY_VERSION_MINIMUM=3.5

cd gba-link-connection/examples/LinkSPI_demo/
make rebuild
cp LinkSPI_demo.mb.gba ../../../build/

cd ../../../build/
# xxd -i equivalent (xxd is not always installed)
python3 - <<'EOF'
data = open("LinkSPI_demo.mb.gba", "rb").read()
with open("gba_rom.hpp", "w") as f:
    f.write("#pragma once\ninline constexpr \nunsigned char LinkSPI_demo_mb_gba[] = {\n")
    for i in range(0, len(data), 12):
        f.write("  " + ", ".join("0x%02x" % b for b in data[i:i+12]) + ",\n")
    f.write("};\ninline constexpr unsigned int LinkSPI_demo_mb_gba_len = %d;\n" % len(data))
EOF

cd ../GP2040-CE/
mkdir -p build/
cd build/
cmake ../
make -j$(nproc)
cp GP2040-CE_0.7.1_Pico.uf2 ../../build/gblink-gamepad.uf2
echo
echo "Built build/gblink-gamepad.uf2"

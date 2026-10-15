# Building and running on Linux

Tested on **Pop!_OS 22.04** with **GCC 14**, MiniUPnPc 2.2.6, Vulkan-Hpp 1.3.290, and Protobuf 29.0.

## Prerequisites

### System packages

```bash
sudo apt update
sudo apt install \
    build-essential cmake ninja-build pkg-config \
    libvulkan-dev mesa-vulkan-drivers \
    libminiupnpc-dev \
    libxbyak-dev \
    python3 python3-pip \
    git curl wget
```

### GCC 14 (required, GCC 11/12 fail on C++23)

```bash
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt update
sudo apt install gcc-14 g++-14
```

### Protobuf (must be built from source with `-fPIC`)

The system Protobuf (3.21) lacks the CMake config files and is not built as PIC, which causes linker errors when building shared libraries.

```bash
git clone -b v29.0 https://github.com/protocolbuffers/protobuf.git
cd protobuf
git submodule update --init --recursive
cmake -S . -B build \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_BUILD_TYPE=Release \
    -Dprotobuf_BUILD_TESTS=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build build --parallel $(nproc)
sudo cmake --install build
sudo ldconfig
cd ..
```

## Building

```bash
cd ~/uncharted_pc
export CC=gcc-14
export CXX=g++-14
bash build.sh
```

## Running

```bash
export BB_GAME_DIR="/path/to/CUSA02320"   # the folder with eboot.bin
./run.sh
```

Optional environment variables:

| Variable | Default | Description |
|---|---|---|
| `BB_EXE` | `u1` | `u1`, `u2`, `u3`, or a raw executable name |
| `BB_USER_DIR` | `./user` | Where saves and shader cache live |
| `BB_TIMEOUT` | `0` | Watchdog timeout in seconds (0 = infinite) |
| `BB_VBLANK_HZ` | `60` | VBlank frequency |
| `BB_CONTENT_SKU` | `full` | `full` or `trial` |

## Game data extraction

The game files must be extracted and decrypted. The community uses `ps4-pkg-tools` for the PKG and `psarcy` for `.psarc` files.

```bash
# Extract the PKG
git clone https://github.com/xXJSONDeruloXx/ps4-pkg-tools.git
cd ps4-pkg-tools && mkdir build && cd build && cmake .. && make -j$(nproc)
./ps4-pkg-tools /path/to/UP9000-CUSA02320_00-UNCHARTEDTRILOGY-A0100-V0100.pkg /path/to/output

# Extract PSARCs (optional but recommended)
cargo install psarcy
cd /path/to/output/CUSA02320/u1data/build/main
psarcy -e core.psarc && mv core/* . && rmdir core
psarcy -e bin.psarc  && mv bin/* .  && rmdir bin
psarcy -e uiextra.psarc && mv uiextra/* . && rmdir uiextra
```

## Known issues

- The emulator reports `watchdog timeout` a few seconds into gameplay because `.pak` mounting (`pak19/ps4/*.pak`) is incomplete. This is orthogonal to the build fixes.
- The FSR 3 upscaler requires the shader hashes set by `run.sh` (`BB_VELOCITY_PS`, `BB_UPSCALE_BEFORE_CS`, `BB_HUDLESS_PS`). Without them, the upscaler falls back to "not run".

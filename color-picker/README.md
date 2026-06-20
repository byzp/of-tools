# of-color-picker

Sniffs `OutfitColorantSelectRsp` from game traffic (TCP ports 11001–11003), then
scans `uvy 0.000–1.000` to find the closest dye-color match for any target color
you pick. A frameless, always-on-top overlay shows the closest match (hex,
similarity, uvy, slot), marks the best of the five dye colors, and positions a
uvy guide line.

This repo ships **two implementations of the same tool**:

| Folder | Implementation |
|--------|----------------|
| [`python/`](python/) | Original — PyQt5 + scapy + numba |
| [`cpp/`](cpp/)       | Rewrite — Win32/GDI+, single self-contained `picker.exe`, no Python runtime |

Both behave the same: same protocol framing, same `msg_id=1652` parsing, the same
swirl-noise algorithm (the C++ port is **bit-identical** to the numba version),
the same CIE76 search, and the same overlay layout.

```
color-picker/
├─ resources/      shared textures (swirlnoisetexture/1..4.png)
├─ net.proto       shared protocol schema
├─ python/         main.py, algo.py, ui.py
└─ cpp/            *.cpp / *.hpp, build.bat, CMakeLists.txt
```

## Differences

| Aspect | `python/` (original) | `cpp/` (rewrite) |
|---|---|---|
| GUI toolkit | PyQt5 (Qt widgets) | Win32 layered window + GDI+ (per-pixel-alpha overlay) |
| Color dialog | Qt's built-in `QColorDialog` | **custom GDI reimplementation** matching the Qt one (Win32's native `ChooseColor` looked different, so it was rebuilt) |
| Packet capture | `scapy` + npcap; `psutil` for the interface | `wpcap.dll` loaded dynamically at runtime — **no npcap SDK needed** |
| Protobuf | generated `net_pb2` (the `protobuf` package) | hand-written minimal wire parser (`pb.hpp`) — no protobuf dependency |
| Snappy | `python-snappy` | hand-written raw-format decompressor (`snappy_raw.hpp`) |
| PNG decode | Pillow | vendored `stb_image.h` |
| Algorithm | numba JIT (`@njit`) over numpy | plain C++ (`float`/`/O2`); identical float32 path, **bit-identical output** |
| Search speed | numba-accelerated | texture cache + sRGB LUT + target-Lab-once + parallel uvy scan + a single coalescing worker (~1.5 ms/search) |
| Dependencies | numpy, pillow, scapy, psutil, python-snappy, protobuf, numba | none beyond MSVC + Windows SDK to build; npcap runtime to run |
| Distribution | Python source (or PyInstaller bundle) | one `picker.exe` |

Minor visual divergences in the rewrite: font anti-aliasing can differ by a
sub-pixel from Qt, and the color dialog is a re-creation (not Qt itself).

## Shared prerequisites

- **npcap** (both versions capture with it): https://npcap.com/dist/npcap-1.87.exe
- **textures** — download and extract so that
  `resources/swirlnoisetexture/{1..4}.png` exist at the repo root:
  https://github.com/byzp/of-ps/releases/download/v2.1/resources.zip

> Capture gotcha: connection interceptors such as **Proxifier** prevent npcap
> from seeing the game traffic. If no dye data is captured, close them.

## Running the Python version

```
pip install numpy pillow scapy psutil python-snappy protobuf numba
# compile the schema into python/ (needs protoc)
protoc --python_out=python net.proto
python python/main.py
```

## Running the C++ version

Requirements: **Visual Studio** (Desktop C++ workload) + the **Windows 10/11
SDK**. From a *x64 Native Tools Command Prompt for VS*:

```
cd cpp
build.bat            ::  or:  cmake -B build -A x64 && cmake --build build --config Release
picker.exe
```

`picker.exe --selftest` runs offline checks (snappy vectors + a 7-target search
regression) without the game.

Both resolve the shared `resources/` automatically (one level up from their
folder). Open the outfit dye UI in-game and select a palette to trigger the
`OutfitColorantSelectRsp` packet, then pick a target color in the overlay.

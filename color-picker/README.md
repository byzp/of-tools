# of-color-picker

Sniffs `OutfitColorantSelectRsp` from game traffic (ports 11001–11003),
then scans `uvy 0.000–1.000` to find the closest color match for any
target hex you type.

```
pip install numpy pillow scapy psutil python-snappy protobuf
```

### how to use

1. download and install [npcap](https://npcap.com/dist/npcap-1.87.exe)

2. Download the necessary [texture resources](https://github.com/byzp/of-ps/releases/download/v2.1/resources.zip) for the colorist and extract them to the current directory

3. Download [net.proto](https://github.com/byzp/of-ps/blob/main/proto/net.proto), compile proto, and then run(You can also download the packaged program from the release)

The sniffer starts automatically. Open the outfit dye UI in-game and
select a palette — that triggers the server to send
`OutfitColorantSelectRsp` (msg_id=1652) containing 64 swirl params
and a `picture_id` (texture index 1–4).

Once captured, type any hex color on stdin:

```
> d6aa00
  d6aa00 -> #d6aa00  sim=1.0  uvy=1.0  slot=1
```

- Type another color → immediately re-searches with the same params.
- Receive a new packet → re-searches for the last-typed color with
  the new params.
- Empty line = ignored. `Ctrl+C` to quit.

**npcap** is required on Windows for packet capture:
https://npcap.com/dist/npcap-1.87.exe

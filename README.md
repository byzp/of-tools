# of-tools

Overfield utility tools.

**Prerequisite**: [npcap](https://npcap.com/dist/npcap-1.87.exe) on Windows.

## Tools

### translator

Overlay chat translator with Google Translate / AI support.

```
cd translator
pip install -r requirements.txt
python main.py
```

### color-picker

Dye color matcher — given a target hex, finds the closest dye result
by scanning the full uvy range.

```
cd color-picker
pip install numpy pillow scapy psutil python-snappy protobuf
python picker.py
```

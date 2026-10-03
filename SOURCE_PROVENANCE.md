# Source provenance — Release Final Clean

## Exact source pins

```text
Open SIMH   87eb7d5e96f9ce0ee6ac183e20160e5c486b0712
pdp7-unix   555eb30fc76b8fa29095d32eca9a43e9b1638288
M5Unified   4fb444784c85791e0b0207701392b42be234b2e7
M5GFX       729297d6e3d657ddc1ec5189bac2f2ea68828085
ESP-IDF     5.5.4
```

## Release Clean source fingerprints

```text
third_party/simh/PDP18B/pdp18b_cpu.c
dc8f20aa6e3faa414ca43368f6a040486301c138406165c6e94859ab289bf427

third_party/simh/PDP18B/pdp18b_g2tty.c
c132a389fb79fbf541fd612c32122ca1094dec5d8bd3b530031cd0f6675bb469

src/main.cpp
671a2d4194613af5a97006fc098edc01258ac26d33934351979bdc0d1cbb026c
```

The GRAPHIC-II source uses the pinned Open SIMH `GRAPHICS2` guard. An older historical `lars/graphics2` snapshot differs at that guard and is not the Release Final source.

The accepted runtime state includes UNIX V0 boot, RB09 persistence, local GRAPHIC-II, USB/A164 input, simultaneous Wi-Fi remote TTY, corrected Space Travel, the B compiler Hello World path, and Release Final Clean without the investigation/timing traces.

# Building simhP4 — PDP-7 UNIX V0

This repository contains the **Release Final Clean** source tree.

## Requirements

- M5Stack Tab5 / ESP32-P4
- ESP-IDF 5.5.4
- Python 3
- Git with submodule support
- GNU make and a host C toolchain for building the pinned PDP-7 UNIX reconstruction tools

## Clone

```sh
git clone --recurse-submodules https://github.com/Layer812/simhP4.git
cd simhP4
```

Pinned sources:

```text
M5Unified   4fb444784c85791e0b0207701392b42be234b2e7
M5GFX       729297d6e3d657ddc1ec5189bac2f2ea68828085
pdp7-unix   555eb30fc76b8fa29095d32eca9a43e9b1638288
Open SIMH   87eb7d5e96f9ce0ee6ac183e20160e5c486b0712
ESP-IDF     5.5.4
```

The Open SIMH subset required by the embedded PDP-7 build is vendored under `third_party/simh`. The simhP4 changes to the PDP-7 CPU and GRAPHIC-II path are included there with upstream notices preserved.

## Prepare UNIX V0 assets

The generated disk image is not duplicated in Git. Build it from the pinned `pdp7-unix` submodule:

```sh
make -C third_party/pdp7-unix
python tools/prepare_assets.py
```

The preparation script accepts only:

```text
image.fs  0892392eb8de98db5a0ac5862fb836ab171f9bc7bd6c460e553bab1f1407f6ed
boot.rim  a69adf03a700058300501b2e4a74e732b344fd175e727c9a87c7c7b7132bd4f2
```

At first boot simhP4 applies guarded Space Travel and B-tools migrations to the SD-card copy. It does not blindly replace an existing disk.

## Build

Activate ESP-IDF 5.5.4, then:

```sh
idf.py build
```

The development machine used `G:\simhP4` and `COM9`. After a successful build, an app-only update can be flashed with:

```sh
idf.py -p COM9 app-flash
```

A first-time device can use the normal full ESP-IDF flash procedure.

## Release Clean invariants

The public tree preserves GRAPHIC-II, the R17 packed-text fix, held A164 controls, the R19 diff presenter, R19C graphics cadence, RB09 persistence, USB/local input, Wi-Fi remote TTY, Space Travel, and the B compiler environment.

Investigation-only R14 traces and R19B timing observers are removed.

---

# 日本語

このリポジトリには、PDP-7 UNIX V0版 simhP4 の **Release Final Clean** ソースを収録しています。

まずsubmoduleを含めて取得します。

```sh
git clone --recurse-submodules https://github.com/Layer812/simhP4.git
cd simhP4
```

固定した `pdp7-unix` から初期diskとboot RIMを生成します。

```sh
make -C third_party/pdp7-unix
python tools/prepare_assets.py
```

`prepare_assets.py` はReleaseで使用した既知SHA256に一致する場合だけassetを配置します。

ESP-IDF 5.5.4を有効にしたあと、

```sh
idf.py build
```

でビルドできます。開発時のportはCOM9でした。

Release CleanではR14系の調査traceとR19B timing observerだけを撤去し、GRAPHIC-II、Space Travel、B環境、RB09永続化、USB/A164入力、Wi-Fi remote TTYを維持しています。

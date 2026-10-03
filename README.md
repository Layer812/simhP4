# simhP4 — PDP-7 UNIX V0

![simhP4 PDP-7 UNIX V0](title.png)

A portable PDP-7 UNIX V0 environment for **M5Stack Tab5 / ESP32-P4**, built on Open SIMH and the reconstructed PDP-7 UNIX sources.

### Highlights

- PDP-7 UNIX V0 running locally on M5Stack Tab5
- Bell Labs GRAPHIC-II display support
- USB / A164 keyboard input
- Persistent UNIX disk on SD card
- **Wi-Fi remote TTY: a second console can log in and be used at the same time as the local Tab5 console**
- Restored **Space Travel (`st`)** and **`psych`**
- Working B compiler toolchain, including the `hello.b` → `Hello, World!` path

### Install with M5Burner

M5Burner share code:

```text
qz8IApmUlBDOpGBP
```

The repository is being prepared as the source/documentation home for this build.

### Wi-Fi remote console

simhP4 exposes an additional UNIX V0 TTY over Wi-Fi. The local Tab5 console remains active, so **you can log in from another computer or terminal at the same time and use it as a separate console**.

Default connection information:

```text
SSID:     SIMHP4-UNIXV0
Password: pdp7unix
Host:     192.168.4.1
TCP port: 10007
```

Connect a raw TCP terminal client to `192.168.4.1:10007`. This is a UNIX V0 TTY connection, not SSH.

## About UNIX V0

UNIX V0 is the earliest UNIX system reconstructed for the PDP-7, based on the work done at Bell Labs around 1969–1970.

It predates many things we now take for granted in UNIX: normal pathnames, a modern shell environment, standard C tools, and even some familiar directory conventions. Programs and files are accessed in a much more primitive way, which makes UNIX V0 both unusual and historically interesting to use.

This simhP4 build runs PDP-7 UNIX V0 on RetroP4 using Open SIMH, with local display, keyboard input, persistent disk storage, GRAPHIC-II support, and several original PDP-7 programs such as **Space Travel (`st`)** and **`psych`**.

> This is a historical computing environment.  
> Do not expect commands or file handling to behave exactly like modern UNIX.

---

## Quick start

After booting, log in at the prompt.

Example:

```text
login: dmr
password: dmr
```

The shell prompt is:

```text
@
```

Some user directories contain a link named `system`, which gives access to system commands.

---

## Common UNIX V0 commands

| Command | Purpose | Example |
|---|---|---|
| `ls` | List files in the current directory | `ls` |
| `cat` | Print one or more files | `cat b_readme` |
| `cp` | Copy a file | `cp file1 file2` |
| `rm` | Remove a file | `rm file` |
| `rn` | Rename a file | `rn old new` |
| `ln` | Create a link | `ln dd dmr .` |
| `chmod` | Change file mode | `chmod 17 file` |
| `chown` | Change file owner | `chown 14 file` |
| `stat` | Show file information | `stat file` |
| `date` | Show the date | `date` |
| `od` | Dump file contents | `od file` |
| `ed` | Text editor | `ed file` |
| `as` | PDP-7 assembler | `as ops.s bl.s hello.s bi.s` |
| `b` | B compiler | `b hello.b hello.s` |

### Important pathname note

UNIX V0 does **not** behave like modern UNIX path handling.

For example, do not assume that names such as:

```text
system/display
```

work as modern pathnames.

Many operations are performed relative to the current directory, and the filesystem layout is closer to a graph of directory links than to the later UNIX tree model.

---

## B compiler

A simple B program can be compiled and assembled like this:

```text
@ b hello.b hello.s
@ as ops.s bl.s hello.s bi.s
I
II
ops.s
bl.s
hello.s
bi.s
@ a.out
Hello, World!
```

The assembly order is important:

```text
ops.s
bl.s
hello.s
bi.s
```

`ops.s` contains opcode and system-call definitions used by the PDP-7 assembler.

Example `hello.b`:

```c
main $(
  write('He');
  write('ll');
  write('o,');
  write(' W');
  write('or');
  write('ld');
  write('!*n');
$)
```

---

## Space Travel

Run:

```text
@ st
```

`st` is the famous PDP-7 **Space Travel** program associated with the early development of UNIX.

Controls:

| Button | Key | Action |
|---:|---|---|
| 1 | `1` | Quit |
| 2 | `2` | New game / reset |
| 3 | `3` or ↓ | Thrust |
| 4 | `4` or ↑ | Reverse thrust |
| 5 | `5` or → | Rotate right |
| 6 | `6` or ← | Rotate left |
| 7 | `7` or `Z` | Scale up |
| 8 | `8` or `X` | Scale down |

The goal is to fly around the simulated solar system and land on planets without crashing.

`L` indicates a landing state.  
`CL` indicates a crash landing.

There is no conventional score-based ending; use button **1** to quit.

---

## psych

Run:

```text
@ psych
```

`psych` is an interactive GRAPHIC-II display program.

Controls:

| Button | Key | Action |
|---:|---|---|
| 1 | `1` | Decrease parameter 2 |
| 2 | `2` | Increase parameter 2 |
| 3 | `3` or ↓ | Decrease parameter 1 |
| 4 | `4` or ↑ | Increase parameter 1 |
| 5 | `5` or → | Increase display scale |
| 6 | `6` or ← | Decrease display scale |
| 7 | `7` or `Z` | Parameter input / AUTO mode |
| 8 | `8` or `X` | Quit |

After pressing **7 / Z**, entering:

```text
a
```

starts automatic parameter changes.

---

## Other interesting programs

```text
moo
p
ttt
psych
st
```

Some of these programs use the PDP-7 GRAPHIC-II display and may behave very differently from normal terminal programs.

---

## Credits

```text
simhP4 © 2026 simhP4 contributors
Open SIMH based
```

Open SIMH and other upstream components retain their original copyrights and licenses.

This repository keeps third-party license texts under [`LICENSES/`](LICENSES/). In particular:

- Open SIMH code remains under its upstream license; see [`LICENSES/Open-SIMH-LICENSE.txt`](LICENSES/Open-SIMH-LICENSE.txt).
- PDP-7 UNIX reconstruction materials from `DoctorWkt/pdp7-unix` retain their upstream GNU GPL v3 terms where applicable; see [`LICENSES/pdp7-unix-GPL-3.0.txt`](LICENSES/pdp7-unix-GPL-3.0.txt).
- simhP4 / RetroP4 integration code carries its own 2026 contributor copyright notice. No upstream copyright notice is replaced or removed.

See [`NOTICE.md`](NOTICE.md) and [`LICENSES/README.md`](LICENSES/README.md) for the attribution and license map.

---

# 日本語

## UNIX V0について

UNIX V0は、1969～1970年頃にBell LabsでPDP-7向けに作られた最初期UNIXを再構成した環境です。

現在のUNIXでは当たり前になっている、通常のパス名、整ったシェル環境、Cコンパイラ、一般的なディレクトリ構造などがまだ存在しない時代のシステムです。そのため操作方法は現代UNIXとはかなり異なりますが、UNIXがどのように始まったのかを実際に触って体験できる、とても興味深い環境です。

このsimhP4版では、RetroP4上でOpen SIMHを使ってPDP-7 UNIX V0を実行します。ローカル画面、キーボード入力、ディスク保存、GRAPHIC-II表示に対応し、**Space Travel (`st`)** や **`psych`** などのPDP-7プログラムも動作します。

> 歴史的なコンピュータ環境のため、  
> 現代UNIXと同じコマンド動作やファイル操作を期待しないでください。

### 主な機能

- M5Stack Tab5 / ESP32-P4上でPDP-7 UNIX V0を実行
- Bell Labs GRAPHIC-II表示
- USB / A164キーボード入力
- SDカード上のUNIXディスク永続化
- **Wi-Fi remote TTY。Tab5本体のローカルコンソールを使ったまま、別PCからもう1つのコンソールへ同時ログイン可能**
- **Space Travel (`st`)**、**`psych`**
- Bコンパイラ環境と `hello.b` → `Hello, World!` の実動経路

### M5Burner

シェアコード:

```text
qz8IApmUlBDOpGBP
```

### Wi-Fi別コンソール

simhP4はWi-Fi経由でもう1本のUNIX V0 TTYを提供します。**Tab5本体のローカル画面・キーボードを使用中でも、別のPCや端末から同時にログインして別コンソールとして利用できます。**

デフォルト接続情報:

```text
SSID:     SIMHP4-UNIXV0
Password: pdp7unix
Host:     192.168.4.1
TCP port: 10007
```

`192.168.4.1:10007`へraw TCP terminalで接続します。SSHではなく、UNIX V0のTTY接続です。

---

## 起動

起動後、loginします。

例:

```text
login: dmr
password: dmr
```

シェルのプロンプトは:

```text
@
```

です。

---

## UNIX V0の主なコマンド

| コマンド | 内容 | 例 |
|---|---|---|
| `ls` | 現在のディレクトリを表示 | `ls` |
| `cat` | ファイル内容を表示 | `cat b_readme` |
| `cp` | ファイルコピー | `cp file1 file2` |
| `rm` | ファイル削除 | `rm file` |
| `rn` | ファイル名変更 | `rn old new` |
| `ln` | リンク作成 | `ln dd dmr .` |
| `chmod` | mode変更 | `chmod 17 file` |
| `chown` | owner変更 | `chown 14 file` |
| `stat` | ファイル情報表示 | `stat file` |
| `date` | 日付表示 | `date` |
| `od` | ファイルダンプ | `od file` |
| `ed` | テキストエディタ | `ed file` |
| `as` | PDP-7アセンブラ | `as ops.s bl.s hello.s bi.s` |
| `b` | Bコンパイラ | `b hello.b hello.s` |

### パス名について

UNIX V0は現代UNIXのようなパス名を扱いません。

たとえば、

```text
system/display
```

のような指定が、そのまま現代UNIXのパスとして動くとは限りません。

多くの操作は現在のディレクトリを基準に行われます。ファイルシステムも、後のUNIXのような単純なtreeというより、directory linkによるgraphに近い構造です。

---

## Bコンパイラ

簡単なBプログラムは次のようにコンパイルできます。

```text
@ b hello.b hello.s
@ as ops.s bl.s hello.s bi.s
I
II
ops.s
bl.s
hello.s
bi.s
@ a.out
Hello, World!
```

assemblyの順番は重要です。

```text
ops.s
bl.s
hello.s
bi.s
```

`ops.s`にはPDP-7 assemblerが利用するopcodeとsystem call定義が入っています。

`hello.b`の例:

```c
main $(
  write('He');
  write('ll');
  write('o,');
  write(' W');
  write('or');
  write('ld');
  write('!*n');
$)
```

---

## Space Travel

起動:

```text
@ st
```

`st`は、初期UNIX開発と深く関係するPDP-7版 **Space Travel** です。

操作:

| ボタン | キー | 動作 |
|---:|---|---|
| 1 | `1` | 終了 |
| 2 | `2` | New game / reset |
| 3 | `3` または ↓ | 推進 |
| 4 | `4` または ↑ | 逆推進 |
| 5 | `5` または → | 右回転 |
| 6 | `6` または ← | 左回転 |
| 7 | `7` または `Z` | Scale up |
| 8 | `8` または `X` | Scale down |

太陽系を移動し、惑星への着陸を試みるゲームです。

`L` は着陸状態、  
`CL` はCrash Landingを示します。

一般的なscore制の終了条件はなく、**1**で終了します。

---

## psych

起動:

```text
@ psych
```

`psych`はGRAPHIC-IIを使うinteractive graphics programです。

操作:

| ボタン | キー | 動作 |
|---:|---|---|
| 1 | `1` | parameter 2を減少 |
| 2 | `2` | parameter 2を増加 |
| 3 | `3` または ↓ | parameter 1を減少 |
| 4 | `4` または ↑ | parameter 1を増加 |
| 5 | `5` または → | 表示scaleを拡大 |
| 6 | `6` または ← | 表示scaleを縮小 |
| 7 | `7` または `Z` | parameter入力 / AUTO |
| 8 | `8` または `X` | 終了 |

**7 / Z** を押したあと、

```text
a
```

を入力するとAUTO modeになります。

---

## その他のプログラム

```text
moo
p
ttt
psych
st
```

GRAPHIC-IIを使用するプログラムもあり、通常のterminal programとはかなり違う動作をします。

---

## Credits

```text
simhP4 © 2026 simhP4 contributors
Open SIMH based
```

Open SIMHなどのupstream componentのcopyrightとlicenseは、それぞれ元のものを維持します。

第三者ライセンス本文は [`LICENSES/`](LICENSES/) に分離して収録します。

- Open SIMH由来コード: [`LICENSES/Open-SIMH-LICENSE.txt`](LICENSES/Open-SIMH-LICENSE.txt)
- `DoctorWkt/pdp7-unix` 由来のPDP-7 UNIX再構成物: 該当部分はupstreamのGNU GPL v3条件を維持し、[`LICENSES/pdp7-unix-GPL-3.0.txt`](LICENSES/pdp7-unix-GPL-3.0.txt) を収録
- simhP4 / RetroP4の2026年統合部分: 独自のcontributor copyright noticeを維持

upstreamのcopyright noticeをsimhP4の表記で置き換えることはしません。詳細は [`NOTICE.md`](NOTICE.md) と [`LICENSES/README.md`](LICENSES/README.md) を参照してください。
# simhP4 Selector UI

Status: design baseline for `microvax-bsd43`

## Goal

Keep the existing simhP4 visual language: black CRT background, green neon text, thin glowing frames, and the same compact information density. Add a reusable pre-boot selector without changing the accepted PDP-7 UNIX V0 runtime path.

## Flow

```text
simhP4
  -> MACHINE SELECT
       PDP-7
       MicroVAX II
  -> OS SELECT
       (machine-specific OS list)
  -> BOOT
```

The user always selects a machine first, then an OS.

## Initial registry

```text
PDP-7
  UNIX V0

MicroVAX II
  4.3BSD
```

Future OS entries such as VMS, ULTRIX, NetBSD, etc. can be added without changing the selector layout.

## Screen 1: machine

Left/main list:

```text
simhP4

MACHINE SELECT

> PDP-7
  MicroVAX II
```

Right information pane shows a short description of the highlighted machine and the currently available OS count.

## Screen 2: OS

```text
simhP4 / MicroVAX II

OS SELECT

> 4.3BSD
```

The right pane shows a short OS description and media state.

## Input

- Up / Down: move selection
- Enter: select / continue
- Escape or Backspace: return to previous stage
- Touch: select row / continue when practical
- A164 and USB HID should use the same host-side selector event queue

## Input ownership

Do not send selector keystrokes directly into a guest device.

Introduce one host-side frontend input route:

```text
A164 / USB HID / touch
        |
        v
simhP4 frontend input
        |
        +-- selector active -> selector queue
        |
        +-- guest active    -> selected machine console/input bridge
```

For PDP-7 after boot, the existing GRAPHIC-II input route remains unchanged.

## Visual invariants

Reuse the existing simhP4 CRT helpers and palette from `src/main.cpp`:

- black/very dark green background
- neon green outer/mid/core text glow
- existing frame glow
- no new graphical theme
- no animated splash that delays boot

## Savepoint rule

The accepted PDP-7 Release Final path remains unchanged on `main`.

All selector/MicroVAX work is developed on:

```text
microvax-bsd43
```

before merge.

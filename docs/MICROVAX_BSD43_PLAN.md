# MicroVAX II + 4.3BSD bring-up plan

Branch: `microvax-bsd43`

## Target

First VAX machine:

```text
MicroVAX II
Open SIMH model: VAX_630
storage path: RQ / MSCP
OS: 4.3BSD
```

Open SIMH's MicroVAX II target is composed from the VAX630 source set and the PDP-11 RQ/MSCP controller implementation. Keep that upstream device split rather than inventing a new VAX machine model.

## Phase S0 — selector shell

- add machine/OS registry
- add host-side selector input queue
- draw selector using current simhP4 neon CRT style
- PDP-7 -> UNIX V0 remains the existing accepted boot path
- MicroVAX II -> 4.3BSD entry may initially stop at a controlled "not built yet" screen

No PDP-7 guest semantics change.

## Phase V0 — MicroVAX II compile probe

Bring in only the upstream source set needed for MicroVAX II:

- VAX CPU / MMU / FPA / CIS core
- VAX630 system/device glue
- standard console/timer devices
- RQ/MSCP disk controller

First acceptance target:

```text
compile
link
reset
known CPU/model identity
no OS media required
```

## Phase V1 — host portability layer

Move host-specific services behind simhP4 interfaces:

- guest console RX/TX
- monotonic time
- SD block/file access
- display/status frontend
- machine lifecycle

CPU1 remains guest execution authority. CPU0 remains host/UI/SD/Wi-Fi authority.

## Phase V2 — 4.3BSD media

Use an SD-backed VAX disk image. Do not embed a full 4.3BSD system disk in ESP flash.

Requirements:

- exact user-provided/legally sourced image identified by SHA256
- read-only evidence first
- determine required disk geometry/type and boot command from that exact image
- no guessing or destructive conversion
- original image preserved; working copy used for persistence

## Phase V3 — first BSD boot

Acceptance ladder:

1. MicroVAX II reset/vector path
2. console output
3. RQ/MSCP probe
4. boot block read
5. kernel banner
6. root device mount
7. single-user shell
8. multi-user login

Stop at the first deterministic failure and fix only the owning layer.

## Phase V4 — selector integration

Once 4.3BSD reaches a stable login:

```text
MACHINE SELECT
  PDP-7
> MicroVAX II

OS SELECT
> 4.3BSD

BOOT
```

Later OS additions remain data-driven.

## Non-goals for first bring-up

- no VAX graphics
- no broad VAX model matrix yet
- no speculative timing tuning
- no networking until console + storage boot is proven
- no changes to the PDP-7 Release Final savepoint

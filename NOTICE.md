# Notices and attribution

## simhP4 / RetroP4 integration

Copyright (c) 2026 simhP4 contributors

This notice applies to original simhP4 / RetroP4 integration code and simhP4-authored modifications created for the ESP32-P4 / M5Stack Tab5 port.

That original work is licensed under the MIT License. See `LICENSE` and `LICENSES/simhP4-MIT.txt`.

Upstream and reconstructed components retain their own copyright notices and license terms. Those notices must not be removed or replaced by the simhP4 copyright line, and the simhP4 MIT grant does not override them.

## Open SIMH

simhP4 uses Open SIMH-derived PDP-7/PDP18B simulation code. Open SIMH retains its upstream copyright notices and license terms.

The exact Open SIMH base used for this Release Final Clean source is recorded in `SOURCE_PROVENANCE.md`. The required embedded subset is vendored under `third_party/simh`, with upstream notices preserved.

License texts:

- `LICENSES/Open-SIMH-LICENSE.txt`
- `LICENSES/SIMH_LICENSE.txt`

Important upstream author attributions present in the PDP-7 / GRAPHIC-II path remain intact in the source files where they occur.

## PDP-7 UNIX reconstruction materials

The UNIX V0 filesystem, programs, historical reconstruction material, and related build inputs used by this project are based in part on `DoctorWkt/pdp7-unix`.

The repository pins that source as a Git submodule. Its upstream GNU GPL v3 terms and notices remain applicable to that material:

- `LICENSES/pdp7-unix-GPL-3.0.txt`

## M5Stack libraries

`M5Unified` and `M5GFX` are pinned as Git submodules. They retain their respective upstream copyright and license notices; simhP4 does not replace or relicense them.

## Trademarks and historical names

UNIX, Bell Labs, PDP-7, M5Stack, ESP32 and other product or organization names are used descriptively to identify historical systems, hardware, or compatible components. simhP4 is not presented as an official product of those organizations.

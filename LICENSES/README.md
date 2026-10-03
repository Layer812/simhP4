# License map

simhP4 combines original integration work with upstream and reconstructed components that use different licenses. Keep the license and attribution of each component with that component.

| Component / material | License / notice |
|---|---|
| simhP4 / RetroP4 original integration work | Copyright (c) 2026 simhP4 contributors; see source-file notices, `../LICENSE.md`, and `../NOTICE.md` |
| Open SIMH-derived simulator code | Open SIMH upstream license; see `Open-SIMH-LICENSE.txt` and `SIMH_LICENSE.txt` |
| `DoctorWkt/pdp7-unix` reconstruction source | Pinned Git submodule; upstream GNU GPL v3 terms; see `pdp7-unix-GPL-3.0.txt` |
| M5Unified | Pinned Git submodule; retains its upstream license and copyright notices |
| M5GFX | Pinned Git submodule; retains its upstream license and copyright notices |
| ESP-IDF / other SDK components | Not relicensed by simhP4; retain their own upstream terms |

These files do not replace copyright or license headers already present in upstream source files.

Exact dependency commits and Release Clean source fingerprints are recorded in [`../SOURCE_PROVENANCE.md`](../SOURCE_PROVENANCE.md).

## Project-level license

The third-party license files in this directory do **not** by themselves grant a new blanket license for original simhP4 integration code. Rights for that original code are those stated in the applicable source-file notices unless its copyright holders explicitly select an additional project-level license.

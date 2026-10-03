# Open SIMH subset used by simhP4

This directory contains the minimal Open SIMH source subset required by the ESP-IDF PDP-7 build.

Upstream base commit:

```text
87eb7d5e96f9ce0ee6ac183e20160e5c486b0712
```

Upstream copyright and license notices are preserved. See `LICENSES/SIMH_LICENSE.txt` and `LICENSES/Open-SIMH-LICENSE.txt`.

The embedded host modifies the PDP-7 CPU / GRAPHIC-II integration files required by simhP4. Release Final Clean removes the R14 investigation traces and R19B timing observers while preserving guest-visible GRAPHIC-II and input behavior.

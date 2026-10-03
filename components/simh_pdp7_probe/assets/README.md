# Generated UNIX V0 assets

`image.fs` and `boot.rim` are generated from the pinned `third_party/pdp7-unix` source and are intentionally not duplicated in Git.

```sh
make -C third_party/pdp7-unix
python tools/prepare_assets.py
```

The preparation script verifies the exact Release seed hashes before copying them here.

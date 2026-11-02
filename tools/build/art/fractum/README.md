# FractumMorrowind's HOME menu art

The fork's icon and banner: Fractum Halo's ring and moon (`fractum-halo.png`, from FractumSeraph/OpenCE's
`port/assets/icon`) in Morrowind's colours, the ring gold and the moon Bloodmoon's red.

- `make_fractum_art.py` writes the repository's `icon.png` (48 x 48, which the Makefile makes `mw3ds.smdh`'s icon
  from) and `banner.png` here (256 x 128).
- `banner.bnr` is the flat banner `tools/build/make_cia.py` packs; the HOME menu's sound in it is replaced when the
  CIA is packed (Morrowind's title music, from your Data Files or `MW3DS_BANNER_TUNE`). Made with
  [bannertool](https://github.com/Epicpkmn11/bannertool) (built from its source, commit 51dac92):

  ```sh
  bannertool makebanner -i tools/build/art/fractum/banner.png -a tools/build/banner.wav -o tools/build/art/fractum/banner.bnr
  ```

Upstream MW3DS's own art (the 3D banner, `tools/build/make_art.py`) stays in `tools/build`.

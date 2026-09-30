# Orbit menu runtime check

Uses the project's resolved LVGL, the production crest renderer and assets,
and the actual Noto fonts. Only ESP allocation and clock APIs are substituted.
Run after resolving the firmware's managed components:

```sh
cmake -S scripts/tests/orbit_menu_runtime -B /tmp/orbit-menu-runtime -G Ninja
cmake --build /tmp/orbit-menu-runtime -j 8
/tmp/orbit-menu-runtime/orbit_menu_runtime /tmp/orbit-menu-frames
```

The executable checks the empty timer remains still across a full breathing
period, a running timer animates and updates, removal restores the empty page,
hide clears the surface, and repeated K/V switches preserve List and Notes.
It exports PPM frames of all six pages for visual inspection. V uses the same
palette expansion as the board. Physical button, NVS, PSRAM and display-driver
acceptance still requires the ring. The separate `test_orbit_menu_theme.py`
checks production theme storage adapters and queued blue-long-press priorities.

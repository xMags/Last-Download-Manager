# Toolbar artwork

Resume, Stop, Stop All, Delete and Delete Completed were extracted from the installed
reference executable at the user's request.
Source SHA-256: `279ceed9aaa646650716ee326c0d996be4ce5db4b252cf5b2747f4938a7ce737`.
These assets are the reference artwork, not original LDM artwork.

Source bitmap resources: 495 (56 x 42 cells), 498 (84 x 63 cells), and 501
(32 x 32 cells), language 1033, cells 1 through 5 with zero-based indexing.
The square center of each cell preserves its original alpha channel. ICO files
contain 16, 20, 24, 32, 40, 48, 64 and 96-pixel frames, resampled from the nearest
source at least as large, or the largest available source. Disabled frames use
RGB(160,160,160) with the same alpha coverage as their normal counterparts.

Windows loads the embedded ICO resources at the toolbar's current DPI. No the reference
installation or external image files are required at runtime.

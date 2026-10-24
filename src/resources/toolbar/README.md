# Toolbar artwork

Icons for Resume, Stop, Stop All, Delete, Delete Completed, Options, Scheduler, Start Queue
and Stop Queue. Add URL is drawn at run time (`src/ui/GlyphIcons.cpp`). Text labels and queue
dropdown arrows are rendered by the native toolbar, separately from the artwork.

Each ICO file holds 16, 20, 24, 32, 40, 48, 64 and 96-pixel frames with an alpha channel.
Disabled frames use RGB(160,160,160) with the same alpha coverage as their normal
counterparts.

Windows loads the embedded ICO resources at the toolbar's current DPI; no external image
files are needed at run time.

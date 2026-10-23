#pragma once

#include <windows.h>

// Painting for the standard controls Windows draws wrongly in dark mode (checklist V-08): list
// headers keep dark text, group box captions and check box or radio labels take the theme's
// near-black text, tab controls stay light, and disabled static text is embossed for a light
// background. Theme::ApplyToControl attaches these; nothing else should need to.
namespace ldm::ui {

class Theme;

// Subclasses `control` when it is a kind listed above, so that it paints with `theme`'s colors
// while the theme is dark and exactly as Windows does while it is light. Attaching again is
// harmless. `theme` must outlive the control.
void AttachDarkPainting(HWND control, const Theme& theme);

}  // namespace ldm::ui

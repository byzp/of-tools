// A modal color picker that replicates the look & behavior of PyQt5's
// QColorDialog (basic/custom color grids, a hue-saturation 2D field + value bar,
// HSV/RGB spin fields, an HTML hex field, "add to custom colors", a screen-color
// picker, and OK/Cancel). Replaces the native Win32 ChooseColor so the dialog
// matches the original Python version.
#pragma once
#include <windows.h>

// Shows the dialog modally. Returns true and sets `out` if confirmed; false if
// cancelled. `custom` holds 16 persistent custom-color slots (updated in place).
bool choose_color_qt(HWND owner, COLORREF initial, COLORREF& out,
                     COLORREF custom[16]);

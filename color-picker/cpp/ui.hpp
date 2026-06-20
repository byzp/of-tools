// Floating picker window — Win32 layered window + GDI+, reproducing ui.py
// (frameless, translucent, always-on-top tool window with a uvy bar, guide-line
// overlay, five dye squares, a color-strip picker, and a result log).
#pragma once
#include <functional>
#include <string>

#include "search.hpp"

// Called when the user picks a target color; receives "#rrggbb".
using TargetChangedCb = std::function<void(const std::string&)>;

// Create and show the window, then run the message loop (blocks).
void ui_run(TargetChangedCb cb);

// Push a search result to the window (thread-safe; marshals to the UI thread).
void ui_post_result(const Result& r);

// Request the window to close (thread-safe). Used by --uitest.
void ui_close();

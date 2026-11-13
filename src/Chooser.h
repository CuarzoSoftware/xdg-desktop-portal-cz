#ifndef CHOOSER_H
#define CHOOSER_H

#include <Portal.h>

/**
 * @brief Picks what to cast.
 *
 * - One choice: chosen right away.
 * - Otherwise a chooser command, if set in ~/.config/xdg-desktop-portal-cz/config or available:
 *   `chooser_cmd` (or a menu tool: wofi, fuzzel, bemenu, wmenu) gets one line per choice on its
 *   input ("Monitor: <name> <description>" / "Window: <title> (<app id>)") and prints the chosen one.
 *   For screens only, `output_chooser_cmd` (default `slurp -f %o -or` if available) prints an
 *   output name.
 * - Without one: the first screen.
 *
 * The compositor may still ask the user before capturing (see LImageCaptureManager in Louvre).
 *
 * @return `false` if cancelled.
 */
bool ChooseTarget(UInt32 types, Target &target) noexcept;

#endif // CHOOSER_H

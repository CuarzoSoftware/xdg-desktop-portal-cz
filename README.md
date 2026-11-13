# xdg-desktop-portal-cz

The [xdg-desktop-portal](https://flatpak.github.io/xdg-desktop-portal/) backend of the Cuarzo desktop.

It currently implements **screen casting** (`org.freedesktop.impl.portal.ScreenCast`), used by browsers,
OBS, video calls, etc. to share screens and windows, through the capture protocols implemented by
[Louvre](https://github.com/CuarzoSoftware/Louvre):

- [ext-image-capture-source](https://wayland.app/protocols/ext-image-capture-source-v1): what to capture (screens and windows)
- [ext-image-copy-capture](https://wayland.app/protocols/ext-image-copy-capture-v1): the frames, with damage tracking
- [ext-foreign-toplevel-list](https://wayland.app/protocols/ext-foreign-toplevel-list-v1): the windows that can be shared

Frames are streamed with [PipeWire](https://pipewire.org/), as dma-bufs (zero copy) or shared memory, with
damage, timestamp and transform metadata. A frame is only produced when something changed on the source.

Every capture must also be accepted by the compositor (see `LImageCaptureManager` in Louvre), which may
ask the user first: `Start` is answered once it's accepted.

## Choosing what to share

- With a single screen (and no windows requested) it's shared right away.
- Otherwise a chooser command is used, configured in `~/.config/xdg-desktop-portal-cz/config`:

  ```ini
  # Screens only: prints the chosen output name (default: slurp -f %o -or, if installed)
  output_chooser_cmd=slurp -f %o -or

  # Screens and windows: a menu reading one choice per line on stdin and printing the chosen one
  # (default: wofi, fuzzel, bemenu or wmenu, the first installed)
  chooser_cmd=wofi -d -n
  ```

- Without any chooser, the first screen is shared.

Sessions can be restored (ScreenCast v4 `persist_mode`): the same screen is shared again without asking.

## Installing

Built with the rest of Cuarzo (`with_portal`, needs `libpipewire-0.3`). It installs:

- `libexec/xdg-desktop-portal-cz`, D-Bus activated (and a systemd user service)
- `share/xdg-desktop-portal/portals/cz.portal` (`UseIn=Cuarzo`)
- `share/xdg-desktop-portal/cuarzo-portals.conf`: ScreenCast from this backend, the rest from GTK

Crystals sets `XDG_CURRENT_DESKTOP=Cuarzo` and exports it (and `WAYLAND_DISPLAY`) to D-Bus activation.
If `xdg-desktop-portal` was already running from another session, restart it:
`systemctl --user restart xdg-desktop-portal`.

Debug output: `CZ_PORTAL_LOG_LEVEL=4` (0 none … 4 debug).

## Credits

The screen casting design (PipeWire negotiation, buffers and damage handling) follows
[xdg-desktop-portal-wlr](https://github.com/emersion/xdg-desktop-portal-wlr) (MIT).

# PureC-OS-QT-Terminal

Terminal emulator of PureC OS, written in C on libgui (`PureC-OS-LibGui`) and
the video-backend display server. Package and program name: `terminal`
(`/usr/bin/terminal`).

- real pseudo-terminal (`openpty`), the shell is `$SHELL`, the user's login
  shell or `/bin/sh`; `TERM=xterm-256color`
- xterm emulation (`src/vt.c`): cursor movement, erase/insert/delete, scroll
  regions, 16/256/true colours, bold/faint/underline/inverse, alternate screen
  (vim, less), UTF-8, window title (OSC 0/2), cursor position report,
  bracketed paste
- 5000 lines of scrollback: mouse wheel or Shift+PgUp / Shift+PgDn
- tabs: Ctrl+Shift+T new, Ctrl+Shift+W close, Ctrl+PgUp / Ctrl+PgDn switch,
  the cross or the middle button on a tab closes it
- selection with the mouse, Ctrl+Shift+C copy, Ctrl+Shift+V or the middle
  button paste (the clipboard lives inside the terminal)

## Build

Inside the PureC-OS tree (libgui and libvb are linked statically):

    make -C programs/PureC-OS-QT-Terminal            # bin/terminal/terminal
    make -C programs/PureC-OS-QT-Terminal package    # bin/terminal/terminal-VERSION-1-x86_64.pkg.tar.gz (+ .sig)
    python3 mk/pkg/publish.py --registry Trusted bin/terminal/terminal-VERSION-1-x86_64.pkg.tar.gz

On PureC OS: `pkg -Sy && pkg -S terminal`. The package conflicts with the old
Qt `qt-terminal`: remove it first with `pkg -R qt-terminal`.

# PureC-OS-QT-Terminal

Terminal emulator for PureC OS, written with Qt 6 Widgets. It runs on the
video-backend display server (Qt platform plugin `purec`).

- real pseudo-terminal (`openpty`), the shell is `$SHELL`, the user's login
  shell or `/bin/sh`; `TERM=xterm-256color`
- xterm emulation: cursor movement, erase/insert/delete, scroll regions,
  16/256/true colours, bold/underline/inverse, alternate screen (vim, less),
  UTF-8, window title (OSC 0/2), cursor position report
- 5000 lines of scrollback: mouse wheel or Shift+PgUp / Shift+PgDn
- tabs: Ctrl+Shift+T new, Ctrl+Shift+W close, Ctrl+PgUp / Ctrl+PgDn switch
- selection with the mouse, Ctrl+Shift+C copy, Ctrl+Shift+V or the middle
  button paste

## Build

Inside the PureC-OS tree (this repository is `programs/PureC-OS-QT-Terminal`),
after Qt is built into `bin/sysroot` (`mk/cxx/fetch.sh`, `mk/cxx/build.sh`,
`mk/qt/fetch.sh`, `mk/qt/configure.sh`, then build and install `bin/qt-build`):

    make -C programs/PureC-OS-QT-Terminal            # bin/qt-terminal-build/qt-terminal
    make -C programs/PureC-OS-QT-Terminal package    # bin/repo/qt-terminal-VERSION-1-x86_64.pkg.tar.gz (+ .sig)

Attach the package and its `.sig` to a GitHub release of this repository; on
PureC OS add `qt-terminal = https://github.com/pablaofficeal/PureC-OS-QT-Terminal`
under `[trusted]` in `/etc/pkg/pkg.conf` and run `pkg -Sy && pkg -S qt-terminal`.

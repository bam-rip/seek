# Seek

A Spotlight-style launcher for Windows 10, written as one C file using only Win32.

**[Download seek.exe](https://github.com/bam-rip/seek/releases/latest/download/seek.exe)** (latest release, Windows 64-bit). Run it and press Alt+Space. No installer is needed. Windows SmartScreen may warn you the first time because the exe isn't signed. Choose "More info", then "Run anyway".

- **Alt+Space** opens it. Start typing to find apps, files and folders.
- **Enter** opens the selected item. **Ctrl+Enter** shows it in Explorer.
- **Esc** clears the text, and a second Esc closes Seek.
- Typing maths shows the answer, and Enter copies it. It handles `+ - * / % ^ !`, brackets, implicit
  multiplication (`2pi`, `3(4+1)`), the constants `pi e tau phi`, and the functions `sqrt cbrt abs exp ln
  log log2 log10 sin cos tan asin acos atan sinh cosh tanh floor ceil round trunc fact gamma pow mod min
  max hypot`. `log(x, base)` and `atan(y, x)` also work. Trig uses radians.
- The last row searches the web for what you typed.
- Apps you open often move up the results.
- Everything is held in memory (about 10 MB for 140k files), so a search takes well under 2 ms.
- A watcher notices when files change, so the disk is only walked again when something actually changed.

## Build

Run `build.bat`. It needs the MSVC Build Tools.

Dev flags:
- `seek.exe --bench` prints index and search timings.
- `seek.exe --calc "expr"` prints what the calculator would show.
- `seek.exe --shot "query" out.bmp` renders the window to a file.

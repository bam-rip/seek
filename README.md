# Seek

A Spotlight-style launcher for Windows 10, written as one C file using only Win32.

- **Alt+Space** opens it. Start typing to find apps, files and folders.
- **Enter** opens the selected item. **Ctrl+Enter** shows it in Explorer.
- **Esc** clears the text, and a second Esc closes Seek.
- Typing maths like `12*(3+4)` shows the answer, and Enter copies it.
- The last row searches the web for what you typed.
- Apps you open often move up the results.
- Everything is held in memory, so a search takes about 2 ms per keystroke.

## Build

Run `build.bat`. It needs the MSVC Build Tools.

Dev flags:
- `seek.exe --bench` prints index and search timings.
- `seek.exe --shot "query" out.bmp` renders the window to a file.

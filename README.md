# TUI Charmap

A terminal character map for browsing Unicode, emoticons, and styled text.

## Build

Requirements: C++17 compiler, GNU Make, `pkg-config`, ncursesw, and ICU (`icu-uc`).

```sh
make
./tuicharmap
```

For a debug build, run `make debug`; it creates `./tuicharmap-debug`.

On Debian/Ubuntu, install the build dependencies with:

```sh
sudo apt install build-essential pkg-config libncursesw5-dev libicu-dev
```

## Use

- `F1` / `F2` / `F3`: Character map / Faces / Stylizer
- Arrow keys or `h` / `j` / `k` / `l`: Navigate
- `Enter`: Copy selection and exit
- `q` or `Esc`: Quit

<details>
<summary>Project details</summary>

- C++17 terminal UI using ncursesw and ICU.
- Character map supports Unicode-name and codepoint search; Faces provides emoticons; Stylizer previews formatted text.
- Built with GNU Make. Run `make clean` to remove build artifacts.

</details>

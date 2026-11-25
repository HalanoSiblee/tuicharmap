# TUI Charmap

A terminal character map for browsing Unicode, emoticons, and styled text.

![Character map](./media/0e466833-6a14-4eae-a756-a657546dd473)
![Faces](./media/37404d7c-1015-44a4-9833-d4c8043e42d2)
![Stylizer](./media/89a82c28-ab83-4fbb-8504-b55866b4c911)
![Character search](./media/773e40ee-5906-43d5-98de-fadb7e08a9b7)

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

This C++17 terminal UI uses ncursesw and ICU. The Character Map supports Unicode-name and codepoint search; Faces provides emoticons; Stylizer previews formatted text. Run `make clean` to remove build artifacts.

</details>

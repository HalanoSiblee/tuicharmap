# Tui Charmap
TUI Character Map

---

![](./media/0e466833-6a14-4eae-a756-a657546dd473)
![](./media/37404d7c-1015-44a4-9833-d4c8043e42d2)
![](./media/89a82c28-ab83-4fbb-8504-b55866b4c911)
![](./media/773e40ee-5906-43d5-98de-fadb7e08a9b7)

<details>
  <summary>Old rust version</summary>
  
![img](./media/42d95f63-b44c-4e11-9c28-27fd70327e73)
![img](./media/4d110889-42ae-4128-889d-cf13d81120fa)
![img](./media/8e988c94-825f-4a54-ad77-840eb839adcf)
![img](./media/be6891f3-d290-4730-a54e-fd22af9322c8)
![img](./media/f3df118d-d809-45c7-af5c-23833dc310a6)


## hotkeys
|Key|Event|
|---|---|
|F1-F12|section jump|
|TAB|search next|
|Shift+TAB|previous search|
|PageUp/Down|Page scrolling|
|Escape|exit|
|Enter|print char&exit|

---
Made possible by ♡ :

- crossterm
- ratatui
- unicode_categories
- unicode_names2

</details>

---


# hotkeys

## Global Controls

| Key | Action |
| :--- | :--- |
| `Tab` | Cycle to the next tab (Charmap → Faces → Stylizer) |
| `Shift + Tab` | Cycle to the previous tab |
| `F1` | Jump to the Charmap tab |
| `F2` | Jump to the Faces tab |
| `F3` | Jump to the Stylizer tab |
| `q` / `Esc` | Quit the application |
| `Enter` | **Copy selection** to clipboard, print to stdout, and exit |

---

## Charmap Tab (`F1`)

| Key | Action |
| :--- | :--- |
| `h` / `Left Arrow` | Move selection left |
| `j` / `Down Arrow` | Move selection down |
| `k` / `Up Arrow` | Move selection up |
| `l` / `Right Arrow` | Move selection right |
| `PgUp` | Jump up one full page |
| `PgDn` | Jump down one full page |
| `Home` | Jump to the first Unicode character |
| `End` | Jump to the last Unicode character |
| `/` | Open search prompt (Fuzzy text search by Unicode name) |
| `?` | Open search prompt prefixed with `U+` (Hex search, e.g., `U+2603`) |
| `n` / `F4` | Jump to the **next** search result |
| `N` / `F5` | Jump to the **previous** search result |

---

## Faces Tab (`F2`)

| Key | Action |
| :--- | :--- |
| `h` / `Left Arrow` | Move selection left |
| `j` / `Down Arrow` | Move selection down |
| `k` / `Up Arrow` | Move selection up |
| `l` / `Right Arrow` | Move selection right |
| `/` | Open ID search prompt (type a number and press `Enter`) |
| `Esc` | Close the ID search prompt |

---

## Stylizer Tab (`F3`)

| Key | Action |
| :--- | :--- |
| `a-z`, `0-9`, etc. | Type text (updates preview automatically) |
| `Backspace` | Delete the last typed character |
| `k` / `Up Arrow` | Select previous style (Bold, Italic, Bold+Italic) |
| `j` / `Down Arrow` | Select next style (Bold, Italic, Bold+Italic) |

*(Note: Pressing `Enter` in the Stylizer tab copies the text wrapped in the selected ANSI escape codes, e.g., `\033[1mYour Text\033[0m`).*

---

## Mouse Support

| Action | Context | Effect |
| :--- | :--- | :--- |
| **Wheel Up/Down** | Charmap & Faces | Scroll the view |
| **Left Click** | Charmap | Select the clicked character |
| **Left Click** | Faces | Select the clicked emoticon |

---

# tuicharmap - Architecture & Internals

This explains the internal design of `tuicharmap`, how the event loops function, and the specific performance optimizations used to make it fast, responsive, and cross-platform.

## 1. Core Stack
*   **Language:** C++17
*   **TUI Library:** `ncursesw` (Required for wide-character and UTF-8 support)
*   **Unicode Data:** `libicu` (International Components for Unicode)
*   **Memory/Storage:** POSIX `mmap` for disk caching
*   **Build System:** GNU Make (with `pkg-config` auto-detection)

---

## 2. Progressive Indexing & `mmap` Cache (`unicode.cpp`)

Scanning 1.1 million Unicode codepoints using ICU and `wcwidth()` takes ~300–500ms. If done synchronously on startup, the app feels frozen. If done every time, it wastes CPU cycles.

### The `mmap` Cache Trick
On the first run, we build a static array of "renderable" codepoints and save it to `/tmp/tuicharmap.cache`. 
On subsequent runs, we bypass the build entirely using `mmap`. The OS maps the file directly into RAM. Loading takes **~1 millisecond**.

The cache consists of a `CacheHeader` and four parallel arrays:
1.  `m_index` (`uint32_t*`): The actual Unicode codepoints.
2.  `m_widths` (`uint8_t*`): Cached `wcwidth` values (1 or 2).
3.  `m_offsets` (`uint32_t*`): Offsets into the name string blob.
4.  `m_names` (`char*`): Pre-uppercased UTF-8 strings of the Unicode names.

### Progressive Loading (Fallback)
If the cache is missing, we build it in a background thread. To keep the UI responsive, we use an atomic counter (`std::atomic<size_t> g_count`) and batch-update it every 1000 items. The main thread reads up to `g_count`, meaning the grid populates live as the background thread scans.

---

## 3. The Search Engine (`search.cpp`)

To keep the UI perfectly smooth, search is completely decoupled from the rendering loop.

### Persistent Worker Thread
Instead of spawning a new thread per keystroke (which destroys performance), we use a **single persistent worker thread**.
*   When you type, we lock a mutex, update `current_query_`, and increment an atomic generation counter (`gen_`).
*   We notify a `std::condition_variable`. 
*   The worker thread wakes up, grabs the new query, and starts scanning the `mmap` cache.
*   If you type another key while it's scanning, `gen_` increments. The worker checks `if (gen_ != active_gen_) break;` inside the loop and aborts immediately, prioritizing the newest query.

### Fuzzy Scoring Algorithm
The search uses a subsequence matching algorithm with heavily weighted scoring to ensure intuitive results:
*   **Exact match:** `100,000` points.
*   **Prefix match:** `10,000` points.
*   **Substring match:** `5,000` points.
*   **Word boundary bonus:** `+100` points (e.g., matching the 'L' in `LATIN`).
*   **Consecutive run bonus:** `+20` points per sequential character.
*   **Gap penalty:** `-5` points for every skipped character.

---

## 4. Cross-Platform Clipboard (`clipboard.cpp`)

Spawning a subprocess (like `wl-copy`, `xclip`, or `pbcopy`) on every keypress causes a "fork bomb" effect and stutters the terminal. We solve this with a background worker thread that coalesces requests.

### Platform Detection
The clipboard command is determined at compile time:
*   **macOS:** Uses `pbcopy`.
*   **Windows:** Uses `clip`.
*   **Android/Termux:** Uses `termux-clipboard-set` (detected via `__ANDROID__` macro).
*   **Linux:** Uses a shell fallback chain checking for Wayland (`wl-copy`), then X11 (`xclip`, `xsel`). If compiled on Linux but run in Termux, a runtime check for `$TERMUX_VERSION` or `$PREFIX` swaps to `termux-clipboard-set`.

### The Coalescing Worker
When a copy is requested, we store the string in a `pending_` variable and notify a condition variable. The worker wakes up, grabs the string, and runs `popen()`. If you press `Enter` to copy and exit simultaneously, `clip::flush()` blocks the main thread until the data is handed off (with a 50ms safety sleep for Wayland daemon latency).

---

## 5. UI Rendering & Event Loop (`main.cpp`)

The UI is split into two distinct `ncurses` windows to minimize full-screen redrawing:
*   `stdscr`: Handles the top header bar and the bottom status bar.
*   `grid_win`: A dedicated `WINDOW*` that only draws the table/grid.

### Window Stacking
When refreshing `ncurses` windows, order matters. We use `wnoutrefresh()` to queue the windows and `doupdate()` to paint them:
```cpp
wnoutrefresh(stdscr);   // MUST be painted first (background)
wnoutrefresh(grid_win); // Painted second (foreground/overlay)
doupdate();             // Flushes both to the terminal
```
*If this order is reversed, `stdscr` will erase the grid.*

### The Dynamic Event Loop
The loop uses `timeout()` to dynamically control the blocking behavior of `getch()`:
*   **Normal state:** `timeout(-1)`. The app sleeps, using 0% CPU, until a key is pressed.
*   **Searching state:** `timeout(30)`. The loop wakes up 33 times a second to check if the background search thread has returned new results.
*   **Stylizer state:** `timeout(250)`. The loop wakes up every 250ms to refresh the typing preview.

---

## 6. Performance Tricks & Optimizations

### 1. The `wcwidth` Bottleneck
Calling `wcwidth()` inside the drawing loop takes a mutex lock in `glibc`. For a 200x50 grid, that's 10,000 locks per frame. We completely eliminated this by precalculating the widths during the index build and reading from `uint8_t* m_widths`.

### 2. Continuous Line Drawing
Instead of drawing a full 8-character box per cell, we draw continuous horizontal and vertical lines using `ACS_HLINE`, `ACS_VLINE`, and `ACS_PLUS`. This reduces the number of `ncurses` draw calls by 60% and allows the grid to look like a clean spreadsheet.

### 3. Paste Guard
If a user accidentally pastes a massive block of text containing newlines, `ncurses` will send a flood of `Enter` keys, which would instantly commit the search and exit the app. 
We use `std::chrono::steady_clock` to measure the time between keystrokes. If a `\n` arrives `< 20ms` after the previous key, we know it's a paste and ignore the `Enter` command.

### 4. Stylizer ANSI Handling
The F3 tab shows a visual preview (using `A_BOLD` and `A_ITALIC` attributes) but copies the raw ANSI escape sequences to the clipboard (`\033[1mText\033[0m`). This allows you to paste styled text into Discord, Slack, or IRC. The stdout output on exit also prints the raw ANSI string.

---

## 7. Build System (`Makefile`)

The project uses a custom `Makefile` with two profiles controlled by the `PROFILE` variable (defaulting to `release`).

*   **Release (`make`):** Uses `-O3 -flto` for maximum speed and `-s` to strip symbols during linking. Outputs to `./tuicharmap`.
*   **Debug (`make debug`):** Uses `-g -O0` and defines `-DDEBUG`. Outputs to `./tuicharmap-debug`.
*   **Dependencies:** Uses `pkg-config` to locate `ncursesw` and `icu-uc` flags automatically. Object files are isolated in `build/<profile>/` to prevent cross-contamination.

---

## 8. The Three Tabs

1.  **Charmap (F1):** A grid of all renderable Unicode characters. Supports fuzzy search by name and exact hex search (`U+1F600`).
2.  **Faces (F2):** A grid of ASCII/Unicode emoticons (embedded via `faces_data.hpp`). Supports navigating via arrow keys or typing an ID number to jump directly to it.
3.  **Stylizer (F3):** A text input buffer that renders three formatted previews. Hitting `Enter` wraps the text in ANSI codes and exits.


---

Project tree

```
.
├── LICENSE
├── Makefile
├── README.md
├── rust
│   ├── Cargo.lock
│   ├── Cargo.toml
│   └── src
│       └── main.rs
└── src
    ├── clipboard.cpp
    ├── clipboard.hpp
    ├── faces_data.hpp
    ├── main.cpp
    ├── search.cpp
    ├── search.hpp
    ├── unicode.cpp
    └── unicode.hpp
```
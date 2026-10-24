#include "clipboard.hpp"
#include "search.hpp"
#include "unicode.hpp"
#include "faces_data.hpp"
#include <ncurses.h>
#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <wchar.h>
namespace {
    enum class Tab {
        Charmap, Faces, Stylizer
    };
    constexpr int kCellW = 4;
    constexpr int kCellH = 2;
    constexpr int kGridX = 0;
    constexpr int kGridY = 3;
    constexpr size_t kMaxSearchLen = 1024;
    WINDOW * grid_win = nullptr;
    std::string utf8(uint32_t cp) {
        std::string s;
        if (cp < 0x80)s += (char)cp;
        else if (cp < 0x800) {
            s += (char)(0xC0 | (cp >> 6));
            s += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += (char)(0xE0 | (cp >> 12));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        } else {
            s += (char)(0xF0 | (cp >> 18));
            s += (char)(0x80 | ((cp >> 12) & 0x3F));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        }
        return s;
    }
    struct App {
        Tab current_tab = Tab::Charmap;
        size_t cursor = 0;
        size_t view_top = 0;
        int cols = 1;
        int rows = 1;
        bool searching = false;
        std::string search_q;
        std::vector<search::Hit> hits;
        std::vector<uint32_t> history;
        size_t history_pos = 0;
        size_t face_cursor = 0;
        int face_view_row = 0;
        bool f2_searching = false;
        std::string f2_search_q;
        std::string style_text;
        int style_idx = 0;
        std::string status_msg;
        bool dirty = true;
    };
    search::Engine g_search;
    size_t total() {
        return uc::count();
    }
    void recompute_layout(App & a) {
        int avail_w = COLS;
        int avail_h = LINES - kGridY - 1;
        a.cols = std::max(1, (avail_w - 1) / kCellW);
        a.rows = std::max(1, (avail_h - 1) / kCellH);
        a.view_top = (a.view_top / (size_t)a.cols) * (size_t)a.cols;
    }
    void ensure_visible(App & a) {
        if (total() == 0) {
            a.view_top = 0;
            return;
        }
        size_t stride = (size_t)a.cols;
        size_t cur_row = a.cursor / stride;
        size_t top_row = a.view_top / stride;
        size_t rows = (size_t)a.rows;
        if (cur_row < top_row)top_row = cur_row;
        if (cur_row >= top_row + rows)top_row = cur_row - rows + 1;
        size_t last_row = (total() - 1) / stride;
        if (top_row + rows - 1 > last_row)top_row = (last_row + 1 >= rows) ? last_row - rows + 1 : 0;
        a.view_top = top_row * stride;
    }
    void ensure_visible_face(App & a) {
        int win_h = LINES - kGridY - 1;
        int f_cols = std::max(1, COLS / 16);
        int f_rows = std::max(1, win_h / 2);
        int cursor_row = a.face_cursor / f_cols;
        if (cursor_row < a.face_view_row)a.face_view_row = cursor_row;
        if (cursor_row >= a.face_view_row + f_rows)a.face_view_row = cursor_row - f_rows + 1;
    }
    void move_cursor(App & a, int64_t delta) {
        size_t t = total();
        if (t == 0)return;
        int64_t cur = (int64_t)a.cursor + delta;
        if (cur < 0)cur = 0;
        if (cur >= (int64_t)t)cur = (int64_t)t - 1;
        a.cursor = (size_t)cur;
        ensure_visible(a);
        a.dirty = true;
    }
    void page_jump(App & a, int direction) {
        size_t t = total();
        if (t == 0)return;
        size_t stride = (size_t)a.cols;
        size_t off = a.cursor - a.view_top;
        size_t top_row = a.view_top / stride;
        size_t last_row = (t - 1) / stride;
        size_t rows = (size_t)a.rows;
        size_t new_top_row;
        if (direction < 0) {
            new_top_row = (top_row >= rows) ? top_row - rows : 0;
        } else {
            new_top_row = top_row + rows;
            size_t max_top_row = (last_row + 1 >= rows) ? last_row - rows + 1 : 0;
            if (new_top_row > max_top_row)new_top_row = max_top_row;
        }
        a.view_top = new_top_row * stride;
        a.cursor = std::min(a.view_top + off, t - 1);
        ensure_visible(a);
        a.dirty = true;
    }
    void jump_to_cp(App & a, uint32_t cp) {
        size_t i = uc::find(cp);
        if (i == SIZE_MAX)return;
        a.cursor = i;
        ensure_visible(a);
        a.dirty = true;
    }
    bool is_hex_query(const std::string & q) {
        return q.size() >= 2 && (q[0] == 'U' || q[0] == 'u') && q[1] == '+';
    }
    void commit_search(App & a) {
        a.status_msg.clear();
        if (is_hex_query(a.search_q)) {
            std::string hex = a.search_q.substr(2);
            uint32_t cp = 0;
            bool ok = !hex.empty();
            for (char c : hex) {
                int d = -1;
                if (c >= '0' && c <= '9')d = c - '0';
                else if (c >= 'a' && c <= 'f')d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')d = c - 'A' + 10;
                else {
                    ok = false;
                    break;
                }
                cp = (cp << 4) | (uint32_t)d;
                if (cp > 0x10FFFF) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                size_t i = uc::find(cp);
                if (i != SIZE_MAX) {
                    a.cursor = i;
                    a.history.clear();
                    a.history.push_back(cp);
                    a.history_pos = 0;
                    ensure_visible(a);
                } else {
                    a.status_msg = "U+" + hex + " not found or unprintable";
                }
            } else {
                a.status_msg = "Invalid hex input";
            }
            return;
        }
        if (!a.hits.empty()) {
            a.history.clear();
            a.history.reserve(a.hits.size());
            for (auto &h : a.hits)a.history.push_back(h.cp);
            a.history_pos = 0;
            jump_to_cp(a, a.history[0]);
        }
    }
    void draw_grid_charmap(const App & a, WINDOW * win, int win_h) {
        size_t t = total();
        for (int gy = 0; gy <= a.rows; ++gy) {
            int y = gy * kCellH;
            if (y >= win_h)break;
            for (int gx = 0; gx <= a.cols; ++gx) {
                int x = gx * kCellW;
                if (x >= COLS)break;
                mvwaddch(win, y, x, ACS_PLUS);
                for (int i = 1; i < kCellW && x + i < COLS; ++i) {
                    mvwaddch(win, y, x + i, ACS_HLINE);
                }
            }
        }
        for (int gx = 0; gx <= a.cols; ++gx) {
            int x = gx * kCellW;
            if (x >= COLS)break;
            for (int gy = 0; gy < a.rows; ++gy) {
                int y = gy * kCellH + 1;
                if (y >= win_h)break;
                mvwaddch(win, y, x, ACS_VLINE);
            }
        }
        for (int gy = 0; gy < a.rows; ++gy) {
            int y = gy * kCellH + 1;
            if (y >= win_h)break;
            for (int gx = 0; gx < a.cols; ++gx) {
                int x = gx * kCellW + 1;
                if (x + kCellW > COLS)break;
                size_t i = a.view_top + (size_t)gy * a.cols + (size_t)gx;
                if (i >= t)return;
                uint32_t cp = uc::cp_at(i);
                bool sel = (i == a.cursor);
                if (sel)wattron(win, A_REVERSE);
                for (int k = 0; k < kCellW - 1; ++k) {
                    mvwaddch(win, y, x + k, ' ');
                }
                wchar_t ws[2] = {
                    (wchar_t)cp, 0
                };
                mvwaddwstr(win, y, x, ws);
                if (sel)wattroff(win, A_REVERSE);
            }
        }
    }
    void draw_grid_faces(const App & a, WINDOW * win, int win_h) {
        int f_cols = std::max(1, COLS / 16);
        int start_idx = a.face_view_row * f_cols;
        for (int i = start_idx; i < (int)kFaces.size(); ++i) {
            int gy = (i / f_cols) - a.face_view_row;
            int gx = i % f_cols;
            int y = gy * 2;
            int x = gx * 16;
            if (y >= win_h || x >= COLS)break;
            bool sel = (i == (int)a.face_cursor);
            if (sel)wattron(win, A_REVERSE);
            mvwprintw(win, y, x, " [%3d] %-10s ", i + 1, kFaces[i].c_str());
            if (sel)wattroff(win, A_REVERSE);
        }
    }
    void draw_stylizer(const App & a, WINDOW * win, int win_h) {
        mvwprintw(win, 2, 2, "Type text below (Wait 250ms for preview, Enter to copy):");
        mvwprintw(win, 4, 2, "Input: %s_", a.style_text.c_str());
        mvwprintw(win, 7, 2, "1. Bold only:");
        if (a.style_idx == 0)wattron(win, A_REVERSE);
        wattron(win, A_BOLD);
        mvwprintw(win, 8, 4, "%s", a.style_text.c_str());
        wattroff(win, A_BOLD);
        if (a.style_idx == 0)wattroff(win, A_REVERSE);
        mvwprintw(win, 10, 2, "2. Italic only:");
        if (a.style_idx == 1)wattron(win, A_REVERSE);
#ifdef A_ITALIC
        wattron(win, A_ITALIC);
#else
        wattron(win, A_UNDERLINE);
#endif
        mvwprintw(win, 11, 4, "%s", a.style_text.c_str());
#ifdef A_ITALIC
        wattroff(win, A_ITALIC);
#else
        wattroff(win, A_UNDERLINE);
#endif
        if (a.style_idx == 1)wattroff(win, A_REVERSE);
        mvwprintw(win, 13, 2, "3. Bold + Italic:");
        if (a.style_idx == 2)wattron(win, A_REVERSE);
        wattron(win, A_BOLD);
#ifdef A_ITALIC
        wattron(win, A_ITALIC);
#else
        wattron(win, A_UNDERLINE);
#endif
        mvwprintw(win, 14, 4, "%s", a.style_text.c_str());
#ifdef A_ITALIC
        wattroff(win, A_ITALIC);
#else
        wattroff(win, A_UNDERLINE);
#endif
        wattroff(win, A_BOLD);
        if (a.style_idx == 2)wattroff(win, A_REVERSE);
    }
    void draw(const App & a) {
        size_t t = total();
        erase();
        attron(A_BOLD);
        if (a.current_tab == Tab::Charmap) {
            uint32_t cursor_cp = (a.cursor < t) ? uc::cp_at(a.cursor) : 0;
            mvprintw(0, 0, " tuicharmap [F1]  Faces [F2]  Stylizer [F3] (Tab to cycle) | U+%04X [%zu / %zu]", cursor_cp, a.cursor + 1, t);
        } else if (a.current_tab == Tab::Faces) {
            mvprintw(0, 0, " Charmap [F1]  tuicharmap Faces [F2]  Stylizer [F3] (Tab to cycle) | %zu faces", kFaces.size());
        } else {
            mvprintw(0, 0, " Charmap [F1]  Faces [F2]  tuicharmap Stylizer [F3] (Tab to cycle)");
        }
        attroff(A_BOLD);
        for (int x = getcurx(stdscr); x < COLS; ++x)addch(' ');
        int sy = LINES - 1;
        attron(A_REVERSE);
        for (int x = 0; x < COLS; ++x)mvaddch(sy, x, ' ');
        if (!a.status_msg.empty()) {
            mvprintw(sy, 0, "%-*s", COLS, (" " + a.status_msg).c_str());
        } else if (a.current_tab == Tab::Charmap) {
            if (a.searching) {
                std::string prompt = " /" + a.search_q + "_";
                if (a.search_q.size() >= kMaxSearchLen)prompt = " /" + a.search_q + " [LIMIT]";
                if (!a.hits.empty())prompt += "   [" + std::to_string(a.hits.size()) + " hits]";
                mvprintw(sy, 0, "%-*s", COLS, prompt.c_str());
            } else {
                uint32_t cp = (a.cursor < t) ? uc::cp_at(a.cursor) : 0;
                char hex[16];
                std::snprintf(hex, sizeof(hex), "U+%04X", cp);
                std::string line = std::string(" ") + hex + "  " + uc::name(cp) + "   | Enter:copy /:search n/N:next/prev q:quit";
                mvprintw(sy, 0, "%-*s", COLS, line.c_str());
            }
        } else if (a.current_tab == Tab::Faces) {
            if (a.f2_searching) {
                std::string prompt = " /ID: " + a.f2_search_q + "_";
                mvprintw(sy, 0, "%-*s", COLS, prompt.c_str());
            } else {
                mvprintw(sy, 0, "%-*s", COLS, " Arrows: navigate   /: search by ID   Enter: copy & exit   q: quit");
            }
        } else {
            mvprintw(sy, 0, "%-*s", COLS, " Up/Down: select style   Enter: copy & exit   q: quit");
        }
        attroff(A_REVERSE);
        int win_h = LINES - kGridY - 1;
        if (win_h > 0) {
            if (!grid_win || getmaxy(grid_win) != win_h || getmaxx(grid_win) != COLS) {
                if (grid_win)delwin(grid_win);
                grid_win = newwin(win_h, COLS, kGridY, 0);
                if (has_colors())wbkgd(grid_win, COLOR_PAIR(1));
            }
            werase(grid_win);
            if (a.current_tab == Tab::Charmap)draw_grid_charmap(a, grid_win, win_h);
            else if (a.current_tab == Tab::Faces)draw_grid_faces(a, grid_win, win_h);
            else draw_stylizer(a, grid_win, win_h);
        }
        wnoutrefresh(stdscr);
        if (grid_win)wnoutrefresh(grid_win);
        doupdate();
    }
}

int main() {
    setlocale(LC_ALL, "");
    uc::warmup();
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    mouseinterval(0);
    mousemask(ALL_MOUSE_EVENTS, nullptr);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, -1, -1);
        bkgd(COLOR_PAIR(1));
    }
    App app;
    recompute_layout(app);
    ensure_visible(app);
    bool quit = false;
    bool do_copy_on_exit = false;
    std::string result_str;
    auto last_input = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    while (!quit) {
        auto now = std::chrono::steady_clock::now();
        bool is_paste = (now - last_input < std::chrono::milliseconds(20));
        if (app.current_tab == Tab::Stylizer)timeout(250);
        else if (app.searching || app.f2_searching)timeout(30);
        else timeout( -1);
        if (g_search.ready()) {
            app.hits = g_search.take();
            app.dirty = true;
        }
        if (app.dirty) {
            draw(app);
            app.dirty = false;
        }
        int ch = getch();
        if (ch == KEY_MOUSE) {
            MEVENT ev;
            if (getmouse( &ev) == OK) {
                if (app.current_tab == Tab::Charmap) {
                    if (ev.bstate & (BUTTON4_PRESSED | BUTTON4_CLICKED)) {
                        page_jump(app, -1);
                    } else if (ev.bstate & (BUTTON5_PRESSED | BUTTON5_CLICKED)) {
                        page_jump(app, 1);
                    } else if (ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED)) {
                        int rel_y = ev.y - kGridY - 1;
                        int rel_x = ev.x - kGridX;
                        if (rel_y >= 0 && rel_x >= 0) {
                            int gy = rel_y / kCellH;
                            int gx = rel_x / kCellW;
                            if (gx >= 0 && gx < app.cols && gy >= 0 && gy < app.rows) {
                                size_t i = app.view_top + (size_t)gy * app.cols + (size_t)gx;
                                if (i < total()) {
                                    app.cursor = i;
                                    ensure_visible(app);
                                    app.dirty = true;
                                }
                            }
                        }
                    }
                } else if (app.current_tab == Tab::Faces) {
                    int f_cols = std::max(1, COLS / 16);
                    if (ev.bstate & (BUTTON4_PRESSED | BUTTON4_CLICKED)) {
                        if (app.face_cursor >= (size_t)f_cols)app.face_cursor -= f_cols;
                        ensure_visible_face(app);
                        app.dirty = true;
                    } else if (ev.bstate & (BUTTON5_PRESSED | BUTTON5_CLICKED)) {
                        if (app.face_cursor + f_cols < kFaces.size())app.face_cursor += f_cols;
                        ensure_visible_face(app);
                        app.dirty = true;
                    } else if (ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED)) {
                        int rel_y = ev.y - kGridY;
                        int rel_x = ev.x - kGridX;
                        if (rel_y >= 0 && rel_x >= 0) {
                            int gy = rel_y / 2;
                            int gx = rel_x / 16;
                            int i = (app.face_view_row + gy) * f_cols + gx;
                            if (i >= 0 && i < (int)kFaces.size() && gx < f_cols) {
                                app.face_cursor = i;
                                ensure_visible_face(app);
                                app.dirty = true;
                            }
                        }
                    }
                }
            }
            continue;
        }
        if (ch == '\t') {
            app.current_tab = (Tab)(((int)app.current_tab + 1) % 3);
            app.dirty = true;
            continue;
        }
        if (ch == KEY_BTAB) {
            app.current_tab = (Tab)(((int)app.current_tab + 2) % 3);
            app.dirty = true;
            continue;
        }
        if (ch == KEY_F(1)) {
            app.current_tab = Tab::Charmap;
            app.dirty = true;
            continue;
        }
        if (ch == KEY_F(2)) {
            app.current_tab = Tab::Faces;
            app.dirty = true;
            continue;
        }
        if (ch == KEY_F(3)) {
            app.current_tab = Tab::Stylizer;
            app.dirty = true;
            continue;
        }
        if (ch != ERR) {
            last_input = now;
            if (ch != KEY_RESIZE) {
                app.status_msg.clear();
                app.dirty = true;
            }
        }
        if (app.current_tab == Tab::Stylizer) {
            if (ch == 27 || ch == 'q') {
                quit = true;
            } else if (ch == '\n' || ch == KEY_ENTER) {
                if (app.style_idx == 0)result_str = "\033[1m" + app.style_text + "\033[0m";
                else if (app.style_idx == 1)result_str = "\033[3m" + app.style_text + "\033[0m";
                else result_str = "\033[1;3m" + app.style_text + "\033[0m";
                do_copy_on_exit = true;
                quit = true;
            } else if (ch == KEY_UP || ch == 'k') {
                app.style_idx = std::max(0, app.style_idx - 1);
            } else if (ch == KEY_DOWN || ch == 'j') {
                app.style_idx = std::min(2, app.style_idx + 1);
            } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (!app.style_text.empty())app.style_text.pop_back();
            } else if (ch >= 32 && ch < 127) {
                app.style_text += (char)ch;
            }
            continue;
        }
        if (app.current_tab == Tab::Faces) {
            if (app.f2_searching) {
                if (ch == 27) {
                    app.f2_searching = false;
                } else if (ch == '\n' || ch == KEY_ENTER) {
                    if (!app.f2_search_q.empty()) {
                        try {
                            int id = std::stoi(app.f2_search_q);
                            if (id >= 1 && id <= (int)kFaces.size()) {
                                app.face_cursor = id - 1;
                                ensure_visible_face(app);
                            }
                        } catch ( ... ) {
                        }
                    }
                    app.f2_searching = false;
                } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                    if (!app.f2_search_q.empty())app.f2_search_q.pop_back();
                } else if (ch >= '0' && ch <= '9') {
                    app.f2_search_q += (char)ch;
                }
                continue;
            }
            if (ch == 27 || ch == 'q') {
                quit = true;
            } else if (ch == '/') {
                app.f2_searching = true;
                app.f2_search_q.clear();
            } else if (ch == '\n' || ch == KEY_ENTER) {
                if (!kFaces.empty()) {
                    result_str = kFaces[app.face_cursor];
                    do_copy_on_exit = true;
                    quit = true;
                }
            } else if (ch == KEY_LEFT || ch == 'h') {
                if (app.face_cursor > 0)app.face_cursor--;
                ensure_visible_face(app);
            } else if (ch == KEY_RIGHT || ch == 'l') {
                if (app.face_cursor < kFaces.size() - 1)app.face_cursor++;
                ensure_visible_face(app);
            } else if (ch == KEY_UP || ch == 'k') {
                int f_cols = std::max(1, COLS / 16);
                if (app.face_cursor >= (size_t)f_cols)app.face_cursor -= f_cols;
                ensure_visible_face(app);
            } else if (ch == KEY_DOWN || ch == 'j') {
                int f_cols = std::max(1, COLS / 16);
                if (app.face_cursor + f_cols < kFaces.size())app.face_cursor += f_cols;
                ensure_visible_face(app);
            }
            continue;
        }
        if (app.searching) {
            if (ch == 27) {
                app.searching = false;
                g_search.cancel();
            } else if (ch == '\n' || ch == KEY_ENTER) {
                if (!is_paste) {
                    commit_search(app);
                    app.searching = false;
                    g_search.cancel();
                }
            } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (!app.search_q.empty())app.search_q.pop_back();
                if (app.search_q.empty() || is_hex_query(app.search_q)) {
                    app.hits.clear();
                    g_search.cancel();
                } else g_search.start(app.search_q);
            } else if (ch >= 32 && ch < 127) {
                if (app.search_q.size() < kMaxSearchLen) {
                    app.search_q += (char)ch;
                    if (is_hex_query(app.search_q)) {
                        app.hits.clear();
                        g_search.cancel();
                    } else g_search.start(app.search_q);
                }
            }
            continue;
        }
        switch (ch) {
            case 'q':
            case 27:
                quit = true;
                break;
            case '\n':
            case KEY_ENTER:
                if (app.cursor >= total())break;
                result_str = utf8(uc::cp_at(app.cursor));
                do_copy_on_exit = true;
                quit = true;
                break;
            case KEY_LEFT:
            case 'h':
                move_cursor(app, -1);
                break;
            case KEY_RIGHT:
            case 'l':
                move_cursor(app, 1);
                break;
            case KEY_UP:
            case 'k':
                move_cursor(app, -app.cols);
                break;
            case KEY_DOWN:
            case 'j':
                move_cursor(app, app.cols);
                break;
            case KEY_PPAGE:
                page_jump(app, -1);
                break;
            case KEY_NPAGE:
                page_jump(app, 1);
                break;
            case KEY_HOME:
                app.cursor = 0;
                ensure_visible(app);
                break;
            case KEY_END:
                app.cursor = total() ? total() - 1 : 0;
                ensure_visible(app);
                break;
            case 'n':
            case KEY_F(4):
                if (!app.history.empty()) {
                    app.history_pos = (app.history_pos + 1) % app.history.size();
                    jump_to_cp(app, app.history[app.history_pos]);
            }
            break;
            case 'N':
            case KEY_F(5):
                if (!app.history.empty()) {
                    app.history_pos = (app.history_pos + app.history.size() - 1) % app.history.size();
                    jump_to_cp(app, app.history[app.history_pos]);
            }
            break;
            case '/':
                app.searching = true;
                app.search_q.clear();
                app.hits.clear();
                g_search.cancel();
                break;
            case '?':
                app.searching = true;
                app.search_q = "U+";
                app.hits.clear();
                g_search.cancel();
                break;
            case KEY_RESIZE:
                recompute_layout(app);
                ensure_visible(app);
                break;
            default:
                break;
        }
    }
    g_search.cancel();
    endwin();
    if (do_copy_on_exit && !result_str.empty()) {
        clip::copy(result_str);
        clip::flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::fwrite(result_str.data(), 1, result_str.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
    return 0;
}

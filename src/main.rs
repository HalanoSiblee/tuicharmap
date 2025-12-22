use crossterm::{
    event::{self, DisableMouseCapture, EnableMouseCapture, Event, KeyCode, KeyEventKind, MouseButton, MouseEventKind},
    execute,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
};
use ratatui::{prelude::*, widgets::*};
use std::{env, io, process, sync::mpsc, thread, time::Duration};
use unicode_names2::name;

const VERSION: &str = env!("CARGO_PKG_VERSION");
const MAX_UNICODE: u32 = 0x10FFFF;

struct App {
    search_query: String,
    cursor_pos: u32,
    tx: mpsc::Sender<String>,
    rx: mpsc::Receiver<Vec<u32>>,
    search_results: Vec<u32>,
    search_index: usize,
    last_grid_cols: u32,
    last_start_row: u32,
    last_grid_height: u32,
}

impl App {
    fn new(tx: mpsc::Sender<String>, rx: mpsc::Receiver<Vec<u32>>) -> Self {
        Self {
            search_query: String::new(),
            cursor_pos: 0x0020,
            tx,
            rx,
            search_results: Vec::new(),
            search_index: 0,
            last_grid_cols: 1,
            last_start_row: 0,
            last_grid_height: 1,
        }
    }

    fn update_search(&mut self) {
        let _ = self.tx.send(self.search_query.clone());
    }

    fn next_match(&mut self) {
        if !self.search_results.is_empty() {
            self.search_index = (self.search_index + 1) % self.search_results.len();
            self.cursor_pos = self.search_results[self.search_index];
        }
    }
}

fn main() -> Result<(), io::Error> {
    let args: Vec<String> = env::args().collect();
    if args.contains(&"--version".to_string()) || args.contains(&"-V".to_string()) {
        println!("\x1B]8;;https://github.com/HalanoSiblee/tuicharmap\x07tuicharmap \x1B]8;;\x07 {}",VERSION);
        process::exit(0);
    }

    let (query_tx, query_rx) = mpsc::channel::<String>();
    let (result_tx, result_rx) = mpsc::channel::<Vec<u32>>();

    thread::spawn(move || {
        while let Ok(query) = query_rx.recv() {
            if query.is_empty() {
                let _ = result_tx.send(Vec::new());
                continue;
            }
            let normalized = query.to_uppercase();
            let mut matches = Vec::new();
            for code in 0..=MAX_UNICODE {
                if let Some(ch) = std::char::from_u32(code) {
                    if let Some(n) = name(ch) {
                        if n.to_string().contains(&normalized) {
                            matches.push(code);
                        }
                    }
                }
                if matches.len() >= 1000 { break; }
            }
            let _ = result_tx.send(matches);
        }
    });

    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen, EnableMouseCapture)?;
    let mut terminal = Terminal::new(CrosstermBackend::new(stdout))?;

    let mut app = App::new(query_tx, result_rx);
    let res = run_loop(&mut terminal, &mut app);

    disable_raw_mode()?;
    execute!(terminal.backend_mut(), LeaveAlternateScreen, DisableMouseCapture)?;
    terminal.show_cursor()?;

    if let Ok(Some(final_char)) = res {
        print!("{}", final_char);
        io::Write::flush(&mut io::stdout())?;
    }
    Ok(())
}

fn run_loop<B: Backend>(terminal: &mut Terminal<B>, app: &mut App) -> io::Result<Option<char>> {
    loop {
        if let Ok(matches) = app.rx.try_recv() {
            app.search_results = matches;
            app.search_index = 0;
            if let Some(&first) = app.search_results.first() {
                app.cursor_pos = first;
            }
        }

        terminal.draw(|f| ui(f, app))?;

        if event::poll(Duration::from_millis(10))? {
            match event::read()? {
                Event::Key(key) if key.kind == KeyEventKind::Press => match key.code {
                    KeyCode::Esc => return Ok(None),
                    KeyCode::Enter => return Ok(std::char::from_u32(app.cursor_pos)),
                    KeyCode::Tab => app.next_match(),
                    
                    KeyCode::Left => app.cursor_pos = app.cursor_pos.saturating_sub(1),
                    KeyCode::Right => app.cursor_pos = app.cursor_pos.saturating_add(1),
                    KeyCode::Up => app.cursor_pos = app.cursor_pos.saturating_sub(app.last_grid_cols),
                    KeyCode::Down => app.cursor_pos = app.cursor_pos.saturating_add(app.last_grid_cols),
                    
                    KeyCode::PageUp => app.cursor_pos = app.cursor_pos.saturating_sub(app.last_grid_cols * app.last_grid_height),
                    KeyCode::PageDown => app.cursor_pos = app.cursor_pos.saturating_add(app.last_grid_cols * app.last_grid_height),
                    
                    KeyCode::F(n) if n >= 1 && n <= 12 => app.cursor_pos = (MAX_UNICODE / 11) * (n as u32 - 1),

                    KeyCode::Backspace => { app.search_query.pop(); app.update_search(); }
                    KeyCode::Char(c) => { app.search_query.push(c); app.update_search(); }
                    _ => {}
                },
                Event::Mouse(mouse) => match mouse.kind {
                    MouseEventKind::Down(MouseButton::Left) => {
                        let grid_y = mouse.row.saturating_sub(4) as u32; 
                        let grid_x = mouse.column.saturating_sub(1) as u32;
                        let col = grid_x / 3;
                        if col < app.last_grid_cols {
                            let new_pos = (app.last_start_row + grid_y) * app.last_grid_cols + col;
                            if new_pos <= MAX_UNICODE { app.cursor_pos = new_pos; }
                        }
                    }
                    MouseEventKind::ScrollUp => app.cursor_pos = app.cursor_pos.saturating_sub(app.last_grid_cols * 2),
                    MouseEventKind::ScrollDown => app.cursor_pos = app.cursor_pos.saturating_add(app.last_grid_cols * 2),
                    _ => {}
                }
                _ => {}
            }
        }
    }
}

fn ui(f: &mut Frame, app: &mut App) {
    let chunks = Layout::default()
        .constraints([Constraint::Length(3), Constraint::Min(0), Constraint::Length(3)])
        .split(f.area());

    let result_count = if app.search_query.is_empty() { 
        "".into() 
    } else { 
        format!(" [{}/{}]", if app.search_results.is_empty() { 0 } else { app.search_index + 1 }, app.search_results.len()) 
    };
    
    let search = Paragraph::new(Line::from(vec![
        Span::raw(&app.search_query),
        Span::styled(result_count, Style::default().fg(Color::DarkGray))
    ])).block(Block::bordered().title(" Search (TAB: Next, Enter: Select) "));
    f.render_widget(search, chunks[0]);

    let area = chunks[1].inner(Margin { vertical: 1, horizontal: 1 });
    
    let cols = (area.width / 3).max(1) as u32;
    let rows = area.height as u32;
    
    let start_row = (app.cursor_pos / cols).saturating_sub(rows / 2);
    
    app.last_grid_cols = cols; 
    app.last_start_row = start_row; 
    app.last_grid_height = rows;

    let mut lines = Vec::new();
    for r in 0..rows {
        let mut spans = Vec::new();
        for c in 0..cols {
            let code = (start_row + r) * cols + c;
            if code > MAX_UNICODE { break; }
            
            let ch = std::char::from_u32(code).unwrap_or(' ');
            let style = if code == app.cursor_pos {
                Style::default().add_modifier(Modifier::REVERSED)
            } else { 
                Style::default() 
            };
            
            spans.push(Span::styled(format!(" {} ", ch), style));
        }
        lines.push(Line::from(spans));
    }

    let grid_widget = Paragraph::new(lines)
        .block(Block::bordered().title(" Character Map "))
        .wrap(Wrap { trim: false });
    
    f.render_widget(grid_widget, chunks[1]);

    let ch = std::char::from_u32(app.cursor_pos).unwrap_or(' ');
    let char_name = name(ch).map(|n| n.to_string()).unwrap_or_else(|| "Unknown".into());
    f.render_widget(
        Paragraph::new(format!(" U+{:04X} | {} ", app.cursor_pos, char_name))
            .block(Block::bordered().title(" Selection ")),
        chunks[2],
    );
}

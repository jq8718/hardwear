/* lcd_vt.c -- mini VT100-subset terminal rendered on the ST7789.
 *
 * The device mirrors a vibetty AI session over screen_text, which is a raw
 * ANSI/VT byte stream (tag 0x00 = full-screen replay, tag 0x01 = incremental
 * pty output). A full emulator is overkill for a 28x38 panel, so this parses
 * the subset real terminal TUIs (Ink etc.) actually emit: cursor positioning,
 * CR/LF/BS/TAB, erase (J/K/X), insert/delete (L/M/P/@), SGR (ignored: the
 * panel renders one foreground color), alternate-screen and DEC private modes
 * (ignored), and OSC 0/2 window-title capture for the status header. */

#include <stdio.h>
#include <string.h>

#include "st7789.h"
#include "lcd_vt.h"

#define LCD_SCALE 2             /* text is drawn at 2x */
#define ROW_STEP  18            /* 8-row glyph at 2x (16 px) + 2 px gap */
#define X_MARGIN  4
#define HEADER_H  20            /* px above the terminal area */
#define TERM_Y    (HEADER_H + 2)
#define CHAR_W    ST7789_CHAR_W(LCD_SCALE)   /* 12 px per char at 2x */

#define COL_BG     0x0000
#define COL_FG     0xFFFF
#define COL_GRID   0x2104
#define COL_WAIT   0x07E0
#define COL_WORK   0x07FF
#define COL_OFF    0xF800
#define COL_CONN   0xFD20
#define COL_BOOT   0xBDF7

/* parser modes */
enum {
    SM_PLAIN = 0,
    SM_ESC,
    SM_CSI,          /* inside ESC [ ... final byte */
    SM_OSC,          /* inside ESC ] ... BEL / ST  */
    SM_CS,           /* ESC ( ) * + : expect one charset byte to drop */
    SM_ESC_ST,       /* ESC ] ... ESC \ (ST): drop backslash */
};

static uint8_t s_grid[LCDVT_ROWS][LCDVT_COLS];
static uint64_t s_dirty;            /* bit per row */
static uint8_t s_r, s_c;            /* cursor */

static lcd_vt_state_t s_status = LCD_ST_BOOT;
static char s_title[40] = "VibeKey";
static bool s_header_dirty = true;

/* --- parsing state, resumable across feed() chunks --- */
static uint8_t s_mode = SM_PLAIN;
static char s_csi[24];              /* raw param text until final byte */
static uint8_t s_csi_len;
static char s_osc[40];              /* OSC payload capture (title) */
static uint8_t s_osc_len;
static char s_osc_pre[4];           /* OSC leading type digits before ';' */
static uint8_t s_osc_pre_len;
static bool s_osc_cap;              /* capturing after a valid 0;/2; prefix */
static char s_save_r, s_save_c;

static void dirty_row(int r)
{
    if (r >= 0 && r < LCDVT_ROWS) {
        s_dirty |= 1ULL << r;
    }
}

static void clear_rows(int r0, int r1)
{
    if (r0 < 0) r0 = 0;
    if (r1 >= LCDVT_ROWS) r1 = LCDVT_ROWS - 1;
    for (int r = r0; r <= r1; r++) {
        memset(s_grid[r], ' ', LCDVT_COLS);
        dirty_row(r);
    }
}

/* scroll the whole buffer; dir=+1 content up (LF at bottom), dir=-1 down. */
static void scroll_buf(int dir)
{
    if (dir > 0) {
        for (int r = 1; r < LCDVT_ROWS; r++) {
            memcpy(s_grid[r - 1], s_grid[r], LCDVT_COLS);
        }
        memset(s_grid[LCDVT_ROWS - 1], ' ', LCDVT_COLS);
    } else {
        for (int r = LCDVT_ROWS - 1; r > 0; r--) {
            memcpy(s_grid[r], s_grid[r - 1], LCDVT_COLS);
        }
        memset(s_grid[0], ' ', LCDVT_COLS);
    }
    s_dirty = ~0ULL;
}

static void linefeed(void)
{
    if (s_r >= LCDVT_ROWS - 1) {
        scroll_buf(1);
    } else {
        s_r++;
    }
}

static void putc_vt(char ch)
{
    s_grid[s_r][s_c] = (uint8_t) ch;
    dirty_row(s_r);
    if (++s_c >= LCDVT_COLS) {
        s_c = 0;
        linefeed();
    }
}

/* --- CSI parameter parse --- */
static void csi_exec(void)
{
    int p[8] = {0};
    int pc = 0;
    int cur = 0;
    bool have = false;
    for (int i = 0; i < s_csi_len; i++) {
        char ch = s_csi[i];
        if (ch == '?') {            /* DEC private: semantics ignored */
            continue;
        }
        if (ch >= '0' && ch <= '9') {
            cur = cur * 10 + (ch - '0');
            have = true;
        } else if (ch == ';') {
            if (pc < 8) {
                p[pc++] = have ? cur : 0;
            }
            cur = 0;
            have = false;
        } else {
            /* intermediate bytes (space, !) precede the final byte; skip */
        }
    }
    if (pc < 8) {
        p[pc++] = have ? cur : 0;
    }
    int p0 = pc > 0 && p[0] > 0 ? p[0] : 1;
    int p1 = pc > 1 && p[1] > 0 ? p[1] : 1;
    char fin = s_csi[s_csi_len - 1];
    int r = s_r;
    int c = s_c;

    switch (fin) {
    case 'H': case 'f':            /* cursor home / position */
        s_r = (uint8_t)((p0 > LCDVT_ROWS) ? LCDVT_ROWS - 1 : p0 - 1);
        s_c = (uint8_t)((p1 > LCDVT_COLS) ? LCDVT_COLS - 1 : p1 - 1);
        break;
    case 'A': s_r = (uint8_t)((s_r - p0) < 0 ? 0 : s_r - p0); break;
    case 'B': s_r = (uint8_t)((s_r + p0) > LCDVT_ROWS - 1 ? LCDVT_ROWS - 1 : s_r + p0); break;
    case 'C': s_c = (uint8_t)((s_c + p0) > LCDVT_COLS - 1 ? LCDVT_COLS - 1 : s_c + p0); break;
    case 'D': s_c = (uint8_t)((s_c - p0) < 0 ? 0 : s_c - p0); break;
    case 'G': s_c = (uint8_t)((p0 > LCDVT_COLS) ? LCDVT_COLS - 1 : p0 - 1); break;
    case 'd': s_r = (uint8_t)((p0 > LCDVT_ROWS) ? LCDVT_ROWS - 1 : p0 - 1); break;
    case 'J':                       /* erase display */
        if (p[0] == 2 || p[0] == 3 || (pc == 1 && p[0] == 0 && s_r == 0 && s_c == 0)) {
            clear_rows(0, LCDVT_ROWS - 1);
        } else if (p[0] == 0) {
            clear_rows(s_r + 1, LCDVT_ROWS - 1);
            for (int cc = s_c; cc < LCDVT_COLS; cc++) {
                s_grid[s_r][cc] = ' ';
            }
            dirty_row(s_r);
        } else if (p[0] == 1) {
            for (int cc = 0; cc <= s_c; cc++) {
                s_grid[s_r][cc] = ' ';
            }
            dirty_row(s_r);
            clear_rows(0, s_r - 1);
        } else {
            clear_rows(0, LCDVT_ROWS - 1);
        }
        break;
    case 'K':                       /* erase line */
        if (p[0] == 0) {
            for (int cc = s_c; cc < LCDVT_COLS; cc++) s_grid[s_r][cc] = ' ';
        } else if (p[0] == 1) {
            for (int cc = 0; cc <= s_c; cc++) s_grid[s_r][cc] = ' ';
        } else {
            memset(s_grid[s_r], ' ', LCDVT_COLS);
        }
        dirty_row(s_r);
        break;
    case 'X': {                     /* erase n chars */
        int n = p0;
        for (int cc = s_c; cc < LCDVT_COLS && n > 0; cc++, n--) s_grid[s_r][cc] = ' ';
        dirty_row(s_r);
        break;
    }
    case 'L': {                     /* insert n blank lines (down) */
        int n = p0;
        if (n > LCDVT_ROWS - 1 - s_r) n = LCDVT_ROWS - 1 - s_r;
        for (int rr = LCDVT_ROWS - 1; rr > s_r + n - 1 && rr - n >= s_r; rr--) {
            memcpy(s_grid[rr], s_grid[rr - n], LCDVT_COLS);
        }
        for (int rr = s_r; rr < s_r + n && rr < LCDVT_ROWS; rr++) {
            memset(s_grid[rr], ' ', LCDVT_COLS);
        }
        for (int rr = s_r; rr < LCDVT_ROWS; rr++) dirty_row(rr);
        break;
    }
    case 'M': {                     /* delete n lines (up) */
        int n = p0;
        if (n > LCDVT_ROWS - 1 - s_r) n = LCDVT_ROWS - 1 - s_r;
        for (int rr = s_r; rr + n < LCDVT_ROWS; rr++) {
            memcpy(s_grid[rr], s_grid[rr + n], LCDVT_COLS);
        }
        clear_rows(LCDVT_ROWS - n, LCDVT_ROWS - 1);
        for (int rr = s_r; rr < LCDVT_ROWS; rr++) dirty_row(rr);
        break;
    }
    case 'P': {                     /* delete n chars left of cursor in row */
        int n = p0;
        if (n > LCDVT_COLS - 1 - s_c) n = LCDVT_COLS - 1 - s_c;
        for (int cc = s_c; cc + n < LCDVT_COLS; cc++) {
            s_grid[s_r][cc] = s_grid[s_r][cc + n];
        }
        for (int cc = LCDVT_COLS - n; cc < LCDVT_COLS; cc++) s_grid[s_r][cc] = ' ';
        dirty_row(s_r);
        break;
    }
    case '@': {                     /* insert n blanks at cursor in row */
        int n = p0;
        if (n > LCDVT_COLS - 1 - s_c) n = LCDVT_COLS - 1 - s_c;
        for (int cc = LCDVT_COLS - 1; cc >= s_c + n; cc--) {
            s_grid[s_r][cc] = s_grid[s_r][cc - n];
        }
        for (int cc = s_c; cc < s_c + n && cc < LCDVT_COLS; cc++) s_grid[s_r][cc] = ' ';
        dirty_row(s_r);
        break;
    }
    case 'm':                       /* SGR color: panel is single-color; ignore */
    default:
        (void) r;
        (void) c;
        break;
    }
    dirty_row(s_r);
}

void lcd_vt_feed(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char ch = (char) data[i];
        if (s_mode == SM_PLAIN) {
            if (ch == 0x1b) {
                s_mode = SM_ESC;
            } else if (ch == '\r') {
                s_c = 0;
                dirty_row(s_r);
            } else if (ch == '\n') {
                if (s_c == 0 && s_r < LCDVT_ROWS - 1) {
                    s_r++;
                    dirty_row(s_r);
                } else {
                    linefeed();
                }
            } else if (ch == '\b') {
                if (s_c > 0) s_c--;
                dirty_row(s_r);
            } else if (ch == '\t') {
                s_c = (uint8_t)((s_c / 8 + 1) * 8);
                if (s_c >= LCDVT_COLS) {
                    s_c = LCDVT_COLS - 1;
                }
            } else if (ch == 0x0c) {   /* form feed ~ clear */
                clear_rows(0, LCDVT_ROWS - 1);
                s_r = 0;
                s_c = 0;
            } else if (ch >= 0x20 && ch <= 0x7e) {
                putc_vt(ch);
            } else {
                /* bell, controls, and >=0x80 UTF-8 continuation: ignored */
            }
        } else if (s_mode == SM_ESC) {
            if (ch == '[') {
                s_csi_len = 0;
                s_mode = SM_CSI;
            } else if (ch == ']') {
                s_osc_len = 0;
                s_osc_pre_len = 0;
                s_osc_cap = false;
                s_mode = SM_OSC;
            } else if (ch == '(' || ch == ')' || ch == '*' || ch == '+') {
                s_mode = SM_CS;
            } else if (ch == 'c') {
                clear_rows(0, LCDVT_ROWS - 1);
                s_r = 0;
                s_c = 0;
                s_mode = SM_PLAIN;
            } else if (ch == 'D') {
                linefeed();
                s_mode = SM_PLAIN;
            } else if (ch == 'M') {     /* reverse index */
                if (s_r == 0) {
                    scroll_buf(-1);
                } else {
                    s_r--;
                    dirty_row(s_r);
                }
                s_mode = SM_PLAIN;
            } else if (ch == 'E') {
                s_c = 0;
                linefeed();
                s_mode = SM_PLAIN;
            } else if (ch == '7') {
                s_save_r = s_r;
                s_save_c = s_c;
                s_mode = SM_PLAIN;
            } else if (ch == '8') {
                s_r = s_save_r;
                s_c = s_save_c;
                dirty_row(s_r);
                s_mode = SM_PLAIN;
            } else {
                s_mode = SM_PLAIN;      /* ignore everything else */
            }
        } else if (s_mode == SM_CSI) {
            if (ch >= 0x40 && ch <= 0x7e) {
                if (s_csi_len < (int) sizeof(s_csi)) {
                    s_csi[s_csi_len++] = ch;
                }
                csi_exec();
                s_mode = SM_PLAIN;
            } else if (s_csi_len < (int) sizeof(s_csi)) {
                s_csi[s_csi_len++] = ch;
            }
        } else if (s_mode == SM_OSC) {
            if (ch == 0x07) {           /* BEL terminates OSC */
                s_mode = SM_PLAIN;
                if (s_osc_cap && s_osc_len) {
                    s_osc[s_osc_len] = 0;
                    lcd_vt_set_status(s_status, s_osc);
                }
            } else if (ch == 0x1b) {
                s_mode = SM_ESC_ST;     /* expect ST backslash */
            } else if ((unsigned char) ch < 0x20 || (unsigned char) ch > 0x7e) {
                /* C1/control bytes in payload: ignore */
            } else if (!s_osc_cap) {
                /* leading type digits, then ';' starts capture for 0/2 only
                 * (window title), so OSC 8 hyperlinks never clobber it. */
                if (ch >= '0' && ch <= '9') {
                    if (s_osc_pre_len < sizeof(s_osc_pre) - 1) {
                        s_osc_pre[s_osc_pre_len++] = ch;
                    }
                } else if (ch == ';' && s_osc_pre_len == 1 &&
                           (s_osc_pre[0] == '0' || s_osc_pre[0] == '2')) {
                    s_osc_cap = true;
                    s_osc_len = 0;
                }
            } else if (s_osc_len < (int) sizeof(s_osc) - 1) {
                s_osc[s_osc_len++] = ch;
            }
        } else if (s_mode == SM_ESC_ST) {
            s_mode = SM_PLAIN;
        } else if (s_mode == SM_CS) {
            s_mode = SM_PLAIN;          /* drop the charset byte */
        }
    }
}

void lcd_vt_feed_baseline(const uint8_t *data, size_t len)
{
    clear_rows(0, LCDVT_ROWS - 1);
    s_r = 0;
    s_c = 0;
    s_mode = SM_PLAIN;
    lcd_vt_feed(data, len);
}

/* --- rendering --- */

static uint16_t status_color(void)
{
    switch (s_status) {
    case LCD_ST_WAITING:   return COL_WAIT;
    case LCD_ST_WORKING:   return COL_WORK;
    case LCD_ST_OFFLINE:   return COL_OFF;
    case LCD_ST_CONNECTING:return COL_CONN;
    default:               return COL_BOOT;
    }
}

static const char *status_label(void)
{
    switch (s_status) {
    case LCD_ST_WAITING:    return "WAITING";
    case LCD_ST_WORKING:    return "WORKING";
    case LCD_ST_OFFLINE:    return "OFFLINE";
    case LCD_ST_CONNECTING: return "LINKING";
    default:                return "BOOT";
    }
}

void lcd_vt_set_status(lcd_vt_state_t st, const char *title)
{
    s_status = st;
    if (title && title[0]) {
        snprintf(s_title, sizeof(s_title), "%s", title);
        for (char *p = s_title; *p; p++) {
            if ((unsigned char) *p < 0x20 || (unsigned char) *p > 0x7e) {
                *p = '?';
            }
        }
    }
    s_header_dirty = true;
}

static void draw_header(void)
{
    uint16_t col = status_color();
    /* whole band in the status color; text on it in black for contrast */
    st7789_fill_rect(0, 0, ST7789_WIDTH, HEADER_H, col);
    st7789_fill_rect(0, HEADER_H - 2, ST7789_WIDTH, 2, COL_GRID);

    int label_x = 6;
    st7789_draw_text(status_label(), label_x, 2, COL_BG, col, LCD_SCALE);

    /* instance title, truncated to the space right of an 8-char label zone */
    const int title_x = label_x + 8 * CHAR_W;      /* 102 */
    int maxc = (ST7789_WIDTH - title_x - 4) / CHAR_W;
    if (maxc < 1) {
        maxc = 1;
    }
    int n = (int) strlen(s_title);
    if (n > maxc) {
        n = maxc;
    }
    if (n > 0) {
        char line[LCDVT_COLS + 1];
        memcpy(line, s_title, n);
        line[n] = 0;
        st7789_draw_text(line, title_x, 2, COL_BG, col, LCD_SCALE);
    }
    s_header_dirty = false;
}

static void draw_row(int r)
{
    char line[LCDVT_COLS + 1];
    int n = 0;
    for (int c = 0; c < LCDVT_COLS; c++) {
        unsigned char ch = s_grid[r][c];
        line[n++] = (ch >= 0x20 && ch <= 0x7e) ? (char) ch : ' ';
    }
    while (n > 0 && line[n - 1] == ' ') {
        n--;
    }
    int y = TERM_Y + r * ROW_STEP;
    /* clear the whole grid row first so shrunk text leaves no ghost pixels */
    st7789_fill_rect(X_MARGIN, y, LCDVT_COLS * CHAR_W,
                     ST7789_FONT_H * LCD_SCALE, COL_BG);
    if (n > 0) {
        line[n] = 0;
        st7789_draw_text(line, X_MARGIN, y, COL_FG, COL_BG, LCD_SCALE);
    }
}

void lcd_vt_poll(void)
{
    bool need_commit = false;
    if (s_header_dirty) {
        draw_header();
        need_commit = true;
    }
    if (s_dirty != 0) {
        need_commit = true;
        uint64_t mask = s_dirty;
        s_dirty = 0;
        for (int r = 0; r < LCDVT_ROWS; r++) {
            if (mask & (1ULL << r)) {
                draw_row(r);
            }
        }
    }
    if (need_commit) {
        st7789_commit();
    }
}

void lcd_vt_init(void)
{
    clear_rows(0, LCDVT_ROWS - 1);
    s_r = 0;
    s_c = 0;
    s_mode = SM_PLAIN;
    st7789_fill_screen(COL_BG);
    s_header_dirty = true;
    lcd_vt_set_status(LCD_ST_BOOT, "VibeKey");
    draw_header();
    st7789_commit();
}

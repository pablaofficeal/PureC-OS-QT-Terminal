#include "vtscreen.h"

static const int ScrollbackMax = 5000;

static const QRgb Palette16[16] = {
    0x1E1E2E, 0xF38BA8, 0xA6E3A1, 0xF9E2AF, 0x89B4FA, 0xF5C2E7, 0x94E2D5, 0xBAC2DE,
    0x585B70, 0xF38BA8, 0xA6E3A1, 0xF9E2AF, 0x89B4FA, 0xF5C2E7, 0x94E2D5, 0xA6ADC8,
};

static QRgb color256(int n)
{
    if (n < 16)
        return Palette16[n];
    if (n < 232) {
        n -= 16;
        static const int steps[6] = {0, 95, 135, 175, 215, 255};
        return qRgb(steps[n / 36], steps[(n / 6) % 6], steps[n % 6]);
    }
    int v = 8 + (n - 232) * 10;
    return qRgb(v, v, v);
}

VtScreen::VtScreen(int cols, int rows)
    : m_cols(qMax(cols, 2)), m_rows(qMax(rows, 2)), m_screen(&m_main)
{
    m_pen.attrs = DefaultFg | DefaultBg;
    m_savedPen = m_pen;
    m_main = QVector<QVector<Cell>>(m_rows, QVector<Cell>(m_cols, blank()));
    m_altBuf = m_main;
    m_bottom = m_rows - 1;
}

Cell VtScreen::blank() const
{
    Cell c;
    c.bg = m_pen.bg;
    c.attrs = DefaultFg | (m_pen.attrs & DefaultBg);
    return c;
}

int VtScreen::param(int index, int fallback) const
{
    if (index >= m_args.size() || m_args[index] <= 0)
        return fallback;
    return m_args[index];
}

void VtScreen::clampCursor()
{
    m_cx = qBound(0, m_cx, m_cols - 1);
    m_cy = qBound(0, m_cy, m_rows - 1);
    m_wrapPending = false;
}

void VtScreen::resize(int cols, int rows)
{
    cols = qMax(cols, 2);
    rows = qMax(rows, 2);
    if (cols == m_cols && rows == m_rows)
        return;
    auto fit = [&](QVector<QVector<Cell>> &screen, bool keepBottom) {
        while (screen.size() > rows) {
            if (keepBottom && m_cy > 0) {
                if (&screen == &m_main)
                    m_scrollback.push_back(screen.front());
                screen.removeFirst();
                if (&screen == m_screen)
                    m_cy--;
            } else {
                screen.removeLast();
            }
        }
        while (screen.size() < rows)
            screen.append(QVector<Cell>(cols, blank()));
        for (auto &line : screen)
            line.resize(cols);
    };
    fit(m_main, m_screen == &m_main);
    fit(m_altBuf, m_screen == &m_altBuf);
    while (int(m_scrollback.size()) > ScrollbackMax)
        m_scrollback.pop_front();
    m_cols = cols;
    m_rows = rows;
    m_top = 0;
    m_bottom = rows - 1;
    clampCursor();
}

void VtScreen::feed(const char *data, int size)
{
    for (int i = 0; i < size; ++i) {
        unsigned char b = static_cast<unsigned char>(data[i]);
        switch (m_state) {
        case Ground:
            if (m_utf8Left) {
                if ((b & 0xC0) == 0x80) {
                    m_utf8 = (m_utf8 << 6) | (b & 0x3F);
                    if (--m_utf8Left == 0)
                        putChar(m_utf8);
                    continue;
                }
                m_utf8Left = 0;
                putChar(U'�');
            }
            if (b == 0x1B) {
                m_state = Escape;
            } else if (b < 0x20 || b == 0x7F) {
                control(char(b));
            } else if (b < 0x80) {
                putChar(b);
            } else if ((b & 0xE0) == 0xC0) {
                m_utf8 = b & 0x1F;
                m_utf8Left = 1;
            } else if ((b & 0xF0) == 0xE0) {
                m_utf8 = b & 0x0F;
                m_utf8Left = 2;
            } else if ((b & 0xF8) == 0xF0) {
                m_utf8 = b & 0x07;
                m_utf8Left = 3;
            } else {
                putChar(U'�');
            }
            break;
        case Escape:
            escape(char(b));
            break;
        case Csi:
            if (b >= 0x40 && b <= 0x7E) {
                m_state = Ground;
                m_args.clear();
                for (const QByteArray &part : m_params.split(';'))
                    m_args.append(part.isEmpty() ? 0 : part.toInt());
                csi(char(b));
            } else if (b == '?' || b == '>' || b == '=') {
                m_private = true;
            } else if (b < 0x20) {
                control(char(b));
            } else if (m_params.size() < 64) {
                m_params.append(char(b));
            }
            break;
        case Osc:
            if (b == 0x07) {
                osc();
                m_state = Ground;
            } else if (b == 0x1B) {
                m_state = OscEscape;
            } else if (m_osc.size() < 512) {
                m_osc.append(char(b));
            }
            break;
        case OscEscape:
            osc();
            m_state = Ground;
            if (b != '\\')
                escape(char(b));
            break;
        case Charset:
            m_state = Ground;
            break;
        }
    }
}

void VtScreen::putChar(char32_t ch)
{
    if (m_wrapPending && m_autowrap) {
        m_cx = 0;
        newline();
    }
    m_wrapPending = false;
    Cell &c = row(m_cy)[m_cx];
    c = m_pen;
    c.ch = ch;
    if (m_cx == m_cols - 1)
        m_wrapPending = true;
    else
        m_cx++;
}

void VtScreen::control(char c)
{
    switch (c) {
    case '\r':
        m_cx = 0;
        m_wrapPending = false;
        break;
    case '\n':
    case '\v':
    case '\f':
        newline();
        break;
    case '\b':
        if (m_cx > 0)
            m_cx--;
        m_wrapPending = false;
        break;
    case '\t':
        m_cx = qMin(m_cols - 1, (m_cx / 8 + 1) * 8);
        break;
    default:
        break;
    }
}

void VtScreen::newline()
{
    index();
}

void VtScreen::index()
{
    if (m_cy == m_bottom)
        scrollUp(m_top, m_bottom, 1);
    else if (m_cy < m_rows - 1)
        m_cy++;
    m_wrapPending = false;
}

void VtScreen::reverseIndex()
{
    if (m_cy == m_top)
        scrollDown(m_top, m_bottom, 1);
    else if (m_cy > 0)
        m_cy--;
}

void VtScreen::scrollUp(int top, int bottom, int n)
{
    n = qMin(n, bottom - top + 1);
    for (int i = 0; i < n; ++i) {
        if (m_screen == &m_main && top == 0) {
            m_scrollback.push_back(row(top));
            if (int(m_scrollback.size()) > ScrollbackMax)
                m_scrollback.pop_front();
        }
        m_screen->removeAt(top);
        m_screen->insert(bottom, QVector<Cell>(m_cols, blank()));
    }
}

void VtScreen::scrollDown(int top, int bottom, int n)
{
    n = qMin(n, bottom - top + 1);
    for (int i = 0; i < n; ++i) {
        m_screen->removeAt(bottom);
        m_screen->insert(top, QVector<Cell>(m_cols, blank()));
    }
}

void VtScreen::eraseCells(int y, int x0, int x1)
{
    QVector<Cell> &line = row(y);
    for (int x = qMax(0, x0); x < qMin(m_cols, x1); ++x)
        line[x] = blank();
}

void VtScreen::escape(char c)
{
    m_state = Ground;
    switch (c) {
    case '[':
        m_state = Csi;
        m_params.clear();
        m_private = false;
        break;
    case ']':
        m_state = Osc;
        m_osc.clear();
        break;
    case '(':
    case ')':
    case '*':
    case '+':
        m_state = Charset;
        break;
    case '7':
        m_savedX = m_cx;
        m_savedY = m_cy;
        m_savedPen = m_pen;
        break;
    case '8':
        m_cx = m_savedX;
        m_cy = m_savedY;
        m_pen = m_savedPen;
        clampCursor();
        break;
    case 'D':
        index();
        break;
    case 'E':
        m_cx = 0;
        index();
        break;
    case 'M':
        reverseIndex();
        break;
    case 'c':
        m_pen = Cell();
        m_pen.attrs = DefaultFg | DefaultBg;
        switchAlt(false);
        for (int y = 0; y < m_rows; ++y)
            eraseCells(y, 0, m_cols);
        m_cx = m_cy = 0;
        m_top = 0;
        m_bottom = m_rows - 1;
        m_cursorVisible = true;
        m_appCursor = false;
        m_autowrap = true;
        break;
    default:
        break;
    }
}

void VtScreen::switchAlt(bool on)
{
    if (on == m_alt)
        return;
    m_alt = on;
    if (on) {
        m_savedX = m_cx;
        m_savedY = m_cy;
        m_savedPen = m_pen;
        m_screen = &m_altBuf;
        for (int y = 0; y < m_rows; ++y)
            eraseCells(y, 0, m_cols);
    } else {
        m_screen = &m_main;
        m_cx = m_savedX;
        m_cy = m_savedY;
        m_pen = m_savedPen;
    }
    m_top = 0;
    m_bottom = m_rows - 1;
    clampCursor();
}

void VtScreen::setMode(bool on)
{
    for (int mode : m_args) {
        if (!m_private) {
            continue;
        }
        switch (mode) {
        case 1: m_appCursor = on; break;
        case 7: m_autowrap = on; break;
        case 25: m_cursorVisible = on; break;
        case 47:
        case 1047:
        case 1049: switchAlt(on); break;
        case 2004: m_bracketedPaste = on; break;
        default: break;
        }
    }
}

void VtScreen::csi(char final)
{
    int n = param(0, 1);
    switch (final) {
    case '@': {
        QVector<Cell> &line = row(m_cy);
        for (int i = 0; i < n && m_cx < m_cols; ++i) {
            line.insert(m_cx, blank());
            line.removeLast();
        }
        break;
    }
    case 'A': m_cy = qMax(m_cy - n, m_cy >= m_top ? m_top : 0); m_wrapPending = false; break;
    case 'B': m_cy = qMin(m_cy + n, m_cy <= m_bottom ? m_bottom : m_rows - 1); m_wrapPending = false; break;
    case 'C': m_cx += n; clampCursor(); break;
    case 'D': m_cx -= n; clampCursor(); break;
    case 'E': m_cx = 0; m_cy += n; clampCursor(); break;
    case 'F': m_cx = 0; m_cy -= n; clampCursor(); break;
    case 'G':
    case '`': m_cx = n - 1; clampCursor(); break;
    case 'd': m_cy = n - 1; clampCursor(); break;
    case 'H':
    case 'f':
        m_cy = param(0, 1) - 1;
        m_cx = param(1, 1) - 1;
        clampCursor();
        break;
    case 'J': {
        int mode = m_args.isEmpty() ? 0 : m_args[0];
        if (mode == 0) {
            eraseCells(m_cy, m_cx, m_cols);
            for (int y = m_cy + 1; y < m_rows; ++y)
                eraseCells(y, 0, m_cols);
        } else if (mode == 1) {
            for (int y = 0; y < m_cy; ++y)
                eraseCells(y, 0, m_cols);
            eraseCells(m_cy, 0, m_cx + 1);
        } else {
            for (int y = 0; y < m_rows; ++y)
                eraseCells(y, 0, m_cols);
            if (mode == 3)
                m_scrollback.clear();
        }
        break;
    }
    case 'K': {
        int mode = m_args.isEmpty() ? 0 : m_args[0];
        if (mode == 0)
            eraseCells(m_cy, m_cx, m_cols);
        else if (mode == 1)
            eraseCells(m_cy, 0, m_cx + 1);
        else
            eraseCells(m_cy, 0, m_cols);
        break;
    }
    case 'L':
        if (m_cy >= m_top && m_cy <= m_bottom)
            scrollDown(m_cy, m_bottom, n);
        break;
    case 'M':
        if (m_cy >= m_top && m_cy <= m_bottom) {
            n = qMin(n, m_bottom - m_cy + 1);
            for (int i = 0; i < n; ++i) {
                m_screen->removeAt(m_cy);
                m_screen->insert(m_bottom, QVector<Cell>(m_cols, blank()));
            }
        }
        break;
    case 'P': {
        QVector<Cell> &line = row(m_cy);
        for (int i = 0; i < n && m_cx < line.size(); ++i) {
            line.removeAt(m_cx);
            line.append(blank());
        }
        break;
    }
    case 'S': scrollUp(m_top, m_bottom, n); break;
    case 'T': scrollDown(m_top, m_bottom, n); break;
    case 'X': eraseCells(m_cy, m_cx, m_cx + n); break;
    case 'm': sgr(); break;
    case 'r': {
        int top = param(0, 1) - 1, bottom = param(1, m_rows) - 1;
        if (top < bottom && bottom < m_rows) {
            m_top = top;
            m_bottom = bottom;
            m_cx = 0;
            m_cy = 0;
        }
        break;
    }
    case 's':
        m_savedX = m_cx;
        m_savedY = m_cy;
        break;
    case 'u':
        m_cx = m_savedX;
        m_cy = m_savedY;
        clampCursor();
        break;
    case 'h': setMode(true); break;
    case 'l': setMode(false); break;
    case 'n':
        if (reply && param(0, 0) == 6)
            reply(QByteArray("\x1b[") + QByteArray::number(m_cy + 1) + ';' + QByteArray::number(m_cx + 1) + 'R');
        else if (reply && param(0, 0) == 5)
            reply("\x1b[0n");
        break;
    case 'c':
        if (reply && !m_private)
            reply("\x1b[?1;2c");
        break;
    default:
        break;
    }
}

void VtScreen::sgr()
{
    if (m_args.isEmpty())
        m_args.append(0);
    for (int i = 0; i < m_args.size(); ++i) {
        int a = m_args[i];
        if (a == 0) {
            m_pen = Cell();
            m_pen.attrs = DefaultFg | DefaultBg;
        } else if (a == 1) {
            m_pen.attrs |= Bold;
        } else if (a == 2) {
            m_pen.attrs |= Faint;
        } else if (a == 4) {
            m_pen.attrs |= Underline;
        } else if (a == 7) {
            m_pen.attrs |= Inverse;
        } else if (a == 22) {
            m_pen.attrs &= ~(Bold | Faint);
        } else if (a == 24) {
            m_pen.attrs &= ~Underline;
        } else if (a == 27) {
            m_pen.attrs &= ~Inverse;
        } else if (a >= 30 && a <= 37) {
            m_pen.fg = Palette16[a - 30];
            m_pen.attrs &= ~DefaultFg;
        } else if (a >= 90 && a <= 97) {
            m_pen.fg = Palette16[a - 90 + 8];
            m_pen.attrs &= ~DefaultFg;
        } else if (a == 39) {
            m_pen.attrs |= DefaultFg;
        } else if (a >= 40 && a <= 47) {
            m_pen.bg = Palette16[a - 40];
            m_pen.attrs &= ~DefaultBg;
        } else if (a >= 100 && a <= 107) {
            m_pen.bg = Palette16[a - 100 + 8];
            m_pen.attrs &= ~DefaultBg;
        } else if (a == 49) {
            m_pen.attrs |= DefaultBg;
        } else if (a == 38 || a == 48) {
            QRgb color = 0;
            bool ok = false;
            if (i + 2 < m_args.size() && m_args[i + 1] == 5) {
                color = color256(qBound(0, m_args[i + 2], 255));
                i += 2;
                ok = true;
            } else if (i + 4 < m_args.size() && m_args[i + 1] == 2) {
                color = qRgb(m_args[i + 2] & 255, m_args[i + 3] & 255, m_args[i + 4] & 255);
                i += 4;
                ok = true;
            }
            if (!ok)
                break;
            if (a == 38) {
                m_pen.fg = color;
                m_pen.attrs &= ~DefaultFg;
            } else {
                m_pen.bg = color;
                m_pen.attrs &= ~DefaultBg;
            }
        }
    }
}

void VtScreen::osc()
{
    int semi = m_osc.indexOf(';');
    if (semi < 0)
        return;
    int code = m_osc.left(semi).toInt();
    if (code == 0 || code == 2) {
        m_title = QString::fromUtf8(m_osc.mid(semi + 1));
        if (titleChanged)
            titleChanged(m_title);
    }
}

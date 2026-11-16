#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtGui/QRgb>
#include <deque>
#include <functional>

struct Cell {
    char32_t ch = U' ';
    QRgb fg = 0;
    QRgb bg = 0;
    quint8 attrs = 0;
};

class VtScreen
{
public:
    enum Attr : quint8 {
        Bold = 1,
        Faint = 2,
        Underline = 4,
        Inverse = 8,
        DefaultFg = 16,
        DefaultBg = 32,
    };

    VtScreen(int cols, int rows);

    void feed(const char *data, int size);
    void resize(int cols, int rows);

    int cols() const { return m_cols; }
    int rows() const { return m_rows; }
    int cursorX() const { return m_cx; }
    int cursorY() const { return m_cy; }
    bool cursorVisible() const { return m_cursorVisible; }
    bool appCursorKeys() const { return m_appCursor; }
    bool bracketedPaste() const { return m_bracketedPaste; }
    bool altScreen() const { return m_alt; }
    int scrollbackSize() const { return int(m_scrollback.size()); }
    const QVector<Cell> &scrollbackLine(int index) const { return m_scrollback[size_t(index)]; }
    const QVector<Cell> &screenLine(int y) const { return (*m_screen)[y]; }
    QString title() const { return m_title; }

    std::function<void(const QByteArray &)> reply;
    std::function<void(const QString &)> titleChanged;

private:
    enum State { Ground, Escape, Csi, Osc, OscEscape, Charset };

    void putChar(char32_t ch);
    void control(char c);
    void escape(char c);
    void csi(char final);
    void osc();
    void sgr();
    void newline();
    void index();
    void reverseIndex();
    void scrollUp(int top, int bottom, int n);
    void scrollDown(int top, int bottom, int n);
    void eraseCells(int y, int x0, int x1);
    void clampCursor();
    void setMode(bool on);
    void switchAlt(bool on);
    Cell blank() const;
    int param(int index, int fallback) const;
    QVector<Cell> &row(int y) { return (*m_screen)[y]; }

    int m_cols, m_rows;
    QVector<QVector<Cell>> m_main, m_altBuf;
    QVector<QVector<Cell>> *m_screen;
    std::deque<QVector<Cell>> m_scrollback;
    int m_cx = 0, m_cy = 0;
    int m_savedX = 0, m_savedY = 0;
    Cell m_pen;
    Cell m_savedPen;
    bool m_wrapPending = false;
    int m_top = 0, m_bottom = 0;
    bool m_cursorVisible = true;
    bool m_appCursor = false;
    bool m_autowrap = true;
    bool m_bracketedPaste = false;
    bool m_alt = false;

    State m_state = Ground;
    QByteArray m_params;
    bool m_private = false;
    QVector<int> m_args;
    QByteArray m_osc;
    QString m_title;
    char32_t m_utf8 = 0;
    int m_utf8Left = 0;
};

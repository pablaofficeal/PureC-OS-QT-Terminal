#include "terminalwidget.h"

#include <QtGui/QClipboard>
#include <QtGui/QFontDatabase>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>

static const QRgb DefaultFg = 0xCDD6F4;
static const QRgb DefaultBg = 0x1E1E2E;
static const QRgb CursorColor = 0xF5E0DC;
static const QRgb SelectionColor = 0x45475A;
static const int Margin = 4;

TerminalWidget::TerminalWidget(QWidget *parent)
    : QWidget(parent), m_screen(100, 30)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_InputMethodEnabled, false);
    QFont font(QStringLiteral("DejaVu Sans Mono"));
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(15);
    setFont(font);
    QFontMetrics fm(font);
    m_cellW = qMax(1, fm.horizontalAdvance(QLatin1Char('M')));
    m_cellH = qMax(1, fm.height());
    m_ascent = fm.ascent();
    m_repaint.setSingleShot(true);
    m_repaint.setInterval(8);
    connect(&m_repaint, &QTimer::timeout, this, [this] { update(); });
    m_screen.reply = [this](const QByteArray &data) { m_pty.write(data); };
    m_screen.titleChanged = [this](const QString &t) { emit titleChanged(t); };
    connect(&m_pty, &PtySession::received, this, &TerminalWidget::received);
    connect(&m_pty, &PtySession::finished, this, [this](int) { emit finished(); });
}

QSize TerminalWidget::sizeHint() const
{
    return QSize(100 * m_cellW + 2 * Margin, 30 * m_cellH + 2 * Margin);
}

bool TerminalWidget::start()
{
    return m_pty.start(m_screen.cols(), m_screen.rows());
}

QString TerminalWidget::title() const
{
    return m_screen.title();
}

void TerminalWidget::received(const QByteArray &data)
{
    m_screen.feed(data.constData(), int(data.size()));
    if (m_scroll && !m_screen.altScreen())
        m_scroll = qMin(m_scroll, m_screen.scrollbackSize());
    scheduleUpdate();
}

void TerminalWidget::scheduleUpdate()
{
    if (!m_repaint.isActive())
        m_repaint.start();
}

void TerminalWidget::resizeEvent(QResizeEvent *)
{
    int cols = qMax(2, (width() - 2 * Margin) / m_cellW);
    int rows = qMax(2, (height() - 2 * Margin) / m_cellH);
    if (cols != m_screen.cols() || rows != m_screen.rows()) {
        m_screen.resize(cols, rows);
        m_pty.resize(cols, rows);
    }
    update();
}

const QVector<Cell> &TerminalWidget::viewLine(int y) const
{
    int line = y - m_scroll;
    if (line < 0)
        return m_screen.scrollbackLine(m_screen.scrollbackSize() + line);
    return m_screen.screenLine(line);
}

bool TerminalWidget::selected(int line, int x) const
{
    if (m_selStart.y() < 0)
        return false;
    QPoint a = m_selStart, b = m_selEnd;
    if (b.y() < a.y() || (b.y() == a.y() && b.x() < a.x()))
        std::swap(a, b);
    if (line < a.y() || line > b.y())
        return false;
    if (line == a.y() && x < a.x())
        return false;
    if (line == b.y() && x > b.x())
        return false;
    return a != b;
}

void TerminalWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(DefaultBg));
    QFont normal = font();
    QFont bold = font();
    bold.setBold(true);
    int rows = m_screen.rows(), cols = m_screen.cols();
    int base = m_screen.scrollbackSize() - m_scroll;
    for (int y = 0; y < rows; ++y) {
        const QVector<Cell> &line = viewLine(y);
        int py = Margin + y * m_cellH;
        int x = 0;
        while (x < cols && x < line.size()) {
            const Cell &c0 = line[x];
            auto colors = [&](const Cell &c, int cx, QRgb &fg, QRgb &bg) {
                fg = (c.attrs & VtScreen::DefaultFg) ? DefaultFg : c.fg;
                bg = (c.attrs & VtScreen::DefaultBg) ? DefaultBg : c.bg;
                if (c.attrs & VtScreen::Inverse)
                    std::swap(fg, bg);
                if (c.attrs & VtScreen::Faint)
                    fg = qRgb(qRed(fg) * 2 / 3, qGreen(fg) * 2 / 3, qBlue(fg) * 2 / 3);
                if (selected(base + y, cx))
                    bg = SelectionColor;
            };
            QRgb fg0, bg0;
            colors(c0, x, fg0, bg0);
            int end = x + 1;
            QString text;
            text.append(QString::fromUcs4(&c0.ch, 1));
            while (end < cols && end < line.size()) {
                const Cell &c = line[end];
                QRgb fg, bg;
                colors(c, end, fg, bg);
                if (fg != fg0 || bg != bg0 || (c.attrs & (VtScreen::Bold | VtScreen::Underline)) !=
                                                    (c0.attrs & (VtScreen::Bold | VtScreen::Underline)))
                    break;
                text.append(QString::fromUcs4(&c.ch, 1));
                ++end;
            }
            QRect r(Margin + x * m_cellW, py, (end - x) * m_cellW, m_cellH);
            if (bg0 != DefaultBg)
                p.fillRect(r, QColor(bg0));
            if (!text.trimmed().isEmpty()) {
                p.setFont((c0.attrs & VtScreen::Bold) ? bold : normal);
                p.setPen(QColor(fg0));
                for (int i = 0; i < text.size(); ++i) {
                    QChar ch = text.at(i);
                    if (ch != QLatin1Char(' '))
                        p.drawText(Margin + (x + i) * m_cellW, py + m_ascent, QString(ch));
                }
                if (c0.attrs & VtScreen::Underline)
                    p.fillRect(QRect(r.x(), py + m_cellH - 2, r.width(), 1), QColor(fg0));
            }
            x = end;
        }
    }
    if (m_scroll == 0 && m_screen.cursorVisible()) {
        QRect cr(Margin + m_screen.cursorX() * m_cellW, Margin + m_screen.cursorY() * m_cellH, m_cellW, m_cellH);
        if (m_focused) {
            p.fillRect(cr, QColor(CursorColor));
            const Cell &c = m_screen.screenLine(m_screen.cursorY())[m_screen.cursorX()];
            if (c.ch != U' ') {
                p.setPen(QColor(DefaultBg));
                p.setFont((c.attrs & VtScreen::Bold) ? bold : normal);
                p.drawText(cr.x(), cr.y() + m_ascent, QString::fromUcs4(&c.ch, 1));
            }
        } else {
            p.setPen(QColor(CursorColor));
            p.drawRect(cr.adjusted(0, 0, -1, -1));
        }
    }
    if (m_scroll > 0) {
        QString info = QStringLiteral(" %1/%2 ").arg(m_scroll).arg(m_screen.scrollbackSize());
        QFontMetrics fm(normal);
        QRect ir(width() - fm.horizontalAdvance(info) - 8, 4, fm.horizontalAdvance(info), fm.height());
        p.fillRect(ir, QColor(SelectionColor));
        p.setPen(QColor(DefaultFg));
        p.drawText(ir, Qt::AlignCenter, info);
    }
}

void TerminalWidget::scrollView(int lines)
{
    if (m_screen.altScreen())
        return;
    m_scroll = qBound(0, m_scroll + lines, m_screen.scrollbackSize());
    update();
}

void TerminalWidget::send(const QByteArray &data)
{
    if (m_scroll) {
        m_scroll = 0;
        update();
    }
    m_pty.write(data);
}

void TerminalWidget::keyPressEvent(QKeyEvent *e)
{
    Qt::KeyboardModifiers mods = e->modifiers();
    bool ctrl = mods & Qt::ControlModifier, shift = mods & Qt::ShiftModifier, alt = mods & Qt::AltModifier;
    int key = e->key();
    if (ctrl && shift && key == Qt::Key_C) {
        copy();
        return;
    }
    if (ctrl && shift && key == Qt::Key_V) {
        paste();
        return;
    }
    if (shift && key == Qt::Key_PageUp) {
        scrollView(m_screen.rows() / 2);
        return;
    }
    if (shift && key == Qt::Key_PageDown) {
        scrollView(-m_screen.rows() / 2);
        return;
    }
    bool app = m_screen.appCursorKeys();
    QByteArray seq;
    switch (key) {
    case Qt::Key_Up: seq = app ? "\x1bOA" : "\x1b[A"; break;
    case Qt::Key_Down: seq = app ? "\x1bOB" : "\x1b[B"; break;
    case Qt::Key_Right: seq = app ? "\x1bOC" : "\x1b[C"; break;
    case Qt::Key_Left: seq = app ? "\x1bOD" : "\x1b[D"; break;
    case Qt::Key_Home: seq = app ? "\x1bOH" : "\x1b[H"; break;
    case Qt::Key_End: seq = app ? "\x1bOF" : "\x1b[F"; break;
    case Qt::Key_PageUp: seq = "\x1b[5~"; break;
    case Qt::Key_PageDown: seq = "\x1b[6~"; break;
    case Qt::Key_Insert: seq = "\x1b[2~"; break;
    case Qt::Key_Delete: seq = "\x1b[3~"; break;
    case Qt::Key_F1: seq = "\x1bOP"; break;
    case Qt::Key_F2: seq = "\x1bOQ"; break;
    case Qt::Key_F3: seq = "\x1bOR"; break;
    case Qt::Key_F4: seq = "\x1bOS"; break;
    case Qt::Key_F5: seq = "\x1b[15~"; break;
    case Qt::Key_F6: seq = "\x1b[17~"; break;
    case Qt::Key_F7: seq = "\x1b[18~"; break;
    case Qt::Key_F8: seq = "\x1b[19~"; break;
    case Qt::Key_F9: seq = "\x1b[20~"; break;
    case Qt::Key_F10: seq = "\x1b[21~"; break;
    case Qt::Key_F11: seq = "\x1b[23~"; break;
    case Qt::Key_F12: seq = "\x1b[24~"; break;
    case Qt::Key_Return:
    case Qt::Key_Enter: seq = "\r"; break;
    case Qt::Key_Backspace: seq = ctrl ? "\x08" : "\x7f"; break;
    case Qt::Key_Tab: seq = "\t"; break;
    case Qt::Key_Backtab: seq = "\x1b[Z"; break;
    case Qt::Key_Escape: seq = "\x1b"; break;
    default: break;
    }
    if (seq.isEmpty()) {
        QString text = e->text();
        if (text.isEmpty())
            return;
        seq = text.toUtf8();
    }
    if (alt && !seq.startsWith('\x1b'))
        seq.prepend('\x1b');
    send(seq);
}

void TerminalWidget::wheelEvent(QWheelEvent *e)
{
    int steps = e->angleDelta().y() / 40;
    if (!steps)
        steps = e->angleDelta().y() > 0 ? 1 : -1;
    if (m_screen.altScreen()) {
        QByteArray seq = steps > 0 ? "\x1b[A" : "\x1b[B";
        for (int i = 0; i < qAbs(steps); ++i)
            m_pty.write(seq);
        return;
    }
    scrollView(steps);
}

QPoint TerminalWidget::cellAt(const QPoint &pos) const
{
    int x = qBound(0, (pos.x() - Margin) / m_cellW, m_screen.cols() - 1);
    int y = qBound(0, (pos.y() - Margin) / m_cellH, m_screen.rows() - 1);
    return QPoint(x, m_screen.scrollbackSize() - m_scroll + y);
}

void TerminalWidget::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    if (e->button() == Qt::LeftButton) {
        m_selStart = m_selEnd = cellAt(e->position().toPoint());
        m_selecting = true;
        update();
    } else if (e->button() == Qt::MiddleButton) {
        paste();
    }
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_selecting)
        return;
    m_selEnd = cellAt(e->position().toPoint());
    update();
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || !m_selecting)
        return;
    m_selecting = false;
    m_selEnd = cellAt(e->position().toPoint());
    if (m_selStart == m_selEnd)
        m_selStart = m_selEnd = QPoint(-1, -1);
    update();
}

QString TerminalWidget::selectionText() const
{
    if (m_selStart.y() < 0)
        return QString();
    QPoint a = m_selStart, b = m_selEnd;
    if (b.y() < a.y() || (b.y() == a.y() && b.x() < a.x()))
        std::swap(a, b);
    QStringList lines;
    int sb = m_screen.scrollbackSize();
    for (int line = a.y(); line <= b.y(); ++line) {
        const QVector<Cell> &cells = line < sb ? m_screen.scrollbackLine(line) : m_screen.screenLine(line - sb);
        int from = line == a.y() ? a.x() : 0;
        int to = line == b.y() ? b.x() : int(cells.size()) - 1;
        QString text;
        for (int x = from; x <= to && x < cells.size(); ++x)
            text.append(QString::fromUcs4(&cells[x].ch, 1));
        while (text.endsWith(QLatin1Char(' ')))
            text.chop(1);
        lines.append(text);
    }
    return lines.join(QLatin1Char('\n'));
}

void TerminalWidget::copy()
{
    QString text = selectionText();
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void TerminalWidget::paste()
{
    QByteArray data = QGuiApplication::clipboard()->text().toUtf8();
    if (data.isEmpty())
        return;
    data.replace('\n', '\r');
    if (m_screen.bracketedPaste())
        data = "\x1b[200~" + data + "\x1b[201~";
    send(data);
}

void TerminalWidget::focusInEvent(QFocusEvent *)
{
    m_focused = true;
    update();
}

void TerminalWidget::focusOutEvent(QFocusEvent *)
{
    m_focused = false;
    update();
}

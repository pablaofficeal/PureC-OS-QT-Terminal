#pragma once

#include "ptysession.h"
#include "vtscreen.h"

#include <QtCore/QPoint>
#include <QtCore/QTimer>
#include <QtWidgets/QWidget>

class TerminalWidget : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalWidget(QWidget *parent = nullptr);

    bool start();
    QString title() const;
    QSize sizeHint() const override;

signals:
    void titleChanged(const QString &title);
    void finished();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    bool focusNextPrevChild(bool) override { return false; }

private:
    void received(const QByteArray &data);
    void scheduleUpdate();
    void scrollView(int lines);
    const QVector<Cell> &viewLine(int y) const;
    QPoint cellAt(const QPoint &pos) const;
    bool selected(int line, int x) const;
    QString selectionText() const;
    void copy();
    void paste();
    void send(const QByteArray &data);

    VtScreen m_screen;
    PtySession m_pty;
    QTimer m_repaint;
    int m_cellW = 8;
    int m_cellH = 16;
    int m_ascent = 12;
    int m_scroll = 0;
    bool m_focused = false;
    bool m_selecting = false;
    QPoint m_selStart{-1, -1};
    QPoint m_selEnd{-1, -1};
};

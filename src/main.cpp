#include "terminalwidget.h"

#include <QtCore/QDir>
#include <QtGui/QFontDatabase>
#include <QtGui/QKeyEvent>
#include <QtPlugin>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTabWidget>

Q_IMPORT_PLUGIN(QPurecIntegrationPlugin)

class TerminalWindow : public QMainWindow
{
public:
    TerminalWindow()
    {
        m_tabs = new QTabWidget(this);
        m_tabs->setDocumentMode(true);
        m_tabs->setTabsClosable(true);
        m_tabs->setMovable(true);
        m_tabs->tabBar()->setFocusPolicy(Qt::NoFocus);
        setCentralWidget(m_tabs);
        connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int i) { closeTab(m_tabs->widget(i)); });
        connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) { syncTitle(); });
        setStyleSheet(QStringLiteral(
            "QMainWindow, QTabWidget::pane { background: #1e1e2e; border: none; }"
            "QTabBar { background: #181825; }"
            "QTabBar::tab { background: #181825; color: #a6adc8; padding: 5px 14px; border: none; }"
            "QTabBar::tab:selected { background: #1e1e2e; color: #cdd6f4; border-top: 2px solid #89b4fa; }"));
        newTab();
    }

    void newTab()
    {
        auto *term = new TerminalWidget;
        int index = m_tabs->addTab(term, QStringLiteral("Shell"));
        m_tabs->setCurrentIndex(index);
        m_tabs->tabBar()->setVisible(m_tabs->count() > 1);
        connect(term, &TerminalWidget::titleChanged, this, [this, term](const QString &t) {
            int i = m_tabs->indexOf(term);
            if (i >= 0)
                m_tabs->setTabText(i, t.isEmpty() ? QStringLiteral("Shell") : t.left(32));
            syncTitle();
        });
        connect(term, &TerminalWidget::finished, this, [this, term] { closeTab(term); });
        if (!term->start())
            term->deleteLater();
        term->setFocus();
    }

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        QMainWindow::keyPressEvent(e);
    }

    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ShortcutOverride) {
            auto *k = static_cast<QKeyEvent *>(e);
            Qt::KeyboardModifiers m = k->modifiers();
            if ((m & Qt::ControlModifier) && (m & Qt::ShiftModifier)) {
                if (k->key() == Qt::Key_T) {
                    newTab();
                    e->accept();
                    return true;
                }
                if (k->key() == Qt::Key_W) {
                    closeTab(m_tabs->currentWidget());
                    e->accept();
                    return true;
                }
            }
            if ((m & Qt::ControlModifier) && k->key() == Qt::Key_PageDown && m_tabs->count() > 1) {
                m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
                e->accept();
                return true;
            }
            if ((m & Qt::ControlModifier) && k->key() == Qt::Key_PageUp && m_tabs->count() > 1) {
                m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count());
                e->accept();
                return true;
            }
        }
        return QMainWindow::event(e);
    }

private:
    void closeTab(QWidget *w)
    {
        int i = m_tabs->indexOf(w);
        if (i < 0)
            return;
        m_tabs->removeTab(i);
        w->deleteLater();
        if (m_tabs->count() == 0) {
            close();
            return;
        }
        m_tabs->tabBar()->setVisible(m_tabs->count() > 1);
        if (m_tabs->currentWidget())
            m_tabs->currentWidget()->setFocus();
    }

    void syncTitle()
    {
        auto *term = qobject_cast<TerminalWidget *>(m_tabs->currentWidget());
        QString t = term ? term->title() : QString();
        setWindowTitle(t.isEmpty() ? QStringLiteral("Terminal") : t);
    }

    QTabWidget *m_tabs;
};

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "purec");
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", "/usr/share/fonts");
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("qt-terminal"));
    for (const QString &dir : {QStringLiteral("/usr/share/qt-terminal/fonts"), QStringLiteral("/usr/share/fonts")}) {
        const QDir fonts(dir);
        for (const QString &file : fonts.entryList({QStringLiteral("*.ttf")}, QDir::Files))
            QFontDatabase::addApplicationFont(fonts.filePath(file));
    }
    QFont ui(QStringLiteral("DejaVu Sans"));
    ui.setPixelSize(13);
    QApplication::setFont(ui);
    TerminalWindow window;
    window.resize(window.sizeHint().expandedTo(QSize(860, 540)));
    window.show();
    return app.exec();
}

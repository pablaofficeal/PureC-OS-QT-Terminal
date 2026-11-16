#pragma once

#include <QtCore/QObject>
#include <QtCore/QSocketNotifier>
#include <QtCore/QTimer>

class PtySession : public QObject
{
    Q_OBJECT
public:
    explicit PtySession(QObject *parent = nullptr);
    ~PtySession() override;

    bool start(int cols, int rows);
    void write(const QByteArray &data);
    void resize(int cols, int rows);
    bool running() const { return m_pid > 0; }

signals:
    void received(const QByteArray &data);
    void finished(int status);

private:
    void readable();
    void reap();

    int m_master = -1;
    int m_pid = -1;
    QSocketNotifier *m_notifier = nullptr;
    QTimer m_reaper;
};

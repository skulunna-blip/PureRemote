#pragma once

#include <QObject>
#include <QString>

class QAction;
class QMenu;
class QSystemTrayIcon;

// 托盘常驻。远程桌面被控端要一直挂着，所以关窗口只收进托盘、不退进程。
class TrayController : public QObject
{
    Q_OBJECT

public:
    explicit TrayController(QObject* parent = nullptr);

    void show();
    bool isAvailable() const;

    // 弹个气泡。被控端"有人连进来了"就靠它 —— 不能拦连接，只告诉一声。
    void notify(const QString& title, const QString& body, int msec = 6000);

public slots:
    void setIdText(const QString& id);
    void setStatusText(const QString& text);

signals:
    void showWindowRequested();
    void connectRequested();      // 「连接…」：叫出窗口并聚焦到地址簿
    void refreshRequested();
    void quitRequested();

private:
    QSystemTrayIcon* m_tray = nullptr;
    QMenu*           m_menu = nullptr;
    QAction*         m_actId = nullptr;
    QAction*         m_actStatus = nullptr;
    QString          m_id;
    QString          m_status;
};

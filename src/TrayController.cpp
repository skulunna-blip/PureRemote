#include "TrayController.h"

#include "I18n.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>

TrayController::TrayController(QObject* parent)
    : QObject(parent)
{
    m_tray = new QSystemTrayIcon(QApplication::windowIcon(), this);
    m_tray->setToolTip(QStringLiteral("PureRemote"));

    m_menu = new QMenu();

    // 前两项是只读信息，禁用态；禁用项在深色菜单里偏灰，但比放状态栏好找
    m_actId = m_menu->addAction(TR("本机 ID：—"));
    m_actId->setEnabled(false);
    m_actStatus = m_menu->addAction(TR("状态：启动中…"));
    m_actStatus->setEnabled(false);

    m_menu->addSeparator();
    QAction* actShow = m_menu->addAction(TR("显示主窗口"));
    // 把联系人逐个列进托盘（免开窗一点就连）留到 M2.5：那要 addMenu 占位 + 只 clear()
    // 重建子项、顶层永不重建，M2 不摊这个复杂度。
    QAction* actConnect = m_menu->addAction(TR("连接…"));
    QAction* actRefresh = m_menu->addAction(TR("重新读取 ID"));
    m_menu->addSeparator();
    QAction* actQuit = m_menu->addAction(TR("退出"));

    connect(actShow, &QAction::triggered, this, &TrayController::showWindowRequested);
    connect(actConnect, &QAction::triggered, this, &TrayController::connectRequested);
    connect(actRefresh, &QAction::triggered, this, &TrayController::refreshRequested);
    connect(actQuit, &QAction::triggered, this, &TrayController::quitRequested);

    m_tray->setContextMenu(m_menu);

    // 左键单击（Windows 上是双击）也叫出窗口
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick)
                    emit showWindowRequested();
            });
}

void TrayController::show()
{
    if (m_tray)
        m_tray->show();
}

bool TrayController::isAvailable() const
{
    return m_tray && QSystemTrayIcon::isSystemTrayAvailable();
}

void TrayController::notify(const QString& title, const QString& body, int msec)
{
    if (!m_tray || !QSystemTrayIcon::supportsMessages())
        return;
    m_tray->showMessage(title, body, QSystemTrayIcon::Information, msec);
}

void TrayController::setIdText(const QString& id)
{
    m_id = id;
    const QString shown = id.isEmpty() ? TR("—") : id;
    if (m_actId)
        m_actId->setText(TR("本机 ID：%1").arg(shown));
    if (m_tray)
        m_tray->setToolTip(TR("PureRemote\n本机 ID：%1\n%2").arg(shown, m_status));
}

void TrayController::setStatusText(const QString& text)
{
    m_status = text;
    if (m_actStatus)
        m_actStatus->setText(TR("状态：%1").arg(text));
    if (m_tray)
        m_tray->setToolTip(TR("PureRemote\n本机 ID：%1\n%2")
                               .arg(m_id.isEmpty() ? TR("—") : m_id, text));
}

#include "NotifyPolicy.h"

#include "AppConfig.h"
#include "ConnectionWatcher.h"
#include "I18n.h"
#include "RustDeskController.h"

NotifyPolicy::NotifyPolicy(AppConfig* config, RustDeskController* rd, ConnectionWatcher* watcher,
                           QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_rd(rd)
    , m_watcher(watcher ? watcher : new ConnectionWatcher(this))
{
    connect(m_watcher, &ConnectionWatcher::remoteConnected, this,
            &NotifyPolicy::remoteConnected);
    connect(m_watcher, &ConnectionWatcher::statusMessage, this,
            &NotifyPolicy::statusMessage);
}

QString NotifyPolicy::modeToString(Mode m)
{
    switch (m) {
    case Mode::Toast:   return QStringLiteral("toast");
    case Mode::Confirm: return QStringLiteral("confirm");
    case Mode::Off:     break;
    }
    return QStringLiteral("off");
}

NotifyPolicy::Mode NotifyPolicy::modeFromString(const QString& s)
{
    if (s == QLatin1String("toast"))
        return Mode::Toast;
    if (s == QLatin1String("confirm"))
        return Mode::Confirm;
    return Mode::Off;
}

NotifyPolicy::Mode NotifyPolicy::mode() const
{
    return modeFromString(m_config->notifyMode());
}

void NotifyPolicy::setMode(Mode m)
{
    m_config->setNotifyMode(modeToString(m));
    if (!m_config->save())
        emit statusMessage(TR("外壳配置写不进去（%1）").arg(AppConfig::configFilePath()));
}

bool NotifyPolicy::applyNow(QString* errorOut, bool manageProcess)
{
    // 同 PasswordPolicy：停-写-起期间不能让看门狗插手
    RustDeskController::Maintenance guard(m_rd);

    const Mode m          = mode();
    const bool wasRunning = m_rd->isRunning();

    if (manageProcess && wasRunning && !m_rd->stop()) {
        if (errorOut)
            *errorOut = TR("停不掉正在运行的 RustDesk，已放弃写入（否则会被它回读覆盖）");
        return false;
    }

    // 只有「需要确认」才用 RustDesk 自己的连接管理器；「提示」和「关闭」都写 password，
    // 区别只在外壳这边开不开日志监听。
    const QString approve = (m == Mode::Confirm) ? QStringLiteral("click")
                                                 : QStringLiteral("password");
    const bool ok = m_rd->setOption(QStringLiteral("approve-mode"), approve);

    if (!ok) {
        if (errorOut)
            *errorOut = TR("写 %1 的 [options] 失败（写前已备份，失败会自动回滚）")
                            .arg(RustDeskController::optionConfigPath());
        if (manageProcess && wasRunning)
            m_rd->start();                       // 写失败也要把服务拉回来
        return false;
    }

    // 用 requestEnabled 而不是 setEnabled：日志监听可能还有别的消费方
    // （「用完即换」要盯同一条"会话结束"标记），不能因为提示关掉了就把监听一起关掉。
    m_watcher->requestEnabled(this, m != Mode::Off);

    // 常驻开着的时候无论如何都要起回来
    if (manageProcess && (wasRunning || m_rd->keepAlive()))
        m_rd->start();

    if (m == Mode::Confirm)
        emit statusMessage(TR("已开启连接确认：有人连进来要你点接受才连得上"));
    else if (m == Mode::Toast)
        emit statusMessage(TR("已开启连接提示：有人连进来会弹通知，不拦连接"));
    else
        emit statusMessage(TR("连接提示已关闭"));
    return true;
}

void NotifyPolicy::start()
{
    // 把内存里的监听开关和配置对齐。不主动重启 RustDesk —— 启动时它多半还没跑；
    // 真在跑的话，approve-mode 得等下次 applyNow 或它自己重启才生效。
    //
    // ⚠ 这里**不能**再顺手 prime()。prime 会把游标推到文件末尾，而监听可能已经因为
    //    另一个消费方（「用完即换」）开着并在往前推进了 —— 再拉回末尾就会把它刚读到的
    //    那几行跳过去。"从关到开"那一次 prime 由 ConnectionWatcher 自己在
    //    requestEnabled 里做，只做一次。
    m_watcher->requestEnabled(this, mode() != Mode::Off);
}

#pragma once

#include <QObject>
#include <QString>

class AppConfig;
class ConnectionWatcher;
class RustDeskController;

// 「有人连进来时被控端怎么办」—— 三档，见 AppConfig::notifyMode() 的注释。
//
// **为什么改这个也要重启 RustDesk**：approve-mode 存在 RustDesk2.toml 的 [options] 里，
// RustDesk 只在启动时把它读进进程内的 CONFIG2，之后不回读文件。和密码是同一个道理。
//
// **「提示」那档 RustDesk 自己是没有的**：源码里只有 password（静默）/ click（必须点接受）
// / both（默认，没设密码才弹）。所以中间那档由外壳盯日志实现（ConnectionWatcher），
// approve-mode 照样写 password —— 连接正常通过，只是外壳额外弹个气泡。
class NotifyPolicy : public QObject
{
    Q_OBJECT

public:
    enum class Mode { Off, Toast, Confirm };

    // watcher 传 nullptr 就自己造一个（老调用照旧）。真跑的时候由 main 建**一个**、
    // 分给需要它的几方 —— 「用完即换」也要盯同一份日志，两边各造一个会各读一遍，
    // 而且"谁开了谁没开"会各说各话。
    NotifyPolicy(AppConfig* config, RustDeskController* rd, ConnectionWatcher* watcher = nullptr,
                 QObject* parent = nullptr);

    Mode mode() const;
    void setMode(Mode m);            // 只改外壳配置，不碰 RustDesk（要 applyNow 才落地）

    // 把 approve-mode 写进 RustDesk2.toml 的 [options]，并按模式开关日志监听。
    // manageProcess=false 是给测试用的：不碰真进程。
    bool applyNow(QString* errorOut = nullptr, bool manageProcess = true);

    void start();                    // 启动时把监听开关和配置对齐

signals:
    void remoteConnected();
    void statusMessage(const QString& msg);

private:
    static QString modeToString(Mode m);
    static Mode    modeFromString(const QString& s);

    AppConfig*          m_config  = nullptr;
    RustDeskController* m_rd      = nullptr;
    ConnectionWatcher*  m_watcher = nullptr;
};

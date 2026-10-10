#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QTimer;

// 盯着 RustDesk 的日志，感知"有人连进来了"和"那个人走了"。
//
// 为什么要这么绕：RustDesk 的 IPC 管道（\\.\pipe\RustDesk\query）对第三方是关死的
// （ipc/auth.rs 的 executable_paths_match 要求对端 exe 的规范路径和服务端完全相同），
// 外壳拿不到任何事件推送。日志是唯一可用的信号源。
//
// **要盯两个文件，不是一个**（2026-10-08 实测踩到）：
//   %APPDATA%\RustDesk\log\server\rustdesk_rCURRENT.log   ← `rustdesk.exe --server`（无窗口）
//   %APPDATA%\RustDesk\log\rustdesk_rCURRENT.log          ← 带界面的实例
// 外壳用 `--server` 拉起被控端（无窗口），它的日志在**前者**；用户自己双击 RustDesk
// 开的界面实例写在后者。两边都可能有人连进来，所以两个都得看。
//
// 被控端每次成功建立**远程桌面 / 摄像头**连接都会写这一行（源码 src/server/connection.rs
// 的 login 分支，line ~2117）：
//   INFO [src\server\connection.rs:2117] peer info supported_encoding: SupportedEncoding {...}
// 2026-10-08 用"本机连本机"实测确认过它真的会出现，也确认了客户端侧对应那行来自
// src/client.rs —— 自连时两条同时在，必须靠源文件路径区分，不能只匹配后半句。
//
// 会话**结束**另有一行（2026-10-09 在一份真日志里逐行读出来的，见下面 disconnectRe
// 的注释）：这行是「用完即换」换密码的触发点 —— 必须在**对端走了之后**才换，
// 早一步（有人连上来就换）等于把刚连上的那个人掐断。
//
// 已知边界（都是 RustDesk 的限制，不是实现偷懒）：
//  * **文件传输 / 终端**连接走另一个分支，不写 peer info 那行 → 那两种连接不会触发
//    "有人连进来"的提示；但它们的**结束**会写 Connection closed → 「用完即换」会把
//    它们也算一次（给了码的人用过一次就换，正是那一档想要的效果）
//  * 日志里**不含对端 ID** → 提示说不出"是谁连的"，也分不清"断开后立刻连上的是不是
//    同一个人"（「用完即换」的宽限期就是在给这种分不清留余地）
class ConnectionWatcher : public QObject
{
    Q_OBJECT

public:
    explicit ConnectionWatcher(QObject* parent = nullptr);

    // 开关。可以有**多个**消费方各自表达需不需要（「连接提示」要它，「用完即换」也要它，
    // 两者互相独立 —— 用户完全可能关掉提示却仍想要用完即换）。任何一方要，就轮询。
    void setEnabled(bool on) { requestEnabled(this, on); }
    void requestEnabled(QObject* who, bool on);
    bool isEnabled() const { return !m_requesters.isEmpty(); }

    // 从"此刻的末尾"开始看。启动时调用，免得把历史连接当成新连接报出来。
    void prime();

    // 要盯的日志（存在的那几个）。日志目录是从 %APPDATA% 推出来的。
    static QStringList logPaths();
    static QString currentLogPath();   // 兼容旧调用：返回界面实例那个

    // 把要盯的日志换成别的。**只给测试用** —— 真跑的时候永远看 RustDesk 自己的日志。
    // 传空列表 = 什么都不盯。
    void setLogPaths(const QStringList& paths);
    QStringList watchedPaths() const;

    // 立刻看一次，不等定时器。测试用。
    void pollNow() { poll(); }

signals:
    void remoteConnected();
    void remoteDisconnected();
    void statusMessage(const QString& msg);

private slots:
    void poll();

private:
    struct Tail {
        QString   path;
        qint64    pos = -1;      // -1 = 还没 prime
        QByteArray sig;          // 文件头签名，用来认出"这个文件被换掉了"（见 poll 里的注释）
    };

    void applyEnablement();

    QTimer*           m_timer = nullptr;
    QVector<Tail>     m_tails;          // 空 = 用 logPaths()
    QSet<QObject*>    m_requesters;     // 谁在要这个监听（析构了自动退出）
    bool              m_primeOnNextPoll = false;
};

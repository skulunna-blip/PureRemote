#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class AddressBook;
class AppConfig;
class RustDeskController;
class TsnetBridge;

class QProcess;
class QTimer;

// 一次连接的编排。两条路：
//
//  官方 ID 档：  拿对方的 RustDesk ID → 直接 --connect。不碰桥接。
//  tsnet 档：    桥接就位 → **等对端真的上线** → 才 --connect 127.0.0.1:21119。
//
// ⚠ 顺序不能反。M4.0 闸门实测：对端没上线就 dial，会白等一次 Windows 系统级 connect
//   超时（20s+），而且错得很难看。所以 WaitingPeer 这个阶段是必须的，不是保险。
//
// ⚠ 还有个坑：TsnetBridge::peerStateChanged 是**边沿触发**的（状态没变不重发）。
//   对端可能在我们接槽之前就已经上线 —— 那唯一一次 online=true 会被丢掉，然后干等到超时。
//   所以桥接 ready 之后必须用 **level getter（bridge->peerOnline()）兜底**，不能只等信号。
//
// ⚠ tsnet 档自己分两种桥接，别搞混：
//   · **常驻（Mode::Serve）**：外壳启动时就拉起来了，这台机器一直挂在 tailnet 上。
//     这里只做三件事 —— 把"这次连谁"写给桥接（peerfile）、等它报 online、拉客户端。
//     **绝不 start()/stop() 它**：那是整台机器的对外可达性，不是这次连接的东西。
//   · **一次性（Mode::Connect）**：老路径，连接期间起、连完停。测试（C/E 段）走这条。
//
// ⚠ 常驻模式下 peerOnline() **必须配上 peerName() 一起看**：它是"上一行 PEER 说的那个
//   对端"的状态，可能是上一次连接的那个人。只看 online 就会拿着别人的结论去连 —
//   在 M4.0 那个坑上一模一样地再摔一次。
class ConnectSession : public QObject
{
    Q_OBJECT

public:
    enum class Phase {
        Idle,        // 没在连
        Preparing,   // 桥接在登录 tailnet（tsnet 档才有这一步）
        WaitingPeer, // 桥接好了，等对端上线
        Launching,   // 正在拉起客户端进程
        Running,     // 客户端在跑 = 会话进行中
        Failed
    };

    ConnectSession(AppConfig* config, RustDeskController* rd, TsnetBridge* bridge,
                   AddressBook* book, QObject* parent = nullptr);
    ~ConnectSession() override;

    Phase   phase() const { return m_phase; }
    QString phaseText() const;             // 给人看的一句话
    QString lastError() const { return m_lastError; }
    int     contactIndex() const { return m_index; }
    bool    isActive() const;              // 非 Idle 且非 Failed

    // index 是地址簿里的下标。目标地址按 AppConfig::netMode() 取。
    void connectTo(int index);

    // 连一条**没存进地址簿**的联系人（界面上直接填了连接码就点「连接」走这条）。
    // 跟 connectTo 的差别只有两点：不记 lastConnectedAt（没有下标可记），
    // 也不回写地址簿 —— 地址簿是"存过的常用设备"，不是连人的前置条件。
    // 其余（走不走桥接、要不要 --password）跟 connectTo 完全一条路。
    void connectToAdhoc(const class Contact& c);

    // 直接把一个地址交给 rustdesk（冷启动 `PureRemote.exe --connect <id>` 走这条）。
    // **恒走官方 ID 档**：命令行给的就是个裸地址，没有"这条联系人在哪个档位"这回事。
    void connectToTarget(const QString& target, const QString& password = QString());

    void disconnectNow();                  // 收客户端 + 停桥接，回 Idle

    // —— 无头测试钩子（沿用 TsnetBridge::feedLineForTest 那套惯例，不引虚接口）——
    // dryRun 会**抑制两样东西**：客户端进程不真起（只把构造出来的参数记下来），
    // tsnet 档也**不真起桥接**（C 段因此不需要 authkey、不受网络影响）。
    // 代价是：C 段证明的只是"时序与参数构造"，不证明 RustDesk 真会按这个参数连上，
    // 也不证明真 pmrelay 上那条"对端不上线不放行"的闸门 —— 后者归 E 段。
    void setDryRunForTest(bool on) { m_dryRun = on; }
    bool dryRunForTest() const { return m_dryRun; }
    // 等对端上线的超时。默认 60s（真实使用够用）；E 段要等一个**永远不上线**的名字，
    // 照默认值跑一次得干等一分钟，测试里调短。
    void setPeerTimeoutForTest(int ms) { m_peerTimeoutMs = ms; }
    void feedBridgeReadyForTest(const QString& detail = QString());
    void feedPeerStateForTest(bool online, const QString& relay = QString(),
                              const QString& direct = QString());
    void feedBridgeErrorForTest(const QString& what, const QString& detail = QString());
    void feedBridgeStoppedForTest();
    // 等对端超时那条：真跑要等满 m_peerTimeoutMs（C 段是同步的，事件循环不转），
    // 所以直接调那个槽。钉的是**超时文案里有没有本机自己的状态**。
    void feedPeerTimeoutForTest() { onPeerTimeout(); }
    QStringList lastConnectArgsForTest() const { return m_lastArgs; }
    QString lastTargetForTest() const { return m_lastTarget; }
    bool    clientSpawnedForTest() const { return m_launched; }

signals:
    void phaseChanged(int phase, const QString& text);
    void failed(const QString& msg);
    void sessionFinished();                // 客户端退出 / 被断开之后收完尾

private slots:
    void onBridgeReady(const QString& mode, const QString& detail);
    void onPeerState(bool online, const QString& relay, const QString& direct);
    void onBridgeError(const QString& what, const QString& detail);
    void onBridgeStopped();
    void onPeerTimeout();

private:
    // QProcess::finished 带的是 QProcess::ExitStatus，直接连签名对不上 —— 在建立连接的
    // 那个 lambda 里转成 int 再进来。
    void onClientFinished(int exitCode, int exitStatus);
    void setPhase(Phase p, const QString& msg = QString());
    void fail(const QString& msg);
    void launchClient(const QString& target, const QString& password);
    // 三条 public 入口共用的尾巴：按档位决定走桥接还是直接 --connect，然后拉起客户端。
    // **不含清状态**（m_index 之类由各入口自己按原语义设）—— 那样才能保证 connectTo
    // 和 connectToTarget 的对外行为逐字不变（--booktest 的 C 段钉着它们）。
    // `allowTsnet=false` 只有冷启动那条路用，理由见 connectToTarget 的注释。
    void beginConnect(const class Contact& c, bool allowTsnet);
    void beginTsnet(const class Contact& c);
    void cleanup(bool stopBridge);
    bool tsnetMode() const;
    // 桥接是不是"外壳启动就拉起来、全程不退出"的那个（Mode::Serve）。是的话这次连接
    // 只换对端、不碰进程生命周期。
    bool persistentBridge() const;
    // 桥接报的在线状态，**是不是关于 m_armedPeer 这个对端的**。
    // 名字对不上就说明那是上一次连接的结论，不能拿来用。
    bool peerIsCurrentAndOnline() const;

    AppConfig*          m_config = nullptr;
    RustDeskController* m_rd     = nullptr;
    TsnetBridge*        m_bridge = nullptr;
    AddressBook*        m_book   = nullptr;

    Phase   m_phase = Phase::Idle;
    QString m_lastError;
    int     m_index = -1;

    QProcess* m_client = nullptr;
    QTimer*   m_peerTimer = nullptr;
    int       m_peerTimeoutMs = 60000;
    bool      m_launched = false;          // 客户端"曾经起来过"（dry-run 下也置位）

    bool        m_dryRun = false;
    bool        m_startingBridge = false;  // start() 内部会先 stop()，那期间发的 stopped() 不算数
    bool        m_cleaning = false;        // 我们自己收尾时停的桥接，不算"桥接断了"
    QStringList m_lastArgs;
    QString     m_lastTarget;
    QString     m_armedPeer;   // 这次写给常驻桥接的对端（空 = 不是常驻模式）
    // 这次连接的目标（在 beginConnect 里从传进来的联系人取，来源与地址簿无关）。
    // 放行那一刻**只认这两个**：回头按 m_index 取会把「填码直接连」那条路取成空。
    QString     m_pendingHost;
    QString     m_pendingPassword;
};

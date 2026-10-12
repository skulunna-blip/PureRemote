#include "ConnectSession.h"

#include "AddressBook.h"
#include "AppConfig.h"
#include "I18n.h"
#include "RustDeskController.h"
#include "TsnetBridge.h"

#include <QDateTime>
#include <QProcess>
#include <QTimer>

namespace {
// 首次 tsnet 登录实测 ~40s（要注册节点 + 拉 DERP 列表），这里给三倍余量。
// 别照抄某次单次实测的值 —— 那是网络最好的那一次。
constexpr int kBridgeReadyMs = 120000;
constexpr int kPeerOnlineMs  = 60000;
} // namespace

ConnectSession::ConnectSession(AppConfig* config, RustDeskController* rd, TsnetBridge* bridge,
                               AddressBook* book, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_rd(rd)
    , m_bridge(bridge)
    , m_book(book)
{
    // 槽要在 start() **之前**接好：start() 内部会先 stop()，中间可能立刻冒信号出来。
    if (m_bridge) {
        connect(m_bridge, &TsnetBridge::ready, this, &ConnectSession::onBridgeReady);
        connect(m_bridge, &TsnetBridge::peerStateChanged, this, &ConnectSession::onPeerState);
        connect(m_bridge, &TsnetBridge::bridgeError, this, &ConnectSession::onBridgeError);
        connect(m_bridge, &TsnetBridge::stopped, this, &ConnectSession::onBridgeStopped);
    }

    m_peerTimer = new QTimer(this);
    m_peerTimer->setSingleShot(true);
    connect(m_peerTimer, &QTimer::timeout, this, &ConnectSession::onPeerTimeout);
}

ConnectSession::~ConnectSession()
{
    cleanup(true);
}

bool ConnectSession::isActive() const
{
    return m_phase != Phase::Idle && m_phase != Phase::Failed;
}

bool ConnectSession::tsnetMode() const
{
    return m_config && m_config->netMode() == QLatin1String("tsnet");
}

// 常驻桥接 = 外壳启动时就拉起来、这台机器一直挂在 tailnet 上的那个（Mode::Serve）。
// 判据用 mode() 而不是"谁把它起来的"：mode 是桥接自己的事实，接线方式变了也不会骗人。
bool ConnectSession::persistentBridge() const
{
    return m_bridge && m_bridge->mode() == TsnetBridge::Mode::Serve && m_bridge->isRunning();
}

bool ConnectSession::peerIsCurrentAndOnline() const
{
    if (!m_bridge || !m_bridge->peerOnline())
        return false;
    if (m_armedPeer.isEmpty())
        return true;   // 一次性桥接：对端是写死在命令行的那个，name 不会跑偏
    return m_bridge->peerName().compare(m_armedPeer, Qt::CaseInsensitive) == 0;
}

QString ConnectSession::phaseText() const
{
    switch (m_phase) {
    case Phase::Idle:        return TR("未连接");
    case Phase::Preparing:   return TR("正在登录 tailnet…");
    case Phase::WaitingPeer: return TR("正在等对方上线…");
    case Phase::Launching:   return TR("正在发起连接…");
    case Phase::Running:     return TR("已发起 —— 见 RustDesk 窗口");
    case Phase::Failed:      return m_lastError;
    }
    return QString();
}

void ConnectSession::setPhase(Phase p, const QString& msg)
{
    m_phase = p;
    if (!msg.isEmpty())
        m_lastError = msg;
    emit phaseChanged(int(p), phaseText());
}

void ConnectSession::fail(const QString& msg)
{
    m_lastError = msg;
    setPhase(Phase::Failed);
    emit failed(msg);
}

void ConnectSession::connectTo(int index)
{
    if (isActive()) {
        emit failed(TR("已经在连接中了 —— 先断开再试"));
        return;
    }
    if (!m_book || index < 0 || index >= m_book->count()) {
        fail(TR("先在上面的列表里选一条联系人"));
        return;
    }

    m_lastError.clear();
    m_index      = index;
    m_lastArgs.clear();
    m_lastTarget.clear();
    m_launched   = false;

    beginConnect(m_book->at(index), /*allowTsnet*/ true);
}

// 地址簿之外的一条临时目标：界面上直接填了码点「连接」走这条。
// 跟 connectTo 的差别只有 m_index = -1（于是 launchClient 不会去回写
// lastConnectedAt —— 没有下标可写，地址簿里本来也没这条）。
void ConnectSession::connectToAdhoc(const Contact& c)
{
    if (isActive()) {
        emit failed(TR("已经在连接中了 —— 先断开再试"));
        return;
    }
    m_lastError.clear();
    m_index    = -1;
    m_launched = false;
    m_lastArgs.clear();
    m_lastTarget.clear();

    if (!c.isValid()) {
        fail(TR("没有可连的地址"));
        return;
    }
    beginConnect(c, /*allowTsnet*/ true);
}

void ConnectSession::connectToTarget(const QString& target, const QString& password)
{
    if (isActive()) {
        emit failed(TR("已经在连接中了 —— 先断开再试"));
        return;
    }
    m_lastError.clear();
    m_index    = -1;              // 不是地址簿里的一条，所以不记 lastConnectedAt
    m_launched = false;
    m_lastArgs.clear();
    m_lastTarget.clear();

    if (target.isEmpty()) {
        fail(TR("没有可连的地址"));
        return;
    }

    Contact c;
    c.rustdeskId   = target;
    c.peerPassword = password;
    // ⚠ allowTsnet=false 是**故意的**：命令行给的就是个裸地址，
    //   没有"这条联系人在哪个档位"这回事 —— 冷启动 `--connect <id>` 恒走官方档。
    beginConnect(c, /*allowTsnet*/ false);
}

// 三条入口共用的尾巴。**不清状态**，m_index 之类由各入口按原语义自己设好再进来。
void ConnectSession::beginConnect(const Contact& c, bool allowTsnet)
{
    m_armedPeer.clear();   // 由 beginTsnet 按"这次连谁"重新置上（非 tsnet 档全程为空）

    if (allowTsnet && tsnetMode()) {
        beginTsnet(c);
        return;
    }

    const QString target = c.targetFor(QStringLiteral("id"));
    if (target.isEmpty()) {
        fail(TR("这条联系人没填 RustDesk ID —— 改成官方 ID 档就得把它补上，或者切到 tailnet 档"));
        return;
    }
    launchClient(target, c.peerPassword);
}

void ConnectSession::beginTsnet(const Contact& c)
{
    const QString host = c.targetFor(QStringLiteral("tsnet"));
    if (host.isEmpty()) {
        fail(TR("这条联系人没填 tailnet 主机名 —— 切到官方 ID 档，或把它补上"));
        return;
    }
    if (!m_bridge) {
        fail(TR("tailnet 桥接没准备好（pmrelay.exe 没找到）"));
        return;
    }

    // ── 常驻桥接：只换对端，绝不碰进程生命周期 ──────────────────────────────
    // 这个进程就是"这台机器挂在 tailnet 上"这件事本身（别人连得进来全靠它）。
    // 连接结束把它停掉 = 顺手把整台机器的可达性关了，那不是一次连接的收尾该干的事。
    if (persistentBridge()) {
        m_armedPeer = host;
        // ⚠ 这一条**不因 dry-run 而跳过**。dry-run 压的是"别起进程"，而这里从头到尾
        //   不碰任何进程（常驻桥接就是外壳自己拉起来的那个）—— 唯一的外部动作是写这个
        //   小文件，而那正是这条路径的全部意义：没写下去，点谁都会连到上一个对端。
        //   压掉它就等于把这条路径变成不可测的。
        if (!m_bridge->setCurrentPeer(host)) {
            fail(TR("写不进桥接的对端文件 —— 程序目录可能是只读的"));
            return;
        }
        if (!m_bridge->isReady()) {
            // 桥接挂了（密钥不对 / 端口被占 / 程序没找到）：这台机器根本没登上 tailnet，
            // 对方看不到它，连出去也没意义。**立刻说清楚**，别让用户白等一分钟 ——
            // 那一分钟换不来任何东西，而"等不到"正是最难自己看懂的一种故障。
            if (!m_bridge->fatalError().isEmpty()) {
                fail(TR("这台机器没登上 tailnet（%1）—— 先在设置里把线路配好，"
                        "它登上去了对方才看得见")
                         .arg(m_bridge->fatalError()));
                return;
            }
            // 还在登录（首次要几十秒）。它 ready 的那一刻会自己接着往下走。
            setPhase(Phase::Preparing);
            return;
        }
        setPhase(Phase::WaitingPeer);
        if (peerIsCurrentAndOnline()) {
            onPeerState(true, m_bridge->peerRelay(), m_bridge->peerDirect());
            return;
        }
        m_peerTimer->start(m_peerTimeoutMs);
        return;
    }

    // ── 一次性桥接（老路径）：连接期间起，连完停 ────────────────────────────
    const QString key = m_config ? m_config->tsnetAuthKey() : QString();
    if (key.isEmpty()) {
        fail(TR("还没填 Tailscale 授权密钥 —— 见下面的“准备情况”"));
        return;
    }
    const QString selfName = m_config ? m_config->tsnetHostname() : QString();
    if (selfName.isEmpty()) {
        fail(TR("还没填本机 tailnet 主机名 —— 见下面的“准备情况”"));
        return;
    }

    m_bridge->setAuthKey(key);
    m_bridge->setHostname(selfName);
    m_bridge->setPeer(host);

    setPhase(Phase::Preparing);

    if (m_dryRun) {
        // dry-run 下**不碰真桥接**：C 段要能在没有 authkey、没有网络的机器上稳定跑完，
        // 而且它验的是时序 —— 起一个真 pmrelay 只会让这一段变成"看网络脸色"。
        // 别把这跟 launchClient 的 dry-run 搞混：那边抑制的是**客户端进程**，两码事。
        return;
    }

    // start() 里会先 stop()，那一下冒出来的 stopped() 不是"桥接断了"，用这个旗子挡掉
    m_startingBridge = true;
    const bool ok = m_bridge->start(TsnetBridge::Mode::Connect);
    m_startingBridge = false;

    if (!ok) {
        fail(TR("拉不起桥接程序 pmrelay.exe"));
        return;
    }
    // 桥接可能**同步**就已经 ready 且对端已在线（本地网络快的时候），
    // 所以这里再兜一次：不能假定信号还在后面。
    if (m_phase == Phase::Preparing && peerIsCurrentAndOnline()) {
        onBridgeReady(QStringLiteral("connect"), m_bridge->lastReadyDetail());
    }
}

void ConnectSession::launchClient(const QString& target, const QString& password)
{
    m_lastTarget = target;
    m_lastArgs   = RustDeskController::connectArgs(target, password);

    setPhase(Phase::Launching);

    if (m_dryRun) {
        // 只记录参数，不起进程。C 段靠它断言时序，不证明真能连上。
        m_launched = true;
        setPhase(Phase::Running);
        if (m_book && m_index >= 0)
            m_book->touchConnectedAt(m_index, QDateTime::currentDateTime());
        return;
    }

    if (!m_rd) {
        fail(TR("内部错误：没有 RustDesk 控制器"));
        return;
    }

    QString err;
    QProcess* p = m_rd->spawnConnectClient(target, password, this, &err);
    if (!p) {
        fail(err.isEmpty() ? TR("拉不起连接客户端") : err);
        cleanup(false);
        return;
    }
    m_client   = p;
    m_launched = true;

    connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus st) { onClientFinished(code, int(st)); });

    if (m_book && m_index >= 0)
        m_book->touchConnectedAt(m_index, QDateTime::currentDateTime());

    setPhase(Phase::Running);
}

void ConnectSession::onClientFinished(int exitCode, int exitStatus)
{
    Q_UNUSED(exitStatus)
    // 客户端退出 = 会话结束（窗口被关掉、或者连都没连上就退了）。
    // 实测确认 --connect 不会"交接给已有实例后自己退出"，所以 finished() 是可信信号。
    if (m_phase == Phase::Running || m_phase == Phase::Launching) {
        if (exitCode != 0 && m_lastError.isEmpty())
            m_lastError = TR("连接客户端退出（代码 %1）").arg(exitCode);
        setPhase(Phase::Idle);
    }
    cleanup(true);
    emit sessionFinished();
}

void ConnectSession::onBridgeReady(const QString& mode, const QString& detail)
{
    Q_UNUSED(mode)
    if (m_phase != Phase::Preparing)
        return;
    Q_UNUSED(detail)
    setPhase(Phase::WaitingPeer);

    // 边沿信号补漏：对端可能在我们接槽之前就已经在线，那唯一一次 online=true 已经丢了。
    // 用 level getter 读当前状态兜底 —— 只等信号就会干等到超时。
    // ⚠ 兜底也要**认名字**：常驻桥接上那个 level 可能是上一次连接的对端留下的。
    if (peerIsCurrentAndOnline()) {
        onPeerState(true, m_bridge->peerRelay(), m_bridge->peerDirect());
        return;
    }
    m_peerTimer->start(m_peerTimeoutMs);
}

void ConnectSession::onPeerState(bool online, const QString& relay, const QString& direct)
{
    Q_UNUSED(relay) Q_UNUSED(direct)
    if (m_phase != Phase::WaitingPeer)
        return;
    if (!online)
        return;                    // 掉线了也不回退，继续等（超时兜底）
    // 常驻桥接是同一个进程连着换对端，线上来的 PEER 不一定是在说我在等的那个 ——
    // 上一次连接的对端还在线的话，这一声 online=true 是它的，用它就会去连错人。
    if (!m_armedPeer.isEmpty() && m_bridge
        && m_bridge->peerName().compare(m_armedPeer, Qt::CaseInsensitive) != 0)
        return;

    m_peerTimer->stop();
    const Contact c = m_book && m_index >= 0 ? m_book->at(m_index) : Contact();
    const QString host = c.targetFor(QStringLiteral("tsnet"));
    if (host.isEmpty()) {
        fail(TR("对端上线了，但这条联系人没填 tailnet 主机名"));
        return;
    }
    launchClient(m_bridge ? m_bridge->localAddress() : QString(), c.peerPassword);
}

void ConnectSession::onPeerTimeout()
{
    if (m_phase != Phase::WaitingPeer)
        return;
    // 等不到人是最难看懂的一种故障：界面上只有一句「正在等对方上线…」，而真正的原因
    // 九成是"对方那台没开着本程序" —— 那台机器在 tailnet 上的存在就是那个桥接进程，
    // 程序没开就等于它不在网上（Tailscale 网页上看到的那一行是**你自己**）。
    // 所以把**本机自己**的状态也带上：本机要是也没登上，问题就在这一头，别去查对端。
    QString self;
    if (m_bridge) {
        if (m_bridge->isReady())
            self = TR("（本机已经在 tailnet 上了）");
        else if (!m_bridge->lastError().isEmpty())
            self = TR("（本机还没登上 tailnet：%1）").arg(m_bridge->lastError());
        else
            self = TR("（本机还在登录 tailnet）");
    }
    fail(TR("等了 %1 秒，对方还没在 tailnet 上出现 —— 最常见的原因是对方那台没开着本程序"
            "（程序不开，它在 tailnet 上就不存在），其次是对方的 tailnet 主机名填错了。%2")
             .arg(m_peerTimeoutMs / 1000)
             .arg(self));
    cleanup(true);
}

void ConnectSession::onBridgeError(const QString& what, const QString& detail)
{
    // 三种"不该管"的情况：正在 start()（内部先 stop）、我们自己收尾停的、以及根本没在连
    if (m_startingBridge || m_cleaning || !isActive())
        return;
    fail(detail.isEmpty() ? what : TR("%1：%2").arg(what, detail));
    cleanup(true);
}

void ConnectSession::onBridgeStopped()
{
    if (m_startingBridge || m_cleaning || !isActive())
        return;                    // start() 内部那次 stop() / 我们自己的收尾，都不算"断了"
    fail(TR("桥接断了"));
    cleanup(false);
}

void ConnectSession::disconnectNow()
{
    if (m_phase == Phase::Idle)
        return;
    cleanup(true);
    setPhase(Phase::Idle);
    emit sessionFinished();
}

void ConnectSession::cleanup(bool stopBridge)
{
    m_peerTimer->stop();
    const bool wasCleaning = m_cleaning;
    m_cleaning = true;           // 收尾期间桥接发的 stopped() 不是"断了"，别反过来再报一次

    if (m_client) {
        // 先礼后兵：terminate 让它自己收尾（RustDesk 要断开会话），1.5s 不走的再杀
        m_client->disconnect(this);      // 别让 kill 再触发一次 finished 回调
        m_client->terminate();
        if (!m_client->waitForFinished(1500)) {
            m_client->kill();
            m_client->waitForFinished(2000);
        }
        m_client->deleteLater();
        m_client = nullptr;
    }

    // ⚠ 常驻桥接（Serve）**永远不停**：它不是这次连接的东西，是这台机器挂在 tailnet
    //   上的那口气。停它 = 别人再也连不进来了，而界面上只表现为"这次连接收尾了"。
    if (stopBridge && m_bridge && m_bridge->isRunning()
        && m_bridge->mode() != TsnetBridge::Mode::Serve)
        m_bridge->stop();

    m_cleaning = wasCleaning;
}

// —— 无头测试钩子 ——

void ConnectSession::feedBridgeReadyForTest(const QString& detail)
{
    onBridgeReady(QStringLiteral("connect"), detail);
}

void ConnectSession::feedPeerStateForTest(bool online, const QString& relay, const QString& direct)
{
    onPeerState(online, relay, direct);
}

void ConnectSession::feedBridgeErrorForTest(const QString& what, const QString& detail)
{
    onBridgeError(what, detail);
}

void ConnectSession::feedBridgeStoppedForTest()
{
    onBridgeStopped();
}

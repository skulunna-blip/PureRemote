#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

// pmrelay.exe 的进程级封装（M4.4）—— 把 RustDesk 的直连口接到 tailnet 上。
//
// 为什么需要这一层：RustDesk 的"直连 IP 访问"只认普通 TCP 地址，而 tailnet 地址只在
// tsnet 进程内部可达。Windows 内核没有内置 TUN，Tailscale 官方客户端必须装
// wintun.sys 内核驱动（= 要管理员），跟"纯绿色免安装"冲突。pmrelay 是纯用户态的桥，
// 外壳只负责把它拉起来、读它的状态、在需要时把它停掉。
//
// 三种模式（协议细节见 scripts/pmrelay/main.go 顶部注释）：
//   Expose  被控端：tailnet :21118 → 127.0.0.1:21118（RustDesk 的直连口）
//   Connect 控制端：127.0.0.1:21119 → 经 tailnet 连 <peer>:21118
//           外壳随后执行 rustdesk.exe --connect 127.0.0.1:21119
//   Serve   **生产路径**：上面两个合一，一个进程 = 一个 tailnet 节点。
//           这台机器常驻在 tailnet 上（别人随时连得进来），本机 21119 按需拨对端，
//           对端从 setCurrentPeer() 写的那个文件里读（外壳点谁就连谁）。
//
// 输出协议：pmrelay 的 **stdout** 是以 "PMRELAY " 开头的空格分隔 key=value 行，
// **stderr** 才是人类日志。分开是为了这里能干净地只解析协议行，不用在几千行
// tsnet 日志里捞东西。
class TsnetBridge : public QObject
{
    Q_OBJECT

public:
    enum class Mode { Off, Expose, Connect, Serve };

    explicit TsnetBridge(QObject* parent = nullptr);
    ~TsnetBridge() override;

    // 找 pmrelay.exe。部署后在 <exe目录>/bin/ 下；开发时 exe 躺在 build/，
    // 而 bin/ 在工程根 —— 跟 RustDeskController 一样两个位置都探。
    static QString findRelayExe();

    void    setRelayPath(const QString& p) { m_relayPath = p; }
    QString relayPath() const;

    // authkey 走**环境变量**而不是命令行：命令行在 tasklist / 任务管理器里是明文可见的，
    // 而这东西等价于账号凭证。
    void setAuthKey(const QString& k) { m_authKey = k; }
    void setHostname(const QString& h) { m_hostname = h; }
    void setStateDir(const QString& d) { m_stateDir = d; }
    void setPeer(const QString& p) { m_peer = p; }
    // serve 模式：对端不是写死在命令行里的，外壳写进这个文件（pmrelay 每 2 秒重读）。
    void setPeerFile(const QString& p) { m_peerFile = p; }
    QString peerFile() const { return m_peerFile; }
    // 把"这次要连谁"写给 pmrelay。**每次连接都要调** —— 换了联系人就得跟着换。
    // 返回 false 只代表文件没写下去（目录建不出来、盘满），不代表网络。
    bool setCurrentPeer(const QString& peer);
    void setListenPort(int p) { m_listenPort = p; }
    void setTargetPort(int p) { m_targetPort = p; }
    void setLocalPort(int p) { m_localPort = p; }
    void setVerbose(bool v) { m_verbose = v; }

    int     localPort() const { return m_localPort; }
    // connect 模式下外壳要 --connect 过去的地址，例如 "127.0.0.1:21119"
    QString localAddress() const;

    // 拉起桥接。已经在跑就先停掉再起（换模式 / 换对端）。
    // 返回 false 只代表"进程没起来"，不代表网络通 —— 网络状态看 ready / peerStateChanged。
    bool start(Mode m);
    void stop();
    bool isRunning() const;
    Mode mode() const { return m_mode; }

    QString tailnetIp() const { return m_tailnetIp; }
    // 桥接登录 + 监听都成了才算 ready（serve 模式下这一刻起，这台机器就挂在 tailnet 上了）
    bool    isReady() const { return !m_tailnetIp.isEmpty(); }
    bool    peerOnline() const { return m_peerOnline; }
    // ⚠ peerOnline() **不能单独用** —— 它是"上一行 PEER 说的那个对端"的状态。
    //   想知道"我现在关心的这个对端在不在线"，得连 peerName() 一起看（见 ConnectSession）。
    QString peerName() const { return m_peerName; }
    QString peerRelay() const { return m_peerRelay; }
    QString peerDirect() const { return m_peerDirect; }
    QString loginUrl() const { return m_loginUrl; }
    QString lastError() const { return m_lastError; }
    // ⚠ 跟 lastError() 是两件事，别混：lastError 记的是**最近一条** ERROR/FATAL，
    //   而 ERROR 里有一类是"这一次转发没连上"（对端没开、RustDesk 没起）——
    //   桥接本身好好活着。fatalError 只记 pmrelay 自己说 FATAL 的那些
    //   （密钥不对、监听端口被占、程序缺失），那才是"这台机器根本没登上 tailnet"。
    //   用 lastError 去判"桥接是不是坏的"会把一次失败的转发误判成整个桥接挂了。
    QString fatalError() const { return m_fatalError; }
    QString lastReadyDetail() const { return m_readyDetail; }

    // 无头测试用：不起进程、不碰网络，直接喂一行协议文本给解析器。
    void feedLineForTest(const QString& line) { parseLine(line); }
    // 无头测试用：真起 serve 要联网登录（无头机上做不到），但"常驻桥接"这条
    // **分叉**必须能被测到 —— 光喂协议行测不出 ConnectSession 走的是哪条路。
    void setModeForTest(Mode m) { m_mode = m; }
    // 无头测试用：假装"外壳已经把常驻桥接拉起来了"（persistentBridge() 看 isRunning()）。
    // ⚠ stop() 第一件事就是把它清掉 —— 于是"有没有人动过这个进程"变成可断言的，
    //   而"停常驻桥接"恰恰是这条路径上最危险的动作（= 别人再也连不进来了）。
    void setRunningForTest(bool on) { m_runningForTest = on; }

signals:
    // 桥接自己起来了（登录成功 + 监听成功）。detail 是给界面看的补充说明。
    void ready(const QString& mode, const QString& detail);
    // 对端上下线 / 路径变化。**外壳应该等 online=true 再发 --connect** ——
    // M4.0 闸门实测：对端还没上线就去 dial，会白等一次 Windows 系统级 connect 超时（20s+）。
    void peerStateChanged(bool online, const QString& relay, const QString& direct);
    void connectionSeen(const QString& from);
    // 首次使用没有 authkey 时，tsnet 会打出一条浏览器登录 URL，这里转给界面做引导。
    void loginUrlAvailable(const QString& url);
    void bridgeError(const QString& what, const QString& detail);
    void logLine(const QString& line);   // pmrelay 的人类日志（stderr）
    void stopped();

private slots:
    void onStdout();
    void onStderr();
    void onFinished(int exitCode);

private:
    void    parseLine(const QString& line);
    QStringList buildArgs(Mode m) const;
    QString stateDir() const;        // m_stateDir 或默认 <exe目录>/data/tsnet
    QString defaultPeerFile() const; // stateDir()/peer.txt

    QProcess* m_proc = nullptr;

    QString m_relayPath;
    QString m_authKey, m_hostname, m_stateDir, m_peer, m_peerFile;
    int     m_listenPort = 21118;
    int     m_targetPort = 21118;
    int     m_localPort  = 21119;
    bool    m_verbose    = false;

    // 解析状态
    Mode              m_mode = Mode::Off;
    QString           m_stdoutBuf;
    QString           m_stderrBuf;
    QString           m_tailnetIp, m_peerRelay, m_peerDirect, m_loginUrl;
    QString           m_peerName;             // 最近一行 PEER 说的是哪个对端
    QString           m_lastError, m_readyDetail;
    QString           m_fatalError;           // 只有 FATAL 行才会写它，见 fatalError()
    bool              m_peerOnline = false;
    bool              m_peerSeen   = false;   // 只在对端状态**变化**时才发信号
    bool              m_runningForTest = false;  // 无头测试用，见 setRunningForTest
};

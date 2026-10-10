#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QString>
#include <QStringList>

// 外壳自己的配置：手写 JSON 存在 exe 同目录（沿用 PureViewer 约定，不用 QSettings）。
// 只存"外壳"需要的东西；RustDesk 自己的配置由 RustDeskController 直接读写它的 toml。
//
// 文件缺失 / JSON 损坏 / 字段类型不对，一律回落默认值，不抛异常——外壳不能因为
// 一个坏配置文件就打不开。
class AppConfig
{
public:
    AppConfig();

    void load();
    bool save() const;

    // bin/rustdesk.exe 的位置。默认为 exe 同目录下的 bin/rustdesk.exe。
    QString rustDeskPath() const { return m_rustDeskPath; }
    void setRustDeskPath(const QString& v) { m_rustDeskPath = v; }

    // 固定密码（明文，仅外壳内部用；写进 RustDesk.toml 后由 RustDesk 加密）。
    QString password() const { return m_password; }
    void setPassword(const QString& v) { m_password = v; }

    // "off"    = 不设固定密码（只用一次性密码）
    // "fixed"  = 一直用同一个
    // "rotate" = 每 rotateHours 小时换一个新密码
    // "timed"  = 限时有效：密码不变，到 rotateHours 小时后自动作废（清掉）
    QString passwordMode() const { return m_passwordMode; }
    void setPasswordMode(const QString& v) { m_passwordMode = v; }

    int rotateHours() const { return m_rotateHours; }
    void setRotateHours(int v) { m_rotateHours = v; }

    // 上次轮换 / 上次设定的时刻（本地时间）。无效值 = 从没轮换过。
    QDateTime passwordRotatedAt() const { return m_passwordRotatedAt; }
    void setPasswordRotatedAt(const QDateTime& v) { m_passwordRotatedAt = v; }

    // 限时密码的作废时刻（本地时间）。无效值 = 无期限（固定 / 轮换模式）。
    QDateTime passwordExpiresAt() const { return m_passwordExpiresAt; }
    void setPasswordExpiresAt(const QDateTime& v) { m_passwordExpiresAt = v; }

    // 首次运行有没有"自动补过一次固定密码"（见 PasswordPolicy::start）。
    //
    // 为什么非要有这么个标记：**光看 password_mode 分不开两种机器**，而它们俩写着
    // 一模一样的 "off"：
    //   false = 全新解压出来的、用户从没碰过密码设置 → 该补一个，否则开箱谁都连不进来
    //   true  = 用户自己选的「关闭」               → 一个字节都不许动
    // 少了这个区分，"开机自动补密码"就会在每次启动时把用户刚删掉的密码偷偷复活，
    // 等于「关闭」这一档变成摆设。这类"看着一样、其实是两回事"的状态，只能显式存。
    bool passwordBootstrapped() const { return m_passwordBootstrapped; }
    void setPasswordBootstrapped(bool v) { m_passwordBootstrapped = v; }

    // 本轮启动时**这个配置文件根本不存在**（真·首次运行）。
    // 只在 load() 里算一次，之后本进程内不再变 —— 所以就算后面 save() 把它建出来了，
    // 这个判断也不会跟着翻。
    //
    // 为什么需要它：`password_bootstrapped` 只是"外壳记不记得自己做过决定"，
    // 它挡不住"配置文件被删了/换了台机器"这种情形。而"自动补一个固定密码"是**写盘**，
    // 判据越窄越好 —— 加上"配置文件夹里连文件都没有"这一条，就等于要求
    // **这台机器上从来没有跑过外壳**，而不是"外壳觉得自己没跑过"。
    bool freshConfigAtLoad() const { return m_freshAtLoad; }

    // 退出时最小化到托盘而不是退出进程
    bool closeToTray() const { return m_closeToTray; }
    void setCloseToTray(bool v) { m_closeToTray = v; }

    // 开机自启（往 HKCU\...\Run 写一条 PureRemote）。默认**关**。
    //
    // 这个字段只是"用户在这个界面里表达过的意愿"的记录；**真正生效的开关状态在
    // 注册表里**，界面上的勾选框读的也是注册表（Autostart::isEnabled()），不是这里。
    // 两份会分家的情形真实存在：绿色包整个目录连同配置拷到另一台机器（配置说开着，
    // 但新机器注册表里没有）、或者用户拿别的工具把启动项清掉了。
    bool autostart() const { return m_autostart; }
    void setAutostart(bool v) { m_autostart = v; }

    // 有人连进来时被控端的反应：
    //   "off"     —— 静默，什么都不显示（approve-mode=password）
    //   "toast"   —— 弹托盘气泡告诉一声，但不拦连接（approve-mode 仍是 password，
    //                提示由外壳自己盯日志实现）
    //   "confirm" —— 弹 RustDesk 的连接管理器，**必须点接受**才连得上（approve-mode=click）
    QString notifyMode() const { return m_notifyMode; }
    void setNotifyMode(const QString& v) { m_notifyMode = v; }

    // ---- tsnet 桥接（M4.4）----
    // Tailscale authkey。等价于账号凭证，所以只存在本机这份 JSON 里（跟明文密码同等对待），
    // 传给 pmrelay 时走**环境变量**不走命令行（命令行在任务管理器里明文可见）。
    // 生成时必须是 reusable + 不过期，且不要勾 ephemeral（离线即删节点 → 被控端会换 IP）。
    QString tsnetAuthKey() const { return m_tsnetAuthKey; }
    void setTsnetAuthKey(const QString& v) { m_tsnetAuthKey = v; }

    // 本机在 tailnet 上显示的主机名。被控端的对端就是靠这个名字找到它的。
    QString tsnetHostname() const { return m_tsnetHostname; }
    void setTsnetHostname(const QString& v) { m_tsnetHostname = v; }

    // 控制端填：要连的那台被控端的 tailnet 主机名（或 IP）。
    QString tsnetPeer() const { return m_tsnetPeer; }
    void setTsnetPeer(const QString& v) { m_tsnetPeer = v; }

    // ---- 连接（M2）----
    // 发起连接时走哪条路：
    //   "official"   = 直接把对方的 RustDesk ID 交给 rustdesk.exe --connect（走官方服务器）
    //   "tsnet"      = 先起 pmrelay 桥接，再 --connect 127.0.0.1:21119（走 tailnet 中继）
    //   "cloudflare" = 同上，但服务器指向自己用 Cloudflare Workers 搭的 hbbs/hbbr
    //   "custom"     = 同上，服务器指向自己填的任意 hbbs/hbbr（frp / 端口映射 / 自建 VPS）
    //
    // 这只是个**偏好**，每条联系人里两个地址都存着，切换档位不用重填。
    //
    // ⚠ 一个字段管四档，**不另设 server_mode**：后三档除了"服务器地址从哪来"完全是同一条路，
    //   拆成两个字段（连接档位 / 服务器档位）就会出现"档位说是 tsnet、服务器写的是自建的"
    //   这种自相矛盾的状态，而它俩本来就必须一致。ConnectSession 只特判 "tsnet" 这一个值。
    //   旧值 "id" 在 load() 里会被归一成 "official"（M2 时期写下的配置不带迁移就还能用）。
    QString netMode() const { return m_netMode; }
    void setNetMode(const QString& v) { m_netMode = v; }

    static QStringList netModes();          // 四个合法值，界面下拉直接用
    static QString netModeTitle(const QString& mode);   // 给人看的一句话

    // ---- 界面语言 ----
    // "zh" / "en"。**存的是具体语言，不存"跟随系统"** —— 跟随系统是"第一次运行时算一次
    // 默认值"的行为（见构造函数），不是一种持续状态：真存了 "auto"，用户在看英文的机器上
    // 手动选回中文就永远没机会生效了。
    // ⚠ 改这个值**当前进程不会变**：界面文案是在 buildUi 里一次性写死的，换语言要靠重启。
    QString lang() const { return m_lang; }
    void setLang(const QString& v) { m_lang = v; }

    // ---- 自建 / 自填服务器（M4.2，只在 cloudflare / custom 档用）----
    // 这四项写进 RustDesk2.toml 的 [options]：custom-rendezvous-server / relay-server /
    // api-server / key。**官方档和 tsnet 档下它们必须是空的**（清掉键 = 回落 RustDesk 默认值）。
    QString customRendezvous() const { return m_customRendezvous; }
    void setCustomRendezvous(const QString& v) { m_customRendezvous = v; }
    QString relayServer() const { return m_relayServer; }
    void setRelayServer(const QString& v) { m_relayServer = v; }
    QString apiServer() const { return m_apiServer; }
    void setApiServer(const QString& v) { m_apiServer = v; }
    QString publicKey() const { return m_publicKey; }
    void setPublicKey(const QString& v) { m_publicKey = v; }

    // 地址簿的**原始 JSON 数组**。这里刻意不做 typed 化 —— 本类只当"原始 JSON 的保险箱"，
    // 校验与容错解析归 AddressBook（配置层不带业务是本项目一贯风格）。
    QJsonArray contacts() const { return m_contacts; }
    void setContacts(const QJsonArray& v) { m_contacts = v; }

    static QString configFilePath();   // <exe目录>/config/puremote.json

private:
    QString   m_rustDeskPath;
    QString   m_password;
    QString   m_passwordMode;
    int       m_rotateHours = 3;
    QDateTime m_passwordRotatedAt;
    QDateTime m_passwordExpiresAt;
    bool      m_passwordBootstrapped = false;   // 没存过 = 没补过（老配置一律当"没补过"，
                                                // 但"有没有密码"那关会挡在前面，见 start()）
    bool      m_freshAtLoad = false;            // load() 那一刻文件不存在
    bool      m_closeToTray = true;
    bool      m_autostart = false;              // 默认关：不声不响就写启动项很招人烦
    QString   m_notifyMode;
    QString   m_tsnetAuthKey;
    QString   m_tsnetHostname;
    QString   m_tsnetPeer;
    QString   m_netMode = QStringLiteral("official");
    QString   m_lang;
    QString   m_customRendezvous;
    QString   m_relayServer;
    QString   m_apiServer;
    QString   m_publicKey;
    QJsonArray m_contacts;
};

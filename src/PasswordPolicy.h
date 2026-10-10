#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

class AppConfig;
class ConnectionWatcher;
class RustDeskController;
class QTimer;

// M3 密码策略：关闭 / 固定密码 / 定时轮换 / 限时有效 / **用完即换**。
//
// **为什么轮换必须重启 RustDesk**：RustDesk 在启动时把配置读进进程内的全局 CONFIG，
// 之后不再回读文件；而第三方又走不了它的 IPC —— `ipc/auth.rs` 的
// `executable_paths_match` 要求对端 exe 的规范化路径**完全等于**自己的，也就是只有
// rustdesk.exe 能连 rustdesk.exe 的命名管道。所以改完 RustDesk.toml 只有重启才生效。
// 代价：轮换瞬间会断掉进行中的连接（间隔几小时一次，可以接受，界面上有提示）。
//
// 「用完即换」（Mode::Once）是唯一一种**不会断掉任何人**的换密码：它的触发点是
// **对端离开之后**（ConnectionWatcher 的 remoteDisconnected），那时候会话已经结束了。
// 详见 shouldRotateAfterSession 的注释。
//
// 密码怎么落地：直接往 RustDesk.toml 顶层写明文，RustDesk 认证侧会走
// `validate_password_storage` 的 legacy plaintext 分支接受它（源码已确认）。
// 写之前必须先停进程，写完再拉起来 —— 顺序反了会被内存里的旧值覆盖回写。
class PasswordPolicy : public QObject
{
    Q_OBJECT

public:
    enum class Mode {
        Off,      // 不设固定密码，只用 RustDesk 的一次性密码
        Fixed,    // 一直用同一个
        Rotate,   // 每 rotateHours 小时换一个新密码
        Timed,    // 限时有效：密码本身不变，到点自动作废（清掉密码）
        Once,     // 用完即换：密码也是固定的，但**有人连过一次、人走了之后**就换成新的
    };

    // watcher 传 nullptr 就自己造一个（老调用照旧）。真跑的时候由 main 建**一个**分给
    // 几方 —— 「用完即换」靠它认"人走了"，而「连接提示」也要盯同一份日志。
    PasswordPolicy(AppConfig* config, RustDeskController* rd,
                   ConnectionWatcher* watcher = nullptr, QObject* parent = nullptr);

    Mode mode() const;
    void setMode(Mode m);

    int  rotateHours() const;
    void setRotateHours(int hours);

    // 外壳记着的当前明文密码（Off 时为空）。RustDesk.toml 里那份读不回明文。
    QString currentPassword() const;

    // 下次"到点"的时刻：Rotate = 下次换新密码，Timed = 作废时刻。
    // 其余模式返回无效 QDateTime。
    QDateTime nextRotationAt() const;

    // 限时模式的作废时刻（= nextRotationAt，名字更直白）。非 Timed 返回无效值。
    QDateTime expiresAt() const;

    // 生成一个随机密码（去掉 0/O/1/l/I 这类易混字符）。
    static QString generatePassword(int length = 10);

    // ⚠ 「关闭」模式下按「立即应用」= **把本机现有的固定密码删掉**，而这个后果光看按钮
    //   名字（"立即应用"）完全猜不到。真踩过：重装/清过配置后默认模式就是「关闭」，
    //   用户点一下，机器上原来那个（RustDesk 原生的、加密的、外壳读不回明文的）固定密码
    //   就被静默删了 —— 表现是"连接别的电脑 → 密码错误"，根因却在被控端已经没有固定密码了，
    //   而且没人看着这台机器时再也连不进来。所以这条判据要单独拎出来，界面点之前先拦一道。
    //
    //   shellPlain —— 外壳自己记着的明文（可能为空：外壳没接管过）
    //   tomlRaw    —— 配置文件里 password 那一行的实际内容（可能是原生加密串）
    // 任何一边非空 = "现在确实有固定密码"，删掉它就真的会把用户锁在外面。
    static bool offModeWipesExisting(const QString& shellPlain, const QString& tomlRaw);

    // **界面上那个"未应用"小字**的判据：配置里选的这一档，跟 RustDesk.toml 里**实际
    // 生效的**东西，对得上吗？
    //
    // 为什么非要有这么个东西：`setMode()` 只改外壳自己的配置（*），真正落地要按
    // 「立即应用」走"停 → 写 → 起"。于是界面上完全可能出现**档位显示「关闭」而机器上
    // 那个固定密码还在生效**的状态 —— 界面在说谎，而且说得很像真的。
    //
    // （*）为什么当初不顺手 apply：切档位就悄悄停一次 RustDesk 更吓人，尤其用户只是
    //     在下拉里划过去看看。所以改成"选归选，应用归应用"，再用这个标记把两者的差距
    //     明说出来。
    //
    //   shellPlain —— 外壳记着的明文（读不回明文时为空）
    //   tomlRaw    —— RustDesk.toml 顶层 password 现在的值（加密串也算"有"）
    static bool needsApply(Mode m, const QString& shellPlain, const QString& tomlRaw);

    // RustDesk 里有一个**外壳读不出明文**的固定密码时，按「立即应用」会不会把它换掉？
    //
    // 用途只有一个：**点「立即应用」之前先弹确认框**（凭据被替换，不能悄悄发生）。
    // 首次运行那条路不走它 —— 那条是自动接管，见 shouldBootstrapPassword。
    // 走到这里的是"跑过外壳之后才漂移出来"的少数情况（比如用户又拿 RustDesk 自己的
    // 窗口设了个密码）。
    //
    // 「关闭」档排除在外：那一档用户要的就是**没有**固定密码，正确动作是删（已有
    // 「关闭固定密码」+ 确认框那条路），不是再装一个新密码进去。
    static bool canTakeOverPassword(Mode m, const QString& shellPlain, const QString& tomlRaw);

    // 「用完即换」该不该换。触发点在**对端离开之后**（remoteDisconnected），不是在
    // "有人连上来"的那一刻 —— 后者会把刚连上的那个人掐断，因为换密码必须重启 RustDesk。
    //
    // 两个否定条件都是踩出来的：
    //  · hasSessionBeenSeen=false：只连上又立刻掉（认证失败、探测）不算"用了一次"
    //  · 距上次轮换不到 minGapSeconds：客户端反复掉线重连时，**不能把 RustDesk 反复
    //    重启**（每次重启那几秒机器都是连不上的）
    static bool shouldRotateAfterSession(bool     hasSessionBeenSeen,
                                         const QDateTime& lastRotatedAt,
                                         const QDateTime& now,
                                         int      minGapSeconds = 30);

    // 首次运行该不该"由外壳接管固定密码"？（start() 里调，纯函数好断言）
    //
    // 这是**写盘**，而且改的是别人的凭据，所以判据只往一个方向放：
    // **只有"这台机器上从来没有跑过外壳"才动手**，其余一律不碰。
    //
    // ⚠ 2026-10-10 改过一次判据：以前还要求"RustDesk 里本来**就没有**密码"才补，
    //   于是"这台机器原本有一个外壳读不出明文的固定密码"被排除在外 —— 而那恰恰是
    //   新用户第一次打开最常撞上的状态：本机那行只有「已设置（密文）」+ 一个谁也看不懂的
    //   按钮，还得先做一次选择才能用。现在**首次运行一律接管**：开箱就是外壳管着的密码，
    //   界面上直接能看能复制；原来那个被替换掉（写前自动备份到 RustDesk.toml.bak）。
    //   这一改动同时把很久以前那次事故的路径**从根上消掉**了：判据不再去看别人的文件
    //   此刻写了什么，只看"我自己的配置文件在不在"。
    //
    // 每一项都是踩出来的，别删：
    struct BootstrapInputs
    {
        Mode    mode              = Mode::Off;
        bool    freshConfig       = false;  // 外壳配置文件夹里连 puremote.json 都没有
        QString shellPlain;                 // 外壳自己记着的明文
        bool    tomlExists        = false;  // RustDesk.toml 在不在
        bool    tomlReadOk        = false;  // 在的话，读进来了吗（读不到 ≠ 没有密码）
        // ⚠ 这一项**不再参与判据**（判据现在是"外壳手上没有明文就接管"）。
        //   留着是为了**说清楚这次接管到底干了什么** —— 它决定提示语是
        //   "已经设好一个固定密码"还是"已经把原来那个换掉了"。
        bool    tomlHasPassword   = false;  // 顶层有 password 行（明文 / 密文都算）
        bool    alreadyBootstrapped = false;// 外壳记着"已经做过决定"（含用户主动选的「关闭」）
    };
    static bool shouldBootstrapPassword(const BootstrapInputs& in);

    // "还没到时候" —— 跟"不该补"要分开：全新机器上 RustDesk 内核刚被拉起来，
    // RustDesk.toml 通常**还没生成**，这时候不能判死（判死的代价是"开箱即用"永远不生效，
    // 因为下次启动配置文件夹里已经有 puremote.json，就不再算首次运行了），
    // 而要过几秒再看一眼。只有"就差文件还没出现"这一种情形算 pending。
    static bool bootstrapPending(const BootstrapInputs& in);

    // 按当前策略立刻落地一次：
    //   Off    → 删掉 RustDesk.toml 的顶层 password
    //   Fixed  → 用已存的固定密码；没有就生成一个并记住
    //   Rotate → 生成新密码，记住并写盘，重置轮换计时
    //   Timed  → **沿用**已有密码（换掉的话已经拿到码的人就连不上了），
    //            没有就生成一个；把作废时刻重置成"现在 + rotateHours"
    // manageProcess=true 时会"停 RustDesk → 写 → 起"（之前没跑且模式为 Off 就不拉起）。
    // 返回是否成功；失败原因写进 errorOut。
    bool applyNow(QString* errorOut = nullptr, bool manageProcess = true);

    // 立刻换一个新密码并重新计时（限时模式下"这个码给错人了"用）。
    bool regenerate(QString* errorOut = nullptr, bool manageProcess = true);

    // 按配置启动定时器。构造后由 main 调一次。
    void start();

    // 无头验收用：不等定时器，立刻跑一次到点判定（同 ConnectionWatcher::pollNow）
    void pollNow() { onTick(); }

signals:
    void passwordChanged(const QString& plain);
    void nextRotationChanged(const QDateTime& when);
    void statusMessage(const QString& msg);

private slots:
    void onTick();
    void onBootstrapTick();       // 首次补密码的"再等一眼"
    void onRemoteConnected();     // 「用完即换」：记下"这一段有人来过"
    void onRemoteDisconnected();  // 「用完即换」：人走了 → 起宽限定时器
    void onOnceGraceElapsed();    // 宽限期到点：还没人回来才真换

private:
    void scheduleNext();
    void persist() const;

    // 「用完即换」要不要盯着连接日志（只有这一档需要，别的档不该白占着轮询）
    void syncWatcher();

    // 日志监听对象。**不一定是自己造的** —— main 里建一个分给几方（见构造函数注释）。
    ConnectionWatcher* m_watcher = nullptr;
    QTimer*            m_onceTimer = nullptr;   // 宽限定时器
    bool               m_sawSession = false;    // 上一段会话有人真的连进来过

    // 首次运行补密码这一整块（start() 和重试定时器都走它，判据只有一份）
    void maybeBootstrapPassword();

    // applyNow / regenerate 的公共实现：reuseExisting=false 就强制生成新密码
    bool applyInternal(bool reuseExisting, QString* errorOut, bool manageProcess);

    // 限时到点：清掉密码并把模式退回 Off。停不掉 RustDesk 时返回 false（下一跳重试）。
    bool expireTimed();

    AppConfig*          m_config = nullptr;
    RustDeskController* m_rd = nullptr;
    QTimer*             m_timer = nullptr;
    QTimer*             m_bootstrapTimer = nullptr;
    int                 m_bootstrapTries = 0;
};

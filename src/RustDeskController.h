#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QProcess;

// 进程级封装 RustDesk CLI —— 外壳不链接任何 RustDesk 库，只调 exe 读 stdout。
// RustDesk 升级换 bin/ 下的运行时即可，外壳不受影响。
//
// 打包决策（踩过坑之后的结论）：**不随包发便携 launcher，直接发解包后的运行时**。
// 便携 rustdesk.exe 是个 25MB 的 launcher，会先把 80MB/106 个文件解到
// %LOCALAPPDATA%\rustdesk\ 再 re-exec；那层在无控制台进程里会卡在解包检查阶段
// （实测 >10s 且永远不产出 ID），而且是整个方案里唯一一处系统写入，跟"纯绿色"冲突。
// 直接把解包结果放进 bin/ 就同时解决这两件事（包从 25MB 涨到 80MB，认了）。
//
// 其它实测确认的行为：
//  * 真身 `--get-id` 0.33s 干净返回，退出不留进程，且**不需要**常驻实例
//  * 固定密码不能走 --password（便携模式被拒："Installation and administrative privileges
//    required!"），只能直接写配置文件
//  * 明文写进**顶层** password 就够了，RustDesk 认证侧直接接受（源码
//    `validate_password_storage` 有 legacy plaintext 分支）。它**不会**在启动时清空或
//    改写明文；只有 Config::store() 被触发时才加密成 "00"+base64(...)，而加载时又会解回明文
//  * 因此写密码仍必须在 RustDesk 未运行时进行，否则被进程内的值覆盖
class RustDeskController : public QObject
{
    Q_OBJECT

public:
    explicit RustDeskController(QObject* parent = nullptr);

    void setExePath(const QString& path) { m_exePath = path; }
    QString exePath() const { return m_exePath; }

    bool isExePresent() const;           // 随包的 launcher 在不在
    bool isRunning() const;              // 系统里是否有 rustdesk.exe 进程（**界面进程也算**）

    // **被控端在不在**：进程表里有没有一个带 `--server` 的 rustdesk.exe。
    //
    // ⚠ 这两个判据不能混用，2026-10-10 真出事：看门狗原来用 isRunning()，而
    //   `rustdesk.exe --tray`（RustDesk 自己的界面进程）**也叫 rustdesk.exe** ——
    //   于是服务端死了、只剩一个 --tray 的时候，看门狗以为一切正常，机器**静默变成
    //   谁都连不进来**（连自己都报「目标设备离线或不存在」）。实测：杀掉那个 --tray
    //   之后 6 秒内看门狗就把 --server 拉回来了 —— 判据错，时机没错。
    //
    // 判据是**命令行里的独立 `--server` 标记**，不是进程名。所以本函数必须去问
    // 命令行（wmic，兜底 PowerShell），**不能再省掉这一步**：
    //  · 用 `isRunning()` 做廉价前置剪枝是不行的 —— tasklist 超时返回的 false 和
    //    "真的没有进程"长得一模一样，而判错的代价是看门狗去重拉一个 `--server`，
    //    实测重复的 --server 会在 `\\.\pipe\RustDesk\query` 上撞车、把**正在服务的
    //    那个**带走（2026-10-10 的 12:41:36 那次就是这么死的）。
    //  · 同理，**问不出来时返回 true**（见 .cpp）：不确定就别动。
    bool isServerRunning() const;

    // 从进程命令行列表里挑出"带 `--server` 的那一条"（纯函数，好断言）。
    // 吃的是 `wmic ... get commandline /format:list` 或 PowerShell CIM 那种文本。
    static bool serverInProcessList(const QString& lines);

    // 拉起 / 结束 RustDesk。写密码必须"停 → 写 → 起"：运行中的实例把配置读进了
    // 进程内的全局 CONFIG，之后不回读文件，还会在 store() 时把内存里的旧密码写回去。
    //
    // **用 `--server` 拉，不用裸 exe**（2026-10-08 实测）：裸 exe 会弹 RustDesk 主窗口；
    // `--server` 走源码 src/core_main.rs:397 的 start_server(true, false)，单进程 ~30MB、
    // 无窗口、正常注册到 rendezvous。代价是它的日志落在
    // %APPDATA%\RustDesk\log\server\ 而不是主日志 —— ConnectionWatcher 两个都盯。
    bool start();
    bool stop();

    // ---- 常驻（被控端的命根子）----
    // 绿色免安装就没有"开机自启的服务"，人不在电脑前没人去双击 RustDesk，
    // 机器重启后就是彻底连不上。所以外壳自己扛：启动时拉起，掉线自动重拉。
    void setKeepAlive(bool on);
    bool keepAlive() const { return m_keepAlive; }

    // 维护窗口：这段时间看门狗不许自作主张拉进程。
    // applyNow 要"停 → 写密码 → 起"，中间那一下要是被看门狗插进来抢先拉起，
    // 写进去的密码会被进程内的旧值覆盖。用 RAII 保证异常安全。
    class Maintenance
    {
    public:
        explicit Maintenance(RustDeskController* c) : m_c(c)
        {
            if (m_c)
                ++m_c->m_suspendDepth;
        }
        ~Maintenance()
        {
            if (m_c)
                --m_c->m_suspendDepth;
        }
        Maintenance(const Maintenance&) = delete;
        Maintenance& operator=(const Maintenance&) = delete;

    private:
        RustDeskController* m_c;
    };

    // 实际用来发 CLI 的 exe：解包后的真身。没解包时返回空串。
    QString cliExePath() const;

    QString id() const { return m_id; }

    // 异步读本机 ID：先问一次，问不到就拉起实例再轮询重试。
    // 调用方不会阻塞。**结束**时一定会发 `idRefreshFinished()`，用 `id()` 取当前值。
    //
    // ⚠ 别拿 `idChanged` 当"读完了"用：它按名字只表示**值变了**，读到同一个 ID 时
    //   **不会**发。界面若靠它收尾，"点第二次重新读取"就会永远停在"读取中…"
    //   （踩过：按钮一直灰着、大字一直是"读取中…"）。"动作结束" 和 "值变了" 是两件事。
    void refreshId();

    // 把固定密码以明文写进 RustDesk.toml 的**顶层** password。要求 RustDesk 未运行，
    // 且配置文件已存在（即 RustDesk 至少跑过一次）。
    // 写前自动备份到 RustDesk.toml.bak，写完立刻复验，失败则回滚并返回 false。
    bool writePasswordPlain(const QString& plain);

    // 删掉顶层 password 行（"只用一次性密码"模式）。同样带备份与复验。
    bool clearPassword();

    // 读回顶层 password 字段原样内容。明文形态下就是明文；被 RustDesk 加密过之后
    // 是 "00..." 密文——所以只用来判断"设没设过密码"，不要拿去显示。
    QString readPasswordRaw() const;

    // ⚠ **`readPasswordRaw()` 返回空串是有歧义的**：可能"确实没设密码"，也可能
    //   "文件读不到 / 刚被别的进程写了一半"。而这两件事的后果差得很远 ——
    //   "没设"可以放心地去设一个，"读不到"下手就是覆盖别人的东西。
    //   这个探针把歧义拆开，给"要不要写盘"这类判断用。
    //   （真踩过：判据只看 `readPasswordRaw().isEmpty()` 的那段代码，把用户的固定密码
    //     当成"没有"给换掉了。）
    struct PasswordProbe
    {
        bool    exists      = false;  // RustDesk.toml 在不在
        bool    readOk      = false;  // 在的话，字节读进来了吗
        bool    hasPassword = false;  // 顶层有 password 行（明文或密文都算）
        QString raw;                  // 那一行的值
    };
    PasswordProbe probePassword() const;

    // RustDesk2.toml 的 [options] 段读写（M4 的 direct-server / relay-server 要用）
    //
    // ⚠ setOption 是**单个键**的便捷写法，它每写一个键就自己备份一次 .bak。
    //   要成组地改服务器配置，必须用下面的 setServerProfile —— 连调四次 setOption
    //   只会留下最后一次的备份，而且中途失败会留下"写了一半"的配置。
    QString getOption(const QString& key) const;
    bool setOption(const QString& key, const QString& value);

    // ---- 网络路径（M4.1/M4.2）----
    // 一条 [options] 改动。**value 为空串 = 删掉这个键** —— 清掉自建服务器就是把这几个键
    // 删干净，让 RustDesk 回落自己的默认值（rs-ny.rustdesk.com）。写空串而不是删键会
    // 让 RustDesk 拿到一个空的服务器地址，比没写更糟。
    struct OptionEdit
    {
        QString key;
        QString value;
    };

    // 一次写完一组键，**带事务语义**：校验全部参数 → 备份一次 → 全部改在内存里 →
    // 一次写盘 → 整体复验 → 任一失败就整体还原成原文件。
    // 写坏这个文件的后果：RustDesk 加载失败、回落到默认设置（服务器悄悄变回官方），
    // 用户看到的是"我配的自建服务器怎么不生效"。所以宁可整体不写，也不留半成品。
    bool setServerProfile(const QVector<OptionEdit>& edits, QString* err = nullptr);

    // ---- 发起连接（M2）----
    // 参数构造单独拎成纯函数，好让 --booktest 断言"空密码绝不产出 --password ''"这类事
    // ——这种东西错了只会表现成"对面弹了个空密码框"，肉眼很难归因。
    // password 为空就**不传** --password，让 RustDesk 自己弹窗问。
    static QStringList connectArgs(const QString& target, const QString& password);

    // 拉起连接客户端。**持有式 QProcess，不用 startDetached**：
    //  · 要能感知对方窗口关掉（finished）才好收尾/停桥接
    //  · 要能主动断开（terminate → kill）
    // 实测（2026-10-09 探针）：已有 --server 常驻时，--connect 进程**不会**把请求交接给
    // 已有实例然后自己退出 —— 它就是真正的客户端，会一直活到窗口关闭。所以 finished()
    // 就是"会话结束"的可信信号，不需要猜短命退出阈值。
    //
    // ⚠ 密码是走**命令行**的，在任务管理器里明文可见。RustDesk 的 CLI 没有环境变量通道，
    //   只能接受（产品本身已经把密码明文存在 RustDesk.toml 里了）。
    QProcess* spawnConnectClient(const QString& target, const QString& password,
                                 QObject* parent, QString* err = nullptr);

    static QString configDir();         // %APPDATA%\RustDesk\config
    static QString mainConfigPath();    // RustDesk.toml（ID / 密码 / salt）
    static QString optionConfigPath();  // RustDesk2.toml（[options]）

    // ⚠ **只给验收用**：把 mainConfigPath() 指到一份**副本**上（空串 = 还原成真实路径）。
    //
    // 为什么必须有这个口：RustDesk.toml 顶层的 password 是这台机器上**唯一一份**固定密码
    // （RustDesk 只存加密串，外壳读不回明文，删了就真没了）。以前那几个验收是拿**真文件**
    // 跑的，靠"跑完再把原字节写回去"收尾 —— 中间的窗口里，用户机器上的密码就是**不存在**的
    // 状态；一旦进程在那个窗口里被打断（timeout 杀进程 / 崩溃 / Ctrl-C），写回不会发生，
    // 用户的密码当场消失。更坏的是下一次正常启动（判据之一是"toml 里没有 password"）
    // 会**静默**补一个机器生成的新密码 —— 用户既连不上、也拿不到新的。
    // 这不是推演：2026-10-10 真发生过一次。
    // 指到副本之后，真文件在整个验收过程里一个字节都不会被碰。
    static void setMainConfigPathForTest(const QString& path);

    // ---- TOML 配置的字节级读写（PeerPrefs 与本类**共用这一份**）----
    //
    // 为什么收到这里而不是各写各的：**行尾/编码策略只允许有一处实现**。
    // 踩过两次的坑是"写盘去掉了 QIODevice::Text、读的时候还留着"—— 一半的修复比不修更坏，
    // 因为读的时候 \r 被悄悄吃掉，于是"整份文件被改行尾"这件事在**任何**用它的比较里
    // 都看不见：测试全绿，文件早被改了。这种"两半必须一致"的东西不能靠复制粘贴维持。
    //
    // 四条共同约定：
    //  1. **按字节读**（不带 QIODevice::Text），否则 \r 在进内存时就被抹掉
    //  2. 不是合法 UTF-8 就**拒绝**：`QString::fromUtf8` 会静默塞 U+FFFD，
    //     "读进来再写回去"等于把坏文件改得更坏；对 RustDesk.toml 那就是丢身份、ID 被换掉
    //  3. 按 `\n` 拆行后**把行尾的 \r 剥掉**（内部一律用干净的行），拼回去时按原风格加回
    //  4. 回滚用**内存里那份原字节**写回，不靠"先删再 copy 备份"（那中间有个文件不存在的窗口）
    static bool        readConfigBytes(const QString& path, QByteArray* out, QString* why = nullptr);
    static bool        writeConfigBytes(const QString& path, const QByteArray& bytes);
    static QString     detectEol(const QByteArray& bytes);          // "\r\n" 或 "\n"
    static QStringList splitConfigLines(const QByteArray& bytes);   // 按 \n 拆 + 剥掉尾部 \r

signals:
    void idChanged(const QString& id);      // **值变了**才发（读回同一个 ID 不发）
    void idRefreshFinished();               // 每次"读 ID"这个动作收尾都发：成功、失败、放弃重试都发
    void statusMessage(const QString& msg);

private:
    // writePasswordPlain / clearPassword 的公共实现：备份 → 深度感知改写 → 复验 → 失败回滚
    bool rewriteTopPassword(const QString& plain, bool remove);

    void startAttempt();
    void retryLater();
    void watchdogTick();
    void runCliAsync(const QString& cli, const QStringList& args, int timeoutMs,
                     std::function<void(const QString&)> done);
    static QString pickId(const QString& rawOutput);

    // setMainConfigPathForTest 的存储；空 = 用真实的 %APPDATA%\RustDesk\config\RustDesk.toml
    static QString s_mainConfigPathOverride;

    QString m_exePath;
    QString m_id;
    int     m_retriesLeft = 0;
    bool    m_triggeredExtraction = false;

    class QTimer* m_watchdog = nullptr;
    bool          m_keepAlive = false;
    int           m_suspendDepth = 0;      // > 0 时看门狗闭嘴（见 Maintenance）
};

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QIcon>
#include <QLayout>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QGroupBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

#include "AddressBook.h"
#include "AppConfig.h"
#include "Autostart.h"
#include "ConnectPanel.h"
#include "ConnectSession.h"
#include "ConnectionWatcher.h"
#include "DarkTheme.h"
#include "Guide.h"
#include "I18n.h"
#include "MainWindow.h"
#include "NetworkProbe.h"
#include "NetworkProfile.h"
#include "PeerPrefs.h"
#include "NotifyPolicy.h"
#include "PasswordPolicy.h"
#include "RustDeskController.h"
#include "SelfCheck.h"
#include "TrayController.h"
#include "TsnetBridge.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
// 管道名带版本号：以后单例协议要改就直接换名字，不会和新旧实例串味
constexpr const char* kInstanceName = "PureRemote.SingleInstance.v1";

// 取 `--flag value` 形式的参数值；没有则返回空串
QString argValue(const QStringList& args, const QString& flag)
{
    const int i = args.indexOf(flag);
    if (i >= 0 && i + 1 < args.size())
        return args.at(i + 1);
    return QString();
}

#ifdef Q_OS_WIN
// WIN32 子系统默认没有控制台，printf 会掉进黑洞。挂到父进程的控制台才能看到输出。
// Git Bash（mintty）用的是 pty 不是 Windows 控制台，这一步会失败 —— 所以下面还会落一份文件。
void attachParentConsole()
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
}
#else
void attachParentConsole() {}
#endif
} // namespace

// 把本次启动的参数转给已经在跑的实例（M2 起会带 --connect <id>）。
// 转完就退出，不再建窗口。
static void forwardToInstance(const QStringList& args)
{
    QLocalSocket sock;
    sock.connectToServer(QString::fromLatin1(kInstanceName));
    if (!sock.waitForConnected(500))
        return;

    QByteArray payload;
    for (const QString& a : args) {
        payload += a.toUtf8();
        payload += '\n';
    }
    sock.write(payload);
    sock.flush();
    sock.waitForBytesWritten(1000);
    sock.disconnectFromServer();
}

// 验收用：把读到的 ID 打到 stdout 并落盘，不建窗口。
// 存在的意义是让"界面显示的 ID == rustdesk.exe --get-id"这条断言能自动化。
static int printIdAndExit(const AppConfig& config)
{
    attachParentConsole();

    RustDeskController probe;
    probe.setExePath(config.rustDeskPath());

    QString got;
    QEventLoop loop;
    QObject::connect(&probe, &RustDeskController::idChanged, &loop,
                     [&got, &loop](const QString& id) {
                         got = id;
                         loop.quit();
                     });
    QTimer::singleShot(25000, &loop, &QEventLoop::quit);

    // 必须等 loop 跑起来再发起：refreshId() 失败时是同步发信号的，
    // 直接调用会让 quit() 打在 exec() 之前，然后白等到超时。
    QTimer::singleShot(0, &probe, [&probe] { probe.refreshId(); });
    loop.exec();

    // ---- 第 2 次：**值没变**再读一遍，这次只等 idRefreshFinished ----
    // 这是用户真撞到过的 bug 的回归测试：点第二次「重新读取」，ID 跟第一次一样，
    // 于是 idChanged 不发（按名字它就只表示"值变了"）→ 界面永远停在「读取中…」、
    // 按钮一直灰着，只能重启。所以"读完了"必须有**自己的**信号，不能借用 idChanged。
    bool reread = false;
    {
        QEventLoop l2;
        QObject::connect(&probe, &RustDeskController::idRefreshFinished, &l2,
                         [&l2, &reread] {
                             reread = true;
                             l2.quit();
                         });
        QTimer::singleShot(20000, &l2, &QEventLoop::quit);
        QTimer::singleShot(0, &probe, [&probe] { probe.refreshId(); });
        l2.exec();
    }

    // 控制台可能挂不上（Git Bash），所以同时写一份文件供脚本读取
    QFile f(QDir::temp().filePath(QStringLiteral("puremote-lastid.txt")));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(got.toUtf8());
        f.close();
    }
    QFile r(QDir::temp().filePath(QStringLiteral("puremote-lastid-reread.txt")));
    if (r.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        r.write(reread ? "OK" : "TIMEOUT");
        r.close();
    }

    std::printf("%s\n", got.toUtf8().constData());
    std::printf("reread=%s\n", reread ? "OK" : "TIMEOUT");
    std::fflush(stdout);
    return (got.isEmpty() || !reread) ? 1 : 0;
}

// ── 无头验收：RustDesk.toml 顶层 password 的读写 ────────────────────────────
//
// 直接在真配置上跑（跑完恢复原样）——mainConfigPath() 是固定的，没有别的入口。
// 这里要盯死的三件事，都是踩过坑的：
//   1. password 必须落在**顶层**（key_pair 与第一个表头之前），不能落进 [keys_confirmed]
//   2. 文件必须仍是合法 TOML（括号平衡是最低门槛）
//   3. enc_id / key_pair 一个字节都不能动 —— 写坏了 RustDesk 会重新注册、ID 会变
static bool passwordLineIsTopLevel(const QString& content)
{
    const QStringList lines = content.split(QLatin1Char('\n'));
    int pw = -1, kp = -1, tbl = -1, count = 0;
    for (int i = 0; i < lines.size(); ++i) {
        const QString t = lines.at(i).trimmed();
        if (t.startsWith(QStringLiteral("password")) && t.contains(QLatin1Char('='))) {
            ++count;
            if (pw < 0)
                pw = i;
        }
        if (kp < 0 && t.startsWith(QStringLiteral("key_pair")))
            kp = i;
        if (tbl < 0 && t.startsWith(QLatin1Char('[')) && t.endsWith(QLatin1Char(']'))
            && !t.contains(QLatin1Char('=')))
            tbl = i;
    }
    if (pw < 0 || count != 1)
        return false;
    if (kp >= 0 && pw > kp)
        return false;
    if (tbl >= 0 && pw > tbl)
        return false;
    return true;
}

static bool bracketsBalanced(const QString& content)
{
    int  depth = 0;
    bool inStr = false;
    for (const QChar ch : content) {
        if (ch == QLatin1Char('\''))
            inStr = !inStr;
        else if (!inStr) {
            if (ch == QLatin1Char('['))
                ++depth;
            else if (ch == QLatin1Char(']'))
                --depth;
        }
    }
    return depth == 0;
}

// ⚠ **按字节读，不带 QIODevice::Text** —— 带 Text 会在读的时候把 \r 悄悄吃掉，
//   于是"写盘时把 \n 变成 \r\n"这种改动在**任何**用它的比较里都看不见。
//   本项目的文件都是 LF，读进来就是原样字节，比较才作数。
static QString readWholeFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

static bool writeWholeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const qint64 n = f.write(data);
    f.close();
    return n == data.size();
}

// key 必须落在 [options] 表里，不能落在顶层或别的表里 —— 和 password 那个坑同源：
// 位置错了 RustDesk 就当没写。
static bool optionInOptionsTable(const QString& content, const QString& key, const QString& value)
{
    const QStringList lines = content.split(QLatin1Char('\n'));
    bool              inOpt = false;
    bool              found = false;
    for (const QString& raw : lines) {
        const QString t = raw.trimmed();
        if (t.startsWith(QLatin1Char('['))) {          // 任何表头都换表
            inOpt = (t == QStringLiteral("[options]"));
            continue;
        }
        if (!inOpt || !t.startsWith(key) || !t.contains(QLatin1Char('=')))
            continue;
        const QString rhs = t.section(QLatin1Char('='), 1).trimmed();
        found = (rhs == QStringLiteral("'%1'").arg(value)
                 || rhs == QStringLiteral("\"%1\"").arg(value));
    }
    return found;
}

static int runPwTest()
{
    attachParentConsole();

    // ⚠ 整个测试跑在**真文件的副本**上，真文件一个字节都不碰。
    //
    //   以前是拿真文件跑、靠末尾"把原字节写回去"收尾（老注释里认过这个风险）。中间那段
    //   窗口里，这台机器**唯一一份**固定密码是不存在的状态 —— 进程一旦在那里面被打断
    //   （timeout 杀 / 崩溃 / Ctrl-C），写回就不会发生；更坏的是下一次正常启动会**静默**
    //   补一个机器生成的新密码，用户既连不上也拿不到新的。2026-10-10 真发生过一次。
    //   副本是**逐字节的真实内容**（不是编的），所以断言质量不变。
    const QString realPath = RustDeskController::mainConfigPath();
    const QString realBytes = readWholeFile(realPath);
    if (realBytes.isEmpty()) {
        std::printf("SKIP: 读不到 %s\n", realPath.toUtf8().constData());
        return 2;
    }
    const QString path = QDir::temp().filePath(QStringLiteral("puremote-pwtest-RustDesk.toml"));
    QFile::remove(path);
    if (!QFile::copy(realPath, path)) {
        std::printf("SKIP: 复制不出副本 %s\n", path.toUtf8().constData());
        return 2;
    }
    RustDeskController::setMainConfigPathForTest(path);

    const QString original = readWholeFile(path);
    if (original.isEmpty()) {
        std::printf("SKIP: 读不到 %s\n", path.toUtf8().constData());
        RustDeskController::setMainConfigPathForTest(QString());
        return 2;
    }

    const QString origEncIdLine =
        [&original] {
            for (const QString& ln : original.split(QLatin1Char('\n')))
                if (ln.trimmed().startsWith(QStringLiteral("enc_id")))
                    return ln;
            return QString();
        }();

    RustDeskController rd;
    int                fails = 0;
    QStringList        report;
    auto               check = [&fails, &report](const char* name, bool ok) {
        report << QStringLiteral("  [%1] %2").arg(ok ? QStringLiteral("PASS")
                                                     : QStringLiteral("FAIL"), QString::fromUtf8(name));
        if (!ok)
            ++fails;
    };

    report << QStringLiteral("──── --pwtest：RustDesk.toml 顶层 password ────");

    // T1 写入
    check("T1 writePasswordPlain 返回 true", rd.writePasswordPlain(QStringLiteral("PwTest01")));
    check("T2 readPasswordRaw 读回刚写的值", rd.readPasswordRaw() == QStringLiteral("PwTest01"));

    QString cur = readWholeFile(path);
    check("T3 password 在顶层（key_pair / 表头之前，且只有一行）", passwordLineIsTopLevel(cur));
    check("T4 文件括号平衡（TOML 结构没被写坏）", bracketsBalanced(cur));
    check("T5 enc_id 行逐字节未变", cur.split(QLatin1Char('\n')).contains(origEncIdLine));
    // 行尾：写进去的必须还是 LF。带 QIODevice::Text 写会把整份身份文件变成 CRLF，
    // 而"整份文件每一行都变了"这件事，只要比较用的读函数带 Text 就永远看不见（踩过）。
    check("T5b 行尾仍是 LF（没被写成 Windows 的 CRLF）", !cur.contains(QLatin1Char('\r')));

    // T6 覆盖（轮换路径）
    check("T6 覆盖写入返回 true", rd.writePasswordPlain(QStringLiteral("PwTest02")));
    check("T7 覆盖后读回新值", rd.readPasswordRaw() == QStringLiteral("PwTest02"));
    cur = readWholeFile(path);
    check("T8 覆盖后仍是单行顶层 password（没有追加出第二行）", passwordLineIsTopLevel(cur));

    // T9 拒绝会写坏 TOML 的密码
    check("T9 含单引号的密码被拒绝", !rd.writePasswordPlain(QStringLiteral("a'b")));
    check("T10 空密码被拒绝（清空请用 clearPassword）", !rd.writePasswordPlain(QString()));

    // T11 删除
    check("T11 clearPassword 返回 true", rd.clearPassword());
    check("T12 删除后读不到 password", rd.readPasswordRaw().isEmpty());
    cur = readWholeFile(path);
    check("T13 删除后括号仍平衡", bracketsBalanced(cur));
    check("T14 删除后 enc_id 行仍在", cur.split(QLatin1Char('\n')).contains(origEncIdLine));

    // 恢复原样（**副本**恢复）
    QFile restore(path);
    if (restore.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        restore.write(original.toUtf8());
        restore.close();
    }
    check("T15 副本已恢复原字节", readWholeFile(path) == original);

    // T16 才是这一项真正要护住的东西：**用户的真文件逐字节没动**。
    // 上面那些 T1-T15 全是"副本上的行为对不对"，只有这一条能证明"没伤到真东西"。
    check("T16 真 RustDesk.toml 逐字节未被改动（这一项跑的是副本）",
          readWholeFile(realPath) == realBytes);
    RustDeskController::setMainConfigPathForTest(QString());
    QFile::remove(path);

    report << QStringLiteral("──── %1（%2 项失败）────")
                  .arg(fails == 0 ? QStringLiteral("全绿") : QStringLiteral("有失败"))
                  .arg(fails);

    // 控制台可能挂不上（Git Bash / 重定向），所以同时写一份文件供脚本读取
    const QString text = report.join(QLatin1Char('\n')) + QLatin1Char('\n');
    QFile out(QDir::temp().filePath(QStringLiteral("puremote-pwtest.txt")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(text.toUtf8());
        out.close();
    }
    std::printf("%s", text.toUtf8().constData());
    std::fflush(stdout);
    return fails == 0 ? 0 : 1;
}

// ── 无头验收：密码策略 ──────────────────────────────────────────────────────
// 全程 manageProcess=false —— 测的是策略与写盘，不去停/起 RustDesk（那会真的断连接）。
// 跑完把 RustDesk.toml 和 puremote.json 都恢复原样。
static int runPolicyTest()
{
    attachParentConsole();

    QStringList report;
    int         fails = 0;
    auto        check = [&fails, &report](const char* name, bool ok) {
        report << QStringLiteral("  [%1] %2").arg(ok ? QStringLiteral("PASS")
                                                      : QStringLiteral("FAIL"),
                                                   QString::fromUtf8(name));
        if (!ok)
            ++fails;
    };

    report << QStringLiteral("──── --poltest：密码策略 ────");

    // ---- 随机密码 ----
    const QString p1 = PasswordPolicy::generatePassword();
    check("P1 默认长度 10", p1.size() == 10);
    check("P2 指定长度生效", PasswordPolicy::generatePassword(16).size() == 16);

    const QString ambiguous = QStringLiteral("0O1lIo");
    bool          clean     = true;
    for (const QChar c : p1) {
        if (ambiguous.contains(c))
            clean = false;
    }
    check("P3 不含易混字符 0O1lIo", clean);

    QSet<QString> seen;
    for (int i = 0; i < 500; ++i)
        seen.insert(PasswordPolicy::generatePassword());
    check("P4 500 次生成无重复", seen.size() == 500);

    // ---- 「关闭」模式下那个按钮会不会删掉现有密码 ----
    //
    // 这是**数据安全**的判据，不是界面矫情：关闭模式按「立即应用」真正做的是
    // clearPassword()，会把本机现有的固定密码删掉。真踩过 —— 清过配置之后默认模式
    // 就是「关闭」，用户点一下，RustDesk.toml 里那个**原生加密的**固定密码就没了；
    // 现象是"连接别的电脑 → 密码错误"，而根因在被控端已经没有固定密码，
    // 且这台机器没人看着时再也连不进来。
    // 判据钉死的要点是：**光看外壳自己记的明文不够** —— 最容易被误删的恰恰是
    // 外壳读不回明文的那一种（界面上显示成「已设置（密文）」，看着完全不像能删的东西）。
    check("P4a 两边都空 → 本来就没密码，点了不该弹确认",
          !PasswordPolicy::offModeWipesExisting(QString(), QString()));
    check("P4b 外壳记着明文 → 有密码，删之前要确认",
          PasswordPolicy::offModeWipesExisting(QStringLiteral("Ab3dEf9h"), QString()));
    check("P4c 只有 toml 里那串（外壳读不回明文）→ 同样要确认",
          PasswordPolicy::offModeWipesExisting(QString(), QStringLiteral("00AXCmr8xyz")));

    // ---- 首次运行"自动补一个固定密码"的边界 ----
    //
    // 绿色包默认「关闭」= 没有固定密码，而 RustDesk 的一次性密码命令行读不出来
    // （只有 --get-id/--set-id/--config），开箱之后这台机器**谁都连不进来**，
    // 用户连临时密码是多少都看不到。所以第一次跑的时候补一个。
    //
    // 这次是**写盘**，而且**会替换掉机器上原有的固定密码**，所以边界只有一个方向：
    // 只有"这台机器上从来没跑过外壳"才动手。分成几条是因为这几个方向各自都有踩坑的写法：
    //   P5b —— 已有的密码是外壳读不出明文的那种：**现在照样接管**（2026-10-10 改的，
    //          见 shouldBootstrapPassword；以前这里是"一个字都不许碰"）
    //   P5c —— 用户自己选过「关闭」之后不能再补，否则「关闭」变成摆设
    //   P5d —— 轮换/限时各有各的到点逻辑，不该由这条兜底插手
    using M  = PasswordPolicy::Mode;
    using BI = PasswordPolicy::BootstrapInputs;
    // 造一个"全新机器"的输入，各条断言只改自己关心的那一项
    const auto freshMachine = [] {
        BI in;
        in.mode         = M::Off;
        in.freshConfig  = true;    // 外壳配置文件夹里连 puremote.json 都没有
        in.tomlExists   = false;   // 连 RustDesk.toml 都还没生成
        return in;
    };

    {
        BI in      = freshMachine();
        in.tomlExists  = true;     // 内核已经把配置落下来了
        in.tomlReadOk  = true;
        check("P5a 全新机器、RustDesk.toml 读到了、两边都没有密码 → 该补一个",
              PasswordPolicy::shouldBootstrapPassword(in));
        check("P5a2 文件还没生成时**不写**（写也写不进去），交给重试",
              !PasswordPolicy::shouldBootstrapPassword(freshMachine()));
    }

    {
        // ⚠ 2026-10-10 **反过来了**，别照着旧版改回去。
        // 旧判据（"机器上已经有密码就一个字都不碰"）把**新用户第一次打开最常撞上的
        // 状态**排除在外：本机那行只有「已设置（密文）」+ 一个没人看得懂的按钮，
        // 用户得先理解并做一个关于凭据的选择才能用。用户实机原话："大部分人看不懂"。
        // 现在首次运行**一律接管**：开箱就是外壳管的密码，界面上直接能看能复制。
        BI in = freshMachine();
        in.tomlExists      = true;
        in.tomlReadOk      = true;
        in.tomlHasPassword = true;   // 外壳读不回明文的那种原生加密串
        check("P5b 首次运行 + 已有原生加密的固定密码 → **接管**（换掉它，原来那个作废）",
              PasswordPolicy::shouldBootstrapPassword(in));

        // 但"读不到"仍然要拦住 —— 那是"不知道"，不是"没有"（P5f 钉的就是这一条）。
        in.tomlReadOk = false;
        check("P5b2 首次运行 + toml 读不到 → 还是不写（不知道就别下手）",
              !PasswordPolicy::shouldBootstrapPassword(in));
    }
    {
        BI in = freshMachine();
        in.alreadyBootstrapped = true;   // 用户自己选过「关闭」
        check("P5c 用户自己选过「关闭」→ 不再补，免得把删掉的密码偷偷复活",
              !PasswordPolicy::shouldBootstrapPassword(in));
    }
    {
        BI in = freshMachine();
        in.mode = M::Rotate;
        const bool rot = !PasswordPolicy::shouldBootstrapPassword(in);
        in.mode = M::Timed;
        const bool tim = !PasswordPolicy::shouldBootstrapPassword(in);
        in.mode        = M::Fixed;
        in.shellPlain  = QStringLiteral("Ab3dEf9h");
        check("P5d 「定时轮换 / 限时」有自己的到点逻辑 → 这条不插手",
              rot && tim && !PasswordPolicy::shouldBootstrapPassword(in));
    }
    {
        // ⚠⚠ 这一条是**这次真出事**的那一种，别删。
        // 判据一旦落到"RustDesk.toml 里现在没有 password 这一行"上，就等于把**别人的
        // 文件在某一瞬间的内容**当成了事实 —— 而 RustDesk 自己会在启动/退出时整份重写
        // 它，那一瞬间读到的可能正好是"没有密码"。实测真发生过：开发机上跑验收，外壳把
        // 用户原生的加密固定密码当成"没有"，写了一个自己生成的新密码进去。
        // 所以必须要求"这台机器从来没跑过外壳"，而不是"外壳觉得自己没做过决定"。
        BI in = freshMachine();
        in.freshConfig = false;      // 跑过外壳（配置文件夹里有 puremote.json）
        in.tomlExists  = true;
        in.tomlReadOk  = true;
        in.tomlHasPassword = false;  // 这一瞬间读到的确实"没有密码"
        check("P5e 不是首次运行 → 哪怕这一瞬间读到的「没有密码」也绝不写"
              "（别人正在重写那个文件，读到的不是事实）",
              !PasswordPolicy::shouldBootstrapPassword(in));

        in.tomlReadOk = false;       // 文件在，但读不到 = "不知道"，不是"没有"
        check("P5f RustDesk.toml 在但读不到 → 不知道就别动别人的东西",
              !PasswordPolicy::shouldBootstrapPassword(in));
    }
    {
        // "不该补" 和 "还没到时候" 必须分开：全新机器上内核刚被拉起来，
        // RustDesk.toml 通常还没生成 —— 判死在这儿，"开箱即用"就永远不生效了
        // （下次启动配置文件夹里已经有 puremote.json，不再算首次运行）。
        const BI notYet = freshMachine();          // tomlExists=false
        check("P5g 全新机器、RustDesk.toml 还没生成 → 不是判死，是「过几秒再看一眼」",
              !PasswordPolicy::shouldBootstrapPassword(notYet)
                  && PasswordPolicy::bootstrapPending(notYet));

        BI in = freshMachine();
        in.freshConfig = false;                    // 跑过外壳
        check("P5h 跑过外壳的机器：既不补，也不挂重试",
              !PasswordPolicy::shouldBootstrapPassword(in)
                  && !PasswordPolicy::bootstrapPending(in));
    }

    // ---- 备份要动到的两个文件 ----
    const QString cfgPath = AppConfig::configFilePath();
    const QString rdRealPath = RustDeskController::mainConfigPath();   // 真文件：只读、只用来比对
    const QByteArray cfgOrig = QFile::exists(cfgPath) ? QByteArray() : QByteArray();
    QByteArray       cfgBackup;
    QByteArray       rdBackup;
    {
        QFile f(cfgPath);
        if (f.open(QIODevice::ReadOnly))
            cfgBackup = f.readAll();
    }
    rdBackup = readWholeFile(rdRealPath).toUtf8();
    Q_UNUSED(cfgOrig);
    if (rdBackup.isEmpty()) {
        report << QStringLiteral("  SKIP 读不到 RustDesk.toml");
    }

    // ⚠ 跟 --pwtest 同一个道理：这一项会真的把 toml 里的 password 清掉（P17 关档）
    //   再生成新的（P28/P31），而那是这台机器**唯一一份**固定密码。所以整个测试改到
    //   **真文件的副本**上跑 —— 真文件一个字节都不碰，中途被打断（timeout 杀 / 崩溃）
    //   也不会毁掉用户的密码。副本就是逐字节的真实内容，断言质量不变。
    //   ⚠ 只把 **RustDesk.toml** 挪到副本上，puremote.json 仍然在真文件上跑（P37 恢复）：
    //     两者的"毁掉"不是一回事 —— 外壳自己的配置里，每个字段我们手里都有一份（contacts /
    //     authkey / 语言全在内存里，恢复就是原字节写回）；而 RustDesk.toml 里那串加密密码
    //     **谁都读不回来**，它丢了就是永久丢了。这里护的是那一个不可恢复的东西。
    const QString rdWorkPath =
        QDir::temp().filePath(QStringLiteral("puremote-poltest-RustDesk.toml"));
    QFile::remove(rdWorkPath);
    if (!rdBackup.isEmpty() && QFile::copy(rdRealPath, rdWorkPath))
        RustDeskController::setMainConfigPathForTest(rdWorkPath);
    const QString rdPath = RustDeskController::mainConfigPath();   // 下面全在副本上动

    AppConfig          cfg;
    RustDeskController rd;
    cfg.load();
    rd.setExePath(cfg.rustDeskPath());
    // 给测试一个**不盯任何文件**的监听：「用完即换」那一档会去要日志监听，真让它在测试里
    // 轮询用户真实的 RustDesk 日志，测试结果就取决于"此刻有没有人连着"了。
    ConnectionWatcher testWatcher;
    testWatcher.setLogPaths({});
    PasswordPolicy pol(&cfg, &rd, &testWatcher);

    // ---- 模式与调度 ----
    pol.setMode(PasswordPolicy::Mode::Fixed);
    check("P5 模式写成 fixed", cfg.passwordMode() == QStringLiteral("fixed"));
    pol.setRotateHours(7);
    check("P6 轮换间隔写成 7", cfg.rotateHours() == 7);
    check("P7 Fixed 模式没有下次轮换时刻", !pol.nextRotationAt().isValid());

    pol.setMode(PasswordPolicy::Mode::Rotate);
    check("P8 Rotate 模式有下次轮换时刻", pol.nextRotationAt().isValid());

    cfg.setPasswordRotatedAt(QDateTime::currentDateTime().addSecs(-7 * 3600 - 60));
    check("P9 距上次轮换超过间隔即判定到期",
          pol.nextRotationAt() <= QDateTime::currentDateTime());

    // ---- 「用完即换」这一档 ----
    // 用户要的是"连一次就自动换密码"。RustDesk 原生的一次性密码**不是**这样的
    // （二进制里唯一的自动轮换是"连续输错太多次才换"），所以这一档是外壳自己实现的：
    // 触发点是日志里的**会话结束**标记，也就是"人走了之后"。放在"有人连上来"那一刻
    // 会把刚连上的那位直接掐断 —— 换密码必须重启 RustDesk。
    pol.setMode(PasswordPolicy::Mode::Once);
    check("P9a Once 落盘", cfg.passwordMode() == QStringLiteral("once"));
    check("P9b Once 没有「到点」这回事（触发点是「有人连过又走了」，不是时间）",
          !pol.nextRotationAt().isValid());

    // 该不该换：三个方向都要钉住
    {
        const QDateTime t0 = QDateTime::currentDateTime();
        check("P9c 没人连过 → 不换",
              !PasswordPolicy::shouldRotateAfterSession(false, QDateTime(), t0));
        check("P9d 有人连过、从来没换过 → 换",
              PasswordPolicy::shouldRotateAfterSession(true, QDateTime(), t0));
        check("P9e 刚从换过 5 秒 → 不换（客户端反复掉线重连不能把 RustDesk 反复重启）",
              !PasswordPolicy::shouldRotateAfterSession(true, t0.addSecs(-5), t0));
        check("P9f 上次换是 31 秒前 → 换",
              PasswordPolicy::shouldRotateAfterSession(true, t0.addSecs(-31), t0));
        check("P9g 恰好 30 秒 → 换（边界算在内）",
              PasswordPolicy::shouldRotateAfterSession(true, t0.addSecs(-30), t0));
    }
    pol.setMode(PasswordPolicy::Mode::Rotate);   // 下面 P10 起走的是轮换

    // ---- 「未应用」标记（界面上那行小字凭什么这么说）----
    //
    // setMode() 只写外壳配置，真正落地要按「立即应用」。所以"档位显示「关闭」、机器上
    // 那个固定密码却还在生效"这种状态是**可能出现**的 —— 界面必须把它说出来，而不是
    // 让按钮名字替用户猜。
    check("P9h 非「关闭」档 + toml 里没有密码 → 未应用",
          PasswordPolicy::needsApply(M::Fixed, QStringLiteral("Ab3dEf9h"), QString()));
    check("P9i 非「关闭」档 + toml 那串跟外壳记的不一样 → 未应用（漂移）",
          PasswordPolicy::needsApply(M::Fixed, QStringLiteral("Ab3dEf9h"),
                                     QStringLiteral("00AXCmr8xyz")));
    check("P9j 逐字相同 → 已应用",
          !PasswordPolicy::needsApply(M::Fixed, QStringLiteral("Ab3dEf9h"),
                                      QStringLiteral("Ab3dEf9h")));
    check("P9k 「关闭」档 + toml 里还留着密码（含加密串）→ 未应用，该删还没删",
          PasswordPolicy::needsApply(M::Off, QString(), QStringLiteral("00AXCmr8xyz")));
    check("P9l 「关闭」档 + toml 也空 → 已应用",
          !PasswordPolicy::needsApply(M::Off, QString(), QString()));
    check("P9m 两边都空的固定密码档 → 未应用（还没落过地）",
          PasswordPolicy::needsApply(M::Fixed, QString(), QString()));

    // ---- 「立即应用」会不会把读不出明文的那个密码换掉 ----
    // RustDesk 里那个加密存的固定密码**读不回明文**（也只能这样），所以要换成外壳能
    // 看能复制的那一个，只能**换掉它** —— 点「立即应用」之前必须先弹确认框。
    // 但不能在「关闭」档给这条 —— 那一档用户要的就是"没有固定密码"，正确动作是删。
    check("P9n 非「关闭」档 + 有密文 + 外壳没记明文 → 可以接管",
          PasswordPolicy::canTakeOverPassword(M::Fixed, QString(), QStringLiteral("00AXCmr8xyz")));
    check("P9o 「关闭」档不给这条出口（那一档要的是删，不是再装一个）",
          !PasswordPolicy::canTakeOverPassword(M::Off, QString(), QStringLiteral("00AXCmr8xyz")));
    check("P9p 外壳已经记着明文 → 不需要「接管」",
          !PasswordPolicy::canTakeOverPassword(M::Fixed, QStringLiteral("Ab3dEf9h"),
                                               QStringLiteral("Ab3dEf9h")));
    check("P9q toml 里没有密码 → 没什么可接管的",
          !PasswordPolicy::canTakeOverPassword(M::Fixed, QString(), QString()));

    // ---- 开机自启：往 HKCU\...\Run 写的那一行 ----
    //
    // 这里一个字节的注册表都不碰（写入路径只能靠实测，见验收第 9 项）。下面是**纯粹
    // 拼/解字符串**的那半 —— 而它恰恰是最容易悄悄写错的地方：写错了不报错、不崩，
    // 只是开机什么都不发生，而用户唯一的发现方式就是"重启之后连不上"。
    {
        const QString spaced = QStringLiteral("C:\\Program Files\\PureRemote\\PureRemote.exe");
        const QString plain  = QStringLiteral("C:\\PureRemote\\PureRemote.exe");

        // 带空格的路径必须加引号：不加的话系统把 "C:\Program" 当程序名、
        // "Files\..." 当参数，开机时静默什么都不发生。
        check("P9r 带空格的 exe 路径写进 Run 值时带引号，且带上 --tray",
              Autostart::runValue(spaced)
                  == QStringLiteral("\"C:\\Program Files\\PureRemote\\PureRemote.exe\" --tray"));
        check("P9s 不带空格的路径不加引号（引号不是装饰，能省就省）",
              Autostart::runValue(plain)
                  == QStringLiteral("C:\\PureRemote\\PureRemote.exe --tray"));

        check("P9t 从带引号的那一行里能把 exe 抠回来（空格不会被当成参数分隔）",
              Autostart::exeFromRunValue(Autostart::runValue(spaced)) == spaced);
        check("P9u 没引号的命令行按第一个空白切（兜的是用户手动改过注册表）",
              Autostart::exeFromRunValue(QStringLiteral("C:\\a\\b.exe --tray"))
                  == QStringLiteral("C:\\a\\b.exe"));

        // ★ 这两条护的是"每次启动都白改一遍注册表"：Windows 路径大小写不敏感、
        //   / 和 \ 等价，拿裸字符串比会把同一个文件判成不同 → needsRepair 恒真 →
        //   外观上完全看不出来（功能没坏，只是在写注册表），只有断言看得见。
        check("P9v 大小写不同 + 斜杠方向不同的同一个路径算同一个 exe",
              Autostart::sameExePath(QStringLiteral("C:\\Program Files\\A\\B.EXE"),
                                     QStringLiteral("c:/program files/a/b.exe")));
        // ⚠ 这里**不能拿带空格的路径**去喂：Run 值里带空格的路径是我们自己加了引号的，
        //   写成不带引号的形式就是"用户手改坏了"，解析器按设计只取第一个空白前的内容。
        //   用不带空格的路径才测得到"写法不同但同一个文件"这件事本身。
        check("P9w 同一个 exe（只是大小写/斜杠写法不同）不判成「需要修」",
              !Autostart::needsRepair(QStringLiteral("C:/Apps/App.EXE --tray"),
                                      QStringLiteral("c:/apps/app.exe")));

        // 空值 = 没开自启，不是"坏了" —— 判成要修就等于在用户没要的情况下装了个启动项。
        check("P9x 注册表里没有这条值 → 不是「需要修」（绝不凭空装启动项）",
              !Autostart::needsRepair(QString(), QStringLiteral("C:\\A\\b.exe")));

        check("P9y 绿色包被挪了目录（路径对不上）→ 需要修",
              Autostart::needsRepair(QStringLiteral("\"D:\\old\\PureRemote.exe\" --tray"),
                                     QStringLiteral("C:\\new\\PureRemote.exe")));
        check("P9z 那一行里没有 --tray（老版本写的 / 被人改过）→ 需要修"
              "（否则开机会弹窗口）",
              Autostart::needsRepair(QStringLiteral("\"C:\\A\\PureRemote.exe\""),
                                     QStringLiteral("C:\\A\\PureRemote.exe")));
    }

    // ---- 真实写盘 ----
    if (rd.isRunning()) {
        report << QStringLiteral("  SKIP RustDesk 正在运行，跳过写盘部分（先关掉它再跑）");
    } else if (!rdBackup.isEmpty()) {
        QString err;

        check("P10 applyNow(Rotate) 成功", pol.applyNow(&err, /*manageProcess=*/false));
        const QString pw1 = cfg.password();
        check("P11 外壳记住了新密码", !pw1.isEmpty() && pw1.size() == 10);
        check("P12 RustDesk.toml 顶层就是它", rd.readPasswordRaw() == pw1);
        check("P13 写进去的 password 在顶层（没落进表里）",
              passwordLineIsTopLevel(readWholeFile(rdPath)));

        check("P14 再轮换一次成功", pol.applyNow(&err, false));
        check("P15 轮换后密码确实变了", !cfg.password().isEmpty() && cfg.password() != pw1);
        check("P16 新密码也写进了 toml", rd.readPasswordRaw() == cfg.password());

        pol.setMode(PasswordPolicy::Mode::Off);
        const bool offOk = pol.applyNow(&err, false);
        check("P17 Off 模式清掉 toml 里的密码", offOk && rd.readPasswordRaw().isEmpty());
        check("P18 Off 模式外壳也不留明文", cfg.password().isEmpty());

        // ⚠ 这条护的是刚刚才补上的那个回退：用户**主动**选「关闭」把密码删掉，是有意的；
        //   要是没留下"已经做过决定"的标记，下次启动「首次运行补密码」会把它装回去，
        //   用户会以为「关闭」这一档坏了。删也是决定的一种 —— 落地就得置位。
        {
            PasswordPolicy::BootstrapInputs bi;
            bi.mode                = pol.mode();
            bi.freshConfig         = false;     // 这台机器跑过外壳
            bi.shellPlain          = cfg.password();
            bi.tomlExists          = true;
            bi.tomlReadOk          = true;
            bi.tomlHasPassword     = !rd.probePassword().raw.isEmpty();
            bi.alreadyBootstrapped = cfg.passwordBootstrapped();
            check("P18b Off 落地后置了「已经做过决定」，下次启动不会再自动补回来",
                  cfg.passwordBootstrapped()
                      && !PasswordPolicy::shouldBootstrapPassword(bi));
        }

        // 固定密码模式：再设一次，确认不轮换时密码保持稳定
        pol.setMode(PasswordPolicy::Mode::Fixed);
        check("P19 Fixed 首次应用成功", pol.applyNow(&err, false));
        const QString fixedPw = cfg.password();
        check("P20 Fixed 再应用仍是同一个密码",
              pol.applyNow(&err, false) && cfg.password() == fixedPw);

        // ---- 限时有效（Timed）----
        // 语义和轮换正相反：**密码本身不变**，变的是作废时刻。因为码是要发给别人的，
        // 重新计时的同时换码等于把已经拿到码的人踢掉。
        pol.setMode(PasswordPolicy::Mode::Timed);
        check("P21 Timed 模式落盘", cfg.passwordMode() == QStringLiteral("timed"));

        pol.setRotateHours(2);
        check("P22 Timed 首次应用成功", pol.applyNow(&err, false));
        const QString timedPw = cfg.password();
        check("P23 Timed 生成并写进了 toml",
              !timedPw.isEmpty() && rd.readPasswordRaw() == timedPw);

        const QDateTime exp1 = pol.expiresAt();
        check("P24 Timed 有作废时刻", exp1.isValid() && pol.nextRotationAt() == exp1);
        {
            // 作废时刻应该落在 "现在 + 2 小时" 附近（留 5 分钟余量给慢机器）
            const qint64 secs = QDateTime::currentDateTime().secsTo(exp1);
            check("P25 作废时刻≈现在+2小时", secs > 2 * 3600 - 300 && secs <= 2 * 3600);
        }

        check("P26 Timed 再应用沿用同一个密码（不换码）",
              pol.applyNow(&err, false) && cfg.password() == timedPw);
        check("P27 Timed 再应用把作废时刻往后推", pol.expiresAt() >= exp1);

        // 换一个码：强制生成新的
        check("P28 regenerate 换出新密码",
              pol.regenerate(&err, false) && cfg.password() != timedPw);
        check("P29 新密码也落了盘", rd.readPasswordRaw() == cfg.password());

        // 到期作废：把作废时刻造到过去，onTick 应该清掉密码并把模式退回 Off
        cfg.setPasswordExpiresAt(QDateTime::currentDateTime().addSecs(-60));
        check("P30 造出已过期的限时码",
              pol.expiresAt().isValid() && pol.expiresAt() <= QDateTime::currentDateTime());
        pol.pollNow();
        check("P31 到期后密码被清掉", rd.readPasswordRaw().isEmpty());
        check("P32 到期后外壳也不留明文", cfg.password().isEmpty());
        check("P33 到期后模式退回 Off", cfg.passwordMode() == QStringLiteral("off"));
        check("P34 到期后没有作废时刻了", !cfg.passwordExpiresAt().isValid());

        // 启动补作废：关机期间到期的不能丢
        pol.setMode(PasswordPolicy::Mode::Timed);
        pol.applyNow(&err, false);
        cfg.setPasswordExpiresAt(QDateTime::currentDateTime().addSecs(-3600));
        pol.start();
        check("P35 start() 补上了关机期间漏掉的作废",
              rd.readPasswordRaw().isEmpty() && cfg.passwordMode() == QStringLiteral("off"));
    }

    // ---- 恢复 ----
    {
        QFile f(rdPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(rdBackup);
    }
    if (cfgBackup.isEmpty()) {
        QFile::remove(cfgPath);               // 原来就没有，删掉测试产物
    } else {
        QFile f(cfgPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(cfgBackup);
    }
    check("P36 RustDesk.toml 副本已恢复", readWholeFile(rdPath) == QString::fromUtf8(rdBackup));
    {
        QFile f(cfgPath);
        bool  same = false;
        if (cfgBackup.isEmpty()) {
            same = !f.exists();
        } else if (f.open(QIODevice::ReadOnly)) {
            same = (f.readAll() == cfgBackup);
        }
        check("P37 puremote.json 已恢复", same);
    }
    // P38 才是这一项真正要护住的东西：**用户的真文件逐字节没动**。
    // P36 只是"副本收干净了"，它证明不了没伤到真东西。
    check("P38 真 RustDesk.toml 逐字节未被改动（这一项跑的是副本）",
          readWholeFile(rdRealPath) == QString::fromUtf8(rdBackup));
    RustDeskController::setMainConfigPathForTest(QString());
    QFile::remove(rdWorkPath);

    report << QStringLiteral("──── %1（%2 项失败）────")
                  .arg(fails == 0 ? QStringLiteral("全绿") : QStringLiteral("有失败"))
                  .arg(fails);

    const QString text = report.join(QLatin1Char('\n')) + QLatin1Char('\n');
    QFile         out(QDir::temp().filePath(QStringLiteral("puremote-poltest.txt")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(text.toUtf8());
        out.close();
    }
    std::printf("%s", text.toUtf8().constData());
    std::fflush(stdout);
    return fails == 0 ? 0 : 1;
}

static int runConnTest()
{
    attachParentConsole();

    QStringList report;
    int         fails = 0;
    auto        check = [&fails, &report](const char* name, bool ok) {
        report << QStringLiteral("  [%1] %2").arg(ok ? QStringLiteral("PASS")
                                                      : QStringLiteral("FAIL"),
                                                   QString::fromUtf8(name));
        if (!ok)
            ++fails;
    };

    report << QStringLiteral("──── --conntest：连接提示 ────");

    AppConfig          cfg;
    RustDeskController rd;
    cfg.load();
    rd.setExePath(cfg.rustDeskPath());
    NotifyPolicy nt(&cfg, &rd);

    const QString cfgPath  = AppConfig::configFilePath();
    const QString optPath  = RustDeskController::optionConfigPath();
    const QByteArray cfgBackup = [&cfgPath] {
        QFile f(cfgPath);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();
    const QByteArray optBackup = readWholeFile(optPath).toUtf8();
    const QString    modeOrig  = cfg.notifyMode();

    // ---- A. 模式持久化 ----
    nt.setMode(NotifyPolicy::Mode::Toast);
    check("N1 Toast 落盘", cfg.notifyMode() == QStringLiteral("toast"));
    nt.setMode(NotifyPolicy::Mode::Confirm);
    check("N2 Confirm 落盘", cfg.notifyMode() == QStringLiteral("confirm"));
    nt.setMode(NotifyPolicy::Mode::Off);
    check("N3 Off 落盘", cfg.notifyMode() == QStringLiteral("off"));
    nt.setMode(NotifyPolicy::Mode::Toast);
    check("N4 三值往返不串味", cfg.notifyMode() == QStringLiteral("toast"));

    // 新开一个 AppConfig 从盘上读，确认真的写进文件了（不是只改了内存）
    {
        AppConfig reread;
        reread.load();
        check("N5 重新读盘还是 toast", reread.notifyMode() == QStringLiteral("toast"));
    }

    // ---- B. approve-mode 落盘 ----
    // RustDesk 在跑的时候不能写：它退出时会把内存里的 CONFIG2 回写，把我们的值盖掉。
    // 这和 poltest 的约定一致 —— 宁可 SKIP 也不做假阳性断言。
    if (rd.isRunning()) {
        report << QStringLiteral("  SKIP RustDesk 正在运行，跳过 approve-mode 写盘部分");
    } else if (optBackup.isEmpty()) {
        report << QStringLiteral("  SKIP 读不到 RustDesk2.toml");
    } else {
        QString err;
        nt.setMode(NotifyPolicy::Mode::Confirm);
        check("N6 Confirm → applyNow 成功", nt.applyNow(&err, /*manageProcess=*/false));
        check("N7 approve-mode 写成 click",
              rd.getOption(QStringLiteral("approve-mode")) == QStringLiteral("click"));
        check("N8 而且确实写在 [options] 表里（不是顶层/别的表）",
              optionInOptionsTable(readWholeFile(optPath), QStringLiteral("approve-mode"),
                                   QStringLiteral("click")));

        nt.setMode(NotifyPolicy::Mode::Toast);
        check("N9 Toast → applyNow 成功", nt.applyNow(&err, false));
        check("N10 Toast 时 approve-mode 是 password（提示不拦连接）",
              rd.getOption(QStringLiteral("approve-mode")) == QStringLiteral("password"));

        nt.setMode(NotifyPolicy::Mode::Off);
        check("N11 Off → applyNow 成功", nt.applyNow(&err, false));
        check("N12 Off 时 approve-mode 也是 password",
              rd.getOption(QStringLiteral("approve-mode")) == QStringLiteral("password"));

        check("N13 写完 RustDesk2.toml 仍是合法 TOML（括号平衡）",
              bracketsBalanced(readWholeFile(optPath)));
    }

    // ---- C. 日志监听（临时文件，完全不碰真日志）----
    {
        // 下面两条"真身"是从 2026-10-08 一次真实连接（本机连本机）的
        // %APPDATA%\RustDesk\log\rustdesk_rCURRENT.log 里逐字节抄下来的（payload 截断）。
        // 关键是**带时间戳前缀**：真日志每行前面都有 [2026-10-08 16:11:15.755192 +08:00]，
        // 自己手写的短行测不出前缀会不会把匹配挡住。
        const QByteArray realServer =
            "[2026-10-08 16:11:15.755192 +08:00] INFO [src\\server\\connection.rs:2117] "
            "peer info supported_encoding: SupportedEncoding { h264: true, h265: true }\n";
        const QByteArray realClient =
            "[2026-10-08 16:11:15.795493 +08:00] INFO [src\\client.rs:3769] "
            "peer info supported_encoding:SupportedEncoding { h264: true, h265: true }\n";

        const QString tmp = QDir::temp().filePath(QStringLiteral("puremote-conntest.log"));
        QFile::remove(tmp);
        {
            QFile f(tmp);
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            f.write(realServer);                     // 开监听之前就有的连接
            f.write("INFO [src\\rendezvous_mediator.rs:99] something else\n");
        }

        ConnectionWatcher w;
        int               hits = 0;
        QObject::connect(&w, &ConnectionWatcher::remoteConnected, [&hits] { ++hits; });

        w.setLogPaths({ tmp });
        w.setEnabled(true);          // prime 到文件末尾

        w.pollNow();
        check("N14 prime 之前的历史连接不补报", hits == 0);

        auto append = [&tmp](const QByteArray& s) {
            QFile f(tmp);
            if (f.open(QIODevice::Append))
                f.write(s);
        };

        append(realClient);
        w.pollNow();
        check("N15 真实客户端行不算（自连时两条同时在）", hits == 0);

        append(realServer);
        w.pollNow();
        check("N16 真实服务端行报一次", hits == 1);

        w.pollNow();
        check("N17 同一行不重复报", hits == 1);

        // 半行：RustDesk 正写到一半，标记还没落全，不能报
        append("INFO [src\\server\\connection.rs:2117] peer info supported_enc");
        w.pollNow();
        check("N18 写到一半的半行不报", hits == 1);

        append("oding: SupportedEncoding\n");
        w.pollNow();
        check("N19 半行补齐后才报", hits == 2);

        // 日志轮转：CURRENT 换成新文件，体积变小 → 视为重置，不补报
        {
            QFile f(tmp);
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            f.write(realServer);
        }
        w.pollNow();
        check("N20 轮转后不把旧内容当新连接报", hits == 2);

        append(realServer);
        w.pollNow();
        check("N21 轮转后新写入的仍能报", hits == 3);

        w.setEnabled(false);
        QFile::remove(tmp);
    }

    check("N22 默认日志路径非空", !ConnectionWatcher::currentLogPath().isEmpty());

    // ---- D. 要盯两个日志 ----
    // 外壳用 `rustdesk.exe --server` 拉起被控端，它的日志在 log\server\ 下；
    // 用户自己双击开的界面实例写在 log\ 下。只盯一个就会漏掉另一边的连接。
    {
        const QStringList def = ConnectionWatcher::logPaths();
        check("N25 默认盯两个日志", def.size() == 2);
        check("N26 第一个是 --server 那份（外壳拉起的常态）",
              def.size() == 2
                  && def.at(0).endsWith(QStringLiteral("server/rustdesk_rCURRENT.log"),
                                       Qt::CaseInsensitive));

        // 两个临时文件，标记只写进第二个 —— 证明真的在轮询每一个，不是只看第一个
        const QString a = QDir::temp().filePath(QStringLiteral("puremote-conn-a.log"));
        const QString b = QDir::temp().filePath(QStringLiteral("puremote-conn-b.log"));
        for (const QString& p : { a, b }) {
            QFile::remove(p);
            QFile f(p);
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            f.write("INFO [src\\rendezvous_mediator.rs:1] noise\n");
        }

        ConnectionWatcher w;
        int               hits = 0;
        QObject::connect(&w, &ConnectionWatcher::remoteConnected, [&hits] { ++hits; });
        w.setLogPaths({ a, b });
        w.setEnabled(true);
        w.pollNow();                                   // prime
        check("N27 prime 后两个文件都不报", hits == 0);

        auto appendTo = [](const QString& p, const QByteArray& s) {
            QFile f(p);
            if (f.open(QIODevice::Append))
                f.write(s);
        };
        appendTo(b, "[2026-10-08 16:11:15.755192 +08:00] INFO "
                    "[src\\server\\connection.rs:2117] peer info supported_encoding: X\n");
        w.pollNow();
        check("N28 第二个文件里的标记也能报", hits == 1);

        w.setEnabled(false);
        QFile::remove(a);
        QFile::remove(b);
    }

    // ---- E. 会话结束 —— 「用完即换」唯一的触发点 ----
    //
    // 下面每一行都是**真日志的逐字节副本**（payload 截断、去掉带对端 IP 的那几行）：
    //   %APPDATA%\RustDesk\log\server\rustdesk_r2026-10-09_18-33-48.log
    // 那是 2026-10-09 一次真实会话的完整生命周期（18:30:15 连上 → 18:30:21 对方主动断开）。
    // 自己手写短行测不出"时间戳前缀会不会把匹配挡住"——这个项目已经因此误判过两次，
    // 所以这里必须用真字节。
    {
        const QByteArray sessStart =
            "[2026-10-09 18:30:15.338592 +08:00] INFO [src\\server\\connection.rs:2117] "
            "peer info supported_encoding: SupportedEncoding { h264: true, h265: true, vp8: true }\n";
        const QByteArray sessClose =
            "[2026-10-09 18:30:21.356189 +08:00] INFO [src\\server\\connection.rs:2847] "
            "receive close reason: \n"
            "[2026-10-09 18:30:21.356229 +08:00] INFO [src\\server\\connection.rs:5249] "
            "#526 Connection closed: Peer close\n"
            "[2026-10-09 18:30:21.356280 +08:00] INFO [src\\server\\connection.rs:1237] "
            "#526 connection loop exited\n";
        // RustDesk **自己关停**（不是对端走人）。来自另一份真日志的原文。
        const QByteArray mgrClose =
            "[2026-10-08 16:11:37.334668 +08:00] INFO [src\\server\\connection.rs:5249] "
            "#268 Connection closed: connection manager\n";
        // 客户端侧 src\client.rs 也会打 "Connection closed"（连不上目标时那条）。
        const QByteArray clientClose =
            "[2026-10-09 12:25:40.762998 +08:00] ERROR [src\\client.rs:4950] "
            "Connection closed: WebRTC failed: Failed to connect\n";

        const QString tmp = QDir::temp().filePath(QStringLiteral("puremote-conntest2.log"));
        QFile::remove(tmp);
        {
            QFile f(tmp);
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            f.write("[2026-10-09 18:30:00.000000 +08:00] INFO [src\\common.rs:706] Testing nat ...\n");
        }

        ConnectionWatcher w;
        int               conn = 0, disc = 0;
        QObject::connect(&w, &ConnectionWatcher::remoteConnected, [&conn] { ++conn; });
        QObject::connect(&w, &ConnectionWatcher::remoteDisconnected, [&disc] { ++disc; });
        w.setLogPaths({ tmp });
        w.setEnabled(true);
        w.pollNow();
        check("N30 prime 之后什么都不报", conn == 0 && disc == 0);

        auto put = [&tmp](const QByteArray& s) {
            QFile f(tmp);
            if (f.open(QIODevice::Append))
                f.write(s);
        };

        put(sessStart);
        w.pollNow();
        check("N31 真会话建立 → 报一次「有人连进来」", conn == 1 && disc == 0);

        put(clientClose);
        w.pollNow();
        check("N32 客户端的 Connection closed 不算（自连时两条同时在）", disc == 0);

        put(mgrClose);
        w.pollNow();
        check("N33 RustDesk 自己关停（connection manager）不算「人走了」", disc == 0);

        put(sessClose);
        w.pollNow();
        check("N34 真会话结束 → 报一次「人走了」", conn == 1 && disc == 1);

        w.pollNow();
        check("N35 同一行不重复报", disc == 1);

        // 日志换成**另一份文件**，而且新文件比旧游标大。
        // 这正是以前会失手的一种：旧代码只按 `size < pos` 判轮转，`size > pos` 时就
        // 从**旧游标**那个偏移开读 —— 新文件开头那几行整段被跳过。加了「用完即换」
        // 之后"外壳自己重启 RustDesk"是常规操作，这条路径必须钉住。
        {
            QFile f(tmp);
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            f.write("[2026-10-09 19:00:00.000001 +08:00] INFO [src\\core_main.rs:398] "
                    "start --server with user tester\n");
            f.write(sessClose);                    // ← 标记就在文件开头
            for (int i = 0; i < 25; ++i)
                f.write("[2026-10-09 19:00:00.000002 +08:00] INFO [src\\common.rs:706] "
                        "Testing nat ...\n");
        }
        w.pollNow();
        check("N36 新文件比旧游标大时，开头那几行也不能被跳过（旧代码从错位置开读）",
              conn == 1 && disc == 2);
        w.pollNow();
        check("N37 新文件不重复报", conn == 1 && disc == 2);

        w.setEnabled(false);
        QFile::remove(tmp);
    }

    // ---- 恢复 ----
    nt.setMode(modeOrig == QStringLiteral("toast")     ? NotifyPolicy::Mode::Toast
               : modeOrig == QStringLiteral("confirm") ? NotifyPolicy::Mode::Confirm
                                                       : NotifyPolicy::Mode::Off);
    if (!optBackup.isEmpty() && !rd.isRunning()) {
        QFile f(optPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(optBackup);
    }
    if (cfgBackup.isEmpty()) {
        QFile::remove(cfgPath);
    } else {
        QFile f(cfgPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(cfgBackup);
    }
    check("N23 puremote.json 已恢复", readWholeFile(cfgPath).toUtf8() == cfgBackup);
    if (!optBackup.isEmpty() && !rd.isRunning())
        check("N24 RustDesk2.toml 已恢复", readWholeFile(optPath).toUtf8() == optBackup);

    report << QStringLiteral("──── %1（%2 项失败）────")
                  .arg(fails == 0 ? QStringLiteral("全绿") : QStringLiteral("有失败"))
                  .arg(fails);

    const QString text = report.join(QLatin1Char('\n')) + QLatin1Char('\n');
    QFile         out(QDir::temp().filePath(QStringLiteral("puremote-conntest.txt")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(text.toUtf8());
        out.close();
    }
    std::printf("%s", text.toUtf8().constData());
    std::fflush(stdout);
    return fails == 0 ? 0 : 1;
}

// ── 无头验收：被控端"在不在"的判据（2026-10-10 事故后新增）────────────────────
//
// 事故：看门狗原来只问 `isRunning()`（系统里有没有叫 rustdesk.exe 的进程），而
// RustDesk **自己的界面进程也叫这个名字**。用户点一次「打开 RustDesk 看一次性密码」
// 之后界面进程就起来了 —— 服务端要是这时候死了，看门狗判成"一切正常"，机器**静默
// 变成谁都连不进来**（在那台机器上连它自己都报「目标设备离线或不存在」）。
// 界面进程**不提供服务**，它只是 IPC 客户端：现场它的日志每秒钟刷一行
// `ERROR [src\tray.rs:277] ipc connection closed: reset by the peer`（见
// %APPDATA%\RustDesk\log\tray\），那就是"我连不上服务端"的抱怨。
//
// 所以判据必须是**命令行里那个独立的 `--server`**。这里喂的全是**真机抓下来的原文**
// （`wmic process where "name='rustdesk.exe'" get commandline /format:list` 的输出，
// 行尾是 wmic 特有的 \r\r\n），不是自己编的字符串 —— 本项目在这上面栽过两次。
//
// 只测解析（不起进程）：真起 / 杀进程那一段的端到端验证在 scripts/verify.sh 第 8 步。
static int runServerTest()
{
    attachParentConsole();

    int         fails = 0;
    QStringList report;
    auto        check = [&fails, &report](const char* name, bool ok) {
        report << QStringLiteral("  [%1] %2").arg(ok ? QStringLiteral("PASS")
                                                     : QStringLiteral("FAIL"), QString::fromUtf8(name));
        if (!ok)
            ++fails;
    };

    report << QStringLiteral("──── --servertest：被控端在不在（--server 而不是进程名）────");

    // 真机 wmic 原文（2026-10-10 抓的）。**路径换成了示例路径**，其余逐字节照抄 ——
    // 两个空格的间隔、`\r\r\n` 行尾都是真机原样，S9 钉的就是这个。
    const QString srvLine =
        QStringLiteral("CommandLine=D:\\Tools\\PureRemote\\bin\\rustdesk.exe"
                       "  --server\r\r\n");
    const QString trayLine =
        QStringLiteral("CommandLine=\"C:\\Program Files\\PureRemote\\bin\\rustdesk.exe\" --tray \r\r\n");
    // wmic 在没有匹配进程时打的一行；也是正常退出
    const QString noneLine = QStringLiteral("No Instance(s) Available.\r\r\n");

    check("S1 只有 --server → 在",
          RustDeskController::serverInProcessList(srvLine));
    check("S2 只有 --tray（**事故现场**）→ 不在",
          !RustDeskController::serverInProcessList(trayLine));
    check("S3 两个都在（顺序无关）→ 在",
          RustDeskController::serverInProcessList(srvLine + trayLine)
              && RustDeskController::serverInProcessList(trayLine + srvLine));
    check("S4 --tray 后面还跟了别的参数 → 仍然不在",
          !RustDeskController::serverInProcessList(
              QStringLiteral("CommandLine=\"C:\\a\\rustdesk.exe\" --tray --silent\r\r\n")));
    check("S5 发起连接的客户端（--connect / --password）→ 不在",
          !RustDeskController::serverInProcessList(
              QStringLiteral("CommandLine=C:\\a\\rustdesk.exe --connect 123456789"
                             " --password Pw123\r\r\n")));
    check("S6 没有匹配进程（No Instance(s) Available.）→ 不在",
          !RustDeskController::serverInProcessList(noneLine));
    check("S7 空输出 → 不在", !RustDeskController::serverInProcessList(QString()));
    check("S8 路径里恰好含 --server 但不是独立 token → 不在（不许被路径骗到）",
          !RustDeskController::serverInProcessList(
              QStringLiteral("CommandLine=\"C:\\apps\\rustdesk--server\\bin\\rustdesk.exe\""
                             " --tray\r\r\n")));
    check("S9 真的 wmic 输出是 \\r\\r\\n 行尾，切 token 时不能被它带偏",
          RustDeskController::serverInProcessList(QStringLiteral("\r\r\n\r\r\n") + srvLine
                                                  + QStringLiteral("\r\r\n\r\r\n\r\r\n")));

    // 〔现场〕不是断言：这台机器此刻是什么状态只有机器自己知道，跟代码对不对无关。
    //   它是给人看的 —— 万一以后又出"连不上"，一眼就能看到判据在这台机器上的取值。
    const RustDeskController rd;
    report << QStringLiteral("  〔现场〕此刻 isRunning()=%1（只问「有没有叫这个名字的进程」）"
                             " · isServerRunning()=%2")
                  .arg(rd.isRunning() ? QStringLiteral("true") : QStringLiteral("false"),
                       rd.isServerRunning() ? QStringLiteral("true") : QStringLiteral("false"));

    report << QStringLiteral("──── %1（%2 项失败）────")
                  .arg(fails == 0 ? QStringLiteral("全绿") : QStringLiteral("有失败"))
                  .arg(fails);

    const QString text = report.join(QLatin1Char('\n')) + QLatin1Char('\n');
    QFile         out(QDir::temp().filePath(QStringLiteral("puremote-servertest.txt")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(text.toUtf8());
        out.close();
    }
    std::printf("%s", text.toUtf8().constData());
    std::fflush(stdout);
    return fails == 0 ? 0 : 1;
}

// ── 无头验收：tsnet 桥接（M4.4）────────────────────────────────────────────────
// 分两段，理由是两段的"能证明什么"完全不同：
//   A 段 纯解析 —— 不起进程、不碰网络，喂协议行给解析器逐项断言。永远跑。
//   B 段 真桥接 —— 真起两个 pmrelay 进程（一 expose 一 connect）把一条 TCP 打通。
//        要联网、要往 tailnet 注册两个测试节点，所以没配 authkey 就 SKIP。
//        只有这一段能证明"pmrelay + 外壳封装"整体成立，A 段证明不了。
static int runTsnetTest()
{
    attachParentConsole();

    QStringList report;
    int         fails = 0;
    auto        check = [&fails, &report](const char* name, bool ok) {
        report << QStringLiteral("  [%1] %2").arg(ok ? QStringLiteral("PASS")
                                                      : QStringLiteral("FAIL"),
                                                   QString::fromUtf8(name));
        if (!ok)
            ++fails;
    };

    report << QStringLiteral("──── --tsnettest：tsnet 桥接 ────");

    AppConfig cfg;
    cfg.load();

    // ---- A 段：协议解析 ----
    {
        TsnetBridge b;
        int         readyCount = 0;
        QString     readyMode, readyDetail, lastRelay, lastUrl, lastWhat, lastDetail;
        int         peerCount = 0, urlCount = 0, errCount = 0;
        bool        lastOnline = false;
        QStringList conns;

        QObject::connect(&b, &TsnetBridge::ready,
                         [&](const QString& m, const QString& d) { ++readyCount; readyMode = m; readyDetail = d; });
        QObject::connect(&b, &TsnetBridge::peerStateChanged,
                         [&](bool on, const QString& r, const QString&) { ++peerCount; lastOnline = on; lastRelay = r; });
        QObject::connect(&b, &TsnetBridge::loginUrlAvailable,
                         [&](const QString& u) { ++urlCount; lastUrl = u; });
        QObject::connect(&b, &TsnetBridge::bridgeError,
                         [&](const QString& w, const QString& d) { ++errCount; lastWhat = w; lastDetail = d; });
        QObject::connect(&b, &TsnetBridge::connectionSeen,
                         [&](const QString& f) { conns << f; });

        b.feedLineForTest(QStringLiteral(
            "PMRELAY READY mode=expose ip=100.101.102.103 hostname=pa listen=21118 target=127.0.0.1:21118"));
        check("T1  READY(expose) 触发信号", readyCount == 1);
        check("T2  expose 的 tailnet IP 解析正确", b.tailnetIp() == QStringLiteral("100.101.102.103"));
        check("T3  expose 的说明里带上了转发关系",
              readyDetail.contains(QStringLiteral("100.101.102.103"))
                  && readyDetail.contains(QStringLiteral("21118")));

        b.feedLineForTest(
            QStringLiteral("PMRELAY PEER name=pa online=true relay=hkg direct=10.0.0.1:1234"));
        check("T4  首次 PEER 触发信号且 online=true", peerCount == 1 && lastOnline);
        check("T5  中继区域与直连地址都解析出来",
              b.peerRelay() == QStringLiteral("hkg") && b.peerDirect() == QStringLiteral("10.0.0.1:1234"));

        // 重复同一状态不能再发一次 —— 否则界面每 2 秒被刷一遍
        b.feedLineForTest(
            QStringLiteral("PMRELAY PEER name=pa online=true relay=hkg direct=10.0.0.1:1234"));
        check("T6  对端状态没变化时不重复发信号", peerCount == 1);

        b.feedLineForTest(QStringLiteral("PMRELAY PEER name=pa online=false relay= direct="));
        check("T7  对端掉线会发信号", peerCount == 2 && !lastOnline);

        b.feedLineForTest(
            QStringLiteral("PMRELAY LOGIN-URL https://login.tailscale.com/a/1c501e260179b7"));
        check("T8  登录 URL 解析正确（不带 key= 的裸 token）",
              urlCount == 1 && b.loginUrl() == QStringLiteral("https://login.tailscale.com/a/1c501e260179b7"));

        b.feedLineForTest(QStringLiteral(
            "PMRELAY ERROR what=peer-dial-failed detail=dial_tcp_100.101.102.103:21118:_no_response"));
        check("T9  ERROR 的 what 与 detail 分开解析",
              errCount == 1 && lastWhat == QStringLiteral("peer-dial-failed")
                  && lastDetail.startsWith(QStringLiteral("dial_tcp")));

        b.feedLineForTest(QStringLiteral("PMRELAY CONN mode=expose from=100.101.102.104:60834"));
        check("T10 CONN 触发连接提示",
              conns.size() == 1 && conns.first() == QStringLiteral("100.101.102.104:60834"));

        const int rBefore = readyCount;
        b.feedLineForTest(QStringLiteral("这不是协议行，应当被忽略"));
        b.feedLineForTest(QString());
        b.feedLineForTest(QStringLiteral("PMRELAY"));
        check("T11 非协议行 / 空行 / 残缺行一律忽略", readyCount == rBefore && errCount == 1);

        b.feedLineForTest(
            QStringLiteral("PMRELAY READY mode=connect local=127.0.0.1:21119 peer=pa:21118"));
        check("T12 READY(connect) 说明里带上本机监听与对端",
              readyDetail.contains(QStringLiteral("127.0.0.1:21119"))
                  && readyDetail.contains(QStringLiteral("pa:21118")));
    }

    // ---- B 段：真桥接 ----
    // authkey 优先取环境变量（CI / 临时跑），其次取配置。
    QString authkey = qEnvironmentVariable("TS_AUTHKEY");
    if (authkey.isEmpty())
        authkey = cfg.tsnetAuthKey();

    if (authkey.isEmpty()) {
        report << QStringLiteral("  SKIP B 段：没配 authkey（puremote.json 的 tsnet_authkey "
                                 "或环境变量 TS_AUTHKEY）—— A 段已证明解析，但没证明真能通");
    } else if (TsnetBridge::findRelayExe().isEmpty()) {
        report << QStringLiteral("  SKIP B 段：找不到 bin/pmrelay.exe");
    } else {
        // 泵事件循环的等待助手：既要等信号，又不能阻塞 Qt 的信号派发
        auto waitUntil = [](const std::function<bool()>& pred, int ms) {
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < ms) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                if (pred())
                    return true;
            }
            return pred();
        };

        // 强制走中继。
        //
        // 为什么：B 段两个 tsnet 节点跑在**同一台机器**上，magicsock 会挑中一条"直连"——
        // 实测那条是 10.8.114.62（本机自己的 LAN 地址）+ 本机自己的 STUN 端口，是个假路径：
        // 握手包进去就没了，两端日志里只剩成对的 `Accept: TCP{...} 52 tcp non-syn` 反复出现，
        // 应用层 20 秒也等不到数据。跨机真 P2P 单机根本造不出来，所以这里不去证明它。
        // 走中继才是能确定的路径，而且正是 M4.0 闸门实测认证过的那条（hkg，中位 54ms）。
        // 跨机 P2P 仍然待验 —— 这一点在报告里也不含糊。
        qputenv("TS_DEBUG_ALWAYS_USE_DERP", "1");

        // 本机回显服务，充当"被转发的真实服务"（真机上这里是 RustDesk 的 21118）
        QTcpServer echo;
        const bool echoUp = echo.listen(QHostAddress::LocalHost, 0);
        const int  echoPort = echo.serverPort();
        QObject::connect(&echo, &QTcpServer::newConnection, &echo, [&echo] {
            while (QTcpSocket* c = echo.nextPendingConnection())
                QObject::connect(c, &QTcpSocket::readyRead, c, [c] { c->write(c->readAll()); });
        });
        check("B1  本机回显服务起得来", echoUp && echoPort > 0);

        const QString base = QCoreApplication::applicationDirPath() + QStringLiteral("/data/tsnet-test");
        TsnetBridge   exp, con;
        exp.setAuthKey(authkey);
        exp.setHostname(QStringLiteral("puremote-test-a"));
        exp.setStateDir(base + QStringLiteral("/a"));
        exp.setTargetPort(echoPort);          // tailnet :21118 → 本机回显
        con.setAuthKey(authkey);
        con.setHostname(QStringLiteral("puremote-test-b"));
        con.setStateDir(base + QStringLiteral("/b"));
        con.setPeer(QStringLiteral("puremote-test-a"));
        con.setLocalPort(21129);              // 避开 21118/21119，免得和真 RustDesk 撞

        bool expReady = false, conReady = false, conSawPeer = false;
        QString expErr, conErr;
        QStringList expLog, conLog;
        // 失败时要有东西可看：桥接自己的人类日志（stderr）留一份，别只留个 PASS/FAIL。
        QObject::connect(&exp, &TsnetBridge::logLine, [&](const QString& l) {
            expLog << l;
            if (expLog.size() > 400)
                expLog.removeFirst();
        });
        QObject::connect(&con, &TsnetBridge::logLine, [&](const QString& l) {
            conLog << l;
            if (conLog.size() > 400)
                conLog.removeFirst();
        });
        exp.setVerbose(true);
        con.setVerbose(true);
        QObject::connect(&exp, &TsnetBridge::ready, [&](const QString&, const QString&) { expReady = true; });
        QObject::connect(&con, &TsnetBridge::ready, [&](const QString&, const QString&) { conReady = true; });
        QObject::connect(&con, &TsnetBridge::peerStateChanged,
                         [&](bool on, const QString&, const QString&) { if (on) conSawPeer = true; });
        QObject::connect(&con, &TsnetBridge::bridgeError,
                         [&](const QString& w, const QString& d) { conErr = w + QLatin1Char(':') + d; });
        QObject::connect(&exp, &TsnetBridge::bridgeError,
                         [&](const QString& w, const QString& d) { expErr = w + QLatin1Char(':') + d; });

        check("B2  拉起被控端桥接（expose）", exp.start(TsnetBridge::Mode::Expose));
        const bool expOk = waitUntil([&] { return expReady || !expErr.isEmpty(); }, 120000);
        check("B3  被控端登录 tailnet 并监听成功", expReady);
        if (!expErr.isEmpty())
            report << QStringLiteral("        被控端错误: %1").arg(expErr);

        if (expReady) {
            check("B4  拉起控制端桥接（connect）", con.start(TsnetBridge::Mode::Connect));
            waitUntil([&] { return conReady || !conErr.isEmpty(); }, 120000);
            check("B5  控制端登录 tailnet 成功", conReady);
            if (!conErr.isEmpty())
                report << QStringLiteral("        控制端错误: %1").arg(conErr);

            // 等对端上线再连 —— 这就是 M4.0 闸门学到的：不等就白等系统级 connect 超时
            const bool sawPeer = waitUntil([&] { return conSawPeer; }, 60000);
            check("B6  控制端看到对端上线", sawPeer);

            bool           passed = false;
            QString        why;
            const QByteArray msg("PING-through-tailnet");
            for (int i = 0; i < 3 && !passed; ++i) {
                QTcpSocket s;
                s.connectToHost(QStringLiteral("127.0.0.1"), con.localPort());

                // 全程泵事件循环，**不能用 waitForReadyRead/waitForConnected 把循环堵住**：
                // 桥接那两个 QProcess 的管道要在同一个循环里排空。堵住它 → pmrelay 的 stderr
                // 管道写满 → 它自己的 log 调用卡住（tsnet 的日志是从网络栈里同步调出来的）
                // → 整条转发停摆。实测：堵 20 秒，一字节都回不来。
                QByteArray    got;
                QString       stage;
                bool          sent = false;
                QElapsedTimer t;
                t.start();
                while (t.elapsed() < 25000) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                    if (s.state() == QAbstractSocket::UnconnectedState && t.elapsed() > 5000) {
                        stage = QStringLiteral("连不上本机 127.0.0.1:%1：%2")
                                    .arg(con.localPort())
                                    .arg(s.errorString());
                        break;
                    }
                    if (!sent && s.state() == QAbstractSocket::ConnectedState) {
                        s.write(msg);
                        s.flush();
                        sent = true;
                    }
                    if (s.bytesAvailable() > 0) {
                        got += s.readAll();
                        if (got.size() >= msg.size())
                            break;
                    }
                }

                if (got == msg) {
                    passed = true;
                } else if (got.isEmpty()) {
                    why = QStringLiteral("第%1次：%2")
                              .arg(i + 1)
                              .arg(stage.isEmpty() ? QStringLiteral("25 秒内没收到任何数据") : stage);
                } else {
                    why = QStringLiteral("第%1次：回了但不是原样：%2")
                              .arg(i + 1)
                              .arg(QString::fromUtf8(got));
                }
                s.abort();
                if (!passed)
                    waitUntil([] { return false; }, 2000);
            }
            check("B7  经 tailnet 转发后原样回显（整条链路）", passed);
            if (!passed || !conErr.isEmpty() || !expErr.isEmpty()) {
                report << QStringLiteral("        控制端: online=%1 relay=%2 direct=%3 local=%4")
                              .arg(con.peerOnline())
                              .arg(con.peerRelay())
                              .arg(con.peerDirect())
                              .arg(con.localPort());
                report << QStringLiteral("        控制端错误: %1").arg(conErr.isEmpty() ? QStringLiteral("(无)") : conErr);
                report << QStringLiteral("        被控端错误: %1").arg(expErr.isEmpty() ? QStringLiteral("(无)") : expErr);
                report << QStringLiteral("        最后一次失败: %1").arg(why);
                report << QStringLiteral("        控制端桥接日志（尾 12 行）:");
                for (const QString& l : conLog.mid(qMax(0, conLog.size() - 12)))
                    report << QStringLiteral("          | %1").arg(l);
                report << QStringLiteral("        被控端桥接日志（尾 12 行）:");
                for (const QString& l : expLog.mid(qMax(0, expLog.size() - 12)))
                    report << QStringLiteral("          | %1").arg(l);
            }
        }

        exp.stop();
        con.stop();
        qunsetenv("TS_DEBUG_ALWAYS_USE_DERP");
        check("B8  两个桥接都停干净", !exp.isRunning() && !con.isRunning());

        report << QStringLiteral("        说明：B 段固定走中继（hkg）。单机上两个 tsnet 节点会挑到一条"
                                 "走不通的假直连（本机自己的 LAN 地址），跨机真 P2P 仍需两台机器验证。");
        Q_UNUSED(expOk);
    }

    report << QStringLiteral("──── %1（%2 项失败）────")
                  .arg(fails == 0 ? QStringLiteral("全绿") : QStringLiteral("有失败"))
                  .arg(fails);

    const QString text = report.join(QLatin1Char('\n')) + QLatin1Char('\n');
    QFile         out(QDir::temp().filePath(QStringLiteral("puremote-tsnettest.txt")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(text.toUtf8());
        out.close();
    }
    std::printf("%s", text.toUtf8().constData());
    std::fflush(stdout);
    return fails == 0 ? 0 : 1;
}

// ── 无头验收：地址簿 / 连接命令 / 时序状态机 / 准备情况 ──────────────────────
//
// 这一段**会改** config/puremote.json（AddressBook 的增删改是立刻落盘的），
// 所以开头备份字节、结尾原样还回去 —— 照 --poltest 那套。
static int runBookTest()
{
    attachParentConsole();

    QStringList report;
    int         fails = 0;
    auto        check = [&fails, &report](const QString& name, bool ok) {
        report << QStringLiteral("  [%1] %2").arg(ok ? QStringLiteral("PASS")
                                                     : QStringLiteral("FAIL"), name);
        if (!ok)
            ++fails;
    };

    report << QStringLiteral("──── --booktest：地址簿 / 连接命令 / 时序状态机 / 准备情况 ────");

    // 备份外壳配置。AddressBook 的每个改动都会整份重写这个文件，不还原就会把
    // 用户真实的联系人列表冲掉。
    const QString cfgPath = AppConfig::configFilePath();
    QByteArray    cfgBackup;
    bool          cfgExisted = false;
    {
        QFile f(cfgPath);
        if (f.open(QIODevice::ReadOnly)) {
            cfgBackup  = f.readAll();
            cfgExisted = true;
        }
    }
    // E 段的 authkey 从**没被改过的备份**里取 —— 后面 A/C 段会反复重写这个文件
    QString authkeyFromFile;
    if (cfgExisted) {
        authkeyFromFile = QJsonDocument::fromJson(cfgBackup)
                              .object()
                              .value(QStringLiteral("tsnet_authkey"))
                              .toString();
    }

    // ══ A 段 地址簿 CRUD / 持久化 / 容错 ══════════════════════════════════
    report << QStringLiteral("A 段 地址簿（真读写 puremote.json，跑完原样还回去）");
    {
        AppConfig cfg;
        cfg.load();
        cfg.setNetMode(QStringLiteral("id"));
        cfg.setContacts(QJsonArray());
        cfg.save();

        AddressBook book(&cfg);
        book.load();
        check(QStringLiteral("A1  空配置里读出 0 条"), book.count() == 0);

        Contact c1;
        c1.name         = QStringLiteral("办公室台式机");
        c1.rustdeskId   = QStringLiteral("123456789");
        c1.tsnetHost    = QStringLiteral("office-pc");
        c1.peerPassword = QStringLiteral("pw-1");
        check(QStringLiteral("A2  add 返回新索引 0"), book.add(c1) == 0);
        check(QStringLiteral("A3  add 后 count == 1"), book.count() == 1);

        Contact c2;
        c2.name      = QStringLiteral("笔记本");
        c2.tsnetHost = QStringLiteral("laptop");          // 只有 tailnet 地址，也该收
        check(QStringLiteral("A4  只有 tailnet 地址的条目也收"), book.add(c2) == 1);
        check(QStringLiteral("A5  两个地址都空的条目被拒"), book.add(Contact()) == -1);

        Contact c1b     = book.at(0);
        c1b.peerPassword = QStringLiteral("pw-2");
        check(QStringLiteral("A6  update 生效返回 true"), book.update(0, c1b));
        check(QStringLiteral("A7  update 越界返回 false"), !book.update(9, c1b));
        check(QStringLiteral("A8  改完名字空着会回落“未命名”"),
              book.update(1, [] { Contact c; c.rustdeskId = QStringLiteral("5"); return c; }())
                  && book.at(1).name == QStringLiteral("未命名"));

        // 新实例从盘重读 —— 证明的是真落了盘，不是只活在内存里
        AddressBook book2(&cfg);
        book2.load();
        check(QStringLiteral("A9  新实例重读得到 2 条"), book2.count() == 2);
        check(QStringLiteral("A10 改过的密码在盘上"), book2.at(0).peerPassword == QStringLiteral("pw-2"));
        check(QStringLiteral("A11 tailnet 主机名往返一致"), book2.at(0).tsnetHost == QStringLiteral("office-pc"));
        check(QStringLiteral("A12 官方 ID 也往返一致"), book2.at(0).rustdeskId == QStringLiteral("123456789"));

        const QDateTime when = QDateTime::currentDateTime();
        book2.touchConnectedAt(1, when);
        AddressBook book3(&cfg);
        book3.load();
        check(QStringLiteral("A13 lastConnectedAt 落了盘"), book3.at(1).lastConnectedAt.isValid());
        check(QStringLiteral("A14 lastConnectedAt 值对得上（秒级）"),
              book3.at(1).lastConnectedAt.secsTo(when) == 0);

        check(QStringLiteral("A15 removeAt 生效"), book3.removeAt(0));
        check(QStringLiteral("A15 删完只剩 1 条"), book3.count() == 1);
        check(QStringLiteral("A15 removeAt 越界返回 false"), !book3.removeAt(5));

        // ---- 容错解析：数组里混进各种坏东西 ----
        QJsonArray dirty;
        dirty.append(QStringLiteral("我是个字符串"));                       // 非对象
        dirty.append(42);                                                  // 数字
        dirty.append(QJsonValue::Null);                                    // null
        dirty.append(QJsonObject { { QStringLiteral("name"), QStringLiteral("没地址") } });
        dirty.append(QJsonObject {                                          // 类型错 + 只有一个地址
            { QStringLiteral("name"), QJsonValue(123) },
            { QStringLiteral("rustdesk_id"), QJsonValue(456) },
            { QStringLiteral("tsnet_host"), QStringLiteral("bad-type-host") } });
        dirty.append(QJsonObject {                                          // 时间坏掉
            { QStringLiteral("rustdesk_id"), QStringLiteral("999") },
            { QStringLiteral("last_connected_at"), QStringLiteral("这不是时间") } });
        dirty.append(QJsonObject {                                          // 完全正常
            { QStringLiteral("name"), QStringLiteral("好的") },
            { QStringLiteral("rustdesk_id"), QStringLiteral("111") } });

        cfg.setContacts(dirty);
        AddressBook dirtyBook(&cfg);
        dirtyBook.load();
        check(QStringLiteral("A15 坏条目跳掉、好条目留下（7 条里活 3 条）"), dirtyBook.count() == 3);
        check(QStringLiteral("A15 地址类型错（数字 456）→ 当空串，不猜成 \"456\" —— 否则这条会被丢掉"),
              dirtyBook.at(0).rustdeskId.isEmpty());
        check(QStringLiteral("A15 ……但它另一个合法地址照收，条目得以存活"),
              dirtyBook.at(0).tsnetHost == QStringLiteral("bad-type-host"));
        check(QStringLiteral("A15 名字类型错 → 回落“未命名”"), dirtyBook.at(0).name == QStringLiteral("未命名"));
        check(QStringLiteral("A15 时间解析失败 → 条目**保留**，只是没有“上次连接”"),
              dirtyBook.at(1).rustdeskId == QStringLiteral("999")
                  && !dirtyBook.at(1).lastConnectedAt.isValid());
        check(QStringLiteral("A15 同上，坏时间不波及同条目的名字"), dirtyBook.at(1).name == QStringLiteral("未命名"));
        check(QStringLiteral("A15 正常条目原样读出"),
              dirtyBook.at(2).name == QStringLiteral("好的")
                  && dirtyBook.at(2).rustdeskId == QStringLiteral("111"));

        // ---- 键名精确断言（防 snake_case 漂移：改了键名，老用户的地址簿会静默清空）----
        {
            AppConfig kc;
            kc.load();
            kc.setContacts(QJsonArray());
            kc.save();
            AddressBook kb(&kc);
            kb.load();
            Contact k;
            k.name       = QStringLiteral("键名测试");
            k.rustdeskId = QStringLiteral("777");
            kb.add(k);

            QFile kf(cfgPath);
            QByteArray raw;
            if (kf.open(QIODevice::ReadOnly))
                raw = kf.readAll();
            const QJsonObject ko  = QJsonDocument::fromJson(raw).object();
            const QJsonArray  karr = ko.value(QStringLiteral("contacts")).toArray();
            check(QStringLiteral("A15 contacts 是数组且有 1 条"), karr.size() == 1);
            const QJsonObject ke = karr.isEmpty() ? QJsonObject() : karr.first().toObject();
            check(QStringLiteral("A15 键名精确：name / rustdesk_id / tsnet_host / peer_password / last_connected_at"),
                  ke.contains(QStringLiteral("name")) && ke.contains(QStringLiteral("rustdesk_id"))
                      && ke.contains(QStringLiteral("tsnet_host")) && ke.contains(QStringLiteral("peer_password"))
                      && ke.contains(QStringLiteral("last_connected_at")));
            check(QStringLiteral("A15 不多写别的键（正好 5 个）"), ke.size() == 5);
            check(QStringLiteral("A15 net_mode 也在同一次保存里写下去了"),
                  ko.value(QStringLiteral("net_mode")).isString());
        }

        // ---- rustdesk_path 不许被持久化 ----
        // 绿色包是可以被搬来搬去的（今天在 C:\tmp、明天解压到 D:\）。这个值是"exe 现在
        // 在哪"的派生量，一旦存进配置又读回来就会**粘死**在旧位置：搬家之后错误提示里
        // 指着不存在的路径，launcher 兜底分支还会去执行那个已删除的文件。
        // 所以规矩是：只认每次启动现探的结果，配置里就算写了也不看、更不再往盘上写。
        {
            const QString bogus = QStringLiteral("C:/nonexistent/old-place/bin/rustdesk.exe");
            QJsonObject  o     = QJsonDocument::fromJson(readWholeFile(cfgPath).toUtf8()).object();
            o.insert(QStringLiteral("rustdesk_path"), bogus);
            writeWholeFile(cfgPath, QJsonDocument(o).toJson());

            AppConfig stale;
            stale.load();
            check(QStringLiteral("A30 配置里塞的过期 rustdesk_path 不被采信"), stale.rustDeskPath() != bogus);
            check(QStringLiteral("A31 不被采信时回落到**现探**的路径（不是空串）"),
                  !stale.rustDeskPath().isEmpty()
                      && stale.rustDeskPath().endsWith(QStringLiteral("bin/rustdesk.exe")));

            stale.save();
            const QJsonObject after = QJsonDocument::fromJson(readWholeFile(cfgPath).toUtf8()).object();
            check(QStringLiteral("A32 save() 不再写回 rustdesk_path"), !after.contains(QStringLiteral("rustdesk_path")));
        }

        // ---- 整份文件坏掉：必须不崩、能安全构造地址簿 ----
        writeWholeFile(cfgPath, QByteArray("{ 这不是 JSON"));
        {
            AppConfig bad;
            bad.load();
            AddressBook badBook(&bad);
            badBook.load();
            check(QStringLiteral("A29 整份文件坏 JSON 不崩，读出 0 条（onAddressBook 不阻断启动）"),
                  badBook.count() == 0);
        }
    }

    // ══ B 段 连接命令构造（纯函数）════════════════════════════════════════
    report << QStringLiteral("B 段 连接命令构造（纯函数，不起进程）");
    {
        const QStringList a1 =
            RustDeskController::connectArgs(QStringLiteral("123456789"), QString());
        check(QStringLiteral("B1  没密码时恰好 2 个参数"), a1.size() == 2);
        check(QStringLiteral("B2  第 1 个是 --connect"), a1.value(0) == QStringLiteral("--connect"));
        check(QStringLiteral("B3  第 2 个是目标原样"), a1.value(1) == QStringLiteral("123456789"));
        check(QStringLiteral("B4  空密码**绝不**产出 --password —— 空串会让 RustDesk 立刻判认证失败"),
              !a1.contains(QStringLiteral("--password")));

        const QStringList a2 =
            RustDeskController::connectArgs(QStringLiteral("123456789"), QStringLiteral("pw-x"));
        check(QStringLiteral("B5  有密码时 4 个参数"), a2.size() == 4);
        check(QStringLiteral("B6  第 3/4 个是 --password <pw>"),
              a2.value(2) == QStringLiteral("--password") && a2.value(3) == QStringLiteral("pw-x"));
        check(QStringLiteral("B7  有/无密码只差那两个参数"), a1.size() + 2 == a2.size());

        const QStringList a3 =
            RustDeskController::connectArgs(QStringLiteral("100.101.102.105:21119"), QString());
        check(QStringLiteral("B8  host:port 形态原样透传（RustDesk 自己认这个写法）"),
              a3.size() == 2 && a3.value(1) == QStringLiteral("100.101.102.105:21119"));
    }

    // ══ C 段 时序状态机（dry-run）═════════════════════════════════════════
    report << QStringLiteral("C 段 时序状态机（dry-run）");
    report << QStringLiteral("      ⚠ dry-run **抑制了真正的 spawn**：这一段证明的是时序与参数构造，"
                             "不证明 RustDesk 真会按这个参数连上。");
    {
        AppConfig cfg;
        cfg.load();
        cfg.setTsnetAuthKey(QStringLiteral("tskey-fake-dry-run-only"));
        cfg.setTsnetHostname(QStringLiteral("self-name"));
        QJsonArray arr;
        arr.append(QJsonObject { { QStringLiteral("name"), QStringLiteral("官方档") },
                                 { QStringLiteral("rustdesk_id"), QStringLiteral("111") } });
        arr.append(QJsonObject { { QStringLiteral("name"), QStringLiteral("tsnet 档") },
                                 { QStringLiteral("rustdesk_id"), QStringLiteral("222") },
                                 { QStringLiteral("tsnet_host"), QStringLiteral("peer-host") } });
        cfg.setContacts(arr);
        cfg.save();

        AddressBook bookC(&cfg);
        bookC.load();
        check(QStringLiteral("C0  联系人读出来 2 条"), bookC.count() == 2);

        // ---- 官方 ID 档：一步到 Running，全程不碰桥接 ----
        TsnetBridge bridge;
        ConnectSession s(&cfg, nullptr, &bridge, &bookC);
        s.setDryRunForTest(true);
        QString fail1;
        QObject::connect(&s, &ConnectSession::failed, [&fail1](const QString& m) { fail1 = m; });

        cfg.setNetMode(QStringLiteral("id"));
        s.connectTo(0);
        check(QStringLiteral("C1  官方 ID 档：一次调用直接到 Running（没有中间态）"),
              s.phase() == ConnectSession::Phase::Running);
        check(QStringLiteral("C2  目标就是那条联系人的 rustdesk_id"),
              s.lastTargetForTest() == QStringLiteral("111"));
        check(QStringLiteral("C3  这条没填密码 → 参数里没有 --password"),
              !s.lastConnectArgsForTest().contains(QStringLiteral("--password")));
        check(QStringLiteral("C4  官方 ID 档**从没碰过桥接**"), !bridge.isRunning());
        s.disconnectNow();
        check(QStringLiteral("C5  断开后回 Idle"), s.phase() == ConnectSession::Phase::Idle);

        // ---- tsnet 档：准备中 → 等对端 → 对端真上线了才发连接 ----
        cfg.setNetMode(QStringLiteral("tsnet"));
        s.connectTo(1);
        check(QStringLiteral("C6  tsnet 档先停在 Preparing"), s.phase() == ConnectSession::Phase::Preparing);
        check(QStringLiteral("C7  Preparing 期间**没有**拉起客户端"), !s.clientSpawnedForTest());
        check(QStringLiteral("C8  dry-run 也没真起桥接进程"), !bridge.isRunning());

        s.feedBridgeReadyForTest();
        check(QStringLiteral("C9  ready 之后进 WaitingPeer"), s.phase() == ConnectSession::Phase::WaitingPeer);
        check(QStringLiteral("C10 WaitingPeer 期间**仍然**不拉客户端"
                             "（这就是 M4.0 闸门那条：对端没上线就去 dial 会白等系统级超时）"),
              !s.clientSpawnedForTest());

        s.feedPeerStateForTest(true, QStringLiteral("hkg"), QString());
        check(QStringLiteral("C11 对端上线后才拉起客户端"), s.clientSpawnedForTest());
        check(QStringLiteral("C12 目标换成桥接的本地地址 127.0.0.1:21119"),
              s.lastTargetForTest() == QStringLiteral("127.0.0.1:21119"));
        check(QStringLiteral("C13 走的是 Running"), s.phase() == ConnectSession::Phase::Running);
        s.disconnectNow();

        // ---- 边沿信号丢失：对端在我们接槽之前就已上线 ----
        // 用**全新的桥接**，免得上一轮的在线状态残留成假阳性
        {
            TsnetBridge    b2;
            ConnectSession s2(&cfg, nullptr, &b2, &bookC);
            s2.setDryRunForTest(true);
            s2.connectTo(1);
            // 先喂 peer（此刻还在 Preparing，onPeerState 会直接返回）——
            // 这一下把 TsnetBridge 的 level 状态置成了 online=true，同时那次边沿信号被丢掉
            b2.feedLineForTest(QStringLiteral("PMRELAY PEER online=true relay=hkg direct=0.0.0.0:1"));
            check(QStringLiteral("C14 提前到的 peer 信号不会把状态机拽到 Running"),
                  s2.phase() == ConnectSession::Phase::Preparing && !s2.clientSpawnedForTest());

            s2.feedBridgeReadyForTest();
            check(QStringLiteral("C15 ready 时靠 level getter 兜回了那个**已经丢掉的边沿信号**，"
                                 "没有干等到超时"),
                  s2.phase() == ConnectSession::Phase::Running);
            check(QStringLiteral("C16 兜回来之后照样拉起客户端，目标还是本地桥接地址"),
                  s2.clientSpawnedForTest()
                      && s2.lastTargetForTest() == QStringLiteral("127.0.0.1:21119"));
            s2.disconnectNow();
        }

        // ---- 桥接报错 ----
        {
            TsnetBridge    b3;
            ConnectSession s3(&cfg, nullptr, &b3, &bookC);
            s3.setDryRunForTest(true);
            QString fail3;
            QObject::connect(&s3, &ConnectSession::failed, [&fail3](const QString& m) { fail3 = m; });
            s3.connectTo(1);
            s3.feedBridgeErrorForTest(QStringLiteral("桥接启动失败"),
                                      QStringLiteral("登录 tailnet 被拒"));
            check(QStringLiteral("C17 桥接报错 → Failed"), s3.phase() == ConnectSession::Phase::Failed);
            check(QStringLiteral("C18 报错路径上**从未**拉起客户端"), !s3.clientSpawnedForTest());
            check(QStringLiteral("C19 失败文案带上了详情（不是光秃秃一句）"),
                  fail3.contains(QStringLiteral("登录 tailnet 被拒")));
        }

        // ---- 等对端时断开：迟到的 peer online 不能把客户端拉起来 ----
        {
            TsnetBridge    b4;
            ConnectSession s4(&cfg, nullptr, &b4, &bookC);
            s4.setDryRunForTest(true);
            s4.connectTo(1);
            s4.feedBridgeReadyForTest();
            check(QStringLiteral("C20 断开前确实在 WaitingPeer"),
                  s4.phase() == ConnectSession::Phase::WaitingPeer);
            s4.disconnectNow();
            s4.feedPeerStateForTest(true, QString(), QString());
            check(QStringLiteral("C21 断开后**迟到**的 peer online 不会再拉起客户端"),
                  !s4.clientSpawnedForTest() && s4.phase() == ConnectSession::Phase::Idle);
        }

        // ---- tsnet 档碰上没填 tailnet 主机名的条目 ----
        {
            Contact only;
            only.name       = QStringLiteral("只有官方 ID");
            only.rustdeskId = QStringLiteral("333");
            const int oi    = bookC.add(only);
            check(QStringLiteral("C22 造出第 3 条（只有官方 ID）"), oi == 2);

            TsnetBridge    b5;
            ConnectSession s5(&cfg, nullptr, &b5, &bookC);
            s5.setDryRunForTest(true);
            QString fail5;
            QObject::connect(&s5, &ConnectSession::failed, [&fail5](const QString& m) { fail5 = m; });
            s5.connectTo(2);
            check(QStringLiteral("C23 tsnet 档遇到没填 tailnet 主机名的条目 → 立刻 Failed（不是干等超时）"),
                  s5.phase() == ConnectSession::Phase::Failed);
            check(QStringLiteral("C24 文案指向“切档或补上主机名”"),
                  fail5.contains(QStringLiteral("tailnet")) && fail5.contains(QStringLiteral("补上")));
        }

        // ---- 没选联系人 ----
        {
            TsnetBridge    b6;
            ConnectSession s6(&cfg, nullptr, &b6, &bookC);
            s6.setDryRunForTest(true);
            QString fail6;
            QObject::connect(&s6, &ConnectSession::failed, [&fail6](const QString& m) { fail6 = m; });
            s6.connectTo(-1);
            check(QStringLiteral("C25 没选联系人 → 可读提示，不崩不静默"),
                  s6.phase() == ConnectSession::Phase::Failed && !fail6.isEmpty());
        }

        // ---- 冷启动 `PureRemote.exe --connect <id>` 走的那条路 ----
        {
            TsnetBridge    b7;
            ConnectSession s7(&cfg, nullptr, &b7, &bookC);
            s7.setDryRunForTest(true);
            // 此刻 cfg 的 netMode 是 tsnet —— 冷启动这条路**必须无视它**
            s7.connectToTarget(QStringLiteral("123456789"));
            check(QStringLiteral("C26 冷启动 --connect <id> **恒走官方 ID 档**，无视当前网络档位"),
                  s7.phase() == ConnectSession::Phase::Running
                      && s7.lastTargetForTest() == QStringLiteral("123456789"));
            check(QStringLiteral("C27 冷启动路径不碰桥接"), !b7.isRunning());
            check(QStringLiteral("C28 冷启动没给密码 → 参数里没有 --password"),
                  !s7.lastConnectArgsForTest().contains(QStringLiteral("--password")));
        }

        // ---- 界面上直接填码点「连接」走的那条（不经过地址簿）----
        //
        // 这是 M9 加的：地址簿降级成"存过的常用设备"，填了码就该能连。
        // 它跟 connectTo 必须**走同一条路**（一样按档位决定走不走桥接、一样拼 --password），
        // 唯一差别是不记「上次连接」—— 所以这里同时钉住"像"和"不像"的两面。
        {
            TsnetBridge    b8;
            ConnectSession s8(&cfg, nullptr, &b8, &bookC);
            s8.setDryRunForTest(true);

            cfg.setNetMode(QStringLiteral("id"));
            Contact ad;
            ad.rustdeskId   = QStringLiteral("123456789");
            ad.peerPassword = QStringLiteral("pw-abc");
            const int before = bookC.count();
            s8.connectToAdhoc(ad);
            check(QStringLiteral("C29 填码直接连：官方档一步到 Running"),
                  s8.phase() == ConnectSession::Phase::Running
                      && s8.lastTargetForTest() == QStringLiteral("123456789"));
            check(QStringLiteral("C30 填码直接连：密码照传（跟 connectTo 同一条路）"),
                  s8.lastConnectArgsForTest().contains(QStringLiteral("pw-abc")));
            check(QStringLiteral("C31 填码直接连**不往地址簿里塞东西**"),
                  bookC.count() == before);
            check(QStringLiteral("C32 填码直接连没有下标 → contactIndex 是 -1（不记「上次连接」）"),
                  s8.contactIndex() == -1);

            s8.disconnectNow();
            cfg.setNetMode(QStringLiteral("tsnet"));
            Contact ad2;
            ad2.tsnetHost = QStringLiteral("peer-host");
            s8.connectToAdhoc(ad2);
            check(QStringLiteral("C33 填码直接连：tsnet 档照样先走桥接（没绕开档位）"),
                  s8.phase() == ConnectSession::Phase::Preparing && !b8.isRunning());
        }
    }

    // ══ D 段 准备情况（纯函数）═══════════════════════════════════════════
    report << QStringLiteral("D 段 准备情况判定（纯函数）");
    {
        SelfCheck::Inputs all;
        all.rustdeskPresent = all.hasId = all.fixedPasswordSet = true;
        all.relayPresent = all.tsnetAuthKeySet = all.tsnetHostnameSet = true;

        const QVector<SelfCheck::Item> items = SelfCheck::evaluate(all);
        check(QStringLiteral("D1  条目数固定为 6（地址簿那条已删，见 SelfCheck.h）"),
              items.size() == 6);
        check(QStringLiteral("D2  全 true 时官方档 missing == 0"), SelfCheck::missingCount(items, false) == 0);
        check(QStringLiteral("D3  全 true 时 tsnet 档 missing == 0"), SelfCheck::missingCount(items, true) == 0);

        auto idxOf = [](const QVector<SelfCheck::Item>& v, const QString& key) {
            for (int i = 0; i < v.size(); ++i)
                if (v.at(i).key == key)
                    return i;
            return -1;
        };

        // 每一项单独缺一次：缺的必须正好是它，且官方档下 tsnetOnly 的那几项不算缺
        struct Case { QString key; std::function<void(SelfCheck::Inputs&)> off; };
        const QVector<Case> cases = {
            { QStringLiteral("rd_present"),
              [](SelfCheck::Inputs& i) { i.rustdeskPresent = false; } },
            { QStringLiteral("has_id"), [](SelfCheck::Inputs& i) { i.hasId = false; } },
            { QStringLiteral("fixed_password"),
              [](SelfCheck::Inputs& i) { i.fixedPasswordSet = false; } },
            { QStringLiteral("relay_present"),
              [](SelfCheck::Inputs& i) { i.relayPresent = false; } },
            { QStringLiteral("tsnet_authkey"),
              [](SelfCheck::Inputs& i) { i.tsnetAuthKeySet = false; } },
            { QStringLiteral("tsnet_hostname"),
              [](SelfCheck::Inputs& i) { i.tsnetHostnameSet = false; } },
        };

        int  n = 0;
        bool allGood = true;
        QStringList bad;
        for (const Case& cs : cases) {
            SelfCheck::Inputs in = all;
            cs.off(in);
            const QVector<SelfCheck::Item> it = SelfCheck::evaluate(in);
            const int                      k  = idxOf(it, cs.key);
            if (k < 0) {
                allGood = false;
                bad << QStringLiteral("%1: 找不到这一项").arg(cs.key);
                continue;
            }
            const bool tsnetOnly = it.at(k).tsnetOnly;
            bool       ok        = (it.at(k).ok == false);
            // 缺的正好是它：别的项必须都还是 true
            int missingNow = 0;
            for (const SelfCheck::Item& x : it)
                if (!x.ok)
                    ++missingNow;
            ok = ok && missingNow == 1;
            // 官方档下 tsnetOnly 的项不计入 missing
            ok = ok && (SelfCheck::missingCount(it, false) == (tsnetOnly ? 0 : 1));
            ok = ok && (SelfCheck::missingCount(it, true) == 1);
            if (!ok) {
                allGood = false;
                bad << QStringLiteral("%1: 官方档 missing=%2 / tsnet 档 missing=%3 / tsnetOnly=%4")
                           .arg(cs.key)
                           .arg(SelfCheck::missingCount(it, false))
                           .arg(SelfCheck::missingCount(it, true))
                           .arg(tsnetOnly ? 1 : 0);
            }
            ++n;
        }
        check(QStringLiteral("D4  6 项各自单独缺失 %1/6 都判对").arg(n), allGood);
        for (const QString& b : bad)
            report << QStringLiteral("        ✗ %1").arg(b);

        // ⚠ 下面这几条**按 key 查，不写下标**。原先它们写的是 items.at(4)/at(5)/at(6)，
        //    删掉 has_contact 之后下标整体前移一位，五条断言全红 —— 而功能一点没坏。
        //    写死的下标只能测出"我改了顺序"，测不出"规则对不对"。
        const QStringList tsnetKeys = { QStringLiteral("relay_present"),
                                        QStringLiteral("tsnet_authkey"),
                                        QStringLiteral("tsnet_hostname") };
        check(QStringLiteral("D5  tsnetOnly 正好是 relay_present / tsnet_authkey / tsnet_hostname "
                             "三项（且只有它们）"),
              [&] {
                  for (const SelfCheck::Item& x : items)
                      if (x.tsnetOnly != tsnetKeys.contains(x.key))
                          return false;
                  return true;
              }());

        const QStringList unfixableKeys = { QStringLiteral("rd_present"),
                                            QStringLiteral("relay_present") };
        check(QStringLiteral("D6  修不了的项标 fixable=false（rd_present / relay_present —— 只能把包补全）"),
              [&] {
                  for (const SelfCheck::Item& x : items)
                      if (unfixableKeys.contains(x.key) && x.fixable)
                          return false;
                  return true;
              }());
        check(QStringLiteral("D7  其余四项都标 fixable=true（每条都能「点一下就补」）"),
              [&] {
                  for (const SelfCheck::Item& x : items)
                      if (!unfixableKeys.contains(x.key) && !x.fixable)
                          return false;
                  return true;
              }());

        SelfCheck::Inputs none;
        const QVector<SelfCheck::Item> allBad = SelfCheck::evaluate(none);
        // 官方档下 tsnetOnly 那三项不算：全缺时只剩 rd_present / has_id / fixed_password
        check(QStringLiteral("D8  全 false 时官方档 missing == 3（那三项 tsnet 的不算）"),
              SelfCheck::missingCount(allBad, false) == 3);
        check(QStringLiteral("D9  全 false 时 tsnet 档 missing == 6（条目总数）"),
              SelfCheck::missingCount(allBad, true) == 6);

        check(QStringLiteral("D10 每项都有非空 key / title / hint"),
              [&items] {
                  for (const SelfCheck::Item& x : items)
                      if (x.key.isEmpty() || x.title.isEmpty() || x.hint.isEmpty())
                          return false;
                  return true;
              }());
    }

    // ══ E 段 真桥接闸门（要 authkey，否则明确 SKIP）════════════════════════
    report << QStringLiteral("E 段 真桥接闸门：对端永远不上线时，到底会不会乱拉客户端");
    {
        QString authkey = qEnvironmentVariable("TS_AUTHKEY");
        if (authkey.isEmpty())
            authkey = authkeyFromFile;

        const QString relay = TsnetBridge::findRelayExe();
        if (authkey.isEmpty() || relay.isEmpty()) {
            report << QStringLiteral("      SKIP —— 没有 authkey 或找不到 pmrelay.exe。"
                                     "**这一段没有验证**：真 pmrelay 上“对端不上线就不放行”"
                                     "这条闸门，这次没被测过。");
        } else {
            const QString base = QCoreApplication::applicationDirPath()
                                 + QStringLiteral("/data/tsnet-test");
            const QString peer = QStringLiteral("puremote-no-such-peer-zzz");

            AppConfig ecfg;
            ecfg.load();
            ecfg.setNetMode(QStringLiteral("tsnet"));
            ecfg.setTsnetAuthKey(authkey);
            ecfg.setTsnetHostname(QStringLiteral("puremote-test-b"));
            QJsonArray earr;
            earr.append(QJsonObject { { QStringLiteral("name"), QStringLiteral("不存在的对端") },
                                      { QStringLiteral("tsnet_host"), peer } });
            ecfg.setContacts(earr);
            AddressBook ebook(&ecfg);
            ebook.load();

            TsnetBridge ebridge;
            ebridge.setRelayPath(relay);
            ebridge.setAuthKey(authkey);
            ebridge.setHostname(QStringLiteral("puremote-test-b"));
            ebridge.setStateDir(base + QStringLiteral("/b"));
            ebridge.setLocalPort(21131);
            ebridge.setVerbose(false);

            ConnectSession es(&ecfg, nullptr, &ebridge, &ebook);
            es.setPeerTimeoutForTest(8000);

            bool    sawReady = false;
            QString failMsg;
            QObject::connect(&ebridge, &TsnetBridge::ready, [&sawReady] { sawReady = true; });
            QObject::connect(&es, &ConnectSession::failed, [&failMsg](const QString& m) { failMsg = m; });

            es.connectTo(0);

            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 180000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                if (sawReady && es.phase() == ConnectSession::Phase::Failed)
                    break;
            }

            if (!sawReady) {
                report << QStringLiteral("      SKIP —— 桥接在 180 秒内没能 ready（网络/控制面问题），"
                                         "**这一段没有验证**。桥接本身的能力由 --tsnettest 覆盖。");
            } else {
                check(QStringLiteral("E1  对端始终不上线 → 最终 Failed"),
                      es.phase() == ConnectSession::Phase::Failed);
                check(QStringLiteral("E2  全程**从未**拉起客户端 —— 真 pmrelay 上闸门确实拦住了"),
                      !es.clientSpawnedForTest());
                check(QStringLiteral("E3  失败文案是“等对端超时”，不是别的错（说明真走到了等待那一步）"),
                      failMsg.contains(QStringLiteral("对方")) && failMsg.contains(QStringLiteral("tailnet")));
                check(QStringLiteral("E4  超时后桥接被收干净"), !ebridge.isRunning());
            }

            es.disconnectNow();
            ebridge.stop();
        }
    }

    report << QStringLiteral("──── %1（%2 项失败）────")
                  .arg(fails == 0 ? QStringLiteral("全绿") : QStringLiteral("有失败"))
                  .arg(fails);

    // 还原配置。备份是空的话说明本来就没有这个文件 —— 那就把测试写出来的删掉。
    if (cfgExisted)
        writeWholeFile(cfgPath, cfgBackup);
    else
        QFile::remove(cfgPath);

    const QString text = report.join(QLatin1Char('\n')) + QLatin1Char('\n');
    QFile         out(QDir::temp().filePath(QStringLiteral("puremote-booktest.txt")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(text.toUtf8());
        out.close();
    }
    std::printf("%s", text.toUtf8().constData());
    std::fflush(stdout);
    return fails == 0 ? 0 : 1;
}

// ---- 画质与性能（按联系人）无头验收 ----
//
// 跑的是一份**仿真的 peers 文件**，全程在 %TEMP% 下自建目录里（PeerPrefs::setDirForTest），
// 一个字节都不碰真实配置。
//
// ⚠ 报告里必须写清这段**证明了什么、没证明什么**：
//   证明的是"文件读写位置对不对、值合不合法、多行数组会不会被写坏"。
//   **不证明** RustDesk 会按这个画质出画 —— 那个要真会话。B 段的四个档位系数
//   （low 0.5 / balanced 0.67 / custom 1.0 / best 1.5）是 2026-10-09 用真自连
//   在日志 `video_service.rs initial quality` 上实测出来的，这里只是把它们固化下来。
static int runQualityTest()
{
    attachParentConsole();

    QStringList report;
    int         fails = 0;
    auto        check = [&fails, &report](const QString& name, bool ok) {
        report << QStringLiteral("  [%1] %2").arg(ok ? QStringLiteral("PASS")
                                                     : QStringLiteral("FAIL"), name);
        if (!ok)
            ++fails;
    };

    report << QStringLiteral("──── --qtest：画质与性能（peers/<对端>.toml）────");
    report << QStringLiteral("说明：跑在临时目录的仿真文件上，不碰真实配置。");
    report << QStringLiteral("      这证明文件读写与校验；**不证明** RustDesk 按此出画。");

    // ══ A 段 值域与校验（纯函数）══════════════════════════════════════════
    report << QStringLiteral("A 段 合法值集合与校验");
    {
        const QStringList iq = PeerPrefs::imageQualities();
        check(QStringLiteral("A1 画质档位恰好是 low/balanced/best/custom（顺序即界面顺序）"),
              iq == QStringList({ QStringLiteral("low"), QStringLiteral("balanced"),
                                  QStringLiteral("best"), QStringLiteral("custom") }));
        check(QStringLiteral("A2 编码器集合含 auto/h264/h265/vp9/av1/vp8"),
              PeerPrefs::codecs() == QStringList({ QStringLiteral("auto"), QStringLiteral("h264"),
                                                   QStringLiteral("h265"), QStringLiteral("vp9"),
                                                   QStringLiteral("av1"), QStringLiteral("vp8") }));

        const PeerPrefs::Quality d = PeerPrefs::defaults();
        check(QStringLiteral("A3 默认值与 RustDesk 自建文件一致（balanced/50/30/auto）"),
              d.imageQuality == QLatin1String("balanced") && d.customQuality == 50
                  && d.fps == 30 && d.codec == QLatin1String("auto"));

        auto rejects = [](const PeerPrefs::Quality& q) {
            return !PeerPrefs::validate(q, nullptr);
        };
        PeerPrefs::Quality ok = PeerPrefs::defaults();

        // `original` / `adaptive_ab` 出现在二进制的同一串字符里，看着像合法档位，但实测
        // 写进去等于默认档 —— 必须被拒，否则用户选了"原画"却拿到 0.67。
        PeerPrefs::Quality bad = ok; bad.imageQuality = QStringLiteral("original");
        check(QStringLiteral("A4 拒绝看着像合法、实测无效的 original"), rejects(bad));
        bad.imageQuality = QStringLiteral("adaptive_ab");
        check(QStringLiteral("A5 拒绝 adaptive_ab"), rejects(bad));
        bad = ok; bad.imageQuality = QStringLiteral("zzzbogus");
        check(QStringLiteral("A6 拒绝完全非法的档位（RustDesk 自己不校验，只能我们拦）"),
              rejects(bad));

        bad = ok; bad.fps = 0;
        check(QStringLiteral("A7 拒绝帧率 0"), rejects(bad));
        bad.fps = 999;
        check(QStringLiteral("A8 拒绝帧率 999"), rejects(bad));
        bad = ok; bad.customQuality = -1;
        check(QStringLiteral("A9 拒绝自定义画质 -1"), rejects(bad));
        bad.customQuality = 101;
        check(QStringLiteral("A10 拒绝自定义画质 101"), rejects(bad));
        bad = ok; bad.codec = QStringLiteral("hevc");
        check(QStringLiteral("A11 拒绝 hevc（RustDesk 用的是 h265 这个名字）"), rejects(bad));

        PeerPrefs::Quality good = ok;
        good.imageQuality = QStringLiteral("custom"); good.customQuality = 88;
        good.fps = 60; good.codec = QStringLiteral("vp9");
        check(QStringLiteral("A12 接受一套正常值"), PeerPrefs::validate(good, nullptr));
    }

    // ══ B 段 文件读写（仿真 peers 文件）════════════════════════════════════
    report << QStringLiteral("B 段 读写 / 作用域 / 多行数组");
    const QString testDir = QDir(QDir::tempPath())
                                .filePath(QStringLiteral("puremote-qtest-peers"));
    QDir(testDir).removeRecursively();
    QDir().mkpath(testDir);
    PeerPrefs::setDirForTest(testDir);

    const QString peerId = QStringLiteral("123456789");
    const QString path   = PeerPrefs::filePath(peerId);

    // 仿真文件照抄真实 peers 文件的形状 —— 关键是**有多行数组**（size = [\n 0,\n …]），
    // 那正是"行首是 [ 就当表头"这种判法会翻车的地方。
    const QByteArray fake =
        "password = []\n"
        "size = [\n"
        "    0,\n"
        "    0,\n"
        "    0,\n"
        "    0,\n"
        "]\n"
        "view_style = 'original'\n"
        "image_quality = 'balanced'\n"
        "custom_image_quality = [50]\n"
        "show_remote_cursor = false\n"
        "\n"
        "[options]\n"
        "remote-menubar-edge = 'top'\n"
        "custom-fps = '30'\n"
        "codec-preference = 'auto'\n"
        "\n"
        "[info]\n"
        "username = 'tester'\n";

    {
        // 字段要填全，否则先被 validate 拦下、报的是编码器非法，测不到"文件不存在"这条
        PeerPrefs::Quality q;
        q.imageQuality = QStringLiteral("best");
        q.codec        = QStringLiteral("auto");
        QString err;
        check(QStringLiteral("B1 文件不存在时拒绝写入（不能自造：自造的文件会被 RustDesk 重置）"),
              !PeerPrefs::write(peerId, q, &err) && err.contains(QStringLiteral("连")));
    }

    writeWholeFile(path, fake);

    PeerPrefs::Quality want;
    want.imageQuality  = QStringLiteral("custom");
    want.customQuality = 88;
    want.fps           = 60;
    want.codec         = QStringLiteral("vp9");
    {
        QString err;
        const bool ok = PeerPrefs::write(peerId, want, &err);
        check(QStringLiteral("B2 完整文件上写入成功"), ok);
        if (!ok)
            report << QStringLiteral("       err = %1").arg(err);
    }

    {
        const PeerPrefs::Quality got = PeerPrefs::read(peerId);
        check(QStringLiteral("B3 读回值与写入值一致"),
              got.imageQuality == want.imageQuality && got.customQuality == want.customQuality
                  && got.fps == want.fps && got.codec == want.codec);
    }

    {
        const QString text  = readWholeFile(path);
        const QStringList ls = text.split(QLatin1Char('\n'));
        int firstTable = -1;
        for (int i = 0; i < ls.size(); ++i) {
            const QString t = ls.at(i).trimmed();
            if (t.startsWith(QLatin1Char('[')) && t.endsWith(QLatin1Char(']'))
                && !t.contains(QLatin1Char('='))) {
                firstTable = i;
                break;
            }
        }
        auto at = [&ls](const QString& key) {
            for (int i = 0; i < ls.size(); ++i)
                if (ls.at(i).trimmed().startsWith(key + QLatin1Char(' '))
                    || ls.at(i).trimmed().startsWith(key + QLatin1Char('=')))
                    return i;
            return -1;
        };
        const int iq  = at(QStringLiteral("image_quality"));
        const int ciq = at(QStringLiteral("custom_image_quality"));
        const int fps = at(QStringLiteral("custom-fps"));
        const int cd  = at(QStringLiteral("codec-preference"));
        const int opt = [&ls] {
            for (int i = 0; i < ls.size(); ++i)
                if (ls.at(i).trimmed() == QLatin1String("[options]"))
                    return i;
            return -1;
        }();
        const int info = [&ls] {
            for (int i = 0; i < ls.size(); ++i)
                if (ls.at(i).trimmed() == QLatin1String("[info]"))
                    return i;
            return -1;
        }();

        check(QStringLiteral("B4 image_quality 落在**顶层**（第一个表头之前）"),
              iq >= 0 && iq < firstTable);
        check(QStringLiteral("B5 custom_image_quality 也在顶层"), ciq >= 0 && ciq < firstTable);
        // 真实文件里这个键是**数组** `[50]`，不是字符串 —— 写成 '88' 的话 RustDesk 读不出来
        check(QStringLiteral("B6 custom_image_quality 保持数组形态 [88]"),
              ciq >= 0 && ls.at(ciq).contains(QStringLiteral("[88]")));
        // 这两个键必须落在 [options] **段内**（[options] 之后、下一个表头之前）。
        // 只判"在 [options] 行之后"是不够的：真文件里 [info]/[transfer] 也在它后面。
        check(QStringLiteral("B7 custom-fps 落在 [options] 段内（不是顶层、也没漏到 [info] 里）"),
              opt >= 0 && info > opt && fps > opt && fps < info);
        check(QStringLiteral("B8 codec-preference 落在 [options] 段内"),
              opt >= 0 && cd > opt && cd < info);

        // 多行数组必须原封不动 —— 写坏它 = 整份 TOML 非法 = RustDesk 丢弃这个对端的设置
        bool arrOk = ls.contains(QStringLiteral("size = ["))
                     && text.count(QStringLiteral("    0,")) == 4;
        check(QStringLiteral("B9 多行数组 size 没被写坏（4 个元素都在）"), arrOk);

        check(QStringLiteral("B10 表外的既有键都还在（password / view_style / show_remote_cursor）"),
              at(QStringLiteral("password")) >= 0 && at(QStringLiteral("view_style")) >= 0
                  && at(QStringLiteral("show_remote_cursor")) >= 0);
        check(QStringLiteral("B11 [info] 段没被吃掉（表尾之后还有内容）"),
              text.contains(QStringLiteral("[info]"))
                  && text.contains(QStringLiteral("username = 'tester'")));
        check(QStringLiteral("B12 备份文件 .bak 生成了"),
              QFile::exists(path + QStringLiteral(".bak")));
    }

    {
        QString err;
        PeerPrefs::write(peerId, want, &err);
        const QString once = readWholeFile(path);
        PeerPrefs::write(peerId, want, &err);
        check(QStringLiteral("B13 幂等：同一个值连写两次，文件字节相同"),
              once == readWholeFile(path));
    }

    {
        // 文件里缺字段 → 该字段取默认值，不是空串/0
        writeWholeFile(path, "image_quality = 'low'\n\n[options]\n");
        const PeerPrefs::Quality q = PeerPrefs::read(peerId);
        check(QStringLiteral("B14 缺字段时逐个回落默认值（缺的取默认，有的照读）"),
              q.imageQuality == QLatin1String("low") && q.customQuality == 50 && q.fps == 30
                  && q.codec == QLatin1String("auto"));
    }

    {
        // 没有 [options] 段 → 宁可不写，也不能猜位置
        writeWholeFile(path, "image_quality = 'low'\n");
        PeerPrefs::Quality q;
        q.imageQuality = QStringLiteral("best");
        QString err;
        check(QStringLiteral("B15 没有 [options] 段时拒绝写入（不猜位置）"),
              !PeerPrefs::write(peerId, q, &err)
                  && err.contains(QStringLiteral("[options]")));
    }

    {
        // read() 必须把不认识的值**原样**读回来，不能悄悄换成默认档。
        // 界面那句「文件里存的是 X，不在可选的四档里」就靠这个契约 ——
        // 若 read 把它换成 balanced，界面会以为一切正常，用户一按应用就把它覆盖掉。
        writeWholeFile(path, "image_quality = 'original'\n\n[options]\n");
        const PeerPrefs::Quality q = PeerPrefs::read(peerId);
        check(QStringLiteral("B15b 不认识的档位原样读回（界面靠它报警），且 validate 拒绝它"),
              q.imageQuality == QLatin1String("original")
                  && !PeerPrefs::validate(q, nullptr));
    }

    // ══ B 段续：拿**真 peers 文件**跑一遍写 ═══════════════════════════════
    //
    // B1-B15 用的仿真文件是**我自己编的**，那属于自证：真实文件的形状有多复杂，只有真文件
    // 说了算。摊开本机那份真文件看，比仿真文件野得多 —— 三个多行数组（size / size_ft /
    // size_pf）、一堆空数组、[options] 里有 `i444 = ''` 这种空串值、后面还跟着
    // [ui_flutter]（值是被引号包着的 JSON，**里面就有 `[` `]` `{` `}`**）、[transfer]……
    // 所以这一段把真文件**复制**到临时目录后原样跑 write()，断言"只动了画质那 4 行，
    // 其余行序一字未变"。源文件全程只读。
    PeerPrefs::clearDirForTest();
    const QString realDir  = PeerPrefs::dirPath();
    const QStringList reals =
        QDir(realDir).entryList({ QStringLiteral("*.toml") }, QDir::Files);
    PeerPrefs::setDirForTest(testDir);   // 再换回来，下面照旧写临时目录

    if (reals.isEmpty()) {
        report << QStringLiteral("B16-B18 SKIP：%1 下没有真 peers 文件"
                                 "（本机还没连过任何对端，没法拿真文件验）")
                      .arg(realDir);
    } else {
        // 只对比"非画质行"的**顺序**：键丢了、多写了、被挪了位置，都会让它不相等。
        // 这比"键名集合相等"严 —— 集合相等抓不到行被挪走。
        // 按**字节**读，不带 QIODevice::Text —— 带 Text 会在读的时候把 \r 吃掉，
        // 那样"写盘时把 \n 变成 \r\n"这种改动就永远看不见（B18 用的 readWholeFile
        // 正好带 Text，所以它证明不了行尾没被动过）。
        auto readRaw = [](const QString& p) -> QByteArray {
            QFile f(p);
            if (!f.open(QIODevice::ReadOnly))
                return QByteArray();
            return f.readAll();
        };

        auto stripQuality = [](const QStringList& ls) {
            QStringList out;
            for (const QString& l : ls) {
                const QString t = l.trimmed();
                if (t.startsWith(QLatin1String("image_quality"))
                    || t.startsWith(QLatin1String("custom_image_quality"))
                    || t.startsWith(QLatin1String("custom-fps"))
                    || t.startsWith(QLatin1String("codec-preference")))
                    continue;
                out << l;
            }
            return out;
        };

        int wrote = 0, readback = 0, intact = 0, eolSame = 0;
        QStringList names;
        for (const QString& fn : reals) {
            const QString id  = QFileInfo(fn).completeBaseName();
            const QString src = QDir(realDir).filePath(fn);
            const QString dst = PeerPrefs::filePath(id);   // 临时目录里的同名副本

            const QString origText = readWholeFile(src);
            if (origText.isEmpty())
                continue;
            names << id;
            writeWholeFile(dst, origText.toUtf8());   // 只读源文件

            PeerPrefs::Quality q;
            q.imageQuality  = QStringLiteral("best");   // 四项都跟真文件里的默认值不同
            q.customQuality = 88;
            q.fps           = 60;
            q.codec         = QStringLiteral("vp9");

            QString err;
            if (PeerPrefs::write(id, q, &err))
                ++wrote;

            const PeerPrefs::Quality got = PeerPrefs::read(id);
            if (got.imageQuality == q.imageQuality && got.customQuality == q.customQuality
                && got.fps == q.fps && got.codec == q.codec)
                ++readback;

            const QStringList a = origText.split(QLatin1Char('\n'));
            const QStringList b = readWholeFile(dst).split(QLatin1Char('\n'));
            if (stripQuality(a) == stripQuality(b))
                ++intact;

            // 行尾：原件是 LF（RustDesk 写的），改完不该变成 CRLF。
            if (readRaw(src).count('\r') == readRaw(dst).count('\r'))
                ++eolSame;
            else
                report << QStringLiteral("       ⚠ %1 的行尾被改了：原 %2 个 \\r → 现 %3 个 \\r")
                              .arg(id)
                              .arg(readRaw(src).count('\r'))
                              .arg(readRaw(dst).count('\r'));

            // 源文件半个字节都不能动（写入目标已经被 setDirForTest 换走了，这条是兜底）
            if (readWholeFile(src) != origText)
                report << QStringLiteral("       ⚠ 真文件被改动了：%1").arg(id);
        }

        const QString which = names.join(QStringLiteral(", "));
        check(QStringLiteral("B16 真文件副本上写入成功（%1 个：%2）").arg(names.size()).arg(which),
              wrote == names.size());
        check(QStringLiteral("B17 写完之后读回来就是新值"), readback == names.size());
        check(QStringLiteral("B18 除画质那 4 行外**其余行序一字未变**（多行数组 / 空数组 /"
                             " 值里带 [ ] { } 的表都原样）"),
              intact == names.size());
        check(QStringLiteral("B19 行尾没被换掉（原件 LF，改完还是 LF —— 别写成 Windows 的 CRLF）"),
              eolSame == names.size());
    }

    // ══ B20 段：专门去造"复查者举出来的那几个形状" ════════════════════════
    //
    // 上面 B16-B19 用的是**真文件**，它给的是"我这份文件没被写坏"的证据；但真文件里
    // 没有这些形状，所以它们证明不了。这一段是我自己编的，编的**目的不是复验真文件，
    // 而是把已知会出事的输入钉死**。每一条都注明了"旧代码在这里会怎么错"，免得以后
    // 有人觉得这是多余的用例把它删掉。
    {
        auto writeText = [](const QString& p, const QByteArray& b) {
            QFile f(p);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return false;
            const bool ok = f.write(b) == b.size();
            f.close();
            return ok;
        };
        auto rawOf = [](const QString& p) {
            QFile f(p);
            if (!f.open(QIODevice::ReadOnly))
                return QByteArray();
            return f.readAll();
        };
        auto splitLF = [](const QByteArray& b) {
            return QString::fromUtf8(b).split(QLatin1Char('\n'));
        };
        auto idxOf = [](const QStringList& ls, const QString& exact) {
            for (int i = 0; i < ls.size(); ++i)
                if (ls.at(i).trimmed() == exact)
                    return i;
            return -1;
        };

        PeerPrefs::setDirForTest(testDir);

        PeerPrefs::Quality q;
        q.imageQuality  = QStringLiteral("best");
        q.customQuality = 77;
        q.fps           = 45;
        q.codec         = QStringLiteral("h264");

        // ---- B20：注释里有**没配对的** `[` 和 `'` ----
        // 旧扫描器：`# 备注…[` 这一行把深度留在 1 再也回不去 → [info] 认不出来 →
        // tableEnd([options]) 返回 EOF → 缺失的 custom-fps / codec-preference 被**追加到
        // 文件末尾**，也就是落进 [info] 里；而 write() 的复验用的是**同一套**判定，
        // 照样读得到值、照样返回成功。TOML 本身合法（键属于 [info]），所以 RustDesk 不报错，
        // 只是这个设置**永远不生效** —— 静默失效最难查。
        // 注：这两个键故意**不在** fixture 里，否则会走"原地替换"分支，就测不出追加位置了。
        {
            const QByteArray fixture = QStringLiteral(
                "image_quality = 'balanced'\n"
                "custom_image_quality = [50]\n"
                "\n"
                "[options]\n"
                "# 备注：这里有个 [ 没配对，还有一个 ' 也没配对\n"
                "\n"
                "[info]\n"
                "username = 'me'\n").toUtf8();
            const QString id = QStringLiteral("edge-comment");
            writeText(PeerPrefs::filePath(id), fixture);

            QString err;
            const bool ok = PeerPrefs::write(id, q, &err);
            check(QStringLiteral("B20 注释里有没配对的 [ / ' 时，写入仍成功"), ok);

            const QStringList ls    = splitLF(rawOf(PeerPrefs::filePath(id)));
            const int         iInfo = idxOf(ls, QStringLiteral("[info]"));
            const int         iFps  = idxOf(ls, QStringLiteral("custom-fps = '45'"));
            const int         iCod  = idxOf(ls, QStringLiteral("codec-preference = 'h264'"));
            const bool placed = iInfo > 0 && iFps > 0 && iCod > 0 && iFps < iInfo && iCod < iInfo;
            if (!placed)
                report << QStringLiteral("       ⚠ [info] 在第 %1 行，custom-fps 在第 %2 行，"
                                         "codec-preference 在第 %3 行（-1 = 没找到）")
                              .arg(iInfo).arg(iFps).arg(iCod);
            check(QStringLiteral("B20b 新键仍插在 [options] 里（没被追加到文件末尾 / [info] 里）"),
                  placed);

            const PeerPrefs::Quality got = PeerPrefs::read(id);
            check(QStringLiteral("B20c 四个值都读得回来"),
                  got.imageQuality == q.imageQuality && got.customQuality == q.customQuality
                      && got.fps == q.fps && got.codec == q.codec);
        }

        // ---- B21：原文件是 CRLF（别的工具写的）----
        // 旧代码：写盘带 QIODevice::Text → 整份被改行尾；而**读**也带 Text 的话，
        // 那份"原文"在比较时 \r 已经被吃掉，改动根本看不见。两个方向都没了。
        // 期望：原风格保留（继续 CRLF），新插入的行也跟着用 CRLF，不能一半一半。
        {
            const QByteArray fixture = QStringLiteral(
                "image_quality = 'balanced'\r\n"
                "\r\n"
                "[options]\r\n"
                "custom-fps = '30'\r\n").toUtf8();
            const QString id = QStringLiteral("edge-crlf");
            writeText(PeerPrefs::filePath(id), fixture);

            QString err;
            const bool ok = PeerPrefs::write(id, q, &err);

            const QByteArray out = rawOf(PeerPrefs::filePath(id));
            check(QStringLiteral("B21 CRLF 的 peers 文件也能写"), ok);
            check(QStringLiteral("B21b 改完仍是**清一色 CRLF**（没有一半 CRLF 一半 LF）"),
                  !out.isEmpty() && out.count('\n') == out.count("\r\n"));
            if (!out.isEmpty() && out.count('\n') != out.count("\r\n"))
                report << QStringLiteral("       ⚠ \\n=%1 个，其中 \\r\\n=%2 个")
                              .arg(out.count('\n')).arg(out.count("\r\n"));

            const PeerPrefs::Quality got = PeerPrefs::read(id);
            check(QStringLiteral("B21c CRLF 文件里的值读得回来"),
                  got.fps == q.fps && got.codec == q.codec
                      && got.imageQuality == q.imageQuality);
        }

        // ---- B22：文件不是合法 UTF-8 ----
        // `QString::fromUtf8` 遇到坏字节会**静默塞 U+FFFD**，于是"读进来再写回去"就把一份
        // 坏文件改得更坏，而且一路绿灯。期望：直接拒绝、**一个字节都不动**（宁可调不了画质，
        // 也不能把 RustDesk 的配置改坏 —— 那会丢 enc_id，ID 都被换掉）。
        {
            const QByteArray fixture =
                QByteArray("image_quality = 'balanced'\n\n[options]\n# ")
                + QByteArray(1, '\xFF') + QByteArray("\ncustom-fps = '30'\n");
            const QString id = QStringLiteral("edge-badutf8");
            writeText(PeerPrefs::filePath(id), fixture);
            const QByteArray beforeBytes = rawOf(PeerPrefs::filePath(id));

            QString    err;
            const bool ok = PeerPrefs::write(id, q, &err);
            check(QStringLiteral("B22 非 UTF-8 的文件被拒绝（不是「读成乱码再写回去」）"), !ok);
            check(QStringLiteral("B22b 被拒之后文件**逐字节没动**"),
                  rawOf(PeerPrefs::filePath(id)) == beforeBytes);
            if (!ok)
                report << QStringLiteral("       拒绝理由：%1").arg(err.left(60));
        }

        // ---- B23：custom_image_quality 是负数 ----
        // 读的时候正则只认 \d+ 的话 `[-5]` 会被读成 **5** —— 界面上一个负数被安静地翻成正数，
        // validate 压根没机会说话（它校验的是界面里那个已经被美化过的值）。
        {
            const QByteArray fixture = QStringLiteral(
                "image_quality = 'custom'\n"
                "custom_image_quality = [-5]\n"
                "\n"
                "[options]\n"
                "custom-fps = '30'\n").toUtf8();
            const QString id = QStringLiteral("edge-neg");
            writeText(PeerPrefs::filePath(id), fixture);
            check(QStringLiteral("B23 负的 custom_image_quality 读回来还是负的（不是被读成 +5）"),
                  PeerPrefs::read(id).customQuality == -5);
        }
    }

    PeerPrefs::clearDirForTest();
    QDir(testDir).removeRecursively();

    // ══ C 段 真 peers 文件只读检查 ═════════════════════════════════════════
    {
        const QString realDir = PeerPrefs::dirPath();
        QDir          d(realDir);
        const QStringList files = d.entryList({ QStringLiteral("*.toml") }, QDir::Files);
        if (files.isEmpty()) {
            report << QStringLiteral("C 段 SKIP：%1 下没有 peers 文件（本机还没连过任何对端）")
                          .arg(realDir);
        } else {
            report << QStringLiteral("C 段 真 peers 文件（只读，不写）");
            int bad = 0;
            for (const QString& fn : files) {
                const QString id = QFileInfo(fn).completeBaseName();
                const PeerPrefs::Quality q = PeerPrefs::read(id);
                if (!PeerPrefs::validate(q, nullptr))
                    ++bad;
            }
            check(QStringLiteral("C1 本机 %1 个真 peers 文件读出来都是合法值").arg(files.size()),
                  bad == 0);
        }
    }

    report << QStringLiteral("──── %1（%2 项失败）────")
                  .arg(fails == 0 ? QStringLiteral("全绿") : QStringLiteral("有失败"))
                  .arg(fails);

    const QString text = report.join(QLatin1Char('\n')) + QLatin1Char('\n');
    QFile         out(QDir::temp().filePath(QStringLiteral("puremote-qtest.txt")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(text.toUtf8());
        out.close();
    }
    std::printf("%s", text.toUtf8().constData());
    std::fflush(stdout);
    return fails == 0 ? 0 : 1;
}

// ---- M4.1 / M4.3 无头验收 ----
//
// A 段全是**离线可判**的：路径标记解析 + tail 循环的"只处理完整行"。
// B 段是**真体检**，要网络。所以 A 段永远绿，B 段取决于本机网络 —— 这正是体检的意义。
// 报告里写明这一点，免得以后看到红的先怀疑代码。
static int runNetCheck()
{
    attachParentConsole();

    QStringList report;
    int         fails = 0;
    auto        check = [&fails, &report](const QString& name, bool ok) {
        report << QStringLiteral("  [%1] %2").arg(ok ? QStringLiteral("PASS")
                                                     : QStringLiteral("FAIL"), name);
        if (!ok)
            ++fails;
    };

    auto pump = [](int ms) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    };

    report << QStringLiteral("──── --netcheck：网络体检 / 连接路径 ────");

    // ---- A 段：路径标记解析（离线）----
    {
        NetworkProbe p;
        QStringList  got;
        QObject::connect(&p, &NetworkProbe::pathChanged,
                         [&got](const QString& s) { got << s; });

        const QString relayLine = QStringLiteral(
            R"(INFO [src\client.rs:1187] 269.8414ms used to establish Relay connection)");
        const QString webrtcLine = QStringLiteral(
            R"(INFO [src\client.rs:1187] 1.0086594s used to establish WebRTC connection)");
        const QString punchLine = QStringLiteral(
            R"(INFO [src\client.rs:1566] 78.8525ms used to establish WebRTC connection with UDP+WebRTC punch)");
        // 被控端那一行跟路径无关，是 ConnectionWatcher 关心的，必须**不**被这里认成路径
        const QString serverLine = QStringLiteral(
            R"(INFO [src\server\connection.rs:2117] peer info supported_encoding: SupportedEncoding {..})");

        check(QStringLiteral("A1  中继标记被识别"), p.feedLogLineForTest(relayLine));
        check(QStringLiteral("A2  结论是「中继」"), p.pathSummary().contains(QStringLiteral("中继")));

        check(QStringLiteral("A3  WebRTC 标记被识别"), p.feedLogLineForTest(webrtcLine));
        check(QStringLiteral("A4  结论是「直连」"), p.pathSummary().contains(QStringLiteral("直连")));

        check(QStringLiteral("A5  WebRTC punch 变体也认"), p.feedLogLineForTest(punchLine));
        check(QStringLiteral("A6  结论仍是「直连」"), p.pathSummary().contains(QStringLiteral("直连")));

        // TCP punch —— 这台机器上最常见的直连形态（国内出口 Symmetric，打洞后走 TCP）。
        // 第一版解析器按"含 WebRTC 才是直连"写，这条不匹配 → 路径永远不显示。
        const QString tcpPunch = QStringLiteral(
            R"(INFO [src\client.rs:1566] 539µs used to establish TCP connection with TCP punch)");
        check(QStringLiteral("A7 TCP punch 变体也认（别再漏这一个）"),
              p.feedLogLineForTest(tcpPunch) && p.pathSummary().contains(QStringLiteral("直连")));

        check(QStringLiteral("A8  无关日志行不认"), !p.feedLogLineForTest(serverLine));
        check(QStringLiteral("A9  无关行没污染结论"), p.pathSummary().contains(QStringLiteral("直连")));

        const int before = got.size();
        p.feedLogLineForTest(webrtcLine);
        check(QStringLiteral("A10  同一结论不重复发信号"), got.size() == before);

        p.feedLogLineForTest(relayLine);
        check(QStringLiteral("A11 结论变了要再发一次"),
              got.size() == before + 1 && p.pathSummary().contains(QStringLiteral("中继")));
        check(QStringLiteral("A12 全程一共只发了 3 次（中继→直连→中继）"), got.size() == 3);
    }

    // ---- A 段续：拿**真日志里的字节**喂解析器 ----
    //
    // 上面 A1-A11 喂的全是我自己写的字符串，那是自证 —— 真实世界到底打哪几种形态，
    // 只能从真日志里读。这一条就是被它抓出来的：全量扫完发现有四种，
    // 而第一版解析器漏了「TCP connection with TCP punch」，那恰好是本机最常见的直连形态，
    // 表现是"路径永远不显示"，自己编字符串的测试全绿也照样发现不了。
    {
        // 只看每个文件最后 256KB —— 日志能涨到几十 MB，而路径行总在最近一次连接的末尾
        constexpr qint64 kTailBytes = 256 * 1024;
        QStringList      realRelay;
        QStringList      realDirect;
        int              scanned = 0;

        for (const QString& path : NetworkProbe::rustdeskLogPaths()) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const qint64 size = f.size();
            if (size > kTailBytes)
                f.seek(size - kTailBytes);
            const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
            for (const QString& ln : lines) {
                if (!ln.contains(QStringLiteral("used to establish")))
                    continue;
                ++scanned;
                if (ln.contains(QStringLiteral("Relay connection")))
                    realRelay << ln.trimmed();
                else
                    realDirect << ln.trimmed();
            }
        }

        report << QStringLiteral("  ---- 真日志扫描：扫到 %1 行 establish"
                                 "（中继 %2 / 直连 %3）----")
                      .arg(scanned).arg(realRelay.size()).arg(realDirect.size());

        if (scanned == 0) {
            report << QStringLiteral("      SKIP —— 本机日志里还没有过连接记录，"
                                     "这一段（真字节分类）**没有验证**。\n"
                                     "      连一次（哪怕是自连）之后重跑就会出现。");
        } else {
            // (1) 一行都不能漏：任何一行都应该被判成中继或直连之一
            NetworkProbe rp;
            int          missed = 0;
            for (const QString& ln : realRelay + realDirect) {
                if (!rp.feedLogLineForTest(ln))
                    ++missed;
            }
            check(QStringLiteral("A13 真日志里每一行都被分类（一行都没漏）"), missed == 0);

            // (2) 分类方向得跟行里的字面一致
            if (!realRelay.isEmpty()) {
                NetworkProbe r;
                r.feedLogLineForTest(realRelay.first());
                check(QStringLiteral("A14 真·中继行 → 判成「中继」"),
                      r.pathSummary().contains(QStringLiteral("中继")));
            } else {
                report << QStringLiteral("      SKIP 真日志里没有中继样本，方向断言没跑");
            }
            if (!realDirect.isEmpty()) {
                NetworkProbe d;
                d.feedLogLineForTest(realDirect.first());
                check(QStringLiteral("A15 真·直连行 → 判成「直连」"),
                      d.pathSummary().contains(QStringLiteral("直连")));
                // 直连那几种变体每一种都得单独过 —— 漏的正是其中一种
                NetworkProbe d2;
                QStringList   variants;
                for (const QString& ln : realDirect) {
                    const QString tail = ln.section(QStringLiteral("used to establish"), 1).trimmed();
                    if (!variants.contains(tail))
                        variants << tail;
                }
                bool allDirect = true;
                for (const QString& ln : realDirect) {
                    NetworkProbe one;
                    one.feedLogLineForTest(ln);
                    if (!one.pathSummary().contains(QStringLiteral("直连")))
                        allDirect = false;
                }
                check(QStringLiteral("A16 直连的 %1 种真实变体**每一种**都判成直连")
                          .arg(variants.size()),
                      allDirect);
                for (const QString& v : variants)
                    report << QStringLiteral("         变体：%1").arg(v);
            } else {
                report << QStringLiteral("      SKIP 真日志里没有直连样本，变体断言没跑");
            }
        }
    }

    // ---- A 段续：tail 循环（真文件，离线）----
    //
    // 验的是"只处理完整行"——RustDesk 写日志时可能正写到一半，切开的半行要是被当成一行，
    // 标记就永远匹配不上，路径显示会莫名其妙地不出现。
    {
        const QString tmpPath =
            QDir::temp().filePath(QStringLiteral("puremote-netcheck-tail.txt"));
        QFile::remove(tmpPath);
        {
            QFile f(tmpPath);
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            f.close();                      // 空文件：prime 会把游标定在 0
        }

        NetworkProbe p;
        QStringList  got;
        QObject::connect(&p, &NetworkProbe::pathChanged,
                         [&got](const QString& s) { got << s; });
        p.setPathLogPathsForTest({ tmpPath });
        p.startPathWatch();
        pump(1700);                          // 让第一轮 poll 完成 prime

        auto append = [&tmpPath](const QByteArray& data) {
            QFile f(tmpPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append))
                f.write(data);
        };

        append(QByteArray("INFO [src\\client.rs:1187] 269ms used to establish Relay connection"));
        pump(1700);
        check(QStringLiteral("A17 半行（没有换行）**不**处理"), got.isEmpty());

        append(QByteArray("\n"));
        pump(1700);
        check(QStringLiteral("A18 补上换行后才认这一行"),
              got.size() == 1 && got.first().contains(QStringLiteral("中继")));

        append(QByteArray("INFO [src\\client.rs:1187] 78ms used to establish WebRTC connection\n"));
        pump(1700);
        check(QStringLiteral("A19 后续整行正常接上"),
              got.size() == 2 && got.last().contains(QStringLiteral("直连")));

        p.stopPathWatch();
        QFile::remove(tmpPath);
    }

    // ---- B 段：真体检（要网络）----
    {
        AppConfig    cfg;
        NetworkProbe p;
        bool         done = false;
        QObject::connect(&p, &NetworkProbe::finished, [&done] { done = true; });

        cfg.load();
        QElapsedTimer t;
        t.start();
        p.run(cfg);
        while (!done && t.elapsed() < 90000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

        check(QStringLiteral("B1  体检在 90 秒内跑完"), done);

        const QVector<NetworkProbe::Item> items = p.items();
        check(QStringLiteral("B2  至少产出了 DNS / UDP 两项"), items.size() >= 2);

        bool sawDns = false;
        bool sawUdp = false;
        for (const NetworkProbe::Item& it : items) {
            if (it.key == QLatin1String("dns_official")) sawDns = true;
            if (it.key == QLatin1String("udp_egress"))   sawUdp = true;
            // 逐项落进报告 —— 报告本身就是给人看的体检结论。
            // 软项即使 ok=false 也标「注意」不标「FAIL」：那是环境事实（IPv6 不可用、
            // 官方中继端口不通），不是故障，写成 FAIL 会让人以为体检没过。
            const QString tag = it.ok    ? QStringLiteral("PASS")
                              : it.hard  ? QStringLiteral("FAIL")
                                         : QStringLiteral("注意");
            report << QStringLiteral("  [%1] %2：%3")
                          .arg(tag, it.hard ? QStringLiteral("硬") : QStringLiteral("软"),
                               it.title);
            for (const QString& ln : it.detail.split(QLatin1Char('\n'))) {
                if (!ln.isEmpty())
                    report << QStringLiteral("         %1").arg(ln);
            }
            if (it.hard && !it.ok)
                ++fails;
        }
        check(QStringLiteral("B3  报告里有 dns_official 这一项"), sawDns);
        check(QStringLiteral("B4  报告里有 udp_egress 这一项"), sawUdp);
        check(QStringLiteral("B5  硬性项全绿（软项是环境事实，不算没准备好）"), p.badCount() == 0);
        check(QStringLiteral("B6  跑完之后 isRunning() 归位"), !p.isRunning());
    }

    report << QStringLiteral("──── %1（%2 项失败）────")
                  .arg(fails == 0 ? QStringLiteral("全绿") : QStringLiteral("有失败"))
                  .arg(fails);

    const QString text = report.join(QLatin1Char('\n')) + QLatin1Char('\n');
    QFile out(QDir::temp().filePath(QStringLiteral("puremote-netcheck.txt")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(text.toUtf8());
        out.close();
    }
    std::printf("%s", text.toUtf8().constData());
    std::fflush(stdout);
    return fails == 0 ? 0 : 1;
}

// ---- M4.2 无头验收 ----
//
// A 段是纯函数：四档各写哪些键、能不能写、引导文案有没有。不碰任何文件。
// B 段真写 RustDesk2.toml：验的是**事务语义**（要么全写对，要么一个字都不动）。
// 所以 B 段必须先停 RustDesk（跑着的时候写它会被进程回写覆盖），没停就 SKIP。
static int runProTest()
{
    attachParentConsole();

    QStringList report;
    int         fails = 0;
    auto        check = [&fails, &report](const QString& name, bool ok) {
        report << QStringLiteral("  [%1] %2").arg(ok ? QStringLiteral("PASS")
                                                     : QStringLiteral("FAIL"), name);
        if (!ok)
            ++fails;
    };

    report << QStringLiteral("──── --proftest：网络档位 → [options] 映射 ────");

    // 构造一份**只活在内存里**的配置（不 load、不 save），完全不碰用户的真配置文件
    AppConfig cfg;
    cfg.setCustomRendezvous(QStringLiteral("hbbs.example.com"));
    cfg.setRelayServer(QStringLiteral("hbbr.example.com:21117"));
    cfg.setApiServer(QStringLiteral("api.example.com"));
    cfg.setPublicKey(QStringLiteral("AbCdEf0123456789"));
    cfg.setTsnetAuthKey(QStringLiteral("tskey-auth-test"));

    auto valueOf = [](const QVector<RustDeskController::OptionEdit>& v, const QString& key) {
        for (const RustDeskController::OptionEdit& e : v) {
            if (e.key == key)
                return e.value;
        }
        return QStringLiteral("<没有这个键>");
    };

    // ---- A 段：映射（纯函数）----
    {
        const QVector<RustDeskController::OptionEdit> off =
            NetworkProfile::editsFor(QStringLiteral("official"), cfg);
        check(QStringLiteral("A1  官方档恰好管 7 个键"), off.size() == 7);
        check(QStringLiteral("A2  官方档**全是空值**（= 把键删干净，回落 RustDesk 默认）"),
              std::all_of(off.cbegin(), off.cend(),
                          [](const RustDeskController::OptionEdit& e) { return e.value.isEmpty(); }));
        check(QStringLiteral("A3  官方档连 direct-server 也不留"),
              valueOf(off, QStringLiteral("direct-server")).isEmpty());

        const QVector<RustDeskController::OptionEdit> ts =
            NetworkProfile::editsFor(QStringLiteral("tsnet"), cfg);
        check(QStringLiteral("A4  tsnet 档 direct-server=Y"),
              valueOf(ts, QStringLiteral("direct-server")) == QStringLiteral("Y"));
        check(QStringLiteral("A5  tsnet 档 direct-access-port=21118"),
              valueOf(ts, QStringLiteral("direct-access-port")) == QStringLiteral("21118"));
        check(QStringLiteral("A6  tsnet 档不写任何 rendezvous 服务器（这条路不经过它）"),
              valueOf(ts, QStringLiteral("custom-rendezvous-server")).isEmpty()
                  && valueOf(ts, QStringLiteral("relay-server")).isEmpty()
                  && valueOf(ts, QStringLiteral("api-server")).isEmpty()
                  && valueOf(ts, QStringLiteral("key")).isEmpty());
        check(QStringLiteral("A7  tsnet 档不开 websocket"),
              valueOf(ts, QStringLiteral("allow-websocket")).isEmpty());

        const QVector<RustDeskController::OptionEdit> cf =
            NetworkProfile::editsFor(QStringLiteral("cloudflare"), cfg);
        check(QStringLiteral("A8  cloudflare 档填上四个服务器字段"),
              valueOf(cf, QStringLiteral("custom-rendezvous-server")) == QStringLiteral("hbbs.example.com")
                  && valueOf(cf, QStringLiteral("relay-server")) == QStringLiteral("hbbr.example.com:21117")
                  && valueOf(cf, QStringLiteral("api-server")) == QStringLiteral("api.example.com")
                  && valueOf(cf, QStringLiteral("key")) == QStringLiteral("AbCdEf0123456789"));
        check(QStringLiteral("A9  cloudflare 档开 allow-websocket（CF 代理不了裸 TCP）"),
              valueOf(cf, QStringLiteral("allow-websocket")) == QStringLiteral("Y"));
        check(QStringLiteral("A10 cloudflare 档不留 direct-server"),
              valueOf(cf, QStringLiteral("direct-server")).isEmpty()
                  && valueOf(cf, QStringLiteral("direct-access-port")).isEmpty());

        const QVector<RustDeskController::OptionEdit> cu =
            NetworkProfile::editsFor(QStringLiteral("custom"), cfg);
        check(QStringLiteral("A11 custom 档填的四个字段跟 cloudflare 一样"),
              valueOf(cu, QStringLiteral("custom-rendezvous-server")) == QStringLiteral("hbbs.example.com")
                  && valueOf(cu, QStringLiteral("key")) == QStringLiteral("AbCdEf0123456789"));
        check(QStringLiteral("A12 custom 档**不开** websocket（自己开的真 TCP 端口不需要）"),
              valueOf(cu, QStringLiteral("allow-websocket")).isEmpty());

        // 键集合不能漂移：editsFor 必须不多不少正好覆盖 managedKeys
        bool keysMatch = (off.size() == NetworkProfile::managedKeys().size());
        for (const QString& k : NetworkProfile::managedKeys()) {
            for (const QVector<RustDeskController::OptionEdit>& v : { off, ts, cf, cu }) {
                if (valueOf(v, k) == QStringLiteral("<没有这个键>"))
                    keysMatch = false;
            }
        }
        check(QStringLiteral("A13 四档管的键集合完全一致，且等于 managedKeys()"), keysMatch);
        check(QStringLiteral("A14 四档的键**顺序**也一致（写盘顺序稳定才好逐条比对）"),
              off.size() == ts.size() && off.size() == cf.size() && off.size() == cu.size()
                  && off.at(0).key == ts.at(0).key && off.at(3).key == cu.at(3).key);
    }

    // ---- A 段：校验（纯函数）----
    {
        QString err;
        AppConfig bare;   // 什么都没有

        check(QStringLiteral("A15 官方档永远能过校验"),
              NetworkProfile::validate(QStringLiteral("official"), bare, &err));
        check(QStringLiteral("A16 官方档没有任何服务器地址也能过"),
              bare.customRendezvous().isEmpty());

        check(QStringLiteral("A17 认不出来的档位过不了"),
              !NetworkProfile::validate(QStringLiteral("wtf"), bare, &err));
        check(QStringLiteral("A18  tsnet 档缺 authkey 过不了"),
              !NetworkProfile::validate(QStringLiteral("tsnet"), bare, &err));
        check(QStringLiteral("A19  tsnet 档有 authkey 就能过"),
              NetworkProfile::validate(QStringLiteral("tsnet"), cfg, &err));

        check(QStringLiteral("A20 cloudflare 档缺地址过不了"),
              !NetworkProfile::validate(QStringLiteral("cloudflare"), bare, &err));
        check(QStringLiteral("A21  而且报错文案说的是「填地址」"),
              err.contains(QStringLiteral("地址")));
        check(QStringLiteral("A22 cloudflare 档有地址就能过"),
              NetworkProfile::validate(QStringLiteral("cloudflare"), cfg, &err));

        AppConfig quoted;
        quoted.setCustomRendezvous(QStringLiteral("hbbs.example.com'x"));
        check(QStringLiteral("A23 地址里有单引号过不了（会写坏 TOML 行）"),
              !NetworkProfile::validate(QStringLiteral("custom"), quoted, &err));
        AppConfig bs;
        bs.setRelayServer(QStringLiteral("hbbr\\x"));
        check(QStringLiteral("A24 地址里有反斜杠过不了"),
              !NetworkProfile::validate(QStringLiteral("custom"), bs, &err));

        check(QStringLiteral("A25 四档都有引导文案，且都不短"),
              NetworkProfile::guide(QStringLiteral("official")).size() > 40
                  && NetworkProfile::guide(QStringLiteral("tsnet")).size() > 40
                  && NetworkProfile::guide(QStringLiteral("cloudflare")).size() > 40
                  && NetworkProfile::guide(QStringLiteral("custom")).size() > 40);
        check(QStringLiteral("A26 tsnet 引导里点出了 ephemeral 那个坑"),
              NetworkProfile::guide(QStringLiteral("tsnet")).contains(QStringLiteral("ephemeral")));
        check(QStringLiteral("A27 cloudflare 引导里点出了 apac 那个坑"),
              NetworkProfile::guide(QStringLiteral("cloudflare")).contains(QStringLiteral("apac")));
    }

    // ---- B 段：真写盘 + 事务语义 ----
    const QString optPath = RustDeskController::optionConfigPath();
    const QByteArray optBackup = readWholeFile(optPath).toUtf8();

    // 只在 [options] 表内、**键名精确相等**的匹配。不复用 optionInOptionsTable ——
    // 那个用 startsWith，查 "key" 会误匹配 "keyboard-xxx" 之类的键。
    auto optEquals = [](const QString& content, const QString& key, const QString& value) {
        bool inOpt = false;
        for (const QString& raw : content.split(QLatin1Char('\n'))) {
            const QString t = raw.trimmed();
            if (t.startsWith(QLatin1Char('['))) {
                inOpt = (t == QStringLiteral("[options]"));
                continue;
            }
            if (!inOpt)
                continue;
            const int eq = t.indexOf(QLatin1Char('='));
            if (eq < 0 || t.left(eq).trimmed() != key)
                continue;
            const QString rhs = t.mid(eq + 1).trimmed();
            if (rhs == QStringLiteral("'%1'").arg(value) || rhs == QStringLiteral("\"%1\"").arg(value))
                return true;
        }
        return false;
    };
    // "这个键不见了"。必须精确比键名：写空值、写成别的表的键、键名只前缀相同，
    // 都不算干净 —— 而"切档后残留旧服务器地址"正是这套机制最容易犯的错。
    auto keyGone = [](const QString& content, const QString& key) {
        bool inOpt = false;
        for (const QString& raw : content.split(QLatin1Char('\n'))) {
            const QString t = raw.trimmed();
            if (t.startsWith(QLatin1Char('['))) {
                inOpt = (t == QStringLiteral("[options]"));
                continue;
            }
            if (!inOpt)
                continue;
            const int eq = t.indexOf(QLatin1Char('='));
            if (eq < 0)
                continue;
            if (t.left(eq).trimmed() == key)
                return false;
        }
        return true;
    };

    RustDeskController rd;
    {
        AppConfig diskCfg;
        diskCfg.load();
        rd.setExePath(diskCfg.rustDeskPath());
    }

    if (optBackup.isEmpty()) {
        report << QStringLiteral("  SKIP 读不到 RustDesk2.toml —— B 段（真写盘 / 事务语义）没跑");
    } else if (rd.isRunning()) {
        report << QStringLiteral("  SKIP RustDesk 正在运行，跳过写盘部分（写进去会被进程回写覆盖）");
    } else {
        QString err;

        // B1-B4：切到 custom 档，逐键复验
        check(QStringLiteral("B1  custom 档写盘成功"),
              NetworkProfile::apply(QStringLiteral("custom"), cfg, &rd, &err));
        QStringList mm = NetworkProfile::mismatches(QStringLiteral("custom"), cfg, &rd);
        check(QStringLiteral("B2  写完的自检完全一致（0 处不符）"), mm.isEmpty());
        if (!mm.isEmpty())
            report << QStringLiteral("         %1").arg(mm.join(QStringLiteral("\n         ")));
        const QString optText1 = readWholeFile(optPath);
        check(QStringLiteral("B3  四个服务器键确实写在 [options] 表里"),
              optEquals(optText1, QStringLiteral("custom-rendezvous-server"), QStringLiteral("hbbs.example.com"))
                  && optEquals(optText1, QStringLiteral("relay-server"), QStringLiteral("hbbr.example.com:21117"))
                  && optEquals(optText1, QStringLiteral("api-server"), QStringLiteral("api.example.com"))
                  && optEquals(optText1, QStringLiteral("key"), QStringLiteral("AbCdEf0123456789")));
        check(QStringLiteral("B4  custom 档 allow-websocket 是**删掉**而不是写空串"),
              keyGone(optText1, QStringLiteral("allow-websocket"))
                  && NetworkProfile::mismatches(QStringLiteral("custom"), cfg, &rd).isEmpty());
        // 行尾：一次切档不该把整份 RustDesk2.toml 变成 CRLF（QIODevice::Text 的坑，见 PeerPrefs.cpp）
        check(QStringLiteral("B4b 写完后行尾仍是 LF（没写成 CRLF）"),
              !optText1.contains(QLatin1Char('\r')));

        // B4c-B4d 反向：原文是 CRLF（别的工具/别的编辑器写的）时，改完必须还是清一色 CRLF。
        // 钉的是"读的时候剥掉 \r、写的时候按原风格加回来"这套约定 —— 只要做了**一半**
        // （比如写盘去掉了 QIODevice::Text 而读的时候还留着），这里立刻红：
        // 那种半拉子状态下读进来的行没有 \r，拼回去全成了 LF，而任何用带 Text 的读函数
        // 做的比较**都看不见**这个改动。跑完立刻把 LF 原文写回，后面的断言照旧按 LF 走。
        {
            const QString lfText = readWholeFile(optPath);
            QFile         crlf(optPath);
            if (crlf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                crlf.write(lfText.toUtf8().replace("\n", "\r\n"));
                crlf.close();
            }
            const bool        okC = NetworkProfile::apply(QStringLiteral("custom"), cfg, &rd, &err);
            const QByteArray  out = readWholeFile(optPath).toUtf8();
            check(QStringLiteral("B4c 原文是 CRLF 时，写完仍是清一色 CRLF（没混行尾）"),
                  okC && !out.isEmpty() && out.count('\n') == out.count("\r\n"));

            QFile back(optPath);
            if (back.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                back.write(lfText.toUtf8());
                back.close();
            }
            check(QStringLiteral("B4d CRLF 试验之后原文逐字节写回"), readWholeFile(optPath) == lfText);
        }

        // B5-B7：切到 tsnet 档 —— 服务器键必须**全部消失**，这是最容易漏的一步
        check(QStringLiteral("B5  tsnet 档写盘成功"),
              NetworkProfile::apply(QStringLiteral("tsnet"), cfg, &rd, &err));
        const QString optText2 = readWholeFile(optPath);
        check(QStringLiteral("B6  tsnet 档挂上了 direct-server / direct-access-port"),
              optEquals(optText2, QStringLiteral("direct-server"), QStringLiteral("Y"))
                  && optEquals(optText2, QStringLiteral("direct-access-port"), QStringLiteral("21118")));
        check(QStringLiteral("B7  tsnet 档把四个服务器键**全清干净**（从 custom 切过来最容易残留）"),
              keyGone(optText2, QStringLiteral("custom-rendezvous-server"))
                  && keyGone(optText2, QStringLiteral("relay-server"))
                  && keyGone(optText2, QStringLiteral("api-server"))
                  && keyGone(optText2, QStringLiteral("key")));

        // B8-B9：切回官方档 = 全部清空
        check(QStringLiteral("B8  官方档写盘成功"),
              NetworkProfile::apply(QStringLiteral("official"), cfg, &rd, &err));
        const QString optText3 = readWholeFile(optPath);
        bool allGone = true;
        for (const QString& k : NetworkProfile::managedKeys()) {
            if (!keyGone(optText3, k))
                allGone = false;
        }
        check(QStringLiteral("B9  官方档之后七个键一个都不剩（真的回落默认了）"), allGone);
        check(QStringLiteral("B10 官方档自检也一致"), NetworkProfile::mismatches(
                  QStringLiteral("official"), cfg, &rd).isEmpty());

        // B11-B12：**事务语义** —— 这一组失败就说明"要么全写要么不写"没做到
        const QByteArray before = readWholeFile(optPath).toUtf8();
        const bool badValueRet = rd.setServerProfile(
            { RustDeskController::OptionEdit{ QStringLiteral("custom-rendezvous-server"),
                                              QStringLiteral("hbbs.example.com") },
              RustDeskController::OptionEdit{ QStringLiteral("relay-server"),
                                              QStringLiteral("bad'value") } },
            &err);
        check(QStringLiteral("B11 一组里有一个坏值 → 整组返回失败"), !badValueRet);
        check(QStringLiteral("B12 而且**一个字都没写**（好的那个也没写进去）"),
              readWholeFile(optPath).toUtf8() == before);

        check(QStringLiteral("B13 失败后没有留下 .bak 之外的残渣（钥匙还是干净的原样）"),
              keyGone(readWholeFile(optPath), QStringLiteral("custom-rendezvous-server")));

        // B14：validate 不过的档位，apply 必须连门都不进
        AppConfig noAddr;
        check(QStringLiteral("B14 validate 不过时 apply 直接拒绝，不碰文件"),
              !NetworkProfile::apply(QStringLiteral("cloudflare"), noAddr, &rd, &err)
                  && readWholeFile(optPath).toUtf8() == before);
    }

    report << QStringLiteral("──── %1（%2 项失败）────")
                  .arg(fails == 0 ? QStringLiteral("全绿") : QStringLiteral("有失败"))
                  .arg(fails);

    // 还原。B 段没跑（SKIP）时 before 没定义，统一用备份还原。
    if (!optBackup.isEmpty())
        writeWholeFile(optPath, optBackup);

    const QString text = report.join(QLatin1Char('\n')) + QLatin1Char('\n');
    QFile out(QDir::temp().filePath(QStringLiteral("puremote-proftest.txt")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(text.toUtf8());
        out.close();
    }
    std::printf("%s", text.toUtf8().constData());
    std::fflush(stdout);
    return fails == 0 ? 0 : 1;
}

// --uitest：界面那几块"纯数据"的体检 —— 样式表、图文教程正文、横幅文案。
//
// **不起窗口**。所以"面板类 bug 只有真实渲染才看得见"那条老规矩在这里不适用：
// ── --autostarttest：开机自启这条**唯一的系统写入**路径的真机验证 ──────────────
//
// 为什么单独一个开关、不进 scripts/verify.sh：
//   那 17 项的前提是「零系统写入」（第 4 步专门钉这一条）。这里是全项目**唯一**会写
//   系统的代码，混进去就把那个前提毁了 —— 所以它只能手动跑，不进默认清单。
//
// ⚠ 它只动 `HKCU\...\CurrentVersion\Run` 里 `PureRemote` 这一个值，而且**必定还原**：
//   开头记住"本来有没有这条"，不管中间怎么失败，退出前都按原状写回去 ——
//   用户真的开着自启的时候跑它，也不该被它关掉。
//
// ⚠ 复验**故意不走 QSettings**：reg query 是另一个进程、另一套代码读的。
//   用同一个 API 复验自己，是这个项目明确踩过的坑（错两次会互相抵消）。
static int runAutostartTest()
{
    attachParentConsole();

    QStringList report;
    int         fails = 0;
    auto        check = [&fails, &report](const QString& name, bool ok) {
        report << QStringLiteral("  [%1] %2").arg(ok ? QStringLiteral("PASS")
                                                     : QStringLiteral("FAIL"), name);
        if (!ok)
            ++fails;
    };

    report << QStringLiteral("──── --autostarttest：真读写 HKCU 的 Run 键 ────");

    const QString exe = QCoreApplication::applicationFilePath();
    const QString key = Autostart::runKeyPath();
    const QString nm  = Autostart::valueName();

    // 原状 —— 跑完一字不差还回去。
    const bool    hadBefore = Autostart::isEnabled();
    const QString valBefore = Autostart::storedRunValue();

    // 独立视角：另起进程用 reg 读，证明值**真的落在 Run 键里**。
    auto regQuery = [&key, &nm]() -> QString {
        QProcess p;
        p.start(QStringLiteral("reg"),
                QStringList{ QStringLiteral("query"), key, QStringLiteral("/v"), nm });
        p.waitForFinished(5000);
        return QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
    };
    // 直接写原始串：只给"还原"和"伪造漂移"用，正式路径一律走 enable/disable。
    auto writeRaw = [&key, &nm](const QString& v) {
        QSettings s(key, QSettings::NativeFormat);
        s.setValue(nm, v);
        s.sync();
    };

    // ---- 干净起点 ----
    {
        QString e;
        check(QStringLiteral("T1 disable() 返回成功（先清干净）"), Autostart::disable(&e)
                                                                   && e.isEmpty());
    }
    check(QStringLiteral("T2 清完之后 reg query 读不到这个值"), regQuery().isEmpty());

    // ---- 开 ----
    QString enErr;
    const bool enOk = Autostart::enable(exe, &enErr);
    check(QStringLiteral("T3 enable(当前 exe) 返回成功"), enOk);
    if (!enErr.isEmpty())
        report << QStringLiteral("       ⚠ enable 报错：%1").arg(enErr);

    const QString out = regQuery();
    if (!out.isEmpty())
        report << QStringLiteral("       reg query 读回：%1")
                      .arg(out.section(QLatin1Char('\n'), 1).trimmed());
    check(QStringLiteral("T4 值确实出现在 Run 键里（reg 另一个进程看得见）"), !out.isEmpty());
    check(QStringLiteral("T5 值里带 --tray —— 不带的话开机要弹窗，等于白开"), enOk
              && out.contains(QStringLiteral("--tray")));

    // 从 reg 的原始输出里把值抠出来（`... REG_SZ <值>`），再按**值自己的**解析走一遍。
    // 这一步是"注册表里那串字符 → 启动命令"的最后一段，只有真值能验。
    const QString rawVal = enOk ? Autostart::storedRunValue() : QString();
    const bool pathOk = Autostart::sameExePath(Autostart::exeFromRunValue(rawVal), exe);
    check(QStringLiteral("T6 从值里解析出的 exe 就是当前这个程序"), pathOk);
    if (!pathOk)
        report << QStringLiteral("       ⚠ 值=%1\n          期望 exe=%2").arg(rawVal, exe);

    check(QStringLiteral("T7 isEnabled() 与注册表一致（开关不会显示成假的状态）"),
          Autostart::isEnabled());
    check(QStringLiteral("T8 刚写的这条不该被判成「要修」"), !Autostart::needsRepair(rawVal, exe));

    // ---- 路径漂移：绿色软件被挪目录之后的那个场景 ----
    writeRaw(Autostart::runValue(QStringLiteral("C:\\Old\\PureRemote.exe")));
    const QString drifted = Autostart::storedRunValue();
    check(QStringLiteral("T9 值指向别的目录 → needsRepair 判真（开机自愈的触发条件）"),
          Autostart::needsRepair(drifted, exe));
    {
        QString e;
        const bool ok = Autostart::enable(exe, &e);   // 自愈就是再 enable 一次
        check(QStringLiteral("T10 自愈之后路径回到当前 exe，且不再判「要修」"),
              ok && !Autostart::needsRepair(Autostart::storedRunValue(), exe));
    }
    // 少了 --tray 也要判「要修」—— 那种值开机能起来，但会弹窗口，不是静默待连。
    writeRaw(QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(exe)));
    check(QStringLiteral("T11 值里没有 --tray → 也判「要修」"),
          Autostart::needsRepair(Autostart::storedRunValue(), exe));

    // ---- 关 ----
    {
        QString e;
        check(QStringLiteral("T12 disable() 返回成功"), Autostart::disable(&e) && e.isEmpty());
    }
    check(QStringLiteral("T13 关掉之后 reg query 又读不到了"), regQuery().isEmpty());
    check(QStringLiteral("T14 关掉之后 isEnabled() 为假"), !Autostart::isEnabled());

    // ---- 还原（无论上面绿红，都得走到这）----
    if (hadBefore)
        writeRaw(valBefore);
    else {
        QString e;
        Autostart::disable(&e);
    }
    const bool restored = (Autostart::isEnabled() == hadBefore)
                          && (!hadBefore || Autostart::storedRunValue() == valBefore);
    check(QStringLiteral("T15 退出前恢复原状（跑测试不许改用户的启动项）"), restored);
    report << QStringLiteral("       还原：本来%1 → 现在%2")
                  .arg(hadBefore ? QStringLiteral("开着（已按原值写回）") : QStringLiteral("关着"),
                       Autostart::isEnabled() ? QStringLiteral("开着") : QStringLiteral("关着"));

    report << QString();
    report << (fails == 0 ? QStringLiteral("结果：全绿")
                          : QStringLiteral("结果：%1 项红").arg(fails));

    // 控制台在 Git Bash / 重定向下不一定挂得上，所以同时写文件供脚本读（跟别的测试一致）
    const QString text = report.join(QLatin1Char('\n')) + QLatin1Char('\n');
    QFile outF(QDir::temp().filePath(QStringLiteral("puremote-autostarttest.txt")));
    if (outF.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        outF.write(text.toUtf8());
        outF.close();
    }
    std::printf("%s", text.toUtf8().constData());
    std::fflush(stdout);
    return fails == 0 ? 0 : 1;
}

// 这一段管的是"字和资源对不对"（链接域名、图片有没有进 qrc、占位符有没有漏参数），
// 至于长什么样，交给三张 --screenshot 图人眼看。
//
// 之所以值得单独一段：这几类错的共同点是**静默**。链接写错域名，界面照常打开；
// 图片没进 qrc，QTextBrowser 只是那一块空着；样式表漏一个参数，只是某个控件颜色不对。
// 不测就得等用户点开教程页才发现。
static int runUiTest()
{
    attachParentConsole();

    // 这一段**固定跑中文**：B 段（横幅文案）和 G 段（图文教程）都是靠 grep 中文句子
    // 判 PASS 的。--lang en 是给截图用的，不该被带进来 —— 带进来就是整片红，而且看起来
    // 像"功能坏了"。英文那半边由下面的 I 段自己 flip 过去验，验完还原。
    I18n::setLang(I18n::Lang::Zh);

    QStringList report;
    int         fails = 0;
    auto        check = [&fails, &report](const QString& name, bool ok) {
        report << QStringLiteral("  [%1] %2").arg(ok ? QStringLiteral("PASS")
                                                     : QStringLiteral("FAIL"), name);
        if (!ok)
            ++fails;
    };

    report << QStringLiteral("──── --uitest：样式表 / 图文教程 / 横幅文案 ────");

    // ---- U 段：全局样式表 ----
    {
        const QString qss = DarkTheme::styleSheet();
        check(QStringLiteral("U1  样式表非空"), !qss.isEmpty());
        check(QStringLiteral("U2  四个新控件都有规则（QGroupBox/QDialog/QPushButton/QTextBrowser）"),
              qss.contains(QStringLiteral("QGroupBox"))
                  && qss.contains(QStringLiteral("QDialog"))
                  && qss.contains(QStringLiteral("QPushButton"))
                  && qss.contains(QStringLiteral("QTextBrowser")));

        // ⚠ 这一条是拦**回头路**的，不是拦手滑。QString 的多参数 arg() 是**按标记在串里
        //    出现的先后**发实参的，不按编号配对 —— 表里少一个编号（比如没有 %4），
        //    后面全体前移一位。历史上就是这么把 `background:%5`（本意 kDarkBg）
        //    发成了 kTextSec，#999999 的灰底铺满输入框/列表。只要还写占位符，就还有这个坑。
        const QRegularExpression marker(QStringLiteral("%[0-9]"));
        check(QStringLiteral("U3  输出里没有残留的 %N 占位符（.arg() 那个坑）"),
              !marker.match(qss).hasMatch());

        // 输入类控件的底色必须都是深色。数一下就知道有没有哪条被漏掉 ——
        // 上面那个坑犯的时候，这些规则还在，只是颜色被发错了，所以按"出现过几次"数。
        const int dark = qss.count(QStringLiteral("background:#1a1a1a"));
        check(QStringLiteral("U4  至少 5 处输入类控件底色=#1a1a1a（实得 %1）").arg(dark), dark >= 5);

        // ⚠ 这条拦的是**静默退化成系统浅色**。全项目以前没有 QTabWidget/QListWidget#guideToc，
        //    不给它们上色的话 Qt 会退回 Fusion 默认画法 —— 在深色板上是一块浅灰，而且
        //    **不报错、界面也"能用"**，跟 QDialogButtonBox 那次一模一样。所以钉住有规则。
        check(QStringLiteral("U5  标签页和教程目录都有深色规则（不会退回系统浅色）"),
              qss.contains(QStringLiteral("QTabBar::tab"))
                  && qss.contains(QStringLiteral("QTabWidget::pane"))
                  && qss.contains(QStringLiteral("QListWidget#guideToc")));
    }

    // ---- G 段：图文教程 ----
    {
        const QStringList topics = Guide::topics();
        check(QStringLiteral("G1  主题不少于 4 个"), topics.size() >= 4);

        bool        allTitled = true;
        bool        allNonEmpty = true;
        bool        allHttps = true;
        bool        allImgsOk = true;
        QStringList badHrefs;
        QStringList badImgs;

        const QRegularExpression hrefRe(QStringLiteral("href=['\"]([^'\"]+)['\"]"));
        const QRegularExpression srcRe(QStringLiteral("src=['\"]([^'\"]+)['\"]"));

        for (const QString& k : topics) {
            if (Guide::topicTitle(k).isEmpty())
                allTitled = false;

            const QString doc = Guide::html(k);
            if (doc.isEmpty()) {
                allNonEmpty = false;
                continue;
            }

            for (auto it = hrefRe.globalMatch(doc); it.hasNext();) {
                const QString u = it.next().captured(1);
                if (!u.startsWith(QStringLiteral("https://"))) {
                    allHttps = false;
                    badHrefs << u;
                }
            }
            // 图片必须真的在 exe 里（rcc 打进去了）。这一条拦的是两种静默失败：
            // 画了图但 app.qrc 忘了列（那种 rcc 会直接构建失败，拦得住），
            // 以及列了图但这里把名字写错（那种只有运行时表现为"图片框空着"）。
            for (auto it = srcRe.globalMatch(doc); it.hasNext();) {
                const QString p = it.next().captured(1);
                QFile f(p);
                if (!f.exists() || f.size() <= 0) {
                    allImgsOk = false;
                    badImgs << p;
                }
            }
        }

        check(QStringLiteral("G2  每个主题都有标题且正文非空"), allTitled && allNonEmpty);
        check(QStringLiteral("G3  所有外链都是 https（坏链：%1）")
                  .arg(badHrefs.isEmpty() ? QStringLiteral("无") : badHrefs.join(QStringLiteral(", "))),
              allHttps);
        check(QStringLiteral("G4  所有图片都在 qrc 里且非空（坏图：%1）")
                  .arg(badImgs.isEmpty() ? QStringLiteral("无") : badImgs.join(QStringLiteral(", "))),
              allImgsOk);

        // tsnet 那篇是**唯一**一篇用户照着做能踩坑的，逐条点名。
        const QString tsnet = Guide::html(QStringLiteral("tsnet"));
        const QStringList must = {
            QStringLiteral("https://login.tailscale.com/start"),
            QStringLiteral("https://login.tailscale.com/admin/settings/keys"),
            QStringLiteral("https://tailscale.com/pricing"),
            QStringLiteral("https://tailscale.com/kb/1244/tsnet"),
            QStringLiteral("https://tailscale.com/kb/1085/auth-keys"),
        };
        for (const QString& u : must)
            check(QStringLiteral("G5  tsnet 教程里有 %1").arg(u), tsnet.contains(u));

        check(QStringLiteral("G6  tsnet 教程点明了 reusable"), tsnet.contains(QStringLiteral("Reusable")));
        check(QStringLiteral("G7  tsnet 教程点明了 ephemeral 的后果（节点被删/地址会变）"),
              tsnet.contains(QStringLiteral("Ephemeral"))
                  && tsnet.contains(QStringLiteral("删掉")));
        check(QStringLiteral("G8  tsnet 教程点明了过期时间选不限制"),
              tsnet.contains(QStringLiteral("过期")) && tsnet.contains(QStringLiteral("No expiry")));
        check(QStringLiteral("G9  每张图下面都写了「示意图」"),
              tsnet.contains(QStringLiteral("示意图"))
                  && Guide::html(QStringLiteral("official")).contains(QStringLiteral("示意图")));

        // 主题 → 档位的映射：认不出来要给 official，不能给空串（空串会让下拉框选不中）
        check(QStringLiteral("G10 四个档位都能映射到一个存在的主题，未知档回落 official"),
              Guide::topicForNetMode(QStringLiteral("tsnet")) == QStringLiteral("tsnet")
                  && Guide::topicForNetMode(QStringLiteral("custom")) == QStringLiteral("custom")
                  && Guide::topicForNetMode(QStringLiteral("没这个档")) == QStringLiteral("official"));
    }

    // ---- B 段：横幅文案 / 第一个可补项（SelfCheck 的两个新纯函数）----
    {
        const auto mk = [](bool rd, bool id, bool pw, bool relay, bool key, bool host) {
            SelfCheck::Inputs in;
            in.rustdeskPresent  = rd;
            in.hasId            = id;
            in.fixedPasswordSet = pw;
            in.relayPresent     = relay;
            in.tsnetAuthKeySet  = key;
            in.tsnetHostnameSet = host;
            return in;
        };

        // 全绿 —— 横幅必须**整条消失**（返回空串），而不是显示"一切正常"。
        // 常年挂一行"正常"就是新的噪音，而"复杂"正是这次要治的病。
        const auto full = SelfCheck::evaluate(mk(true, true, true, true, true, true));
        check(QStringLiteral("B1  六项全齐时 bannerText 为空串（横幅消失）"),
              SelfCheck::bannerText(full, true).isEmpty());
        check(QStringLiteral("B2  六项全齐时 firstFixableAction 为空串"),
              SelfCheck::firstFixableAction(full, true).isEmpty());

        // ★ 回归点：**地址簿空着不该算缺项**。
        // 连接框现在是"填码直接连"，一条联系人都没有照样能连上任何一台机器 ——
        // 留着这条等于在主界面常年挂一条"还差 1 项"，而那一项其实什么都不缺。
        // 单独立一条断言钉住它：把 has_contact 加回去，这里立刻红。
        const auto noBook = SelfCheck::evaluate(mk(true, true, true, true, true, true));
        check(QStringLiteral("B3  地址簿空着不算缺项（横幅照样消失）"),
              SelfCheck::bannerText(noBook, false).isEmpty()
                  && SelfCheck::missingCount(noBook, false) == 0);
        check(QStringLiteral("B4  准备情况里根本不该有 has_contact 这一项"),
              SelfCheck::firstFixableAction(noBook, false).isEmpty());

        // 官方档：tsnet 那三项不算数，缺着也不算"没准备好"
        const auto official = SelfCheck::evaluate(mk(true, true, true, false, false, false));
        check(QStringLiteral("B5  官方档不计 tsnetOnly 三项 → 横幅消失"),
              SelfCheck::bannerText(official, false).isEmpty());
        // 同一个状态切到 tailnet 档：那三项就都算数了（relay 也没装 → 共 3 项）
        check(QStringLiteral("B6  同一状态切 tailnet 档 → 报 3 项"),
              SelfCheck::bannerText(official, true).contains(QStringLiteral("还差 3 项")));

        // 缺一堆的时候横幅不能把 6 个名字全列出来
        const auto empty = SelfCheck::evaluate(mk(false, false, false, false, false, false));
        const QString t = SelfCheck::bannerText(empty, true);
        check(QStringLiteral("B7  缺 6 项时文案截断成 3 个 + 「等」"),
              t.startsWith(QStringLiteral("还差 6 项")) && t.endsWith(QStringLiteral(" 等")));

        // 缺的第一项**不可修**（内核没装）→ 不该指引用户去点设置，而该去看教程
        check(QStringLiteral("B8  第一个缺项不可修（缺内核）→ 返回空串（调用方改开教程）"),
              SelfCheck::firstFixableAction(empty, true).isEmpty());
        // 内核有了、ID 没有 → 第一个可补的是 has_id
        const auto noId = SelfCheck::evaluate(mk(true, false, false, false, false, false));
        check(QStringLiteral("B9  内核在、ID 没读到 → 第一个可补项是 has_id"),
              SelfCheck::firstFixableAction(noId, false) == QStringLiteral("has_id"));
        // 顺序契约：可补项要按 evaluate() 的顺序取第一个，不是随便挑一个
        // （has_id 排在 fixed_password 前面，两个都缺时必须先给 has_id）
        const auto noIdNoPw = SelfCheck::evaluate(mk(true, false, false, true, true, true));
        check(QStringLiteral("B10 多项可补时取 evaluate() 顺序里的第一个（has_id 先于 fixed_password）"),
              SelfCheck::firstFixableAction(noIdNoPw, false) == QStringLiteral("has_id")
                  && SelfCheck::missingCount(noIdNoPw, false) >= 2);
    }

    // ---- I 段：翻译表 + 英文档的图文教程 ----
    //
    // 这一段拦的是**静默的漏翻**。t() 找不到键就退回中文，这在界面上完全看不出来：
    // 中文用户永远不会发现问题，英文用户看到一句中文，而我们自己那张英文截图恰好没
    // 截到那一屏 —— 于是能挂很久。
    //
    // 跟 scripts/check_i18n.py 的分工：这边（运行时）强项是"表本身自洽"——每条都有
    // 译文、译文里没有漏翻的汉字、zh 档恒等；那边（静态扫）强项是"源码里 TR("…") 有没有
    // 漏进表"，因为**没被调用到的键运行时看不见**。两边互补，都得有。
    //
    // ⚠ 结尾必须把语言**还原**。--uitest 全程跑中文是硬约定（B 段/G 段都在 grep 中文
    //   句子），这里 flip 到 En 不还原的话，后面的段会跟着红 —— 而且是"看着像功能坏了"的红。
    {
        const I18n::Lang before = I18n::lang();
        const auto hasCjk = [](const QString& s) {
            for (const QChar& ch : s) {
                const ushort u = ch.unicode();
                if ((u >= 0x3000 && u <= 0x303F)      // 全角标点
                    || (u >= 0x4E00 && u <= 0x9FFF)   // 汉字
                    || (u >= 0xFF00 && u <= 0xFFEF))  // 全角字母数字
                    return true;
            }
            return false;
        };

        const QStringList keys = I18n::keys();
        check(QStringLiteral("I1  翻译表够大且 keys() 与 entryCount() 一致（%1 条）")
                  .arg(I18n::entryCount()),
              I18n::entryCount() >= 300 && keys.size() == I18n::entryCount());

        int         emptyKey = 0;
        QStringList noEn;
        QStringList cjkEn;
        for (const QString& k : keys) {
            if (k.isEmpty())
                ++emptyKey;
            const QString en = I18n::englishFor(k);
            if (en.isEmpty())
                noEn << k;
            else if (hasCjk(en))
                cjkEn << k;
        }
        check(QStringLiteral("I2  没有空键（%1 个）").arg(emptyKey), emptyKey == 0);
        check(QStringLiteral("I3  每条都有英文（缺：%1）")
                  .arg(noEn.isEmpty() ? QStringLiteral("无")
                                      : noEn.mid(0, 3).join(QStringLiteral(" / "))),
              noEn.isEmpty());
        // 英文里还留着汉字 = 复制粘贴时忘了翻。这是漏翻最典型的形态。
        check(QStringLiteral("I4  英文译文里没有残留汉字（漏翻：%1）")
                  .arg(cjkEn.isEmpty() ? QStringLiteral("无")
                                       : cjkEn.mid(0, 3).join(QStringLiteral(" / "))),
              cjkEn.isEmpty());

        // zh 档恒等 —— 整批老断言"字节不变"的地基。它要是塌了，上面 B 段/G 段会跟着
        // 全红，那时候已经看不出根因了；这里单独钉一条，让原因一目了然。
        I18n::setLang(I18n::Lang::Zh);
        int notIdentity = 0;
        for (const QString& k : keys)
            if (I18n::t(k) != k)
                ++notIdentity;
        check(QStringLiteral("I5  zh 档下 t() 是恒等函数（%1 条不符）").arg(notIdentity),
              notIdentity == 0);

        // en 档：表里有的键必须真的换成不含汉字的英文；表里没有的必须**退回中文**，
        // 不是空串 —— 空串会把界面变成一片空白，比显示中文难查得多。
        I18n::setLang(I18n::Lang::En);
        int badEn = 0;
        for (const QString& k : keys) {
            const QString v = I18n::t(k);
            if (v.isEmpty() || hasCjk(v))
                ++badEn;
        }
        check(QStringLiteral("I6  en 档下每条都换成不含汉字的英文（%1 条不符）").arg(badEn),
              badEn == 0);

        const QString ghost = QStringLiteral("__puremote_不存在的键__");
        check(QStringLiteral("I7  表里没有的键在 en 档下退回原文而不是空串"),
              I18n::t(ghost) == ghost);

        // 英文档的图文教程。图片这条是英语版**特有**的风险：三张英文示意图是新画的，
        // 少列一张进 qrc、或者 HTML 里忘写 -en 后缀，中文档那几个断言全都照不到
        // （中文图确实在，G4 照样绿）。
        bool        enDocsOk  = true;
        bool        enHttpsOk = true;
        bool        enImgsOk  = true;
        QStringList enBadDoc, enBadLink, enBadImg;
        const QRegularExpression eh(QStringLiteral("href=['\"]([^'\"]+)['\"]"));
        const QRegularExpression es(QStringLiteral("src=['\"]([^'\"]+)['\"]"));
        for (const QString& k : Guide::topics()) {
            const QString doc = Guide::html(k);
            if (doc.isEmpty()) {
                enDocsOk = false;
                enBadDoc << k;
                continue;
            }
            for (auto it = eh.globalMatch(doc); it.hasNext();) {
                const QString u = it.next().captured(1);
                if (!u.startsWith(QStringLiteral("https://"))) {
                    enHttpsOk = false;
                    enBadLink << u;
                }
            }
            for (auto it = es.globalMatch(doc); it.hasNext();) {
                const QString p = it.next().captured(1);
                QFile f(p);
                if (!f.exists() || f.size() <= 0) {
                    enImgsOk = false;
                    enBadImg << p;
                }
            }
        }
        check(QStringLiteral("I8  英文档每篇教程正文非空（空：%1）")
                  .arg(enBadDoc.isEmpty() ? QStringLiteral("无") : enBadDoc.join(QStringLiteral(" / "))),
              enDocsOk);
        check(QStringLiteral("I9  英文档所有外链都是 https（坏链：%1）")
                  .arg(enBadLink.isEmpty() ? QStringLiteral("无") : enBadLink.join(QStringLiteral(" / "))),
              enHttpsOk);
        check(QStringLiteral("I10 英文档所有图片都在 qrc 里且非空（坏图：%1）")
                  .arg(enBadImg.isEmpty() ? QStringLiteral("无") : enBadImg.join(QStringLiteral(" / "))),
              enImgsOk);
        // 正着再钉一条：英文档引用的必须是 **-en 那套文件名**。
        // 不钉的话"英文教程配中文图"能完整通过上面所有断言 —— 中文图确实存在、确实非空，
        // 只是图上的字是中文。这正是最容易被漏掉的那种"看着都对"。
        check(QStringLiteral("I11 英文档引用的是 -en 后缀的示意图"),
              Guide::html(QStringLiteral("official")).contains(QStringLiteral("net-modes-en.png")));

        I18n::setLang(before);
    }

    report << QString();
    report << (fails == 0 ? QStringLiteral("结果：全绿")
                          : QStringLiteral("结果：%1 项 FAIL").arg(fails));

    const QString text = report.join(QLatin1Char('\n')) + QLatin1Char('\n');
    std::printf("%s", text.toUtf8().constData());
    std::fflush(stdout);

    QFile out(QDir::temp().filePath(QStringLiteral("puremote-uitest.txt")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        out.write(text.toUtf8());

    return fails == 0 ? 0 : 1;
}

// --paneltest：**真的造一个 MainWindow**（不 show）来断言按钮使能态。
//
// 全项目只有这一段造控件，所以单开一段而不是塞进 --uitest：后者契约上写着"不起窗口"
// （见它头顶那段注释和 verify.sh 第 15 步的描述），把造窗口混进去等于把它的"全绿"
// 含义悄悄改掉。而按钮态这条线**必须**有真实控件才测得了 ——
//
//   规则（ConnectPanel::buttons）可以全对，错的是 MainWindow **喂给规则的输入**。
//   实际交付出去的那个 bug 就是这样：地址簿空着点「新建」，保存/连接该亮却全灰，
//   因为旧代码只拿"列表选没选中"一个布尔去喂规则，把"正在填一条新的"整个丢了。
//   纯函数测试永远看不见这种错。
//
// 构造 MainWindow 的开销与安全性已逐条核实：**不起子进程、不写文件、不发网络包、
// 不弹模态窗**（唯一"活"的是 30 秒一跳的界面心跳定时器，回调只读内存）。这里也
// **绝不点「保存修改」**，所以连 puremote.json 的备份/还原都不用做 ——
// 那一步会触发 AddressBook 落盘，是唯一会把测试写进真配置的路径。
static int runPanelTest(AppConfig& config)
{
    attachParentConsole();

    // 断言跟文案无关，钉住中文只是为了让报告在任何语言配置下都长得一样
    I18n::setLang(I18n::Lang::Zh);

    QStringList report;
    int         fails = 0;
    auto        check = [&fails, &report](const QString& name, bool ok) {
        report << QStringLiteral("  [%1] %2").arg(ok ? QStringLiteral("PASS")
                                                     : QStringLiteral("FAIL"), name);
        if (!ok)
            ++fails;
    };

    report << QStringLiteral("──── --paneltest：真控件按钮态（这个 bug 只有它能复现）────");

    // **在内存里**把地址簿清空 —— 不 save()。"地址簿空着"正是这次的回归路径，
    // 而写盘是测试唯一可能污染用户配置的地方。
    config.setContacts(QJsonArray());

    RustDeskController rd;
    rd.setExePath(config.rustDeskPath());
    PasswordPolicy policy(&config, &rd);
    NotifyPolicy   notify(&config, &rd);
    AddressBook    book(&config);
    book.load();
    TsnetBridge    bridge;
    ConnectSession session(&config, &rd, &bridge, &book);
    NetworkProbe   probe;
    // ⚠ **不调 probe.startPathWatch()** —— 那会去 tail 真 RustDesk 日志。
    MainWindow     win(&config, &rd, &policy, &notify, &book, &session, &probe);

    check(QStringLiteral("P1 空地址簿、没选中、没填 → 保存/连接/删除都是灰的"),
          !win.saveButtonEnabledForTest() && !win.connectButtonEnabledForTest()
              && !win.deleteButtonEnabledForTest());

    win.newContactForTest();
    check(QStringLiteral("P2 点了「新建」但还没填地址 → 保存/连接仍然是灰的（没东西可存可连）"),
          !win.saveButtonEnabledForTest() && !win.connectButtonEnabledForTest());

    // ★ 这一条就是那个回归：填了码之后**必须**能存、能连。
    //   旧代码在空地址簿下这一步依然全灰 —— 用户因此一个人都加不进来。
    win.setContactFieldsForTest(QStringLiteral("123456789"), QString(), QString());
    check(QStringLiteral("P3 新建后填了连接码 → 保存和连接都得亮（★ 回归点）"),
          win.saveButtonEnabledForTest() && win.connectButtonEnabledForTest());

    // 清空之后应该变回灰的，别把"亮"粘住
    win.setContactFieldsForTest(QString(), QString(), QString());
    check(QStringLiteral("P4 再把连接码清掉 → 保存/连接变回灰"),
          !win.saveButtonEnabledForTest() && !win.connectButtonEnabledForTest());

    // 官方档下只填 tailnet 主机名 = 档位对不上，不该给一个点了必然失败的「连接」
    win.newContactForTest();
    win.setContactFieldsForTest(QString(), QStringLiteral("peer-host"), QString());
    check(QStringLiteral("P5 官方档只填 tailnet 主机名 → 连接是灰的（档位对不上）"),
          !win.connectButtonEnabledForTest());

    // ---- 同一个输入框，换档位之后结论必须跟着翻 ----
    win.setNetModeForTest(QStringLiteral("tsnet"));
    check(QStringLiteral("P6 tsnet 档 + 填了 tailnet 主机名 → 连接亮"),
          win.connectButtonEnabledForTest());
    win.setContactFieldsForTest(QStringLiteral("123456789"), QString(), QString());
    check(QStringLiteral("P7 tsnet 档只填连接码 → 连接灰（跟 P5 对称）"),
          !win.connectButtonEnabledForTest());
    win.setNetModeForTest(QStringLiteral("official"));

    // ---- 纯函数那半边：控件路径够不着的组合 ----
    //
    // 造不出"会话进行中"的状态（那要真的连一次），但它恰恰是最容易被写漏的一格：
    // 一旦漏了，用户会在连接进行中看到一个还亮着的「连接」。
    {
        ConnectPanel::State s;
        s.bookCount     = 1;
        s.selectedRow   = 0;
        s.editorValid   = true;
        s.targetFilled  = true;
        s.sessionActive = true;
        const ConnectPanel::Buttons b = ConnectPanel::buttons(s);
        check(QStringLiteral("P8 会话进行中 → 连接灰、断开亮"),
              !b.connect && b.disconnect);

        // selectedRow 越界（列表被重建时可能短暂出现）必须当成"没选中"，
        // 否则「删除」会亮着，点下去按越界下标去删。
        ConnectPanel::State o = s;
        o.sessionActive = false;
        o.selectedRow   = 5;
        const ConnectPanel::Buttons b2 = ConnectPanel::buttons(o);
        check(QStringLiteral("P9 selectedRow 越界 → 当成没选中（删除灰、保存照旧看输入框）"),
              !b2.del && b2.save);

        check(QStringLiteral("P10 sourceFor：没填地址 → None（不发起连接，只提示）"),
              ConnectPanel::sourceFor(false, true, true) == ConnectPanel::Source::None);
        check(QStringLiteral("P11 sourceFor：选中的那条一字不差 → Saved（才记「上次连接」）"),
              ConnectPanel::sourceFor(true, true, true) == ConnectPanel::Source::Saved);
        check(QStringLiteral("P12 sourceFor：填的跟选中的不一样 → Adhoc（所见即所连）"),
              ConnectPanel::sourceFor(true, true, false) == ConnectPanel::Source::Adhoc);
        check(QStringLiteral("P13 sourceFor：压根没选 → Adhoc"),
              ConnectPanel::sourceFor(true, false, false) == ConnectPanel::Source::Adhoc);
    }

    // ══ 设置框分页 + 高度 ════════════════════════════════════════════════
    //
    // 这一段治的是"设置太复杂"里最硬的那一半：原来五组一字排开，实测 **580×1061**，
    // 而 1080p 屏去掉任务栏只剩约 1040 —— 底部的「关闭」和整组「界面语言」直接落在
    // 屏幕外面，且全程没有滚动条。这种毛病**截图看不出来**（--screenshot 只存 PNG、
    // 不做任何判断，裁到框外也照样是张好图），只能量尺寸。
    {
        check(QStringLiteral("P14 设置框分成了 3 个标签页"),
              win.settingsTabCountForTest() == 3);

        // 有空页 = 某组挂错了地方或者压根没挂。0 是真实会发生的错（漏调 addWidget），
        // 而界面上只表现为"某一页空白"，不报错。
        check(QStringLiteral("P15 每个标签页都挂到了组（没有空页）"),
              win.settingsTabMinGroupCountForTest() >= 1);

        // 一屏能放下。1080p 减任务栏 ≈ 1040，这里按 900 卡（留出标题栏和 DPI 缩放的余量）。
        const int h = win.settingsHeightForTest();
        check(QStringLiteral("P16 设置框不再比一屏高（实测 %1 px，上限 900）").arg(h),
              h > 0 && h <= 900);

        // ---- 主界面：宽度够不够 ----
        // 挪到最后单开一块，那里**真的要 show()**，别在这里量（构造期的数偏小，见 P45）。
    }

    // ══ 主界面那行「线路」 ═══════════════════════════════════════════════
    //
    // 网络档位原来只躺在设置里、标题还写着「进阶，一般不用改」，很多人不知道有得选。
    // 现在主界面顶上有一行「线路：<档位> · <直连/中继>」，这一组断言钉住
    // "档位一改它就跟着变" —— 否则用户换了档，主界面还在说旧的，比没有更误导。
    {
        const QString before = win.routeTextForTest();
        // 报告里把 <span> 去掉 —— 那是给直连/中继上色的，混在断言名里没法读。
        // 断言本身仍然打在原始富文本上（颜色是它的一部分，不该被这层过滤掉）。
        const QString shown =
            QString(before).remove(QRegularExpression(QStringLiteral("<[^>]*>")));
        check(QStringLiteral("P17 主界面有「线路」那行，且写明当前档位（%1）")
                  .arg(shown),
              before.contains(QStringLiteral("线路"))
                  && before.contains(AppConfig::netModeTitle(config.netMode())));

        win.setNetModeForTest(QStringLiteral("tsnet"));
        const QString afterTsnet = win.routeTextForTest();
        check(QStringLiteral("P18 档位换成 tailnet → 那行跟着变"),
              afterTsnet.contains(AppConfig::netModeTitle(QStringLiteral("tsnet"))));

        win.setNetModeForTest(QStringLiteral("official"));
        check(QStringLiteral("P19 换回官方 → 那行也换回来（不是单向的）"),
              win.routeTextForTest().contains(
                  AppConfig::netModeTitle(QStringLiteral("official"))));

        // 还没连过的时候必须是灰字"还没连过"，**不能瞎报一个直连/中继** ——
        // 没探到路径就编一个，用户会拿着它去排查一个不存在的问题。
        check(QStringLiteral("P20 没探到路径时那行写「还没连过」，不瞎报直连/中继"),
              before.contains(QStringLiteral("还没连过")));
    }

    // ══ 教程左侧目录 ═════════════════════════════════════════════════════
    //
    // 原来的「看哪一段」下拉一次只看得到一条，用户不知道另外三条存在。改成目录列表后
    // 四条同时摆出来，这一组钉住"四条都在、顺序跟 Guide::topics() 一致、
    // 默认落在第一条"，以及**换档位进来能落到对应那篇**（openGuideDialog 的 key→行）。
    {
        const QStringList want = Guide::topics();
        check(QStringLiteral("P21 教程目录列出全部 %1 个主题").arg(want.size()),
              win.guideTopicCountForTest() == want.size());

        bool sameOrder = true;
        for (int i = 0; i < want.size(); ++i)
            if (win.guideTopicKeyForTest(i) != want.at(i))
                sameOrder = false;
        check(QStringLiteral("P22 目录顺序 = Guide::topics()（官方在最前）"), sameOrder);

        check(QStringLiteral("P23 打开教程默认落第一篇，且正文真填上了（%1）")
                  .arg(win.guideCurrentTopicForTest()),
              win.guideCurrentTopicForTest() == want.value(0));
    }

    // ══ 本机密码那行的「复制」════════════════════════════════════════════
    //
    // 又是一类只有用户实机能发现的 bug：**连接码旁边一直有「复制」，密码旁边只有「显示」**。
    // 这种"少了一个控件"的错，任何既有断言都看不见 —— 它不是文案、不是规则，
    // 而截图只存 PNG 不做判断，少个按钮照样是张好图。
    //
    // 两条断言分别堵两个方向：
    //   ① 没明文时必须是**灰的** —— 否则点下去是个空剪贴板；更坏的是顺手把 toml 里
    //      那串密文复制出去，对方拿着根本连不上（"有值就复制"是很自然的写错法）。
    //   ② 有明文时**真拿它撞一次剪贴板** —— 断言按钮亮只证明它"该"亮，
    //      证明不了 `connect()` 那行真接上了、也没证明复制出来的是明文。
    {
        // 剪贴板是用户的私人物品：先存下来，断言完原样还回去。
        QClipboard*   cb    = QApplication::clipboard();
        const QString saved = cb->text();

        win.setLocalPasswordForTest(QString());
        check(QStringLiteral("P24 没有可复制的明文密码时，那个「复制」是灰的"),
              !win.copyPasswordEnabledForTest());

        win.setLocalPasswordForTest(QStringLiteral("Ab3dEf9h"));
        win.copyPasswordForTest();
        check(QStringLiteral("P25 设了固定密码后「复制」亮，且剪贴板里真的是那串密码"),
              win.copyPasswordEnabledForTest()
                  && cb->text() == QStringLiteral("Ab3dEf9h"));

        win.setLocalPasswordForTest(QString());   // 内存里那串测试密码也收干净
        if (saved.isEmpty())
            cb->clear();
        else
            cb->setText(saved);
    }

    // ══ 「现在没人能连进来」那条警告 ═════════════════════════════════════
    //
    // 这是"纯函数算得出缺几项、算不出我们把后果说成了什么"的那一类。用户原话是
    // 「密码未设置的时候为啥连接也要密码」—— 他看到的只是一句轻描淡写的「未设置」，
    // 而真实后果是**这台机器谁都连不进来**（RustDesk 的一次性密码命令行读不出来，
    // 界面上也看不到）。所以这条断言钉的是"后果有没有说出口 + 够不够醒目"。
    //
    // ⚠ 本机真实的 RustDesk.toml 里**是**有密码的，不伪造就永远进不了"一个都没有"
    //   这条分支 —— 于是用 setRawPasswordOverrideForTest 把 toml 那份伪装成空的。
    //   它只影响显示判断；真读那份照旧留给"写完复验"和删密码前那道确认。
    {
        win.setLocalPasswordForTest(QString());
        win.setRawPasswordOverrideForTest(QString());

        check(QStringLiteral("P26 一个密码都没有时，顶部横幅转成红色警告"),
              win.bannerIsDangerForTest());
        check(QStringLiteral("P27 横幅把那句后果说出口了（含「没人能连进来」）"),
              win.bannerTextForTest().contains(QStringLiteral("没人能连进来")));
        check(QStringLiteral("P28 「本机」密码那行的提示也是警告色"),
              win.passwordHintIsWarningForTest());

        // 反面必须有：红色常亮就等于没有红色。
        win.setLocalPasswordForTest(QStringLiteral("Zz9kQw2m"));
        check(QStringLiteral("P29 有密码之后横幅退回常态（不再是红色警告）"),
              !win.bannerIsDangerForTest() && !win.passwordHintIsWarningForTest());

        // 撤掉伪造、回真读。这里外壳明文还在，所以结果与环境无关 —— 断言是确定的。
        win.clearRawPasswordOverrideForTest();
        check(QStringLiteral("P30 撤掉伪造后按真值判断：外壳记着密码 → 依然不是警告"),
              !win.bannerIsDangerForTest());

        win.setLocalPasswordForTest(QString());   // 收干净，别留测试密码在内存里
    }

    // ══ 提示里指的那个按钮，名字当时真的存在吗 ═════════════════════════════
    //
    // 这是上次删掉用户密码那条路径上的**文案版**：有密文时提示让人去点「立即应用」，
    // 可「关闭」档下同一个按钮叫「关闭固定密码」，干的是**删密码**。用户照着提示去
    // 设置框里找「立即应用」—— 找不到，而旁边那个"关掉密码"看着最像，一按就没了。
    // 断言写成"跟着当前模式分支"而不是死认一句话，这样它在任何机器上都确定成立，
    // 而且要钉的性质正是真正重要的那条：**提示必须与按钮当前的标签一致**。
    {
        win.setRawPasswordOverrideForTest(QStringLiteral("00AXCmr8xyz"));
        const QString hint = win.passwordHintForTest();

        // 「关闭」档下这条提示仍然得指向**当时真实存在的**按钮名：那一档里按钮叫
        // 「关闭固定密码」，干的是删。绝不能提「立即应用」。反过来，非「关闭」档下
        // 那句"别点「关闭固定密码」"就是错的 —— 那个按钮那时候压根不在。
        const bool off = win.policyIsOffModeForTest();
        if (off) {
            check(QStringLiteral("P31 「关闭」档下有密文时，提示指向当时真有的按钮"
                                 "「关闭固定密码」，且不提那个档下不存在的「立即应用」"),
                  hint.contains(QStringLiteral("关闭固定密码"))
                      && !hint.contains(QStringLiteral("立即应用")));
        } else {
            // ⚠ 2026-10-10 改了：那一行上的「换成外壳能记住的新密码」按钮**删掉了**
            //   （首次运行改成自动接管，用户看不懂那个名字）。出口改成**指路到设置里
            //   本来就有的「立即应用」**，所以这里断言的是：指的那个按钮名**当时真的存在**，
            //   而且代价（替换 → 原来那个「失效」）说出口了。
            check(QStringLiteral("P31 非「关闭」档下有密文时，提示指路到设置里真有的"
                                 "「立即应用」，且把代价说出口（旧码「失效」）"),
                  hint.contains(QStringLiteral("立即应用"))
                      && hint.contains(QStringLiteral("失效"))
                      && !hint.contains(QStringLiteral("关闭固定密码")));
        }

        win.clearRawPasswordOverrideForTest();
    }

    // ══ 密码策略那一格：档位、「未应用」、剩下的那个逃生口 ════════════════════
    //
    // 这一整块存在的理由跟 P1（地址簿空着时按钮全灰）是同一类：**规则可以全绿，错的
    // 是喂给规则的输入**。needsApply / canTakeOverPassword 那些纯函数在 --poltest 里
    // 已经逐条钉过了；这里钉的是"那几处控件的显隐有没有真接上那两个判据"。
    {
        // 先把现场收干净：外壳无明文、文件里也没有，此时"档位选什么"是唯一变量。
        win.setLocalPasswordForTest(QString());
        win.setRawPasswordOverrideForTest(QString());

        check(QStringLiteral("P32 档位下拉有 5 档（含后加的「用完即换」）"),
              win.policyModeCountForTest() == 5);

        win.setPolicyModeForTest(QStringLiteral("fixed"));
        check(QStringLiteral("P33 选「固定密码」而文件里一个密码都没有 → 「未应用」亮"),
              win.notAppliedVisibleForTest());

        win.setRawPasswordOverrideForTest(QStringLiteral("Ab3dEf9h"));
        win.setLocalPasswordForTest(QStringLiteral("Ab3dEf9h"));
        win.setPolicyModeForTest(QStringLiteral("fixed"));
        check(QStringLiteral("P34 两边逐字一致（= 真落地成功）→ 「未应用」灭"),
              !win.notAppliedVisibleForTest());

        win.setPolicyModeForTest(QStringLiteral("off"));
        check(QStringLiteral("P35 选「不用固定密码」而文件里那个密码还在 → 也亮「未应用」"
                             "（界面不许替用户假装已经删了）"),
              win.notAppliedVisibleForTest());

        // 「未应用」是这个块唯一一条"界面不许说谎"的护栏，反面必须也在 —— 常亮等于没有。
        win.setRawPasswordOverrideForTest(QString());
        win.setPolicyModeForTest(QStringLiteral("off"));
        check(QStringLiteral("P36 两边确实都没有密码 → 「未应用」灭（不是常亮）"),
              !win.notAppliedVisibleForTest());

        // ⚠ P37/P38 原来钉的是「换成外壳能记住的新密码」那个按钮的显隐。按钮已删
        //   （2026-10-10：首次运行改成自动接管，那个名字用户看不懂），这一对断言随之作废。
        //   出口还在，只是换成了设置里本来就有的「立即应用」—— 那条路由上面的 P31 钉
        //   （"提示指的按钮名当时真的存在"），这里不再重复。

        win.setPolicyModeForTest(QStringLiteral("off"));
        check(QStringLiteral("P39 「不用固定密码」档下，「打开 RustDesk 看一次性密码」"
                             "必须露面 —— 这是这一档唯一的逃生口"),
              win.openRdVisibleForTest());

        win.setPolicyModeForTest(QStringLiteral("fixed"));
        check(QStringLiteral("P40 换成别的档之后那个按钮收起来（别的档有自己的固定密码）"),
              !win.openRdVisibleForTest());

        win.setPolicyModeForTest(QStringLiteral("once"));
        check(QStringLiteral("P41 「用完即换」那行说明里写的是「断开」这个时机 —— "
                             "写成「连上就换」会把刚连上的人掐断"),
              win.nextPolicyTextForTest().contains(QStringLiteral("断开")));

        // ⚠ 收干净：档位放回原值、伪造撤掉、内存里的测试密码清空。
        //   setPolicyModeForTest 只改内存不写盘，但留着它还是会污染后面任何读档位的断言。
        win.restorePolicyModeForTest();
        win.clearRawPasswordOverrideForTest();
        win.setLocalPasswordForTest(QString());
    }

    // ══ 开机自启 ═══════════════════════════════════════════════════════════
    //
    // **这一整块一个字节的注册表都不写** —— 写/删只能靠真机实测（验收第 9 项：
    // 开开关 → reg query 有值且带 --tray → 关开关 → 值消失）。这里钉的是另外两件
    // 只有真控件看得见的事：勾选框跟注册表对不对得上、以及文案有没有把限制说出口。
    {
        // 全新 AppConfig（不 load）的默认值必须是 false —— "不声不响就把启动项装上"
        // 是这一项最容易招人烦的失败方式。
        AppConfig fresh;
        check(QStringLiteral("P42 自启的默认值是关的（绝不凭空装启动项）"),
              !fresh.autostart());

        // ⚠ 不断言"现在一定是关的"：那取决于这台机器当前状态，跑一次实测就会翻，
        //   变成一条"改天自己红掉"的断言。要钉的是**接线对不对**：
        //   buildUi 有没有真去读注册表、setChecked 有没有被信号污染。
        check(QStringLiteral("P43 勾选框状态与注册表实际状态一致"
                             "（显示「开着」而注册表里没有，是最坏的失败方式）"),
              win.autostartCheckedForTest() == Autostart::isEnabled());

        // 文案必须说实话：这条走的是 HKCU 的 Run 键，**登录之后**才跑。机器设了登录
        // 密码而人不在的话，开机仍然是连不上的 —— 用户最容易误解的正是这一点。
        const QString bootHint = win.autostartHintForTest();
        check(QStringLiteral("P44 自启的说明里写明了「登录之后」（Run 键不是开机就跑）"),
              bootHint.contains(QStringLiteral("登录之后")));
    }

    // ══ 主界面宽度：会不会把最右边切掉（用户实机报的那条）═══════════════════
    //
    // 主界面在 QScrollArea 里，而横条是 ScrollBarAlwaysOff —— 内容比窗口宽**不是出
    // 滚动条，是被直接切掉**，最右边那列按钮（粘贴 / 显示 / 换成…）就落在屏幕外。
    //
    // ⚠ 两条硬要求，缺一条这断言就是自证：
    //   ① 必须**真的 show 一次**再去量。窗口宽度是靠 LayoutRequest 撑起来的，没显示过的
    //      窗口收不到那个事件 —— 构造期量到的数偏小（实测英文档 588 vs 真渲染 765），
    //      拿它跟窗口宽比当然永远"够"。
    //   ② 英文档必须**另开一个窗口**量。语言是构造时 TR() 求值烤进控件里的，中途
    //      setLang() 不会重译已有控件。**原先这一项只量中文** —— 而中文恰好塞得下，
    //      这个 bug 正是这么溜过去的（英文那行「Replace it with one shell can remember」
    //      比中文长一大截）。
    {
        auto measure = [&](MainWindow& w, const QString& tag) {
            w.show();
            QApplication::processEvents();
            const int need = w.mainContentMinWidthForTest();
            const int have = w.windowWidthForTest();
            check(QStringLiteral("%1（内容至少需要 %2 px，窗口 %3 px）"
                                 "—— 不够时最右边那列按钮会被切掉")
                      .arg(tag).arg(need).arg(have),
                  need > 0 && need <= have);
            report << QStringLiteral("        各组最小宽：%1")
                          .arg(w.mainWidthBreakdownForTest());
            w.hide();
        };

        measure(win, QStringLiteral("P45 中文档主界面不用横向滚动"));

        I18n::setLang(I18n::Lang::En);
        {
            // 跟 win 共用同一批控制器（不新起任何进程、不写盘），只是语言不同。
            MainWindow en(&config, &rd, &policy, &notify, &book, &session, &probe);
            measure(en, QStringLiteral("P46 英文档主界面不用横向滚动"));
        }
        I18n::setLang(I18n::Lang::Zh);
    }

    report << QString();
    report << (fails == 0 ? QStringLiteral("结果：全绿")
                          : QStringLiteral("结果：%1 项 FAIL").arg(fails));

    const QString text = report.join(QLatin1Char('\n')) + QLatin1Char('\n');
    std::printf("%s", text.toUtf8().constData());
    std::fflush(stdout);

    QFile out(QDir::temp().filePath(QStringLiteral("puremote-paneltest.txt")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        out.write(text.toUtf8());

    return fails == 0 ? 0 : 1;
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("PureRemote"));
    QApplication::setApplicationName(QStringLiteral("PureRemote"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/icon.ico")));

    const QStringList args = app.arguments();

    AppConfig config;
    config.load();

    // --lang zh|en：验收专用的显式语言开关（不写盘）。**在这里就先认掉**，理由见后面
    // 「界面语言」那段的注释 ②：这样 `--uitest --lang en` 能真的跑在英文表上，而普通的
    // `--uitest`（哪怕用户配置里存着 en）仍然是中文，grep 中文句子的断言才不会集体失效。
    {
        const QString forced = argValue(args, QStringLiteral("--lang"));
        if (!forced.isEmpty())
            I18n::setLang(I18n::fromCode(forced));
    }

    // 这几个探针放在单例检查之前：它们只读 IPC / 只截图，不干扰已在跑的实例
    if (args.contains(QStringLiteral("--print-id")))
        return printIdAndExit(config);

    if (args.contains(QStringLiteral("--pwtest")))
        return runPwTest();

    if (args.contains(QStringLiteral("--poltest")))
        return runPolicyTest();

    if (args.contains(QStringLiteral("--conntest")))
        return runConnTest();

    if (args.contains(QStringLiteral("--servertest")))
        return runServerTest();

    if (args.contains(QStringLiteral("--tsnettest")))
        return runTsnetTest();

    if (args.contains(QStringLiteral("--booktest")))
        return runBookTest();

    if (args.contains(QStringLiteral("--netcheck")))
        return runNetCheck();

    if (args.contains(QStringLiteral("--proftest")))
        return runProTest();

    if (args.contains(QStringLiteral("--qtest")))
        return runQualityTest();

    if (args.contains(QStringLiteral("--uitest")))
        return runUiTest();

    if (args.contains(QStringLiteral("--paneltest")))
        return runPanelTest(config);

    // ⚠ 全项目唯一会**写系统**的测试（HKCU 的 Run 键），所以故意不放进
    //   scripts/verify.sh 的默认清单 —— 那批的前提是"零系统写入"。只能手动跑。
    if (args.contains(QStringLiteral("--autostarttest")))
        return runAutostartTest();

    // 脚本用的单步开关：设/清固定密码（写配置文件，不需要管理员）
    if (args.contains(QStringLiteral("--pwset"))) {
        attachParentConsole();
        RustDeskController rd;
        const QString pw = argValue(args, QStringLiteral("--pwset"));
        const bool ok = rd.writePasswordPlain(pw);
        std::printf("%s\n", ok ? "OK" : "FAIL");
        std::fflush(stdout);
        return ok ? 0 : 1;
    }
    if (args.contains(QStringLiteral("--pwclear"))) {
        attachParentConsole();
        RustDeskController rd;
        const bool ok = rd.clearPassword();
        std::printf("%s\n", ok ? "OK" : "FAIL");
        std::fflush(stdout);
        return ok ? 0 : 1;
    }

    // `--show` 不需要任何处理：不带 --connect 的启动本来就是"弹窗口"，冷启动和转发都是。
    // 它的存在意义是给 verify.sh 一个**无副作用**的启动参数 —— 以前那一项写的是
    // `--connect 000000000`，M2 之后那会真的发一次连接请求（弹 RustDesk 窗口、可能长挂）。

    const QString shotPath = argValue(args, QStringLiteral("--screenshot"));
    // --screenshot-dialog settings|guide：抓「设置」或「图文教程」那个对话框。
    // 四组设置搬进对话框之后，主窗口截图**照不到它们了** —— 而 verify.sh 第 2 步
    // 只断言"PNG 非空"，覆盖少了不会报错。这两张图就是补那个静默缺口。
    const QString shotDlg = argValue(args, QStringLiteral("--screenshot-dialog"));

    // 单例：已经有实例就把参数转过去然后退出
    if (shotPath.isEmpty() && shotDlg.isEmpty()) {
        QLocalSocket probe;
        probe.connectToServer(QString::fromLatin1(kInstanceName));
        if (probe.waitForConnected(300)) {
            probe.abort();
            forwardToInstance(args.mid(1));
            return 0;
        }
    }

    // 上次异常退出可能留下陈旧的 socket 文件，清掉再监听
    QLocalServer::removeServer(QString::fromLatin1(kInstanceName));
    QLocalServer server;
    if (!server.listen(QString::fromLatin1(kInstanceName))) {
        // 监听失败不致命：只是失去单例能力，功能照常
    }

    // ---- 界面语言 ----
    //
    // ⚠ 分两处，位置都是硬的，别合并：
    //
    // ① **配置里存的那份语言要等到这里才生效** —— 也就是在所有无头开关的早返回之后。
    //    那几个测试（--booktest / --uitest / --pwtest…）全是靠 grep 中文句子判 PASS 的，
    //    而 zh 档下 I18n::t() 是恒等函数、字节不变：只要跑测试时是 zh，所有断言与被改动
    //    之前**逐字节相同**。反过来，要是把 config.lang() 提到早返回之前，一个把语言存成
    //    en 的用户跑 `--uitest` 就会让整批断言集体失效 —— 而且看起来像"功能坏了"。
    //
    // ② **`--lang zh|en` 是个显式开关，在前面就先认掉了**（见 config.load() 下面那段）。
    //    它只给验收用（生成英文截图、让 --uitest 真跑一遍英文表），而且**不写盘** ——
    //    验收不该改用户的配置。
    //
    // 优先级：--lang > 配置里存的 > 构造函数按系统区域算的默认值。
    if (argValue(args, QStringLiteral("--lang")).isEmpty())
        I18n::setLang(I18n::fromCode(config.lang()));

    DarkTheme::apply(app);

    // 开机自启的**路径自愈**。
    //
    // 绿色免安装的包被挪目录是常态（换台机器、换个盘、解压到别处）。注册表里那条值
    // 记的是**绝对路径**，一挪就成了死项 —— 开机时系统照着找个不存在的文件，什么都不
    // 发生，而且不报错、界面上那个开关还老老实实显示"开着"。用户唯一的发现方式就是
    // "重启之后连不上"，那时候往往正是他最需要连上的时候。
    //
    // 所以每次正常启动都对一次：**自启已经开着**、而记的不是现在这个 exe → 按当前路径
    // 改写。注意这条只在"已经开着"时才动，**从不新建** —— 没开自启的机器，启动过程
    // 一个字节都不写注册表（verify.sh 第 4 步"零系统写入"照旧成立）。
    //
    // --screenshot / --screenshot-dialog 那两条路不算"正常启动"：那是验收脚本在跑，
    // 验收过程**不许**写系统（这条例外写进了 verify.sh 第 4 步的注释）。
    const bool shotRun = !shotPath.isEmpty() || !shotDlg.isEmpty();
    if (!shotRun && Autostart::isEnabled()) {
        const QString curExe = QCoreApplication::applicationFilePath();
        const QString have   = Autostart::storedRunValue();
        if (Autostart::needsRepair(have, curExe)) {
            QString aerr;
            if (Autostart::enable(curExe, &aerr))
                qInfo("开机自启的路径已按当前位置更新：%s", qUtf8Printable(curExe));
            else
                qWarning("开机自启路径自愈失败：%s", qUtf8Printable(aerr));
        }
    }

    RustDeskController rd;
    rd.setExePath(config.rustDeskPath());

    // 日志监听只建**一个**，分给需要它的两方：「连接提示」用它认"有人连进来"，
    // 「用完即换」用它认"人走了"。各造一个的话两边各读一遍日志，而且开关会互相打架
    // （提示关掉了不该把用完即换要的监听从底下抽走，反之亦然）。
    ConnectionWatcher watcher;

    PasswordPolicy policy(&config, &rd, &watcher);
    NotifyPolicy   notify(&config, &rd, &watcher);

    // 地址簿必须挂在**这一个** config 实例上：AppConfig::save() 是整文件重写，
    // 再来第二个实例就会两边互相覆盖（后写的把先写的抹掉）。
    AddressBook book(&config);
    book.load();

    // 长驻的桥接对象：只有 tsnet 档才会被拉起来，官方 ID 档全程躺着不动。
    TsnetBridge    bridge;
    ConnectSession session(&config, &rd, &bridge, &book);

    // 网络体检 + 连接路径。**在这里就开路径监听**：它只 tail 日志、从"此刻的末尾"
    // 开始看，历史连接不会被当成新连接报出来；等用户点连接才开的话，第一次连接的
    // 那两行日志很可能已经写过去了。
    NetworkProbe   probe;
    MainWindow win(&config, &rd, &policy, &notify, &book, &session, &probe);
    probe.startPathWatch();

    // 「立即重启」（设置框里换完语言按的那个按钮）。真正的活在 main 里 —— 只有这里
    // 拿得到 QLocalServer。
    //
    // ⚠ 顺序是硬的：**先把这个单例口让出去，再拉新进程**。反过来的话，新进程一启动就
    //   去连这个名字，连上了 → 它把命令行转回来 → 它自己退出 —— 用户看到的是
    //   "点了重启，窗口闪一下没了"，而老进程还活着，等于什么都没发生。
    QObject::connect(&win, &MainWindow::restartRequested, &app, [&server, &app] {
        server.close();
        QLocalServer::removeServer(QString::fromLatin1(kInstanceName));
        QProcess::startDetached(QCoreApplication::applicationFilePath(), QStringList());
        app.quit();
    });

    TrayController tray;
    QObject::connect(&tray, &TrayController::showWindowRequested, &win, [&win] {
        win.showNormal();
        win.raise();
        win.activateWindow();
    });
    // 托盘「连接…」：把窗口叫出来并聚焦到地址簿那一组
    QObject::connect(&tray, &TrayController::connectRequested, &win,
                     &MainWindow::focusAddressBook);
    QObject::connect(&tray, &TrayController::refreshRequested, &win, &MainWindow::refresh);
    QObject::connect(&tray, &TrayController::quitRequested, &app, &QApplication::quit);
    QObject::connect(&rd, &RustDeskController::idChanged, &tray, &TrayController::setIdText);
    QObject::connect(&rd, &RustDeskController::statusMessage, &tray, &TrayController::setStatusText);
    // 有人连进来 → 弹气泡。窗口多半正缩在托盘里，只有气泡能让人看见。
    QObject::connect(&notify, &NotifyPolicy::remoteConnected, &tray, [&tray] {
        tray.notify(TR("有人连进来了"),
                    TR("有人正在远程控制这台电脑。\n"
                       "（不知道是谁 —— 被控端日志里没有对方的 ID）"));
    });
    tray.show();

    // 被控端的命根子：绿色免安装没有"开机自启的服务"，人不在电脑前没人去双击
    // RustDesk，机器重启后就是彻底连不上。所以外壳自己扛着 —— 启动即拉起
    // （--server，无窗口），掉线自动重拉。放在 refresh 之前，让 ID 读到的是真在跑的那份。
    rd.setKeepAlive(true);

    // 密码策略：读配置 + 补一次错过的轮换 + 起定时器。
    // 放在托盘之后，这样它发的 statusMessage 有人接。
    policy.start();
    // 连接提示：只把内存里的监听开关和配置对齐，不动 RustDesk 进程。
    notify.start();

    // 第二次双击图标（或从命令行再起一次）→ 新进程把参数转过来 → 这里接住。
    //
    // ⚠ 以前这里**只**把窗口叫到前台、把 payload 直接扔了 —— 于是"双击带 --connect 连接"
    //   就是个静默无效的假功能：命令发出去了、日志里什么都没发生。现在必须真读出来照做。
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&server, &win, &session] {
        QLocalSocket* c = server.nextPendingConnection();
        if (!c)
            return;

        // payload 按行到（forwardToInstance 一行一个参数），分片到达，所以先攒着
        auto* buf = new QByteArray;
        QObject::connect(c, &QLocalSocket::readyRead, c, [c, buf] { *buf += c->readAll(); });
        QObject::connect(c, &QLocalSocket::disconnected, c, [c, buf, &win, &session] {
            *buf += c->readAll();          // 兜底：最后一点没被 readyRead 捞到的

            const QStringList lines = QString::fromUtf8(*buf)
                                          .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            delete buf;
            c->deleteLater();

            // ⚠ `--tray` 转发过来时**不许把窗口叫出来**。这不是理论问题：开机自启那条
            //   命令行走的就是 `--tray`，而"机器上已经有一个实例在跑"是完全正常的
            //   （用户先手动开着，或者上一轮登录留下的）—— 照旧 showNormal 的话，
            //   每次登录都会看见主窗口自己弹到脸上，自启反而比不自启更烦。
            //   别的参数（--connect / 双击图标）照旧：那两种情况用户就是要看窗口。
            if (!lines.contains(QStringLiteral("--tray"))) {
                win.showNormal();
                win.raise();
                win.activateWindow();
            }

            const int ci = lines.indexOf(QStringLiteral("--connect"));
            if (ci >= 0 && ci + 1 < lines.size()) {
                const QString   target = lines.at(ci + 1);
                const int       pi     = lines.indexOf(QStringLiteral("--password"));
                const QString   pw     = (pi >= 0 && pi + 1 < lines.size())
                                             ? lines.at(pi + 1)
                                             : QString();
                // 恒走官方 ID 档 —— 命令行给的是裸地址，没有"这条联系人在哪个档位"这回事
                session.connectToTarget(target, pw);
            }
        });
    });

    // --tray：**开机自启专用**。起来了但不弹窗口，只在托盘上待着 ——
    // 没有这个开关的话，开机自启就等于每次开机都把主窗口糊到人脸上，比不自启还烦。
    // 它只跳过 show()：托盘图标、keep-alive、日志监听、定时器全部照常起来，
    // 所以"机器重启后仍然待连"这件事是真的成立的。
    const bool trayOnly = args.contains(QStringLiteral("--tray"));
    if (!trayOnly)
        win.show();
    // ⚠ 放在 if 外面：它只改窗口的视觉样式（DWM 深色标题栏），不影响映射与否。
    //   --tray 下 winId() 会惰性创建原生窗口句柄，但**没有 ShowWindow**，所以不会
    //   在任务栏上留下任何东西（WaterCup 那个"幽灵缩略图"的根因是窗口被映射过）。
    DarkTheme::applyNativeTitleBar(win.winId());
    // 同样等事件循环起来再发起（refreshId 失败时是同步发信号的）
    QTimer::singleShot(0, &win, &MainWindow::refresh);

    // 冷启动 `PureRemote.exe --connect <目标> [--password <pw>]`：等窗口立起来再发起，
    // 这样状态栏和「远程连接」那一组都已建好，失败信息有地方落。
    // 已经有实例在跑的话根本走不到这里 —— 上面单例那段已经把参数转过去了。
    if (args.contains(QStringLiteral("--connect"))) {
        const QString target = argValue(args, QStringLiteral("--connect"));
        const QString pw     = argValue(args, QStringLiteral("--password"));
        QTimer::singleShot(300, &app, [&session, target, pw] {
            session.connectToTarget(target, pw);
        });
    }

    // 验收用：真起窗口、等 ID 出来、把窗口抓成 PNG 再退。
    // 面板类 bug 无头测不出来，必须走真实渲染这条路径。
    if (!shotPath.isEmpty()) {
        // --liveconnect：单机自连一次，确认"有人连进来了"这条链路真的通。
        // 日志标记的匹配已经由 --conntest 拿真日志逐字节验过；这里补的是最后一段 ——
        // 真连接 → NotifyPolicy → 界面/托盘到底接上没有。
        // 密码由调用方事先用 --pwset 写好（写密码必须在 RustDesk 停着的时候）。
        const bool live  = args.contains(QStringLiteral("--liveconnect"));
        auto       state = std::make_shared<int>(0);   // 0 等 / 1 通了 / 2 超时 / 3 没密码
        auto       fired = std::make_shared<bool>(false);
        auto       settle = std::make_shared<bool>(false);   // 已排上"等路径轮询"的延时
        auto       rc     = std::make_shared<int>(0);        // 路径断言失败时非 0
        // 诊断计数：出错时能看出卡在哪一步，不用猜
        auto       diag  = std::make_shared<QStringList>();

        // --shot-nopw：把这台机器伪装成"一个固定密码都没有"，好把「现在没人能连进来」
        // 那条红色横幅和密码行的红提示**真渲染出来**。开发机自己是有密码的，不加这个
        // 开关那条分支在截图上永远看不见 —— 而这正是"截图非空 + 断言全绿"都拦不住的
        // 那一类（长度、换行、红底红字，只有眼睛看得见）。
        const bool shotNoPw = args.contains(QStringLiteral("--shot-nopw"));

        auto shot = [&win, &probe, shotPath, shotDlg, live, state, diag, rc, shotNoPw] {
            // 抓**整份内容**，不是窗口那点可见区：主界面套在 QScrollArea 里，
            // win.grab() 只能拍到可见的那几组 —— 折叠线以下的布局错误永远暴露不出来。
            // 这个验收项存在的理由就是"面板类 bug 只有真实渲染才看得见"。
            if (shotNoPw) {
                win.setLocalPasswordForTest(QString());
                win.setRawPasswordOverrideForTest(QString());
                QApplication::processEvents();
            }
            QWidget* target = &win;
            if (!shotDlg.isEmpty()) {
                // 对话框得先 open 才有内容：控件是建好的，但布局要等 show 之后才排。
                // 教程框可以指定看哪一段：`--screenshot-dialog guide:tsnet`。
                // 不指定就是第一篇。tsnet 那篇才是重点（两张图 + 五条真链接），
                // 验收抓的就是它。
                if (shotDlg.startsWith(QLatin1String("guide"))) {
                    const int c = shotDlg.indexOf(QLatin1Char(':'));
                    win.openGuideDialog(c >= 0 ? shotDlg.mid(c + 1) : QString());
                    target = win.guideShotTarget();
                } else {
                    win.openSettingsDialog();
                    // `settings:2` = 抓设置框的第 3 页。分页之后一页只拍得到自己，
                    // 新加的「开机与后台」在「界面」页，不切页就拍不着。
                    if (shotDlg.startsWith(QLatin1String("settings:"))) {
                        const int page = shotDlg.mid(QStringLiteral("settings:").size()).toInt();
                        win.selectSettingsTabForTest(page);
                        QApplication::processEvents();
                    }
                    target = win.settingsShotTarget();
                }
                // 设置框内容比它的窗口高，抓窗口只会拍到前两组 —— 按布局的 sizeHint
                // 撑到内容全高再抓，不然这一项等于没验。
                if (target->layout()) {
                    const int h = target->layout()->sizeHint().height();
                    if (h > target->height())
                        target->resize(target->width(), h);
                }
                QApplication::processEvents();
            } else if (auto* sa = qobject_cast<QScrollArea*>(win.centralWidget())) {
                if (sa->widget())
                    target = sa->widget();
            }
            target->grab().save(shotPath);
            if (live) {
                QString text;
                if (*state == 1)
                    text = QStringLiteral("[PASS] 真连接触发了「有人连进来了」\n");
                else
                    text = QStringLiteral("[FAIL] 没等到连接标记（状态 %1："
                                          "0=还在等 2=45 秒超时 3=配置里没有固定密码）\n")
                               .arg(*state);

                // M4.3 的最后一段：路径标签到底会不会变。
                // 上面那些断言全在验"日志行 → 解析器"，这一段验的是"解析结果 → 界面"。
                // 抓图看不出对错（人得拿眼睛盯），所以直接断言文案内容。
                const QString pathText = probe.pathSummary();
                const bool    pathOk   = pathText.contains(QStringLiteral("直连"))
                                      || pathText.contains(QStringLiteral("中继"));
                text += pathOk
                            ? QStringLiteral("[PASS] 连接路径已显示：%1\n").arg(pathText)
                            : QStringLiteral("[FAIL] 连接路径还是占位文案，界面没更新：'%1'\n")
                                  .arg(pathText);
                if (!pathOk)
                    *rc = 1;

                QFile f(QDir::temp().filePath(QStringLiteral("puremote-liveconnect.txt")));
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    f.write(text.toUtf8());
                    f.write(diag->join(QLatin1Char('\n')).toUtf8());
                    f.write("\n");
                }
            }
            QCoreApplication::exit(*rc);
        };
        auto finish = [shot, fired] {
            if (*fired)
                return;
            *fired = true;
            shot();
        };

        if (live) {
            QObject::connect(&notify, &NotifyPolicy::remoteConnected, &app,
                             [state] { *state = 1; });

            // 用 RustDesk.toml 里那份（--pwset 写的就是它），不是外壳自己记的
            // puremote.json —— 两者可能不同步，而认证只认前者。
            const QString pw = rd.readPasswordRaw();
            *diag << QStringLiteral("  cli=%1").arg(rd.cliExePath());
            *diag << QStringLiteral("  密码长度=%1").arg(pw.size());
            if (pw.isEmpty()) {
                *state = 3;
                QTimer::singleShot(0, &app, finish);
            } else {
                // ID 一出来就自己连自己
                QObject::connect(&rd, &RustDeskController::idChanged, &app,
                                 [&rd, pw, state, diag](const QString& id) {
                    *diag << QStringLiteral("  idChanged: '%1'").arg(id);
                    if (id.isEmpty() || *state != 0)
                        return;
                    const bool ok = QProcess::startDetached(
                        rd.cliExePath(), { QStringLiteral("--connect"), id,
                                           QStringLiteral("--password"), pw });
                    *diag << QStringLiteral("  startDetached(--connect) = %1")
                                 .arg(ok ? QStringLiteral("true") : QStringLiteral("false"));
                });
                auto* poll = new QTimer(&app);
                poll->setInterval(500);
                QObject::connect(poll, &QTimer::timeout, &app, [state, settle, finish, &app] {
                    if (*settle)
                        return;
                    if (*state == 2 || *state == 3) {   // 超时 / 没密码：没什么可等的
                        finish();
                        return;
                    }
                    if (*state == 1) {
                        // 连上之后**不能立刻抓图**。路径标记确实是连上那一刻写进日志的，
                        // 但实测 establish 行只比 peer info 行早 69ms，而路径轮询是 1500ms
                        // 一轮 —— 立刻抓必然拍到"还没连过"的占位文案。第一版就是这么错的：
                        // 图看着"没更新"，其实只是抢在轮询前面了。给两轮的时间再抓。
                        *settle = true;
                        QTimer::singleShot(3500, &app, finish);
                    }
                });
                poll->start();
                QTimer::singleShot(45000, &app, [state, finish] {
                    if (*state == 0)
                        *state = 2;
                    finish();
                });
            }
        } else {
            QObject::connect(&rd, &RustDeskController::idChanged, &app,
                             [finish](const QString&) {
                QTimer::singleShot(1200, finish);   // 留点时间让布局和状态栏刷完
            });
            QTimer::singleShot(30000, &app, finish);
        }
    }

    return app.exec();
}

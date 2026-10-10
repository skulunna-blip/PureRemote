#include "RustDeskController.h"
#include "I18n.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>

#include <memory>

namespace {
// 真身 0.33s 就返回，10s 是给机器卡顿留的余量
constexpr int kAttemptTimeout  = 10000;
// 首次解包要几秒；30 × 800ms = 24s 足够
constexpr int kRetries         = 30;
constexpr int kRetryIntervalMs = 800;

// 拼一个"环境变量 + 子路径"的候选位置；环境变量为空则返回空串
QString envPath(const char* var, const QString& sub)
{
    const QString base = qEnvironmentVariable(var);
    if (base.isEmpty())
        return QString();
    return QDir(base).filePath(sub);
}

// ── RustDesk.toml 顶层 password 的定位 ───────────────────────────────────────
//
// **必须按括号深度判作用域，不能用「行首是不是 [」判表头。** 文件里有
// `key_pair = [[...]]` 这种多行数组，内层 `    [` 行首也是 '['，按行首判会把数组切开，
// password 被插进数组内部 → TOML 非法 → RustDesk 加载失败（`Bad TOML data`）→
// 丢弃 enc_id/key_pair 重新注册 → **本机 ID 被换掉**（实测：本机 ID 会被换成另一个，旧 ID 再也拿不回来）。
//
// 顶层标量区 = 文件开头到「第一个开启数组的行」或「第一个表头行」之前。
struct TopScan
{
    int passwordLine = -1;  // 顶层 password 行下标，没有则 -1
    int regionEnd    = 0;   // 顶层标量区结束位置（新 password 的插入点）
};

TopScan scanTopLevel(const QStringList& lines)
{
    static const QRegularExpression kOpenArray(
        QStringLiteral(R"(^[A-Za-z0-9_\-]+[ \t]*=[ \t]*\[)"));
    static const QRegularExpression kPassword(QStringLiteral(R"(^[ \t]*password[ \t]*=)"));

    TopScan r;
    int     depth = 0;
    bool    inStr = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString ln = lines.at(i);
        const QString s  = ln.trimmed();
        if (depth == 0) {
            // 表头：整行就是 [xxx]（不是赋值）
            if (s.startsWith(QLatin1Char('[')) && s.endsWith(QLatin1Char(']'))
                && !s.contains(QLatin1Char('='))) {
                r.regionEnd = i;
                return r;
            }
            if (kOpenArray.match(ln).hasMatch()) {
                r.regionEnd = i;
                return r;
            }
            if (r.passwordLine < 0 && kPassword.match(ln).hasMatch())
                r.passwordLine = i;
        }
        // 单引号串内不计数（TOML 单引号串里不能再有单引号，所以简单翻转就够）
        for (const QChar ch : ln) {
            if (ch == QLatin1Char('\'')) {
                inStr = !inStr;
            } else if (!inStr) {
                if (ch == QLatin1Char('['))
                    ++depth;
                else if (ch == QLatin1Char(']'))
                    --depth;
            }
        }
    }
    r.regionEnd = lines.size();
    return r;
}

// 从 `password = 'xxx'` 行里取值；取不到返回空串
QString passwordValue(const QString& line)
{
    static const QRegularExpression kVal(
        QStringLiteral(R"(^[ \t]*password[ \t]*=[ \t]*'(.*)'[ \t]*$)"));
    const auto m = kVal.match(line);
    return m.hasMatch() ? m.captured(1) : QString();
}
} // namespace

RustDeskController::RustDeskController(QObject* parent)
    : QObject(parent)
{
}

// 验收用的 RustDesk.toml 覆盖路径，空 = 用真路径（见头文件）。
QString RustDeskController::s_mainConfigPathOverride;

QString RustDeskController::configDir()
{
    return QDir(qEnvironmentVariable("APPDATA"))
        .filePath(QStringLiteral("RustDesk/config"));
}

QString RustDeskController::mainConfigPath()
{
    // 验收用的副本覆盖（见头文件里那段说明：真文件上的 password 是唯一一份，碰不得）
    if (!s_mainConfigPathOverride.isEmpty())
        return s_mainConfigPathOverride;
    return QDir(configDir()).filePath(QStringLiteral("RustDesk.toml"));
}

void RustDeskController::setMainConfigPathForTest(const QString& path)
{
    s_mainConfigPathOverride = path;
}

QString RustDeskController::optionConfigPath()
{
    return QDir(configDir()).filePath(QStringLiteral("RustDesk2.toml"));
}

bool RustDeskController::isExePresent() const
{
    return !m_exePath.isEmpty() && QFileInfo::exists(m_exePath);
}

// 随包的 bin/rustdesk.exe 现在是**真身**（不再是 launcher）—— 打包时直接把解包后的
// 运行时塞进 bin/，这样外壳一个字节都不写 %LOCALAPPDATA%，也就没有 launcher 那层坑。
//
// 真身是 369KB 的薄壳，必须和 librustdesk.dll（40MB）同目录才能跑；
// launcher 是 25MB 的自包含单体，同目录没有这个 dll。用这个区分比按大小猜稳。
QString RustDeskController::cliExePath() const
{
    const auto isRealRuntime = [](const QString& exe) {
        if (exe.isEmpty() || !QFileInfo::exists(exe))
            return false;
        return QFileInfo::exists(QFileInfo(exe).absoluteDir()
                                     .filePath(QStringLiteral("librustdesk.dll")));
    };

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/bin/rustdesk.exe"),       // 部署布局：随包的就是真身
        appDir + QStringLiteral("/../bin/rustdesk.exe"),    // 开发布局：exe 在 build/
        // 兜底：万一包里放的是 launcher，它解包出来的真身在这
        envPath("LOCALAPPDATA", QStringLiteral("rustdesk/rustdesk.exe")),
        envPath("ProgramFiles", QStringLiteral("RustDesk/rustdesk.exe")),
        envPath("ProgramFiles(x86)", QStringLiteral("RustDesk/rustdesk.exe")),
    };
    for (const QString& c : candidates) {
        const QString clean = QDir::cleanPath(c);
        if (isRealRuntime(clean))
            return clean;
    }
    return QString();
}

bool RustDeskController::isRunning() const
{
    QProcess p;
    p.start(QStringLiteral("tasklist"),
            { QStringLiteral("/FI"), QStringLiteral("IMAGENAME eq rustdesk.exe"),
              QStringLiteral("/NH") });
    if (!p.waitForFinished(5000)) {
        p.kill();
        p.waitForFinished(1000);
        return false;
    }
    // 无匹配时 tasklist 打印的是本地化的提示语，不含镜像名 —— 直接找镜像名即可
    return QString::fromLocal8Bit(p.readAllStandardOutput())
        .contains(QStringLiteral("rustdesk.exe"), Qt::CaseInsensitive);
}

namespace {
// 问系统要"所有 rustdesk.exe 的命令行"。返回 false = **没问出来**
// （工具跑不起来 / 超时被杀），跟"问出来了、就是没有"是两件事 —— 调用方必须区别对待。
//
// 为什么不能用 tasklist：它只给进程名、不给命令行，而这里要的恰恰是命令行
// （`--server` 和 `--tray` 的差别只在那里）。实测耗时：tasklist 0.34s、
// wmic 0.57s、PowerShell Get-CimInstance 0.87s。wmic 在新版 Windows 上被移掉了，
// 所以留一条 PowerShell 兜底。
bool queryRustDeskCmdlines(QString* out)
{
    // 用 /format:list 而不是默认的表格：它输出 `CommandLine=...` 一行一条，
    // **不会被列宽折行**（表格在管道里的折行行为是历史坑），解析也更稳。
    QProcess w;
    w.start(QStringLiteral("wmic"),
            { QStringLiteral("process"), QStringLiteral("where"),
              QStringLiteral("name='rustdesk.exe'"), QStringLiteral("get"),
              QStringLiteral("commandline"), QStringLiteral("/format:list") });
    if (w.waitForFinished(5000) && w.exitStatus() == QProcess::NormalExit) {
        // "No Instance(s) Available." 也是正常退出、也走这条路 —— 那种情况的输出里
        // 解析出来自然是"没有 --server"，正是要的答案。
        *out = QString::fromLocal8Bit(w.readAllStandardOutput());
        return true;
    }
    w.kill();
    w.waitForFinished(1000);

    QProcess ps;
    ps.start(QStringLiteral("powershell"),
             { QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
               QStringLiteral("-Command"),
               QStringLiteral("Get-CimInstance Win32_Process -Filter \"name='rustdesk.exe'\""
                              " | Select-Object -ExpandProperty CommandLine") });
    if (ps.waitForFinished(8000) && ps.exitStatus() == QProcess::NormalExit) {
        *out = QString::fromLocal8Bit(ps.readAllStandardOutput());
        return true;
    }
    ps.kill();
    ps.waitForFinished(1000);
    return false;
}
} // namespace

bool RustDeskController::serverInProcessList(const QString& lines)
{
    // 按空白切 token，把两端的引号剥掉再比。真实命令行就这两种形状（本机实测）：
    //     C:\...\bin\rustdesk.exe  --server
    //     "C:\...\bin\rustdesk.exe" --tray
    // 只认**独立的** `--server` token —— 这样 `--tray`、`--connect`，以及"路径里恰好
    // 含 --server"这类都不可能被当成服务端。
    const QStringList toks =
        lines.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const QString& tok : toks) {
        QString t = tok;
        while (t.startsWith(QLatin1Char('"')))
            t.remove(0, 1);
        while (t.endsWith(QLatin1Char('"')))
            t.chop(1);
        if (t == QLatin1String("--server"))
            return true;
    }
    return false;
}

bool RustDeskController::isServerRunning() const
{
    QString lines;
    if (!queryRustDeskCmdlines(&lines)) {
        // 两个查询都跑不动（极罕见：wmic 被移掉 + 执行策略封了 PowerShell）。
        // **按"还活着"处理，不按"死了"**：判 false 会让看门狗每 10 秒去起一个
        // --server，而实测重复的 --server 会在 \\.\pipe\RustDesk\query 上撞车、
        // 把**正在服务的那个**带走。"不确定就别动"永远比"不确定就重拉"安全。
        return true;
    }
    return serverInProcessList(lines);
}

bool RustDeskController::start()
{
    const QString exe = cliExePath();
    if (exe.isEmpty())
        return false;
    // --server：无窗口的服务端（源码 src/core_main.rs:397 → start_server(true, false)）。
    // 裸 exe 会弹出 RustDesk 主窗口，对一个托盘常驻的外壳来说很突兀。
    // 脱离式启动：外壳退出后它还得继续当着被控端。
    return QProcess::startDetached(exe, { QStringLiteral("--server") });
}

void RustDeskController::setKeepAlive(bool on)
{
    if (m_keepAlive == on)
        return;
    m_keepAlive = on;

    if (!on) {
        if (m_watchdog)
            m_watchdog->stop();
        return;
    }

    if (!m_watchdog) {
        m_watchdog = new QTimer(this);
        // 10 秒一次。每跳都要起 tasklist + wmic 去问"到底有没有 --server"，
        // 两下加起来 ~0.9s（会占住主线程），别更勤了。
        m_watchdog->setInterval(10000);
        connect(m_watchdog, &QTimer::timeout, this, &RustDeskController::watchdogTick);
    }
    // ⚠ 判据是 isServerRunning() 而不是 isRunning()：只剩一个 `--tray` 界面进程时
    //   机器是**连不进来**的，而它也占着 rustdesk.exe 这个名字（2026-10-10 事故）。
    if (!isServerRunning()) {
        emit statusMessage(TR("正在拉起 RustDesk 常驻服务…"));
        start();
    }
    m_watchdog->start();
}

void RustDeskController::watchdogTick()
{
    if (m_suspendDepth > 0)             // 正在 applyNow 的停-写-起窗口里，别插手
        return;
    // ⚠ 同上：这里原来问 isRunning()，于是"服务端死了、只剩 --tray"这种
    //   **真人真事**（用户点了「打开 RustDesk 看一次性密码」）会被判成一切正常，
    //   机器静默连不进来、看门狗一次都不拉。
    if (isServerRunning())
        return;
    emit statusMessage(TR("RustDesk 后台服务不在了，正在重新拉起…"));
    if (!start())
        emit statusMessage(TR("重新拉起 RustDesk 失败 —— 检查 %1 是否还在").arg(m_exePath));
}

bool RustDeskController::stop()
{
    if (!isRunning())
        return true;

    QProcess p;
    p.start(QStringLiteral("taskkill"),
            { QStringLiteral("/F"), QStringLiteral("/IM"), QStringLiteral("rustdesk.exe") });
    if (!p.waitForFinished(8000)) {
        p.kill();
        p.waitForFinished(1000);
    }
    // 等进程真的消失再返回 —— 写密码必须在它完全退出之后，否则被它回写覆盖
    for (int i = 0; i < 30; ++i) {
        if (!isRunning())
            return true;
        QThread::msleep(200);
    }
    return !isRunning();
}

// ---------------------------------------------------------------- ID 读取
QString RustDeskController::pickId(const QString& rawOutput)
{
    // 便携 exe 会往 stdout 喷一堆解包噪声，ID 是里面唯一的纯数字行。
    // 先找标准 9 位，找不到再放宽到 6–12 位。
    QString fallback;
    const QStringList lines = rawOutput.split(
        QRegularExpression(QStringLiteral("[\\r\\n]")), Qt::SkipEmptyParts);

    for (const QString& line : lines) {
        QString digits = line.trimmed();
        digits.remove(QLatin1Char(' '));      // 界面上会做 3-3-3 分组
        if (digits.size() < 6 || digits.size() > 12)
            continue;
        bool ok = false;
        digits.toULongLong(&ok);
        if (!ok)
            continue;
        if (digits.size() == 9)
            return digits;                    // 标准 ID，直接采用
        if (fallback.isEmpty())
            fallback = digits;
    }
    return fallback;
}

void RustDeskController::refreshId()
{
    if (cliExePath().isEmpty() && !isExePresent()) {
        emit idChanged(QString());
        emit statusMessage(TR("找不到 rustdesk.exe：%1").arg(m_exePath));
        emit idRefreshFinished();          // 提前返回也要收尾，否则界面卡在"读取中…"
        return;
    }
    m_retriesLeft = kRetries;
    m_triggeredExtraction = false;
    startAttempt();
}

void RustDeskController::startAttempt()
{
    const QString cli = cliExePath();

    if (cli.isEmpty()) {
        // 还没解包：跑一次 launcher 触发自解包，然后等真身出现
        if (!m_triggeredExtraction) {
            m_triggeredExtraction = true;
            emit statusMessage(TR("首次运行：正在解包 RustDesk 运行时…"));
            QProcess::startDetached(m_exePath, QStringList());
        }
        retryLater();
        return;
    }

    if (m_retriesLeft == kRetries)
        emit statusMessage(TR("正在读取本机 ID…"));

    runCliAsync(cli, { QStringLiteral("--get-id") }, kAttemptTimeout,
                [this](const QString& out) {
        const QString id = pickId(out);
        if (!id.isEmpty()) {
            if (id != m_id) {
                m_id = id;
                emit idChanged(m_id);
            }
            emit statusMessage(TR("就绪"));
            // **即使 id 没变也要发**：调用方要的是"这个动作结束了"，不是"值变了"。
            // 少了这一句，第二次点「重新读取」就永远停在"读取中…"（值没变，idChanged 不发）。
            emit idRefreshFinished();
            return;
        }
        retryLater();
    });
}

void RustDeskController::retryLater()
{
    if (--m_retriesLeft <= 0) {
        // 已经有 ID 就别因为一次抖动把它抹掉
        if (m_id.isEmpty())
            emit idChanged(QString());
        emit statusMessage(TR("读不到 ID：RustDesk 未就绪"));
        // 失败这条路同样要收尾：m_id 非空时上面那句 idChanged 是不发的（故意不发，
        // 免得把已知的 ID 抹掉），所以**只能**靠这个信号让界面把按钮放回来。
        emit idRefreshFinished();
        return;
    }
    QTimer::singleShot(kRetryIntervalMs, this, [this] { startAttempt(); });
}

// 异步跑一次 CLI，完成后回调（out = 合并后的 stdout+stderr）。
//
// 输出走**文件**而不是管道：launcher 那层会留下继承 stdout 句柄的常驻子进程，
// 管道因此永远等不到 EOF，QProcess 的 finished 一直不发（表现为每次都卡到超时）。
// 重定向到文件就没这个问题 —— Qt 只在有管道时才起读线程。
// 真身本身是干净退出的，这里保持文件方式只是不想再踩一次。
void RustDeskController::runCliAsync(const QString& cli, const QStringList& args, int timeoutMs,
                                     std::function<void(const QString&)> done)
{
    const QString outFile = QDir::temp().filePath(QStringLiteral("puremote-cli-out.txt"));
    QFile::remove(outFile);

    auto* p = new QProcess(this);
    p->setProcessChannelMode(QProcess::MergedChannels);
    p->setStandardOutputFile(outFile);

    // 进程的 finished / errorOccurred / 超时三条路都会走到 fire，用共享标志保证只回调一次
    auto fired = std::make_shared<bool>(false);
    auto fire  = [p, done, fired, outFile] {
        if (*fired)
            return;
        *fired = true;

        QFile f(outFile);
        QString out;
        if (f.open(QIODevice::ReadOnly))
            out = QString::fromLocal8Bit(f.readAll());

        done(out);
        p->deleteLater();
    };

    connect(p, &QProcess::finished, this, [fire](int, QProcess::ExitStatus) { fire(); });
    connect(p, &QProcess::errorOccurred, this, [fire](QProcess::ProcessError) { fire(); });

    // 上下文对象挂 p：p 被销毁则定时器一并消失，不会打到野指针
    QTimer::singleShot(timeoutMs, p, [p, fire] {
        if (p->state() != QProcess::NotRunning)
            p->kill();
        fire();                       // 无论杀没杀成都不再等，先读文件里已有的内容
    });

    p->start(cli, args);
}

// ---------------------------------------------------------------- 配置文件

QString RustDeskController::readPasswordRaw() const
{
    QByteArray bytes;
    if (!readConfigBytes(mainConfigPath(), &bytes, nullptr))
        return QString();

    const QStringList lines = splitConfigLines(bytes);
    const TopScan     sc    = scanTopLevel(lines);
    if (sc.passwordLine < 0)
        return QString();
    return passwordValue(lines.at(sc.passwordLine));
}

RustDeskController::PasswordProbe RustDeskController::probePassword() const
{
    PasswordProbe p;
    const QString path = mainConfigPath();
    p.exists = QFileInfo::exists(path);
    if (!p.exists)
        return p;

    QByteArray bytes;
    if (!readConfigBytes(path, &bytes, nullptr))
        return p;                       // exists=true, readOk=false —— 关键的那种"不知道"
    p.readOk = true;

    const QStringList lines = splitConfigLines(bytes);
    const TopScan     sc    = scanTopLevel(lines);
    if (sc.passwordLine >= 0) {
        p.hasPassword = true;
        p.raw         = passwordValue(lines.at(sc.passwordLine));
    }
    return p;
}

bool RustDeskController::writePasswordPlain(const QString& plain)
{
    if (plain.isEmpty())
        return false;
    // TOML 单引号字符串：内含单引号会写坏文件；反斜杠会被 QString::replace 当反向引用。
    // 外壳生成的密码都是字母数字，直接拒绝这两种字符最省事。
    if (plain.contains(QLatin1Char('\'')) || plain.contains(QLatin1Char('\\')))
        return false;
    return rewriteTopPassword(plain, /*remove=*/false);
}

bool RustDeskController::clearPassword()
{
    return rewriteTopPassword(QString(), /*remove=*/true);
}

// ---- TOML 配置的字节级读写（声明与"为什么"在 RustDeskController.h）----

bool RustDeskController::readConfigBytes(const QString& path, QByteArray* out, QString* why)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (why)
            *why = TR("读不到");
        return false;
    }
    *out = f.readAll();
    f.close();

    // ⚠⚠ **必须把解码结果落到一个 QString 上**，否则下面这个"检查 UTF-8"的守卫是**空操作**。
    //   QStringDecoder::operator() 返回的是一个**惰性代理**（Qt 源码里就是
    //   `template<typename T> struct EncodedData { operator QString() const { …真解码… } }`），
    //   只有**转成 QString 的那一刻**才真去解码。所以 `dec(bytes);` 这一句什么也没干：
    //   能编译、不报错、`hasError()` 永远 false —— 文件坏没坏它都说"好"。
    //   （实测过 -O0 和 -O2 行为一致，**不是**优化器把调用丢了，是语言层面就惰性。）
    //   这个坑是 B22 抓到的：当时把这段从 PeerPrefs 挪过来，顺手把"赋值给 probe"
    //   改成了"直接调用"，守卫就哑了，而**只有真拿坏文件去跑那一项断言**才看得出来。
    QStringDecoder dec(QStringDecoder::Utf8);
    const QString   decoded = dec(*out);   // ← 这一行是"真的解码"，别删
    Q_UNUSED(decoded);
    if (dec.hasError()) {
        if (why)
            *why = TR("不是合法的 UTF-8（%1 字节，至少有一处坏字节）—— 不认它就不会动它").arg(out->size());
        return false;
    }
    return true;
}

bool RustDeskController::writeConfigBytes(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    // 只写进去一部分（磁盘满）也算失败，别当成功
    const bool ok = f.write(bytes) == bytes.size();
    f.close();
    return ok;
}

QString RustDeskController::detectEol(const QByteArray& bytes)
{
    return bytes.contains("\r\n") ? QStringLiteral("\r\n") : QStringLiteral("\n");
}

QStringList RustDeskController::splitConfigLines(const QByteArray& bytes)
{
    QStringList lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'));
    for (QString& l : lines) {
        if (l.endsWith(QLatin1Char('\r')))
            l.chop(1);
    }
    return lines;
}

// 备份 → 深度感知改写 → 复验 → 失败回滚。见文件顶部 scanTopLevel 的注释：
// 这里绝不能用字符串拼接把 password 追加到文件末尾，那会落进末尾的 [keys_confirmed]
// 表里，顶层就没有 password 了。
bool RustDeskController::rewriteTopPassword(const QString& plain, bool remove)
{
    const QString path = mainConfigPath();
    if (!QFile::exists(path))
        return false;                 // RustDesk 至少跑过一次才会生成配置

    // 按字节读（不带 Text），并且**拒绝非 UTF-8**：这份文件里是 enc_id/key_pair，
    // 拿 QString::fromUtf8 把坏字节变成 U+FFFD 再写回去，等于亲手把身份弄坏。
    QByteArray original;
    if (!readConfigBytes(path, &original, nullptr))
        return false;

    // 写前备份。这份文件里的 enc_id/key_pair 是本机身份，一旦写成非法 TOML，
    // RustDesk 会静默丢弃身份重新注册、把 ID 换掉——备份是唯一的退路。
    const QString backup = path + QStringLiteral(".bak");
    QFile::remove(backup);
    if (!QFile::copy(path, backup))
        return false;

    const QStringList lines = splitConfigLines(original);
    const QString     eol   = detectEol(original);
    const TopScan     sc    = scanTopLevel(lines);
    QStringList       top   = lines.mid(0, sc.regionEnd);
    const QStringList rest  = lines.mid(sc.regionEnd);

    if (sc.passwordLine >= 0)
        top.removeAt(sc.passwordLine);
    while (!top.isEmpty() && top.last().trimmed().isEmpty())
        top.removeLast();
    if (!remove)
        top << QStringLiteral("password = '%1'").arg(plain);
    top << QString();

    QString out = (top + rest).join(eol);
    // 收尾用**原风格**的行尾：原文是 CRLF 的话这里补 "\n" 会让最后一行跟别的行不一样
    // （TOML 两种都收，但没理由给这份含身份密钥的文件加个混行尾的尾巴）。
    // ⚠ 走 writeConfigBytes，**不要**自己开 QIODevice::Text —— 它把每个 \n 写成 \r\n，
    //   整份 RustDesk.toml 每一行都被改掉。
    if (!out.endsWith(eol))
        out += eol;

    if (!writeConfigBytes(path, out.toUtf8()))
        return false;

    // 写完立刻复验：必须能在**顶层**读回期望值，否则回滚，绝不留下半坏的文件。
    const bool ok = remove ? readPasswordRaw().isEmpty() : (readPasswordRaw() == plain);
    if (!ok) {
        // 从**内存里那份原字节**写回，不靠"先删再 copy 备份"：后者有个窗口，删掉了而 copy
        // 失败时这份含身份密钥的文件就真没了（"ID 被换掉"正是这么来的）。备份仅作退路。
        if (!writeConfigBytes(path, original) && !QFile::copy(backup, path))
            return false;              // 都失败：备份还在盘上，等用户手工恢复
        return false;
    }
    return true;
}

QString RustDeskController::getOption(const QString& key) const
{
    QByteArray bytes;
    if (!readConfigBytes(optionConfigPath(), &bytes, nullptr))
        return QString();
    // 拼回成 \n 结尾：下面的正则用 `[ \t]*$` 收尾，而 CRLF 文件里那个 \r 会让它匹配不上
    // （Qt 的 $ 认的是 \n 之前，不认 \r\n 之前）。反正只读，规格化成一种就好。
    const QString s = splitConfigLines(bytes).join(QLatin1Char('\n'));

    const QRegularExpression re(
        QStringLiteral(R"(^[ \t]*%1[ \t]*=[ \t]*'(.*)'[ \t]*$)")
            .arg(QRegularExpression::escape(key)),
        QRegularExpression::MultilineOption);
    const auto m = re.match(s);
    return m.hasMatch() ? m.captured(1) : QString();
}

namespace {
// [options] 表头的行号；-1 = 文件里没有这一段。
// 同样按括号深度判，理由和 scanTopLevel 一样：值里可能带方括号。
int optionsHeaderLine(const QStringList& lines)
{
    int  depth = 0;
    bool inStr = false;
    for (int i = 0; i < lines.size(); ++i) {
        if (depth == 0 && lines.at(i).trimmed() == QLatin1String("[options]"))
            return i;
        for (const QChar ch : lines.at(i)) {
            if (ch == QLatin1Char('\'')) {
                inStr = !inStr;
            } else if (!inStr) {
                if (ch == QLatin1Char('['))
                    ++depth;
                else if (ch == QLatin1Char(']'))
                    --depth;
            }
        }
    }
    return -1;
}

// 从 from 行（不含）起，下一个顶层表头的位置；没有则返回 lines.size()。
// 必须限定范围：RustDesk2.toml 里别的表也可能有同名键，整文件替换会写串。
int optionsTableEnd(const QStringList& lines, int from)
{
    int  depth = 0;
    bool inStr = false;
    for (int i = from + 1; i < lines.size(); ++i) {
        const QString s = lines.at(i).trimmed();
        if (depth == 0 && s.startsWith(QLatin1Char('[')) && s.endsWith(QLatin1Char(']'))
            && !s.contains(QLatin1Char('=')))
            return i;
        for (const QChar ch : lines.at(i)) {
            if (ch == QLatin1Char('\'')) {
                inStr = !inStr;
            } else if (!inStr) {
                if (ch == QLatin1Char('['))
                    ++depth;
                else if (ch == QLatin1Char(']'))
                    --depth;
            }
        }
    }
    return lines.size();
}
} // namespace

bool RustDeskController::setOption(const QString& key, const QString& value)
{
    return setServerProfile({ OptionEdit { key, value } }, nullptr);
}

bool RustDeskController::setServerProfile(const QVector<OptionEdit>& edits, QString* err)
{
    auto fail = [err](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };

    if (edits.isEmpty())
        return fail(TR("没有要写的配置项"));

    // ---- 1. 先把**所有**参数校验一遍，别写了一半才发现第三个值非法 ----
    for (const OptionEdit& e : edits) {
        if (e.key.isEmpty())
            return fail(TR("配置项名为空"));
        // 值会被包进单引号，所以它自己不能带单引号或反斜杠 —— 否则写出非法 TOML，
        // RustDesk 加载失败会把整份配置回落到默认值（包括把这个文件里别的东西一起丢掉）。
        if (e.value.contains(QLatin1Char('\'')) || e.value.contains(QLatin1Char('\\')))
            return fail(TR("配置项 %1 的值里有单引号或反斜杠，会写出非法 TOML").arg(e.key));
    }

    const QString path = optionConfigPath();
    if (!QFile::exists(path))
        return fail(TR("读不到 %1（RustDesk 至少得跑过一次）").arg(path));

    QByteArray original;
    QString    why;
    if (!readConfigBytes(path, &original, &why))
        return fail(TR("读不了 %1：%2").arg(path, why));

    QStringList   lines = splitConfigLines(original);
    const QString eol   = detectEol(original);
    if (optionsHeaderLine(lines) < 0)
        return fail(TR("RustDesk2.toml 里没有 [options] 段 —— 不猜位置，宁可不写"));

    // ---- 2. 全部改在内存里 ----
    for (const OptionEdit& e : edits) {
        const int   hdr = optionsHeaderLine(lines);      // 每轮重取：上一轮可能动过行数
        const int   end = optionsTableEnd(lines, hdr);
        const QString newLine = QStringLiteral("%1 = '%2'").arg(e.key, e.value);
        const QRegularExpression re(
            QStringLiteral(R"(^[ \t]*%1[ \t]*=)").arg(QRegularExpression::escape(e.key)));

        int found = -1;
        for (int i = hdr + 1; i < end; ++i) {
            if (re.match(lines.at(i)).hasMatch()) {
                found = i;
                break;
            }
        }

        if (e.value.isEmpty()) {
            // 空值 = 删键（恢复官方档就是这条路）。键本来就不在 → 什么都不用做。
            if (found >= 0) {
                lines.removeAt(found);
                // 删掉一行会留下一个空行；反复切档位就会越攒越多。把连续空行收敛成一行
                // （lines[hdr] 是 "[options]" 表头，永远不是空行，所以循环天然停得住）。
                while (found > hdr && found < lines.size()
                       && lines.at(found).trimmed().isEmpty()
                       && lines.at(found - 1).trimmed().isEmpty())
                    lines.removeAt(found);
            }
            continue;
        }

        if (found >= 0) {
            lines[found] = newLine;
        } else {
            int at = end;                                    // 插在表尾（跳过尾随空行）
            while (at > hdr + 1 && lines.at(at - 1).trimmed().isEmpty())
                --at;
            lines.insert(at, newLine);
        }
    }

    // ---- 3. 备份一次 → 一次写盘 ----
    const QString backup = path + QStringLiteral(".bak");
    QFile::remove(backup);
    if (!QFile::copy(path, backup))
        return fail(TR("备份 %1 失败，为安全起见没有动原文件").arg(path));

    // 同 writePasswordPlain：走 writeConfigBytes，别自己开 QIODevice::Text
    // （那会把整份 RustDesk2.toml 变成 CRLF）。行尾按原文风格拼回去。
    if (!writeConfigBytes(path, lines.join(eol).toUtf8()))
        return fail(TR("写不进 %1（或只写进去一部分）").arg(path));

    // ---- 4. 整体复验；任一不对就整体还原 ----
    for (const OptionEdit& e : edits) {
        if (getOption(e.key) != e.value) {
            if (!writeConfigBytes(path, original))
                return fail(TR("写入后复验 %1 没对上，回滚也失败了 —— 备份在 %2").arg(e.key, backup));
            return fail(TR("写入后复验 %1 没对上，已整体回滚").arg(e.key));
        }
    }
    return true;
}

// ---- 发起连接（M2）----

QStringList RustDeskController::connectArgs(const QString& target, const QString& password)
{
    QStringList a;
    a << QStringLiteral("--connect") << target;
    // 空密码**绝不**追加 --password：传 `--password ""` 会让 RustDesk 拿空串去认证、
    // 直接判失败，用户就再也没机会手输密码了。留空的本意是"让 RustDesk 自己弹窗问"。
    if (!password.isEmpty())
        a << QStringLiteral("--password") << password;
    return a;
}

QProcess* RustDeskController::spawnConnectClient(const QString& target, const QString& password,
                                                 QObject* parent, QString* err)
{
    const QString exe = cliExePath();
    if (exe.isEmpty()) {
        if (err)
            *err = TR("找不到 bin/rustdesk.exe");
        return nullptr;
    }
    if (target.isEmpty()) {
        if (err)
            *err = TR("没有可连的地址（ID 和 tailnet 主机名都是空的）");
        return nullptr;
    }

    auto* p = new QProcess(parent);          // parent 管生命周期，调用方不用自己删
    p->setProgram(exe);
    p->setArguments(connectArgs(target, password));
    p->start();
    if (!p->waitForStarted(5000)) {
        if (err)
            *err = TR("拉不起连接客户端：%1").arg(p->errorString());
        p->deleteLater();
        return nullptr;
    }
    return p;
}

#include "TsnetBridge.h"
#include "I18n.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QProcessEnvironment>

// ── 路径探测 ─────────────────────────────────────────────────────────────────
// 跟 RustDeskController::cliExePath() 同一套：部署后 pmrelay.exe 在 <exe目录>/bin/，
// 开发时外壳 exe 躺在 build/ 而 bin/ 在工程根，所以两个位置都探。
QString TsnetBridge::findRelayExe()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/bin/pmrelay.exe"),
        appDir + QStringLiteral("/../bin/pmrelay.exe"),
    };
    for (const QString& c : candidates) {
        if (QFileInfo::exists(c))
            return QDir::cleanPath(c);
    }
    return {};
}

QString TsnetBridge::relayPath() const
{
    return m_relayPath.isEmpty() ? findRelayExe() : m_relayPath;
}

QString TsnetBridge::localAddress() const
{
    return QStringLiteral("127.0.0.1:%1").arg(m_localPort);
}

// ── 生命周期 ─────────────────────────────────────────────────────────────────
TsnetBridge::TsnetBridge(QObject* parent) : QObject(parent) {}

TsnetBridge::~TsnetBridge()
{
    stop();
}

bool TsnetBridge::isRunning() const
{
    return m_runningForTest || (m_proc && m_proc->state() != QProcess::NotRunning);
}

// 状态目录：不放 %APPDATA%，跟"绿色免安装"一致，落在 exe 同级。
QString TsnetBridge::stateDir() const
{
    if (!m_stateDir.isEmpty())
        return m_stateDir;
    return QCoreApplication::applicationDirPath() + QStringLiteral("/data/tsnet");
}

QString TsnetBridge::defaultPeerFile() const
{
    return stateDir() + QStringLiteral("/peer.txt");
}

// 把"这次要连谁"写给 pmrelay（serve 模式每 2 秒重读一次）。
// 覆盖写，末尾一个换行：外壳可能来不及写完就被读走，pmrelay 那边 trim 过，不会出问题。
bool TsnetBridge::setCurrentPeer(const QString& peer)
{
    const QString path = m_peerFile.isEmpty() ? defaultPeerFile() : m_peerFile;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(peer.trimmed().toUtf8());
    f.write("\n");
    return true;
}

QStringList TsnetBridge::buildArgs(Mode m) const
{
    const QString dir = stateDir();

    const QString modeName = m == Mode::Expose ? QStringLiteral("expose")
                             : m == Mode::Serve ? QStringLiteral("serve")
                                                : QStringLiteral("connect");

    QStringList a{QStringLiteral("-mode"), modeName,
                  QStringLiteral("-dir"), dir,
                  QStringLiteral("-hostname"),
                  m_hostname.isEmpty() ? QStringLiteral("puremote") : m_hostname,
                  // 外壳一死，桥接不该把 tailnet 节点一直挂着。所以把自己的 PID 报给它，
                  // 它用 OpenProcess(SYNCHRONIZE)+WaitForSingleObject 盯着（Windows 标准做法）。
                  //
                  // 曾经用的是"关掉 stdin，它读到 EOF 就退"，但 QProcess 的 stdin 会
                  // **立刻** EOF，pmrelay 起手就自杀，而且退出码 0 —— 外壳侧连错误都看不到，
                  // 只表现为"桥接永远不 ready"。同一个 pmrelay 从真管道跑（sleep 20 | pmrelay）
                  // 却能撑满 19 秒，说明坏的是 QProcess 的 stdin，不是那套逻辑本身。
                  // 孤儿防护不能建在一个会误报的信号上。
                  QStringLiteral("-parent-pid"),
                  QString::number(QCoreApplication::applicationPid())};

    if (m == Mode::Expose || m == Mode::Serve) {
        a << QStringLiteral("-listen") << QString::number(m_listenPort)
          << QStringLiteral("-target")
          << QStringLiteral("127.0.0.1:%1").arg(m_targetPort);
        if (m == Mode::Serve) {
            a << QStringLiteral("-local") << localAddress() << QStringLiteral("-peerfile")
              << (m_peerFile.isEmpty() ? defaultPeerFile() : m_peerFile);
        }
    } else {
        a << QStringLiteral("-local") << localAddress() << QStringLiteral("-peer") << m_peer
          << QStringLiteral("-peerport") << QString::number(m_listenPort);
    }
    if (m_verbose)
        a << QStringLiteral("-verbose");
    return a;
}

bool TsnetBridge::start(Mode m)
{
    stop();

    if (m == Mode::Off)
        return false;

    const QString exe = relayPath();
    if (exe.isEmpty() || !QFileInfo::exists(exe)) {
        m_lastError = TR("找不到 pmrelay.exe");
        emit bridgeError(QStringLiteral("relay-missing"), m_lastError);
        return false;
    }
    if (m == Mode::Connect && m_peer.isEmpty()) {
        m_lastError = TR("connect 模式必须指定对端");
        emit bridgeError(QStringLiteral("peer-missing"), m_lastError);
        return false;
    }

    // 复位解析状态
    m_stdoutBuf.clear();
    m_stderrBuf.clear();
    m_tailnetIp.clear();
    m_peerRelay.clear();
    m_peerDirect.clear();
    m_peerName.clear();
    m_loginUrl.clear();
    m_lastError.clear();
    m_fatalError.clear();
    m_readyDetail.clear();
    m_peerOnline = false;
    m_peerSeen   = false;

    m_proc = new QProcess(this);
    m_proc->setWorkingDirectory(QFileInfo(exe).absolutePath());

    // authkey 走环境变量：命令行在任务管理器里是明文可见的，而这等价于账号凭证。
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!m_authKey.isEmpty())
        env.insert(QStringLiteral("TS_AUTHKEY"), m_authKey);
    m_proc->setProcessEnvironment(env);

    connect(m_proc, &QProcess::readyReadStandardOutput, this, &TsnetBridge::onStdout);
    connect(m_proc, &QProcess::readyReadStandardError, this, &TsnetBridge::onStderr);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) { onFinished(code); });

    m_mode = m;
    m_proc->start(exe, buildArgs(m));
    if (!m_proc->waitForStarted(10000)) {
        m_lastError = TR("pmrelay 启动失败");
        emit bridgeError(QStringLiteral("start-failed"), m_lastError);
        m_proc->deleteLater();
        m_proc = nullptr;
        m_mode = Mode::Off;
        return false;
    }
    return true;
}

void TsnetBridge::stop()
{
    // 先清测试那个"假装在跑"的旗子：**任何人**调 stop() 都该让 isRunning() 变 false，
    // 否则"这次收尾有没有把常驻桥接一起收走"就没法断言（无头测试里没有真进程）。
    m_runningForTest = false;
    m_fatalError.clear();   // 收掉了就别再拿上一次的死因去解释下一次
    if (!m_proc)
        return;
    QProcess* p = m_proc;
    m_proc      = nullptr;
    m_mode      = Mode::Off;

    if (p->state() != QProcess::NotRunning) {
        // 显式停止就是收进程：pmrelay 没有"收到命令自己退"的通道（stdin 不可靠，
        // 见 start() 里的说明），它只在父进程死掉时自己退。
        // tailnet 侧不担心：状态目录留着，下次起来复用同一个节点密钥，还是同一个设备，
        // 不会像 RustDesk 那样被换掉身份。
        p->terminate();
        if (!p->waitForFinished(1500)) {
            p->kill();
            p->waitForFinished(2000);
        }
    }
    p->deleteLater();
    emit stopped();
}

void TsnetBridge::onFinished(int exitCode)
{
    // stop() 会先把 m_proc 置空再收进程，所以走到这里 `m_proc` 还在 = 我们没喊停，
    // 是 pmrelay 自己走的。
    //
    // 不能只看"退出码非 0"：pmrelay 的 Accept 循环出错时是 `return`，退出码 **0**。
    // 老代码会把这种"桥接已经死了"吞掉，界面上还显示着"运行中"。
    if (m_proc && m_lastError.isEmpty()) {
        m_lastError = exitCode == 0
                          ? TR("pmrelay 意外退出（退出码 0）")
                          : TR("pmrelay 退出，代码 %1").arg(exitCode);
        emit bridgeError(QStringLiteral("exited"), m_lastError);
    }
    m_mode = Mode::Off;
}

// ── 协议解析 ─────────────────────────────────────────────────────────────────
// 行格式：PMRELAY <KIND> k=v k=v ...
// 值里不含空格 —— pmrelay 侧对自由文本做了净化（san()），否则 Windows 的错误消息
// 会把解析撑破。
void TsnetBridge::onStdout()
{
    if (!m_proc)
        return;
    m_stdoutBuf += QString::fromUtf8(m_proc->readAllStandardOutput());
    // 只处理完整的行，半行留在缓冲里等下一块
    int nl;
    while ((nl = m_stdoutBuf.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString line = m_stdoutBuf.left(nl);
        m_stdoutBuf.remove(0, nl + 1);
        parseLine(line);
    }
}

void TsnetBridge::onStderr()
{
    if (!m_proc)
        return;
    m_stderrBuf += QString::fromUtf8(m_proc->readAllStandardError());
    int nl;
    while ((nl = m_stderrBuf.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString line = m_stderrBuf.left(nl).trimmed();
        m_stderrBuf.remove(0, nl + 1);
        if (!line.isEmpty())
            emit logLine(line);
    }
}

void TsnetBridge::parseLine(const QString& raw)
{
    const QString line = raw.trimmed();
    if (!line.startsWith(QLatin1String("PMRELAY ")))
        return;

    const QStringList tok = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tok.size() < 2)
        return;

    const QString kind = tok.at(1);
    QHash<QString, QString> kv;
    for (int i = 2; i < tok.size(); ++i) {
        const int eq = tok.at(i).indexOf(QLatin1Char('='));
        if (eq > 0)
            kv.insert(tok.at(i).left(eq), tok.at(i).mid(eq + 1));
    }

    if (kind == QLatin1String("READY")) {
        m_tailnetIp = kv.value(QStringLiteral("ip"));
        const QString modeName = kv.value(QStringLiteral("mode"));
        if (modeName == QLatin1String("expose")) {
            m_readyDetail = TR("tailnet %1:%2 → 本机 127.0.0.1:%3")
                                .arg(m_tailnetIp, kv.value(QStringLiteral("listen")),
                                     kv.value(QStringLiteral("target")).section(QLatin1Char(':'), -1));
        } else if (modeName == QLatin1String("serve")) {
            m_readyDetail = TR("%1 已在 tailnet 上（%2:%3）；本机 %4 → 对端")
                                .arg(kv.value(QStringLiteral("hostname")), m_tailnetIp,
                                     kv.value(QStringLiteral("listen")),
                                     kv.value(QStringLiteral("local")));
        } else {
            m_readyDetail = TR("%1 → %2（经 tailnet）")
                                .arg(kv.value(QStringLiteral("local")),
                                     kv.value(QStringLiteral("peer")));
        }
        emit ready(modeName, m_readyDetail);
    } else if (kind == QLatin1String("PEER")) {
        const bool    online = kv.value(QStringLiteral("online")) == QLatin1String("true");
        const QString name   = kv.value(QStringLiteral("name"));
        // ⚠ 判"变化"必须连**名字**一起判。常驻桥接是同一个进程连着给不同的对端干活：
        //   上一个对端在线（online=true）、这次要连的那个也刚上线（online=true）——
        //   只看 online 的话这一声根本不发出去，外壳就会一直等着一个**已经在线**的对端。
        const bool changed = !m_peerSeen || online != m_peerOnline || name != m_peerName;
        // 名字和 relay/direct 每次都记：槽里要能判断"这一行说的是不是我在等的那个"
        m_peerName   = name;
        m_peerRelay  = kv.value(QStringLiteral("relay"));
        m_peerDirect = kv.value(QStringLiteral("direct"));
        m_peerOnline = online;
        if (changed) {
            m_peerSeen = true;
            emit peerStateChanged(online, m_peerRelay, m_peerDirect);
        }
    } else if (kind == QLatin1String("CONN")) {
        emit connectionSeen(kv.value(QStringLiteral("from")));
    } else if (kind == QLatin1String("CONNECTED")) {
        emit connectionSeen(kv.value(QStringLiteral("peer")));
    } else if (kind == QLatin1String("LOGIN-URL")) {
        // 行是 "PMRELAY LOGIN-URL https://login.tailscale.com/a/xxx" —— URL 里只有 '/'，
        // 没有空格，所以它整体就是第三个 token，不带 key=。
        m_loginUrl = tok.size() > 2 ? tok.at(2) : QString();
        if (!m_loginUrl.isEmpty())
            emit loginUrlAvailable(m_loginUrl);
    } else if (kind == QLatin1String("ERROR")) {
        m_lastError = kv.value(QStringLiteral("detail"));
        emit bridgeError(kv.value(QStringLiteral("what")), m_lastError);
    } else if (kind == QLatin1String("FATAL")) {
        m_lastError = kv.value(QStringLiteral("detail"));
        // FATAL = pmrelay 自己都不打算继续了（密钥不对 / 监听失败 / 参数错）。
        // 单独记一份，好让外壳一眼分清"这台机器根本没登上 tailnet"和"刚才那次转发没连上"。
        m_fatalError = m_lastError;
        emit bridgeError(kv.value(QStringLiteral("what")), m_lastError);
    }
}

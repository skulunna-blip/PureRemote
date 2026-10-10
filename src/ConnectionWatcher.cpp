#include "ConnectionWatcher.h"

#include "I18n.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTimer>

namespace {
constexpr int kPollMs = 1500;

// 用来认"文件被换掉了"的文件头字节数。真日志第一行的形状是（用户名已隐去）：
//   [2026-10-09 18:33:48.979934 +08:00] INFO [src\core_main.rs:398] start --server with user tester
// 光时间戳（精确到微秒）就足够区分两次启动。
constexpr int kSigBytes = 128;

// 被控端侧的连接建立标记。**必须用源文件路径限定**：客户端侧也会打一行几乎一样的
// `src\client.rs:NNNN] peer info supported_encoding`，自连的时候两条同时出现，
// 只匹配后半句会把"我自己发起的连接"也当成"有人连进来"。
const QRegularExpression& connectRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"(INFO \[src[\\/]server[\\/]connection\.rs:\d+\] peer info supported_encoding)"));
    return re;
}

// 会话**结束**（2026-10-09 在一份真日志 %APPDATA%\RustDesk\log\server\
// rustdesk_r2026-10-09_18-33-48.log 里逐行读出来的，一次真实会话的完整生命周期。
// **对端公网 IP 换成了文档保留地址 203.0.113.0/24**，其余逐字节照抄）：
//
//   18:30:15.327 DEBUG [src\server\connection.rs:1498] #526 Connection opened from 203.0.113.7:7299.
//   18:30:15.338 INFO  [src\server\connection.rs:2117] peer info supported_encoding: ...   ← connectRe 匹配这条
//   18:30:21.356 INFO  [src\server\connection.rs:2847] receive close reason:
//   18:30:21.356 INFO  [src\server\connection.rs:5249] #526 Connection closed: Peer close   ← 这条
//   18:30:21.356 INFO  [src\server\connection.rs:1237] #526 connection loop exited
//
// 两点必须记住：
//  · **必须限定 src\server\connection.rs** —— 客户端侧 src\client.rs 也会打
//    "Connection closed: ..."（连不上目标时就是那条）。
//  · `#N ` 那个连接号在实测里一直有，但**不能强依赖它**（换个版本可能没有），
//    所以写成可选；真正定性的是 `[src\server\connection.rs:NNNN] ... Connection closed:`。
//  · 原因里 `connection manager` 那种**不是对端走人**，是 RustDesk 自己在关停
//    （真日志里有 `#268 Connection closed: connection manager`）。这种不能算"有人连过并离开"，
//    否则外壳自己的重启动就会被记成一次会话结束。判据写在下面的 isPeerDisconnect。
const QRegularExpression& disconnectRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"(INFO \[src[\\/]server[\\/]connection\.rs:\d+\] (?:#\d+ )?Connection closed:)"));
    return re;
}

const QLatin1String kConnectionManagerClose("Connection closed: connection manager");

bool isPeerDisconnect(const QString& line)
{
    if (!disconnectRe().match(line).hasMatch())
        return false;
    return !line.contains(kConnectionManagerClose);
}

// 读文件头若干字节当签名。文件短于 kSigBytes 就有多少读多少 —— 比较时按"前缀"比，
// 这样"文件刚建出来只有 20 字节、过一会儿长到 4000 字节"不会被误判成换文件。
QByteArray readHead(QFile& f, int n)
{
    const qint64 keep = f.pos();
    if (!f.seek(0)) {
        f.seek(keep);
        return QByteArray();
    }
    const QByteArray head = f.read(n);
    f.seek(keep);
    return head;
}
} // namespace

ConnectionWatcher::ConnectionWatcher(QObject* parent)
    : QObject(parent)
{
    setLogPaths(logPaths());
    m_timer = new QTimer(this);
    m_timer->setInterval(kPollMs);
    connect(m_timer, &QTimer::timeout, this, &ConnectionWatcher::poll);
}

QString ConnectionWatcher::currentLogPath()
{
    const QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty())
        return QString();
    return QDir(appData).filePath(QStringLiteral("RustDesk/log/rustdesk_rCURRENT.log"));
}

QStringList ConnectionWatcher::logPaths()
{
    const QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty())
        return {};
    const QDir base(QDir(appData).filePath(QStringLiteral("RustDesk/log")));
    // 顺序有讲究：--server（外壳拉起的那份）排在前面，它是常态。
    return { base.filePath(QStringLiteral("server/rustdesk_rCURRENT.log")),
             base.filePath(QStringLiteral("rustdesk_rCURRENT.log")) };
}

void ConnectionWatcher::setLogPaths(const QStringList& paths)
{
    m_tails.clear();
    for (const QString& p : paths)
        m_tails.append(Tail{ p, -1, QByteArray() });
}

QStringList ConnectionWatcher::watchedPaths() const
{
    QStringList out;
    for (const Tail& t : m_tails)
        out << t.path;
    return out;
}

void ConnectionWatcher::requestEnabled(QObject* who, bool on)
{
    if (!who)
        return;
    if (on) {
        if (m_requesters.contains(who))
            return;
        m_requesters.insert(who);
        // 谁走了就自动放手，免得到时候指针变野的
        connect(who, &QObject::destroyed, this, [this](QObject* o) {
            if (m_requesters.remove(o))
                applyEnablement();
        });
    } else if (!m_requesters.remove(who)) {
        return;
    }
    applyEnablement();
}

void ConnectionWatcher::applyEnablement()
{
    const bool on = !m_requesters.isEmpty();
    if (on == m_timer->isActive())
        return;
    if (on) {
        prime();                 // 开启这一刻之前的连接不算数
        m_timer->start();
    } else {
        m_timer->stop();
        m_primeOnNextPoll = false;
    }
}

void ConnectionWatcher::prime()
{
    // 不在这儿抓快照，而是让第一次 poll 去抓 —— 从 prime() 到第一次 poll 之间
    // 日志文件可能刚被 RustDesk 轮转出来，提前抓会得到"文件还不存在 → 0"，
    // 于是把整份历史当成新内容重放一遍。
    m_primeOnNextPoll = true;
}

void ConnectionWatcher::poll()
{
    const QStringList paths = watchedPaths();
    if (paths.isEmpty()) {
        emit statusMessage(TR("读不到 APPDATA，没法盯 RustDesk 日志"));
        return;
    }

    const bool priming = m_primeOnNextPoll;
    m_primeOnNextPoll = false;

    for (int i = 0; i < paths.size(); ++i) {
        const QString path = paths.at(i);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            // RustDesk 还没跑过 / 日志还没建。不是错误，下次再看。
            continue;
        }

        const qint64 size = f.size();
        if (priming) {
            // 首次：直接把游标推到末尾，历史连接一律不报
            if (i < m_tails.size()) {
                m_tails[i].pos = size;
                m_tails[i].sig = readHead(f, kSigBytes);
            }
            f.close();
            continue;
        }
        if (i >= m_tails.size())
            continue;
        Tail& t = m_tails[i];

        // ⚠ **先认文件，再看位置**（2026-10-09 加，为「用完即换」铺路）。
        //
        // RustDesk 每次启动都是"把 CURRENT **改名**成 rustdesk_r<时刻>.log、再建一个新的
        // CURRENT"（实测：server\ 下 18-33-48 那份装着 18:30 的会话，名字却是重启那一刻）。
        // 于是"当前看的这个文件"会被整份换掉，而**旧游标还留着**。以前只靠 `size < pos`
        // 判轮转，在下面这种情形下会失手：新文件涨得比旧 pos 快（旧日志很短、或者机器刚从
        // 睡眠里醒来、或者我们这一轮被别的事拖住没来得及轮询）→ `size > pos` 成立 →
        // 于是**从新文件的偏移 pos 处开始读**，前面的内容被跳过。以前只在"外壳自己重启
        // RustDesk"时才碰得上，加上「用完即换」之后这是常规操作，必须堵死。
        const QByteArray head = readHead(f, kSigBytes);
        if (t.sig.isEmpty()) {
            // 头一回看见这个文件 → 从末尾开始，不把它的历史当成新事件
            t.sig = head;
            t.pos = size;
            f.close();
            continue;
        }
        if (!head.startsWith(t.sig)) {
            // 换文件了。这是一份**我们从来没看过**的内容，所以从 0 读是安全的、
            // 也是正确的 —— 它不会重放任何我们已经报过的行（旧文件是另一个文件）。
            // 早先那条"轮转后一律不补报"的规矩在这里升级了：真正会重放的只有
            // "同一个文件被原地截断重写"，而那种情况文件的头签名会变，下面 size<pos
            // 那一支仍然兜着。
            t.sig = head;
            t.pos = 0;
        }

        if (size < t.pos) {
            // 同一个文件被就地截短了（不是换文件）。旧内容不补报 —— 那是上一轮的事。
            t.pos = size;
            f.close();
            continue;
        }
        if (size == t.pos) {
            f.close();
            continue;
        }

        if (!f.seek(t.pos)) {
            f.close();
            continue;
        }
        const QByteArray chunk = f.read(size - t.pos);
        f.close();

        // 只处理**完整行**：RustDesk 可能正写到一半，切开的半行会让标记匹配不上。
        // 位置只推进到最后一个换行符，剩下的留给下一轮。
        const int lastNl = chunk.lastIndexOf('\n');
        if (lastNl < 0)
            continue;
        t.pos += lastNl + 1;

        const QList<QByteArray> lines = chunk.left(lastNl).split('\n');
        for (const QByteArray& raw : lines) {
            if (raw.isEmpty())
                continue;
            const QString line = QString::fromUtf8(raw);
            if (connectRe().match(line).hasMatch())
                emit remoteConnected();
            if (isPeerDisconnect(line))
                emit remoteDisconnected();
        }
    }
}

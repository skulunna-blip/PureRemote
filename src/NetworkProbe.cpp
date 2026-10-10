#include "NetworkProbe.h"

#include "AppConfig.h"
#include "I18n.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QHostInfo>
#include <QMap>
#include <QMetaObject>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>

namespace {

constexpr int kTcpTimeoutMs = 4000;
constexpr int kStunTimeoutMs = 3000;
constexpr int kTcpRounds = 3;
constexpr int kPathPollMs = 1500;

// 官方硬编码兜底（源码 src/common.rs: RENDEZVOUS_SERVERS）。
// rs-ny 是唯一一个在国内能解析出来的官方节点（rs-cn/rs-sg/rs-jp 全部 NXDOMAIN，实测）。
const QString kOfficialSignal = QStringLiteral("rs-ny.rustdesk.com");
constexpr quint16 kOfficialSignalPort = 21116;   // hbbs 信令 / ID 注册
constexpr quint16 kOfficialRelayPort  = 21117;   // hbbr 中继

// 少而稳的几个：miwifi 只回 0x0001（plain MAPPED-ADDRESS），其余回 0x0020，
// 所以两种属性都得解析 —— 这是实测出来的，不是照着 RFC 抄的。
const QStringList& stunServers()
{
    static const QStringList s = {
        QStringLiteral("stun.miwifi.com"),        // 国内，只回 plain MAPPED-ADDRESS
        QStringLiteral("stun.chat.bilibili.com"), // 国内
        QStringLiteral("stun.hitv.com"),          // 国内
        QStringLiteral("stun.l.google.com"),
        QStringLiteral("stun.cloudflare.com"),
        QStringLiteral("stun.nextcloud.com"),
    };
    return s;
}

// ---- STUN ----

struct StunAnswer
{
    bool    ok = false;
    QString ip;
    quint16 port = 0;
    QString err;
};

QByteArray buildStunRequest(quint32* txnIdOut)
{
    QByteArray pkt(20, '\0');
    // Message Type = Binding Request，Length = 0（没有属性）
    pkt[0] = 0x00; pkt[1] = 0x01;
    pkt[2] = 0x00; pkt[3] = 0x00;
    // Magic Cookie 0x2112A442
    pkt[4] = char(0x21); pkt[5] = char(0x12);
    pkt[6] = char(0xA4); pkt[7] = char(0x42);
    // 12 字节事务 ID（随机）
    QByteArray txn(12, '\0');
    for (int i = 0; i < 12; ++i)
        txn[i] = char(QRandomGenerator::global()->bounded(256));
    pkt.replace(8, 12, txn);
    if (txnIdOut)
        *txnIdOut = 0;
    return pkt;
}

// 解析 Binding Success Response。
//
// ⚠ **必须校验 Message Type == 0x0101**。这不是防御性代码 —— 第一版把 0x0101 写成了
//   0x0001，结果 7 个服务器全被判成"坏包"，而外层逻辑又把"零个成功"当成了
//   "所有服务器都答了且端口相同" → 假阳性打印出 "Cone NAT"。全失败能伪装成好结果，
//   这种 bug 肉眼根本看不出来，所以类型检查是**功能正确性**的一部分。
StunAnswer parseStunResponse(const QByteArray& pkt)
{
    StunAnswer a;
    if (pkt.size() < 20) {
        a.err = TR("短包（%1 字节）").arg(pkt.size());
        return a;
    }
    const quint16 type = (quint8(pkt[0]) << 8) | quint8(pkt[1]);
    if (type != 0x0101) {
        a.err = TR("不是一个 Binding Success Response（type=0x%1）")
                    .arg(type, 4, 16, QLatin1Char('0'));
        return a;
    }
    const quint16 bodyLen = (quint8(pkt[2]) << 8) | quint8(pkt[3]);
    const int     end     = qMin<int>(pkt.size(), 20 + bodyLen);

    int off = 20;
    while (off + 4 <= end) {
        const quint16 atype = (quint8(pkt[off]) << 8) | quint8(pkt[off + 1]);
        const quint16 alen  = (quint8(pkt[off + 2]) << 8) | quint8(pkt[off + 3]);
        off += 4;
        if (off + alen > end)
            break;

        if ((atype == 0x0020 || atype == 0x0001) && alen >= 8) {
            const quint8  family = quint8(pkt[off + 1]);
            if (family == 0x01) {
                quint16 port = (quint8(pkt[off + 2]) << 8) | quint8(pkt[off + 3]);
                quint32 ip   = (quint8(pkt[off + 4]) << 24) | (quint8(pkt[off + 5]) << 16)
                             | (quint8(pkt[off + 6]) << 8) | quint8(pkt[off + 7]);
                if (atype == 0x0020) {
                    // XOR-MAPPED-ADDRESS：端口和 magic cookie 的高 16 位异或，IP 与整个 cookie 异或
                    port ^= 0x2112;
                    ip   ^= 0x2112A442u;
                }
                a.ok   = true;
                a.ip   = QHostAddress(ip).toString();
                a.port = port;
                return a;
            }
        }
        // 属性按 4 字节对齐
        off += alen + ((4 - (alen % 4)) % 4);
    }
    a.err = TR("没有 MAPPED-ADDRESS / XOR-MAPPED-ADDRESS 属性");
    return a;
}

// 用**同一个** socket 问一个 STUN 服务器。同一个 socket 是 NAT 判定的前提：
// 换了 socket 就换了本地端口，映射端口的比较也就没意义了。
StunAnswer stunQuery(QUdpSocket& sock, const QString& server, quint16 port = 3478)
{
    StunAnswer a;
    const QHostInfo hi = QHostInfo::fromName(server);
    if (hi.error() != QHostInfo::NoError || hi.addresses().isEmpty()) {
        a.err = TR("DNS 解析失败");
        return a;
    }
    QHostAddress dest;
    for (const QHostAddress& ad : hi.addresses()) {
        if (ad.protocol() == QAbstractSocket::IPv4Protocol) { dest = ad; break; }
    }
    if (dest.isNull()) {
        a.err = TR("没有 IPv4 地址");
        return a;
    }

    const QByteArray req = buildStunRequest(nullptr);
    if (sock.writeDatagram(req, dest, port) != req.size()) {
        a.err = TR("发送失败");
        return a;
    }

    QElapsedTimer t;
    t.start();
    while (t.elapsed() < kStunTimeoutMs) {
        // **必须夹住剩下的毫秒数**：waitForReadyRead 收到负数会当成"无限等"，
        // 那这一轮体检就挂死在这一个服务器上了。
        const int left = kStunTimeoutMs - int(t.elapsed());
        if (left <= 0)
            break;
        if (!sock.waitForReadyRead(left))
            break;
        while (sock.hasPendingDatagrams()) {
            // pendingDatagramSize() **可能是 -1**（没有待读数据报时）。拿它去 resize
            // 就是 resize(-1)：QByteArray 变成非法状态，data() 为空、size() 为负，
            // 再喂给 readDatagram 就是往负数长度里写。必须先夹住。
            const qint64 pend = sock.pendingDatagramSize();
            if (pend < 0)
                continue;
            QByteArray buf;
            buf.resize(int(pend) + 1);          // +1：0 长度数据报也能拿到非空指针
            QHostAddress from;
            quint16      fromPort = 0;
            const qint64 n = sock.readDatagram(buf.data(), buf.size(), &from, &fromPort);
            if (n <= 0)
                continue;
            buf.truncate(int(n));
            const StunAnswer r = parseStunResponse(buf);
            if (r.ok)
                return r;
            a.err = r.err;   // 记下最后一个错，全失败时能看出为什么
        }
    }
    if (a.err.isEmpty())
        a.err = TR("超时无应答（UDP 被拦，或该服务器不回）");
    return a;
}

// ---- TCP 延迟 ----

struct TcpAnswer
{
    bool ok = false;
    int  bestMs = 0;
    int  medMs  = 0;
    int  worstMs = 0;
    QString err;
};

TcpAnswer tcpRtt(const QString& host, quint16 port)
{
    TcpAnswer a;
    QList<int> samples;
    for (int i = 0; i < kTcpRounds; ++i) {
        QTcpSocket s;
        QElapsedTimer t;
        t.start();
        s.connectToHost(host, port);
        if (!s.waitForConnected(kTcpTimeoutMs)) {
            a.err = s.errorString();
            s.abort();
            continue;
        }
        samples << int(t.elapsed());
        s.abort();
    }
    if (samples.isEmpty())
        return a;
    std::sort(samples.begin(), samples.end());
    a.ok      = true;
    a.bestMs  = samples.first();
    a.medMs   = samples.at(samples.size() / 2);
    a.worstMs = samples.last();
    return a;
}

// ---- 路径标记（M4.3）----
//
// 这一行是 RustDesk 自己打的，比猜正则可靠。**2026-10-09 把本机所有日志全量扫过一遍**，
// 真实世界一共只出现这四种形态：
//   INFO [src\client.rs:1187] 269.8414ms used to establish Relay connection
//   INFO [src\client.rs:1187] 1.0086594s used to establish WebRTC connection
//   INFO [src\client.rs:1566] 78.8525ms  used to establish WebRTC connection with UDP+WebRTC punch
//   INFO [src\client.rs:1566] 539µs      used to establish TCP connection with TCP punch   ← 别漏这个
//
// 最后一种是这台机器上最常见的（国内出口 Symmetric，打洞后走 TCP）。第一版按
// "含 Relay 就是中继、含 WebRTC 就是直连"写，它一条都不匹配 —— 表现是"路径永远不显示"，
// 而且因为 A 段当时喂的是我自己编的行，测试全绿也发现不了。
// 所以判定改成：Relay = 中继，**其余一律直连**（打洞成功才会打出 establish connection，
// 没打洞成功就是 Relay，没有第三种可能）。
// 判定规则跟上面那段注释是同一件事：**Relay = 中继，其余一律直连**。
// 这台机器上最常见的是 `TCP connection with TCP punch`（直连），打洞失败才会打 Relay。
// ⚠ 返回 false 有两种情形（不是路径标记行 / 是 Relay 行）—— 想区分"这行到底匹配没有"
//   要看 pathFromLine 是不是空串，别拿这个 false 当"没匹配"。
bool lineIsDirect(const QString& line)
{
    if (!line.contains(QStringLiteral("used to establish")))
        return false;                 // 不是路径标记行
    return !line.contains(QStringLiteral("Relay connection"));
}

QString pathFromLine(const QString& line)
{
    if (!line.contains(QStringLiteral("used to establish")))
        return QString();
    return lineIsDirect(line) ? TR("连接路径：P2P 直连（打洞成功）")
                              : TR("连接路径：中继转发（打洞没成功，走服务器中转）");
}

// **要盯三个地方**（2026-10-09 实测）：
//   log/              界面实例（用户双击开的那个）
//   log/flutter_ffi/  CLI 拉起的客户端（1.5.0 把 Rust 核心拆成 FFI 子进程了，
//                     --connect 的日志落在这里而不是主日志 —— 而外壳正是这么拉客户端的）
//   log/server/       --server 被控端（路径行不会出现在这儿，但留着不亏）
// 只盯主日志的话，外壳自己发起的连接一条都读不到。
QStringList defaultPathLogs()
{
    const QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty())
        return {};
    const QDir base(QDir(appData).filePath(QStringLiteral("RustDesk/log")));
    return { base.filePath(QStringLiteral("rustdesk_rCURRENT.log")),
             base.filePath(QStringLiteral("flutter_ffi/rustdesk_rCURRENT.log")),
             base.filePath(QStringLiteral("server/rustdesk_rCURRENT.log")) };
}

// 把 "host" 或 "host:port" 拆开
void splitHostPort(const QString& raw, QString* host, quint16* port, quint16 defPort)
{
    const QString s = raw.trimmed();
    const int     c = s.lastIndexOf(QLatin1Char(':'));
    if (c > 0) {
        bool    okP = false;
        const uint p = s.mid(c + 1).toUInt(&okP);
        if (okP && p > 0 && p <= 65535) {
            *host = s.left(c);
            *port = quint16(p);
            return;
        }
    }
    *host = s;
    *port = defPort;
}

} // namespace

NetworkProbe::NetworkProbe(QObject* parent)
    : QObject(parent), m_alive(new QAtomicInt(1))
{
    m_pathTimer = new QTimer(this);
    m_pathTimer->setInterval(kPathPollMs);
    connect(m_pathTimer, &QTimer::timeout, this, &NetworkProbe::pollPath);
}

NetworkProbe::~NetworkProbe()
{
    if (!m_alive)
        return;
    // 先让 worker 知道"别回主线程了"，再等它退。
    // 超时给得比所有探测的最坏总时长（DNS + N×TCP + 6×STUN，实测约 20 秒）宽得多：
    // wait() 只要成功返回，对象就确定还活着，所以 worker 那边那个 m_alive 检查
    // 就没有竞态窗口了 —— 真正的窗口只会在 wait 超时时出现。
    m_alive->storeRelease(0);
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(30000);
    }
}

int NetworkProbe::badCount() const
{
    int n = 0;
    for (const Item& it : m_items) {
        if (it.hard && !it.ok)
            ++n;
    }
    return n;
}

void NetworkProbe::run(const AppConfig& cfg)
{
    if (m_running)
        return;                       // 连点按钮不该排出一队探测
    m_running = true;

    // cfg 按值捕获 —— AppConfig 全是值成员，拷贝安全。这样工作线程完全不碰主线程的对象。
    QSharedPointer<QAtomicInt> alive = m_alive;
    m_thread = QThread::create([this, cfg, alive] {
        QVector<Item> out;

        auto add = [&out](const QString& key, const QString& title, const QString& detail,
                          bool ok, bool hard) {
            Item it;
            it.key = key; it.title = title; it.detail = detail; it.ok = ok; it.hard = hard;
            out.append(it);
        };

        // ---- 1. 官方信令服务器 DNS ----
        {
            const QHostInfo hi = QHostInfo::fromName(kOfficialSignal);
            if (hi.error() != QHostInfo::NoError || hi.addresses().isEmpty()) {
                add(QStringLiteral("dns_official"),
                    TR("解析不了官方服务器 %1").arg(kOfficialSignal),
                    TR("DNS 查询失败：%1\n→ 换 DNS（223.5.5.5 / 119.29.29.29）或者改用自建服务器。")
                        .arg(hi.errorString()),
                    false, true);
            } else {
                QStringList ips;
                for (const QHostAddress& a : hi.addresses())
                    ips << a.toString();
                add(QStringLiteral("dns_official"),
                    TR("官方服务器 %1 解析正常").arg(kOfficialSignal),
                    ips.join(TR("、")),
                    true, true);
            }
        }

        // ---- 2/3. 官方 TCP 端口 ----
        {
            const TcpAnswer sig = tcpRtt(kOfficialSignal, kOfficialSignalPort);
            if (sig.ok) {
                add(QStringLiteral("tcp_official_signal"),
                    TR("官方信令端口连通（%1 ms）").arg(sig.medMs),
                    TR("%1:%2  最快 %3 / 中位 %4 / 最慢 %5 ms")
                        .arg(kOfficialSignal).arg(kOfficialSignalPort)
                        .arg(sig.bestMs).arg(sig.medMs).arg(sig.worstMs),
                    true, true);
            } else {
                add(QStringLiteral("tcp_official_signal"),
                    TR("官方信令端口连不上"),
                    TR("%1:%2  %3\n→ 信令不通就建立不了连接，换自建服务器或检查防火墙。")
                        .arg(kOfficialSignal).arg(kOfficialSignalPort).arg(sig.err),
                    false, true);
            }

            // 中继端口**不算硬失败**：实测本机就是 CONNECTION REFUSED，但这不是用户能修的，
            // 而且只有打洞失败时才会用到它。标成"环境事实"，别让体检栏无脑飘红。
            const TcpAnswer rly = tcpRtt(kOfficialSignal, kOfficialRelayPort);
            if (rly.ok) {
                add(QStringLiteral("tcp_official_relay"),
                    TR("官方中继端口连通（%1 ms）").arg(rly.medMs),
                    TR("%1:%2  中位 %3 ms")
                        .arg(kOfficialSignal).arg(kOfficialRelayPort).arg(rly.medMs),
                    true, false);
            } else {
                add(QStringLiteral("tcp_official_relay"),
                    TR("官方中继端口不可达"),
                    TR("%1:%2  %3\n→ 只影响「打洞失败后走中继」这条退路；直连不受影响。")
                        .arg(kOfficialSignal).arg(kOfficialRelayPort).arg(rly.err),
                    false, false);
            }
        }

        // ---- 4/5. UDP 出站 + NAT 类型（同一个 socket，这是判定前提）----
        {
            QUdpSocket sock;
            sock.bind(QHostAddress::AnyIPv4, 0);

            struct Sample { QString server; StunAnswer ans; };
            QVector<Sample> samples;
            for (const QString& sv : stunServers())
                samples.append(Sample{ sv, stunQuery(sock, sv) });

            int       okCount = 0;
            QStringList errs;
            for (const Sample& s : samples) {
                if (s.ans.ok) ++okCount;
                else          errs << TR("%1（%2）").arg(s.server, s.ans.err);
            }

            if (okCount == 0) {
                add(QStringLiteral("udp_egress"),
                    TR("UDP 出站没有收到任何回应"),
                    TR("逐个问过 %1 个 STUN 服务器，一个都没回：\n  %2\n→ 打洞需要 UDP 出站。若是公司/校园网封了 UDP，只能走中继或自建服务器。")
                        .arg(stunServers().size())
                        .arg(errs.join(QStringLiteral("\n  "))),
                    false, true);
                add(QStringLiteral("nat"),
                    TR("NAT 类型：判不出来（UDP 不通）"),
                    TR("UDP 出站被拦，NAT 行为无从观察。"),
                    false, false);
            } else {
                add(QStringLiteral("udp_egress"),
                    TR("UDP 出站正常（%1/%2 个 STUN 有回应）").arg(okCount)
                        .arg(stunServers().size()),
                    errs.isEmpty() ? TR("全部服务器都有回应。")
                                   : TR("没回的：\n  %1").arg(errs.join(QStringLiteral("\n  "))),
                    true, true);

                // 按**映射出来的公网 IP** 分组 —— 本机是双出口（国内电信 + 国际 263），
                // 两组 NAT 行为完全不同（实测国内 Symmetric、国际 Cone），
                // 用一个标签概括必然骗人。
                QMap<QString, QList<quint16>> byIp;
                for (const Sample& s : samples) {
                    if (s.ans.ok)
                        byIp[s.ans.ip].append(s.ans.port);
                }

                QStringList lines;
                bool        anySym = false;
                bool        anyCone = false;
                for (auto it = byIp.constBegin(); it != byIp.constEnd(); ++it) {
                    const QList<quint16>& ports = it.value();
                    const bool  uniform = (ports.first() == ports.last());
                    // 只有一个样本的组说明只有一个服务器从这个出口回话，判定不了行为
                    const bool  judged  = (ports.size() >= 2);
                    if (judged) {
                        if (uniform) anyCone = true; else anySym = true;
                    }
                    const QString verdict = !judged
                        ? TR("样本不足，判不了（只有这一个服务器从这个出口回话）")
                        : (uniform ? TR("Cone（端口保持，利于打洞）")
                                   : TR("Symmetric（端口被重映射，打洞难）"));
                    QStringList ps;
                    for (quint16 p : ports) ps << QString::number(p);
                    lines << TR("%1 → 映射端口 %2  ⇒ %3")
                                 .arg(it.key(), ps.join(QStringLiteral(", ")), verdict);
                }

                const QString title = !anySym && !anyCone
                    ? TR("NAT 类型：判不出来（每个出口都只有单个样本）")
                    : (anySym && anyCone
                           ? TR("NAT 类型：混合（不同出口行为不一样）")
                           : (anySym ? TR("NAT 类型：Symmetric（打洞成功率低）")
                                     : TR("NAT 类型：Cone（利于打洞）")));

                add(QStringLiteral("nat"), title,
                    lines.join(QLatin1Char('\n')) +
                        TR("\n（同一个本地端口问不同目的地 → 映射端口变 = Symmetric）"),
                    anySym || anyCone, false);
            }
        }

        // ---- 6. IPv6 ----
        {
            bool        hasGlobal = false;
            QStringList v6;
            for (const QNetworkInterface& ni : QNetworkInterface::allInterfaces()) {
                if (ni.flags().testFlag(QNetworkInterface::IsLoopBack))
                    continue;
                for (const QNetworkAddressEntry& e : ni.addressEntries()) {
                    const QHostAddress a = e.ip();
                    if (a.protocol() != QAbstractSocket::IPv6Protocol)
                        continue;
                    if (a.isLoopback() || a.isLinkLocal())
                        continue;
                    hasGlobal = true;
                    v6 << a.toString();
                }
            }
            if (hasGlobal) {
                add(QStringLiteral("ipv6"), TR("IPv6 可用"),
                    v6.join(TR("、")), true, false);
            } else {
                add(QStringLiteral("ipv6"),
                    TR("IPv6 不可用（只有链路本地）"),
                    TR("没有全局 IPv6 地址 —— 打洞只能用 IPv4 通道。\n这只是个事实，不影响使用。"),
                    false, false);
            }
        }

        // ---- 7. 自建服务器（配了才查）----
        {
            struct Target { QString raw; quint16 defPort; QString label; };
            QVector<Target> targets;
            if (!cfg.customRendezvous().trimmed().isEmpty())
                targets.append(Target{ cfg.customRendezvous(), 21116,
                                       TR("自建信令") });
            if (!cfg.relayServer().trimmed().isEmpty())
                targets.append(Target{ cfg.relayServer(), 21117,
                                       TR("自建中继") });
            if (!cfg.apiServer().trimmed().isEmpty())
                targets.append(Target{ cfg.apiServer(), 21114,
                                       TR("自建 API") });

            for (const Target& t : targets) {
                QString host; quint16 port = 0;
                splitHostPort(t.raw, &host, &port, t.defPort);
                const TcpAnswer r = tcpRtt(host, port);
                if (r.ok) {
                    add(QStringLiteral("custom"), TR("%1 %2:%3 可达（%4 ms）")
                            .arg(t.label).arg(host).arg(port).arg(r.medMs),
                        TR("中位延迟 %1 ms").arg(r.medMs), true, true);
                } else {
                    add(QStringLiteral("custom"), TR("%1 %2:%3 连不上")
                            .arg(t.label).arg(host).arg(port),
                        TR("%1\n→ 检查这台服务器的端口有没有放行、地址填得对不对。")
                            .arg(r.err),
                        false, true);
                }
            }
        }

        // 回主线程。用 functor 形式的 invokeMethod，不需要注册 metatype。
        // **回来之前先确认对象还在**：体检跑到一半窗口被关掉是真会发生的。
        if (!alive->loadAcquire())
            return;
        QMetaObject::invokeMethod(this, [this, out] {
            m_running = false;
            setItems(out);
            emit finished();
        }, Qt::QueuedConnection);
    });

    // 线程回收。**不能直接 connect(finished, m_thread, &QObject::deleteLater)**：
    // deleteLater 什么时候真执行取决于事件循环，跑完那一刻 m_thread 就成了野指针，
    // 析构里的 m_thread->quit() 会踩已释放内存 —— 段错误，而且是"有时才崩"的那种
    // （--netcheck 单独跑没事、在 verify.sh 里连跑十几项之后崩，就是这个原因）。
    // 所以先摘指针再删，删的是捕获下来的那份，不碰 m_thread。
    //
    // 用队列连接（默认）让这段在主线程跑 —— 它只比指针不解引用，比完才删。
    QThread* t = m_thread;
    connect(t, &QThread::finished, this, [this, t] {
        if (m_thread == t)
            m_thread = nullptr;
        t->deleteLater();
    });
    t->start();
}

void NetworkProbe::setItems(const QVector<Item>& v)
{
    m_items = v;
}

// ---- 路径探测（M4.3）----
//
// 这段是**照抄** ConnectionWatcher::poll 的 tail 循环，不是复用它 —— 那个文件 M1/M3
// 已交付、只认一条正则，为了加一条去改它会污染既有行为。宁可复制这二十行。

void NetworkProbe::startPathWatch()
{
    if (m_pathTails.isEmpty())
        setPathLogPathsForTest(defaultPathLogs());
    else {
        for (PathTail& t : m_pathTails)
            t.pos = -1;              // 重新从末尾开始，别把历史连接报成"本次"
    }
    m_pathTimer->start();
}

void NetworkProbe::stopPathWatch()
{
    m_pathTimer->stop();
}

QStringList NetworkProbe::rustdeskLogPaths()
{
    return defaultPathLogs();
}

void NetworkProbe::setPathLogPathsForTest(const QStringList& paths)
{
    m_pathTails.clear();
    for (const QString& p : paths)
        m_pathTails.append(PathTail{ p, -1 });
    m_overridePaths = paths;
}

bool NetworkProbe::feedLogLineForTest(const QString& line)
{
    const QString s = pathFromLine(line);
    if (s.isEmpty())
        return false;                // 不是路径标记行，什么都没变
    // 判定和文案一起算：lineIsDirect() 的 false 分不清"非路径行"和"中继行"，
    // 所以"是否匹配"仍以上面 s 是否为空为准。
    const bool direct = lineIsDirect(line);
    if (s == m_pathSummary)
        return true;                 // 已经是这个结论，不重复发信号
    m_pathSummary = s;
    m_pathDirect  = direct;
    emit pathChanged(m_pathSummary, m_pathDirect);
    return true;
}

void NetworkProbe::pollPath()
{
    for (PathTail& t : m_pathTails) {
        QFile f(t.path);
        if (!f.open(QIODevice::ReadOnly))
            continue;                // 文件还没建，下次再看
        const qint64 size = f.size();
        if (t.pos < 0 || size < t.pos) {
            // 还没定过位，或者日志被轮转（CURRENT 换成新文件，体积变小）→ 只从此刻往后看
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

        // 只处理完整行：RustDesk 可能正写到一半，切开的半行匹配不上。
        const int lastNl = chunk.lastIndexOf('\n');
        if (lastNl < 0)
            continue;
        t.pos += lastNl + 1;

        const QList<QByteArray> lines = chunk.left(lastNl).split('\n');
        for (const QByteArray& raw : lines) {
            if (raw.isEmpty())
                continue;
            feedLogLineForTest(QString::fromUtf8(raw));
        }
    }
}

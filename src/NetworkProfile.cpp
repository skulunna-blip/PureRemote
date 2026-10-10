#include "NetworkProfile.h"

#include "AppConfig.h"
#include "I18n.h"

namespace {

// 这七个键就是这套机制管的全部。换档 = 把这七个重写一遍：
// 用不到的**删掉**（value 空），而不是写空串 —— 写空串 RustDesk 拿到的是一个
// 空服务器地址，比没有这个键更糟（没有键它会回落自己的默认值 rs-ny.rustdesk.com）。
const QString kRendezvous  = QStringLiteral("custom-rendezvous-server");
const QString kRelay       = QStringLiteral("relay-server");
const QString kApi         = QStringLiteral("api-server");
const QString kKey         = QStringLiteral("key");
const QString kWebsocket   = QStringLiteral("allow-websocket");
const QString kDirectSrv   = QStringLiteral("direct-server");
const QString kDirectPort  = QStringLiteral("direct-access-port");

// 直连监听口。源码 src/common.rs:3018 的单测里写死了 ("192.168.1.10:21118", true)。
const QString kDirectPortValue = QStringLiteral("21118");

} // namespace

QStringList NetworkProfile::managedKeys()
{
    return { kRendezvous, kRelay, kApi, kKey, kWebsocket, kDirectSrv, kDirectPort };
}

QVector<RustDeskController::OptionEdit> NetworkProfile::editsFor(const QString& mode,
                                                                 const AppConfig& cfg)
{
    // 先全清。后面每一档只把自己要的填回去 —— 这样"切档"天然就是幂等的：
    // 从 cloudflare 切到 official，不用单独记得去清 relay-server，全清那一步已经做了。
    QVector<RustDeskController::OptionEdit> out;
    out.append(RustDeskController::OptionEdit{ kRendezvous,  QString() });
    out.append(RustDeskController::OptionEdit{ kRelay,       QString() });
    out.append(RustDeskController::OptionEdit{ kApi,         QString() });
    out.append(RustDeskController::OptionEdit{ kKey,         QString() });
    out.append(RustDeskController::OptionEdit{ kWebsocket,   QString() });
    out.append(RustDeskController::OptionEdit{ kDirectSrv,   QString() });
    out.append(RustDeskController::OptionEdit{ kDirectPort,  QString() });

    auto fill = [&out](const QString& key, const QString& value) {
        for (RustDeskController::OptionEdit& e : out) {
            if (e.key == key)
                e.value = value;
        }
    };

    if (mode == QLatin1String("tsnet")) {
        // 被控端要在这台机器上监听 21118，好让 pmrelay 从 tailnet 把连接送过来。
        // 服务器四键保持清空 —— 这条路根本不经过 rendezvous。
        fill(kDirectSrv,  QStringLiteral("Y"));
        fill(kDirectPort, kDirectPortValue);
    } else if (mode == QLatin1String("cloudflare") || mode == QLatin1String("custom")) {
        fill(kRendezvous, cfg.customRendezvous().trimmed());
        fill(kRelay,      cfg.relayServer().trimmed());
        fill(kApi,        cfg.apiServer().trimmed());
        fill(kKey,        cfg.publicKey().trimmed());
        // Cloudflare 只能代理 HTTP/WebSocket，代理不了 21116/21117 的裸 TCP ——
        // 这是 CF 那一档和非 CF 的自建档**唯一**的实质差别，不是文案差别。
        if (mode == QLatin1String("cloudflare"))
            fill(kWebsocket, QStringLiteral("Y"));
    }
    // official：什么都不填 = 全清 = 回落 RustDesk 自带的 rs-ny.rustdesk.com

    return out;
}

bool NetworkProfile::validate(const QString& mode, const AppConfig& cfg, QString* err)
{
    auto fail = [err](const QString& m) {
        if (err) *err = m;
        return false;
    };

    if (!AppConfig::netModes().contains(mode))
        return fail(TR("认不出来的网络档位：%1").arg(mode));

    if (mode == QLatin1String("cloudflare") || mode == QLatin1String("custom")) {
        if (cfg.customRendezvous().trimmed().isEmpty())
            return fail(TR("还没填信令服务器地址。\n自建服务器至少要有一个 hbbs 地址，否则客户端不知道该找谁。"));
        // 地址里带单引号或反斜杠会破坏 TOML 行，RustDesk 会整个加载失败
        const QStringList values = { cfg.customRendezvous(), cfg.relayServer(),
                                     cfg.apiServer(), cfg.publicKey() };
        for (const QString& v : values) {
            if (v.contains(QLatin1Char('\'')) || v.contains(QLatin1Char('\\')))
                return fail(TR("服务器地址里不能有单引号或反斜杠：%1").arg(v));
        }
    }

    if (mode == QLatin1String("tsnet")) {
        if (cfg.tsnetAuthKey().trimmed().isEmpty())
            return fail(TR("tsnet 档要有 Tailscale 授权密钥（authkey）。\n登录控制台 → Settings → Keys 生成一个 reusable 的。"));
    }
    return true;
}

bool NetworkProfile::apply(const QString& mode, const AppConfig& cfg, RustDeskController* rd,
                           QString* err)
{
    if (!rd) {
        if (err) *err = TR("内部错误：没有 RustDesk 控制器");
        return false;
    }
    QString v;
    if (!validate(mode, cfg, &v)) {
        if (err) *err = v;
        return false;
    }
    return rd->setServerProfile(editsFor(mode, cfg), err);
}

QStringList NetworkProfile::mismatches(const QString& mode, const AppConfig& cfg,
                                       const RustDeskController* rd)
{
    QStringList bad;
    if (!rd)
        return { TR("内部错误：没有 RustDesk 控制器") };

    for (const RustDeskController::OptionEdit& e : editsFor(mode, cfg)) {
        const QString actual = rd->getOption(e.key);
        if (actual.trimmed() != e.value.trimmed()) {
            bad << TR("%1：期望 %2，实际 %3")
                       .arg(e.key,
                            e.value.isEmpty() ? TR("（没这个键）") : e.value,
                            actual.isEmpty() ? TR("（没这个键）") : actual);
        }
    }
    return bad;
}

// ⚠ 这个函数**故意不包 TR()** —— 它早就不喂界面了（正经的引导对话框顶替了它），
//    现在只剩 --conntest 拿它当测试基准，grep 里面的 ephemeral 和 apac 两个词。
//    中文原样留着是刻意的，别顺手"翻"它。
QString NetworkProfile::guide(const QString& mode)
{
    if (mode == QLatin1String("tsnet")) {
        return QStringLiteral(
            "这条路不经过任何第三方服务器：两台机器用 Tailscale 的 tsnet 组成一张私有网，\n"
            "RustDesk 的流量从这张网里走（本机实测中继节点在香港，延迟 43–68 ms，\n"
            "而 RustDesk 官方在纽约，220 ms）。\n"
            "\n"
            "要准备什么：\n"
            "  1. 注册 Tailscale（个人版免费、设备不限）\n"
            "  2. 控制台 Settings → Keys → Generate auth key\n"
            "     ⚠ 必须勾 reusable、不要勾 ephemeral、过期时间选不限制\n"
            "       （ephemeral 会在下线时删掉节点，被控端 IP 会变 → 再也连不上）\n"
            "  3. 把 authkey 粘到下面「本机」那一栏的主机名 + 授权密钥里\n"
            "  4. 两台机器填不同的主机名，控制端在联系人里填对方的那个\n"
            "\n"
            "不需要管理员、不装驱动、不写注册表 —— tsnet 全在用户态。");
    }
    if (mode == QLatin1String("cloudflare")) {
        return QStringLiteral(
            "用 Cloudflare Workers 免费额度自己搭 hbbs/hbbr。\n"
            "\n"
            "要准备什么：\n"
            "  1. 按社区方案把 hbbs/hbbr 部署到 Workers（有人写好了一键脚本）\n"
            "  2. ⚠ Workers 的 locationHint 一定要设 apac —— 才会落到香港/日本节点，\n"
            "     也就是实测 43–101 ms 的那一批；默认会飘到欧美\n"
            "  3. 把拿到的域名填到下面三个地址里，key 填服务器公钥\n"
            "\n"
            "⚠ Cloudflare 只能代理 HTTP/WebSocket，代理不了裸 TCP，所以这一档会自动\n"
            "   打开 allow-websocket。");
    }
    if (mode == QLatin1String("custom")) {
        return QStringLiteral(
            "你自己有一台服务器（VPS / 公司内网 / frp 打洞 / 家里路由器端口映射），\n"
            "在上面跑 hbbs + hbbr，把地址填进来。\n"
            "\n"
            "要准备什么：\n"
            "  · hbbs（信令）监听 21116/tcp + 21116/udp，地址填「信令服务器」\n"
            "  · hbbr（中继）监听 21117/tcp，地址填「中继服务器」（可以和信令同一台）\n"
            "  · 服务器公钥填「服务器公钥」（hbbs 首次启动时会打印）\n"
            "\n"
            "地址写 host 或 host:port 都行 —— 端口跟默认值不一样时才需要写。");
    }
    return QStringLiteral(
        "直接用 RustDesk 自带的官方服务器，什么都不用配。\n"
        "\n"
        "它的硬编码兜底只有 rs-ny.rustdesk.com 一台（在纽约）。本机实测信令 220 ms、\n"
        "中继在法国，打洞失败时画面会明显卡。\n"
        "\n"
        "先在下面点「一键体检」看看信令端口通不通 —— 通就用，不通就换上面三档里的一个。");
}

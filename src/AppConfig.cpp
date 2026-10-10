#include "AppConfig.h"

#include "I18n.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace {
// 找 rustdesk.exe。部署后它在 <exe目录>/bin/ 下；开发时 exe 躺在 build/，
// 而 bin/ 在工程根 —— 所以两个位置都探一下。
QString findRustDeskExe()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/bin/rustdesk.exe"),
        appDir + QStringLiteral("/../bin/rustdesk.exe"),
    };
    for (const QString& c : candidates) {
        const QString clean = QDir::cleanPath(c);
        if (QFileInfo::exists(clean))
            return clean;
    }
    return QDir::cleanPath(candidates.first());   // 都没有也给个默认值，报错里能看出找的是哪
}
} // namespace

AppConfig::AppConfig()
{
    m_rustDeskPath = findRustDeskExe();
    // 第一次运行时的语言默认值：跟着系统区域走。
    // 之后 load() 如果读到配置里已经存了语言，就用存的那份覆盖掉这里 —— 这是"只在
    // 第一次运行算一次"的实现方式，不需要额外存一个 has_lang 之类的标志位。
    m_lang = I18n::code(I18n::systemLang());
}

QString AppConfig::configFilePath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("config/puremote.json"));
}

QStringList AppConfig::netModes()
{
    return { QStringLiteral("official"), QStringLiteral("tsnet"),
             QStringLiteral("cloudflare"), QStringLiteral("custom") };
}

QString AppConfig::netModeTitle(const QString& mode)
{
    if (mode == QLatin1String("tsnet"))
        return TR("tailnet（自建 tsnet 桥接）");
    if (mode == QLatin1String("cloudflare"))
        return TR("Cloudflare Workers（自建）");
    if (mode == QLatin1String("custom"))
        return TR("自填地址（frp / 端口映射 / 自建 VPS）");
    return TR("官方服务器");
}

void AppConfig::load()
{
    QFile f(configFilePath());
    // ⚠ 这个判断要在**任何 return 之前**定下来：文件在不在是我自己的事，
    //   而"读不读得懂"是另一回事（坏 JSON 也照样算"不是首次运行"）。
    //   见 AppConfig.h 里那句说明：它决定"要不要自动补一个固定密码"，宁可窄。
    m_freshAtLoad = !f.exists();
    if (!f.exists())
        return;                       // 首次运行：保留默认值
    if (!f.open(QIODevice::ReadOnly))
        return;

    QJsonParseError err {};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;                       // 坏文件：保留默认值，不阻断启动

    const QJsonObject o = doc.object();

    // 逐字段取，类型不对就跳过该字段（不能因为一个字段写坏就丢掉整份配置）
    //
    // ⚠ **`rustdesk_path` 是故意不读的**（老配置里可能还有这个键，忽略掉）。
    //   路径只取决于"exe 在哪个目录"，而**绿色包是可以被搬来搬去的** —— 搬完再启动一次，
    //   构造函数里的 findRustDeskExe() 就重新探到新位置了。一旦把它存进配置又读回来，
    //   它就**粘死**在上次那个位置：搬到 D 盘之后，错误提示里的路径还是老的，
    //   而且 launcher 兜底那条分支（startAttempt 里的 startDetached(m_exePath)）
    //   会去执行一个已经不存在的文件。**每次启动重探一遍才是对的，这里什么都不做。**
    if (o.value(QStringLiteral("password")).isString())
        m_password = o.value(QStringLiteral("password")).toString();
    if (o.value(QStringLiteral("password_mode")).isString())
        m_passwordMode = o.value(QStringLiteral("password_mode")).toString();
    if (o.value(QStringLiteral("rotate_hours")).isDouble())
        m_rotateHours = o.value(QStringLiteral("rotate_hours")).toInt(3);
    if (o.value(QStringLiteral("password_rotated_at")).isString()) {
        // 解析不出来就保持无效值（当作"从没轮换过"），不阻断启动
        const QDateTime dt = QDateTime::fromString(
            o.value(QStringLiteral("password_rotated_at")).toString(), Qt::ISODate);
        if (dt.isValid())
            m_passwordRotatedAt = dt;
    }
    if (o.value(QStringLiteral("password_expires_at")).isString()) {
        const QDateTime dt = QDateTime::fromString(
            o.value(QStringLiteral("password_expires_at")).toString(), Qt::ISODate);
        if (dt.isValid())
            m_passwordExpiresAt = dt;
    }
    if (o.value(QStringLiteral("password_bootstrapped")).isBool())
        m_passwordBootstrapped = o.value(QStringLiteral("password_bootstrapped")).toBool();
    if (o.value(QStringLiteral("close_to_tray")).isBool())
        m_closeToTray = o.value(QStringLiteral("close_to_tray")).toBool();
    if (o.value(QStringLiteral("autostart")).isBool())
        m_autostart = o.value(QStringLiteral("autostart")).toBool();
    if (o.value(QStringLiteral("notify_mode")).isString())
        m_notifyMode = o.value(QStringLiteral("notify_mode")).toString();
    if (o.value(QStringLiteral("tsnet_authkey")).isString())
        m_tsnetAuthKey = o.value(QStringLiteral("tsnet_authkey")).toString();
    if (o.value(QStringLiteral("tsnet_hostname")).isString())
        m_tsnetHostname = o.value(QStringLiteral("tsnet_hostname")).toString();
    if (o.value(QStringLiteral("tsnet_peer")).isString())
        m_tsnetPeer = o.value(QStringLiteral("tsnet_peer")).toString();
    if (o.value(QStringLiteral("net_mode")).isString()) {
        const QString raw = o.value(QStringLiteral("net_mode")).toString();
        // M2 时期写下的值是 "id"，现在叫 "official"。留着兼容，否则老配置一读就被
        // 当成未知值回落，用户会莫名其妙发现档位自己跳回去了。
        m_netMode = (raw == QLatin1String("id")) ? QStringLiteral("official") : raw;
        if (!AppConfig::netModes().contains(m_netMode))
            m_netMode = QStringLiteral("official");   // 认不出来的值一律当官方档
    }
    // 语言：只认 "zh" / "en"，别的值一律当没存过（保留构造函数里按系统算出来的默认值）
    {
        const QString raw = o.value(QStringLiteral("lang")).toString();
        if (raw.compare(QLatin1String("zh"), Qt::CaseInsensitive) == 0
            || raw.compare(QLatin1String("en"), Qt::CaseInsensitive) == 0)
            m_lang = raw.toLower();
    }
    if (o.value(QStringLiteral("custom_rendezvous")).isString())
        m_customRendezvous = o.value(QStringLiteral("custom_rendezvous")).toString();
    if (o.value(QStringLiteral("relay_server")).isString())
        m_relayServer = o.value(QStringLiteral("relay_server")).toString();
    if (o.value(QStringLiteral("api_server")).isString())
        m_apiServer = o.value(QStringLiteral("api_server")).toString();
    if (o.value(QStringLiteral("public_key")).isString())
        m_publicKey = o.value(QStringLiteral("public_key")).toString();
    // 数组本身类型不对就整份丢掉（AddressBook 会当成"没有联系人"）；
    // 数组里单个元素坏掉由 AddressBook 逐条容错，不在这里做。
    if (o.value(QStringLiteral("contacts")).isArray())
        m_contacts = o.value(QStringLiteral("contacts")).toArray();
}

bool AppConfig::save() const
{
    const QString path = configFilePath();
    const QString dir  = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir))
        return false;

    QJsonObject o;
    // **不写 rustdesk_path**：见 load() 里的说明。它是"当前 exe 所在位置"的派生值，
    // 存进配置只会让绿色包被搬走之后还指着老地方。每次启动重探。
    o.insert(QStringLiteral("password"),      m_password);
    o.insert(QStringLiteral("password_mode"), m_passwordMode);
    o.insert(QStringLiteral("rotate_hours"),  m_rotateHours);
    o.insert(QStringLiteral("password_rotated_at"),
             m_passwordRotatedAt.isValid() ? m_passwordRotatedAt.toString(Qt::ISODate) : QString());
    o.insert(QStringLiteral("password_expires_at"),
             m_passwordExpiresAt.isValid() ? m_passwordExpiresAt.toString(Qt::ISODate) : QString());
    o.insert(QStringLiteral("password_bootstrapped"), m_passwordBootstrapped);
    o.insert(QStringLiteral("close_to_tray"), m_closeToTray);
    o.insert(QStringLiteral("autostart"),     m_autostart);
    o.insert(QStringLiteral("notify_mode"),   m_notifyMode);
    o.insert(QStringLiteral("tsnet_authkey"), m_tsnetAuthKey);
    o.insert(QStringLiteral("tsnet_hostname"), m_tsnetHostname);
    o.insert(QStringLiteral("tsnet_peer"),    m_tsnetPeer);
    o.insert(QStringLiteral("net_mode"),      m_netMode);
    o.insert(QStringLiteral("lang"),          m_lang);
    o.insert(QStringLiteral("custom_rendezvous"), m_customRendezvous);
    o.insert(QStringLiteral("relay_server"),  m_relayServer);
    o.insert(QStringLiteral("api_server"),    m_apiServer);
    o.insert(QStringLiteral("public_key"),    m_publicKey);
    o.insert(QStringLiteral("contacts"),      m_contacts);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

class AppConfig;

// 一条联系人。**两个地址都存**：官方 ID 和 tailnet 主机名。连接时按当前网络档位取一个，
// 所以切换档位不用重填。至少得有一个地址，否则这条没意义（解析时会被丢掉）。
struct Contact
{
    QString   name;
    QString   rustdeskId;      // 官方档用：对方的 RustDesk ID
    QString   tsnetHost;       // tsnet 档用：对方在 tailnet 上的主机名
    QString   peerPassword;    // 可选。留空就不给 rustdesk 传 --password，让它自己弹窗问
    QDateTime lastConnectedAt; // 本地时间；无效值 = 没连过

    bool isValid() const { return !rustdeskId.isEmpty() || !tsnetHost.isEmpty(); }

    // 按档位取要连的目标。档位对不上时返回空串（调用方负责报错，别拿错误的目标去连）。
    QString targetFor(const QString& netMode) const
    {
        return netMode == QLatin1String("tsnet") ? tsnetHost : rustdeskId;
    }
};

// 地址簿：联系人的增删改查 + 持久化。
//
// 持久化**借道 AppConfig**，不自己开文件：AppConfig::save() 是整文件重写，要是这里再
// 拿一个 AppConfig 实例，两边会互相覆盖（后写的把先写的抹掉）。所以构造时必须传进
// main.cpp 里那**唯一一个** AppConfig。
//
// 解析一律**逐条容错**：数组里混进字符串/数字/null/缺字段的对象，只有那一条被丢掉，
// 别的照常加载。地址簿坏一条不该让整个界面打不开。
class AddressBook : public QObject
{
    Q_OBJECT

public:
    explicit AddressBook(AppConfig* config, QObject* parent = nullptr);

    // 从 AppConfig 里读出原始 QJsonArray 并容错解析。析构不写盘。
    void load();

    QVector<Contact> contacts() const { return m_contacts; }
    int     count() const { return m_contacts.size(); }
    Contact at(int i) const
    {
        return (i >= 0 && i < m_contacts.size()) ? m_contacts.at(i) : Contact();
    }

    // 增删改。都是**内存改完立刻落盘**并发 changed() —— 地址簿这种量级不值得为
    // "忘了调 save()" 埋一个 bug。返回 false 只代表参数不合法（索引越界 / 两个地址都空）。
    int  add(const Contact& c);              // 返回新索引，失败返回 -1
    bool update(int i, const Contact& c);
    bool removeAt(int i);
    void touchConnectedAt(int i, const QDateTime& when);

    static QString nameKey()            { return QStringLiteral("name"); }
    static QString rustdeskIdKey()      { return QStringLiteral("rustdesk_id"); }
    static QString tsnetHostKey()       { return QStringLiteral("tsnet_host"); }
    static QString peerPasswordKey()    { return QStringLiteral("peer_password"); }
    static QString lastConnectedAtKey() { return QStringLiteral("last_connected_at"); }

signals:
    void changed();

private:
    static Contact     fromJson(const QJsonObject& o);
    static QJsonObject toJson(const Contact& c);
    void               persist();       // 写回 AppConfig 并落盘 + 发 changed()

    AppConfig*       m_config = nullptr;
    QVector<Contact> m_contacts;
};

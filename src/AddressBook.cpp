#include "AddressBook.h"

#include "AppConfig.h"

#include <QJsonArray>
#include <QJsonValue>

namespace {
// 名字空着就填个占位——列表里显示空白行会让人以为条目坏了
const char* kUnnamed = "未命名";
} // namespace

AddressBook::AddressBook(AppConfig* config, QObject* parent)
    : QObject(parent)
    , m_config(config)
{
}

Contact AddressBook::fromJson(const QJsonObject& o)
{
    Contact c;

    // 逐字段用 isString() 显式判类型，不靠 toString() 静默吞掉类型错误
    // （数字会被 toString() 变成 "123"，那不叫容错，那叫猜）
    if (o.value(AddressBook::nameKey()).isString())
        c.name = o.value(AddressBook::nameKey()).toString();
    if (c.name.isEmpty())
        c.name = QString::fromUtf8(kUnnamed);

    if (o.value(AddressBook::rustdeskIdKey()).isString())
        c.rustdeskId = o.value(AddressBook::rustdeskIdKey()).toString();
    if (o.value(AddressBook::tsnetHostKey()).isString())
        c.tsnetHost = o.value(AddressBook::tsnetHostKey()).toString();
    if (o.value(AddressBook::peerPasswordKey()).isString())
        c.peerPassword = o.value(AddressBook::peerPasswordKey()).toString();

    // 时间解析失败**不丢条目**，只是没有"上次连接"这个信息 —— 地址本身还是好的
    if (o.value(AddressBook::lastConnectedAtKey()).isString()) {
        const QDateTime dt = QDateTime::fromString(
            o.value(AddressBook::lastConnectedAtKey()).toString(), Qt::ISODate);
        if (dt.isValid())
            c.lastConnectedAt = dt;
    }

    return c;
}

QJsonObject AddressBook::toJson(const Contact& c)
{
    QJsonObject o;
    o.insert(nameKey(),         c.name.isEmpty() ? QString::fromUtf8(kUnnamed) : c.name);
    o.insert(rustdeskIdKey(),   c.rustdeskId);
    o.insert(tsnetHostKey(),    c.tsnetHost);
    o.insert(peerPasswordKey(), c.peerPassword);
    // 本地时间，跟 password_rotated_at 一个惯例（混进 UTC 会让显示和排序都错）
    o.insert(lastConnectedAtKey(),
             c.lastConnectedAt.isValid() ? c.lastConnectedAt.toString(Qt::ISODate) : QString());
    return o;
}

void AddressBook::load()
{
    m_contacts.clear();
    if (!m_config)
        return;

    const QJsonArray arr = m_config->contacts();
    m_contacts.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        if (!v.isObject())          // 字符串 / 数字 / null 混进来 → 跳过这一条
            continue;
        const Contact c = fromJson(v.toObject());
        if (!c.isValid())           // 两个地址都空 → 连都连不了，留着只会误导人
            continue;
        m_contacts.append(c);
    }
}

void AddressBook::persist()
{
    if (m_config) {
        QJsonArray arr;
        for (const Contact& c : m_contacts)
            arr.append(toJson(c));
        m_config->setContacts(arr);
        m_config->save();
    }
    emit changed();
}

int AddressBook::add(const Contact& c)
{
    if (!c.isValid())
        return -1;
    m_contacts.append(c);
    if (m_contacts.last().name.isEmpty())
        m_contacts.last().name = QString::fromUtf8(kUnnamed);
    persist();
    return m_contacts.size() - 1;
}

bool AddressBook::update(int i, const Contact& c)
{
    if (i < 0 || i >= m_contacts.size() || !c.isValid())
        return false;
    m_contacts[i] = c;
    if (m_contacts.at(i).name.isEmpty())
        m_contacts[i].name = QString::fromUtf8(kUnnamed);
    persist();
    return true;
}

bool AddressBook::removeAt(int i)
{
    if (i < 0 || i >= m_contacts.size())
        return false;
    m_contacts.remove(i);
    persist();
    return true;
}

void AddressBook::touchConnectedAt(int i, const QDateTime& when)
{
    if (i < 0 || i >= m_contacts.size())
        return;
    m_contacts[i].lastConnectedAt = when;
    persist();
}

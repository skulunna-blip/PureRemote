#include "SelfCheck.h"

#include "I18n.h"

#include <QStringList>

namespace SelfCheck {

QVector<Item> evaluate(const Inputs& in)
{
    QVector<Item> v;

    {
        Item i;
        i.key   = QStringLiteral("rd_present");
        i.title = TR("RustDesk 内核");
        i.hint  = TR("随包的 bin/rustdesk.exe。找不到就重装这个绿色包 —— 外壳自己不实现远程桌面协议，全靠它。");
        i.ok    = in.rustdeskPresent;
        i.fixable = false;          // 没法在线修：得把包补全
        v.append(i);
    }
    {
        Item i;
        i.key   = QStringLiteral("has_id");
        i.title = TR("本机 ID 已读出");
        i.hint  = TR("被别人连时对方要念的那串数字。读不到就点右边的“重新读取”。");
        i.ok    = in.hasId;
        i.fixable = true;           // → 触发一次 refreshId()
        v.append(i);
    }
    {
        Item i;
        i.key   = QStringLiteral("fixed_password");
        i.title = TR("已设固定密码");
        i.hint  = TR("没设的话只能用一次性密码，每次连之前都要重新问一次 —— 人不在电脑前就等于连不上。");
        i.ok    = in.fixedPasswordSet;
        i.fixable = true;           // → 跳到密码策略那一组
        v.append(i);
    }
    {
        Item i;
        i.key   = QStringLiteral("relay_present");
        i.title = TR("桥接程序 pmrelay.exe");
        i.hint  = TR("走 tailnet 档才需要它。它是纯用户态的中转程序，不需要驱动、不需要管理员。");
        i.ok    = in.relayPresent;
        i.fixable = false;          // 同样得把包补全
        i.tsnetOnly = true;
        v.append(i);
    }
    {
        Item i;
        i.key   = QStringLiteral("tsnet_authkey");
        i.title = TR("Tailscale 授权密钥");
        i.hint  = TR("走 tailnet 档才需要。在 Tailscale 后台生成一个 reusable、不过期的 authkey 粘进来。");
        i.ok    = in.tsnetAuthKeySet;
        i.fixable = true;           // → 聚焦到输入框
        i.tsnetOnly = true;
        v.append(i);
    }
    {
        Item i;
        i.key   = QStringLiteral("tsnet_hostname");
        i.title = TR("本机 tailnet 主机名");
        i.hint  = TR("走 tailnet 档才需要。这是本机在 tailnet 上的名字，对方靠它找到你。");
        i.ok    = in.tsnetHostnameSet;
        i.fixable = true;           // → 聚焦到输入框
        i.tsnetOnly = true;
        v.append(i);
    }

    return v;
}

int missingCount(const QVector<Item>& items, bool tsnetMode)
{
    int n = 0;
    for (const Item& i : items) {
        if (i.tsnetOnly && !tsnetMode)
            continue;
        if (!i.ok)
            ++n;
    }
    return n;
}

QString bannerText(const QVector<Item>& items, bool tsnetMode)
{
    QStringList names;
    for (const Item& i : items) {
        if (i.tsnetOnly && !tsnetMode)
            continue;
        if (!i.ok)
            names.append(i.title);
    }
    if (names.isEmpty())
        return QString();

    // 最多列三项，再多就收成"等" —— 一行横幅的字数得能一眼看完，列七项就退回清单了。
    constexpr int kMaxShown = 3;
    QString       list      = names.mid(0, kMaxShown).join(TR("、"));
    if (names.size() > kMaxShown)
        list += TR(" 等");
    return TR("还差 %1 项：%2").arg(names.size()).arg(list);
}

QString firstFixableAction(const QVector<Item>& items, bool tsnetMode)
{
    for (const Item& i : items) {
        if (i.tsnetOnly && !tsnetMode)
            continue;
        if (i.ok)
            continue;
        // **第一个缺的那项说了算**，不是"第一个能修的"。顺序是有意义的：
        // 前面那项要是补不了（比如内核文件根本不在），后面这些"能填"的项填了也白填，
        // 照样连不上。这时候指向教程，比引导用户去点一个徒劳的按钮诚实。
        return i.fixable ? i.key : QString();
    }
    return QString();
}

} // namespace SelfCheck

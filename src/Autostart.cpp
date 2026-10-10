#include "Autostart.h"

#include "I18n.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace {

// 带空格就加引号。**空格是唯一的判据**：Windows 的 Run 值里，路径还有其他字符
// 也该引起来（`&` 之类），但那种路径在本项目里不存在，加了只是徒增分支；
// 真正会撞上的是 "C:\Program Files\..." 这种，而它只由空格触发。
QString quoteIfNeeded(const QString& s)
{
    if (s.isEmpty())
        return s;
    if (s.contains(QLatin1Char(' ')) || s.contains(QLatin1Char('\t')))
        return QLatin1Char('"') + s + QLatin1Char('"');
    return s;
}

// 路径归一：分隔符统一、大小写拉平、去掉尾部的斜杠。
// 只用来**比较**，不写回任何地方（所以不会把用户看到的路径改样子）。
QString normalized(const QString& s)
{
    QString out = QDir::fromNativeSeparators(s).toLower();
    while (out.endsWith(QLatin1Char('/')))
        out.chop(1);
    return out;
}

QSettings runSettings()
{
    return QSettings(Autostart::runKeyPath(), QSettings::NativeFormat);
}

} // namespace

QString Autostart::valueName()
{
    // 值名不随语言变 —— 它是注册表里的标识，翻成中文就成了另一条值，
    // 关开关时就删不到自己写的那条了。
    return QStringLiteral("PureRemote");
}

QString Autostart::runKeyPath()
{
    return QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
}

QString Autostart::runValue(const QString& exePath)
{
    return quoteIfNeeded(QDir::toNativeSeparators(exePath)) + QStringLiteral(" --tray");
}

QString Autostart::exeFromRunValue(const QString& value)
{
    const QString v = value.trimmed();
    if (v.isEmpty())
        return QString();
    if (v.at(0) == QLatin1Char('"')) {
        const int end = v.indexOf(QLatin1Char('"'), 1);
        return end < 0 ? v.mid(1) : v.mid(1, end - 1);
    }
    const int sp = v.indexOf(QLatin1Char(' '));
    return sp < 0 ? v : v.left(sp);
}

bool Autostart::sameExePath(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty())
        return false;
    return normalized(a) == normalized(b);
}

QString Autostart::storedRunValue()
{
    QSettings s = runSettings();
    return s.value(valueName()).toString();
}

bool Autostart::isEnabled()
{
    return !storedRunValue().trimmed().isEmpty();
}

bool Autostart::needsRepair(const QString& storedValue, const QString& exePath)
{
    if (storedValue.trimmed().isEmpty())
        return false;   // 没开自启 ≠ 自启坏了
    if (!storedValue.contains(QStringLiteral("--tray")))
        return true;    // 不带 --tray 的那条会让开机弹窗口，等于没起到"静默待连"的作用
    return !sameExePath(exeFromRunValue(storedValue), exePath);
}

bool Autostart::enable(const QString& exePath, QString* errorOut)
{
    // 写别人的启动项之前先确认"要写的这个程序真的在"。exe 路径空或者文件不在，
    // 写进去就是一条开机必然失败的死项 —— 用户看到开关是开的，机器却永远连不上。
    if (exePath.isEmpty() || !QFileInfo::exists(exePath)) {
        if (errorOut)
            *errorOut = TR("找不到本程序自己的可执行文件（%1），没法写自启").arg(exePath);
        return false;
    }

    const QString want = runValue(exePath);
    QSettings s = runSettings();
    s.setValue(valueName(), want);
    s.sync();

    // 写完读回来对。**不能只看 status()** —— 被 ACL / 安全软件挡下时它未必报。
    if (s.value(valueName()).toString() != want) {
        if (errorOut)
            *errorOut = TR("写了注册表但读回来对不上（可能被安全软件或组策略挡了）");
        return false;
    }
    return true;
}

bool Autostart::disable(QString* errorOut)
{
    QSettings s = runSettings();
    s.remove(valueName());
    s.sync();

    if (!s.value(valueName()).toString().isEmpty()) {
        if (errorOut)
            *errorOut = TR("删不掉注册表里那条自启（可能被安全软件或组策略挡了）");
        return false;
    }
    return true;
}

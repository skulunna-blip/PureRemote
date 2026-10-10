#pragma once

#include <QString>
#include <QStringList>

// 图文教程的正文。**纯函数**：只返回 HTML 字符串，不碰控件、不碰配置。
//
// 做成纯函数是为了能无头验：链接写错域名、图片没进 qrc、`.arg` 漏了参数 ——
// 这几类错在界面上都得人点开那一页才看得见，不测就等着上线以后才发现。
//
// 图片只画**示意图**（就是几个方块加箭头），正文里也明写了"示意图"。
// 真实截图拿不到：我们没 Tailscale 账号可截，别人的截图也不该抄。
// 想看真界面，就点正文里的官方链接。
namespace Guide {

// 主题 key 与 AppConfig::netModes() 对齐，顺序 = 界面上主题下拉框的顺序。
QStringList topics();

QString topicTitle(const QString& key);

// 完整 HTML 片段（不含 <html>/<body> 外壳），交给 QTextBrowser::setHtml。
QString html(const QString& key);

// 当前网络档位该默认打开哪个主题；认不出来一律给 "official"。
QString topicForNetMode(const QString& netMode);

} // namespace Guide

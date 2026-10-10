#pragma once

#include <QApplication>
#include <QString>
#include <QtGui/qwindowdefs.h>

// 从 PureViewer 原样移植：Fusion 样式 + 深色 QPalette + 一段全局 stylesheet。
// 纯静态函数，不持有状态，所以不做成 QObject。
namespace DarkTheme {

void apply(QApplication& app);
void applyNativeTitleBar(WId handle);

// 全局样式表本身。**抽成函数是为了能无头断言**：QSS 里靠 `.arg()` 填占位符，
// 少传一个参数就会在样式表里留下悬空的 `%7` —— 那种错在界面上只表现为"某个控件没上色"，
// 不测根本发现不了。`--uitest` 会对返回值做 `%[0-9]` 零命中检查。
QString styleSheet();

} // namespace DarkTheme

#include "DarkTheme.h"

#include <QColor>
#include <QPalette>
#include <QStyle>

#ifdef Q_OS_WIN
#include <dwmapi.h>
#endif

namespace DarkTheme {

namespace {
constexpr const char* kDarkBg   = "#1a1a1a";
constexpr const char* kDarkSurf = "#2a2a2a";
constexpr const char* kAccent   = "#4a9eff";
constexpr const char* kTextPri  = "#e0e0e0";
constexpr const char* kTextSec  = "#999999";
constexpr const char* kBorder   = "#3a3a3a";
} // namespace

QString styleSheet()
{
    // ⚠ 这里**故意不写 `.arg()` 占位符**，颜色全部直接写死。原因是个真实的坑：
    //    QString 的多参数 arg() 是**按标记在串里出现的先后**依次发实参的，不是按编号配对。
    //    于是只要有一个编号缺席（这个表里就没有 %4），后面全体前移一位 ——
    //    以前就是这个毛病：`background:%5` 本意是 kDarkBg，实际拿到 kTextSec，
    //    输入框/下拉框/列表全都渲染成 #999999 的灰底（实测老包 12% 的像素是这个灰）。
    //    写死就没这回事了。下面这几个色值和 apply() 里 QPalette 用的是同一套。
    //    uitest 的 U 段会断言输出里 `%` 后面不跟数字，防的就是有人"顺手"改回 .arg()。
    return QStringLiteral(R"(
        QToolBar { background:#2a2a2a; border-bottom:1px solid #414141; padding:5px 7px; spacing:3px; }
        QToolBar QToolButton { color:#e0e0e0; border:1px solid transparent; border-radius:5px; padding:6px 8px; }
        QToolBar QToolButton:hover { background:#384657; border-color:#526b88; }
        QToolBar QToolButton:pressed { background:#29415f; border-color:#4a9eff; }
        QToolBar QToolButton:focus { border-color:#4a9eff; }
        QToolBar QToolButton:disabled { background:transparent; color:#707070; }
        QStatusBar { background:#2a2a2a; border-top:1px solid #414141; color:#c8c8c8; font-size:12px; min-height:25px; }
        QListWidget { background:#1a1a1a; border:none; outline:none; }
        QListWidget::item { padding:4px 6px; }
        QListWidget::item:selected { background:#4a9eff; border-radius:2px; }
        /* 教程左侧目录。全局 QListWidget 是 border:none 的（地址簿要贴着卡片感），
           这里得把它显回一个框，否则跟正文糊成一片、看不出是可点的目录。
           ⚠ 只写 ::item 的 padding，不写 ::item:selected 的背景 —— 全局那条已经给了
           蓝色选中底，重复写会散成两份维护点。 */
        QListWidget#guideToc { background:#232323; border:1px solid #3a3a3a; border-radius:4px; padding:4px; }
        QListWidget#guideToc::item { padding:6px 8px; }
        /* 设置框的标签页。以前全项目没有 QTabWidget，不写这块的话它会退回 Fusion 默认
           画法，在深色板上是一块浅灰 —— 跟 QDialogButtonBox 那个坑同源。 */
        QTabWidget::pane { border:1px solid #3a3a3a; border-radius:4px; top:-1px; }
        QTabBar::tab { background:#232323; color:#b0b0b0; border:1px solid #3a3a3a;
                       border-bottom:none; border-top-left-radius:4px; border-top-right-radius:4px;
                       padding:6px 16px; margin-right:2px; }
        QTabBar::tab:selected { background:#1a1a1a; color:#e0e0e0; }
        QTabBar::tab:hover:!selected { background:#2f3a46; color:#e0e0e0; }
        QScrollBar:horizontal { background:#1a1a1a; height:6px; }
        QScrollBar::handle:horizontal { background:#555; min-width:20px; border-radius:3px; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width:0; }
        QScrollBar:vertical { background:#1a1a1a; width:6px; }
        QScrollBar::handle:vertical { background:#555; min-height:20px; border-radius:3px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
        QMenu { background:#2a2a2a; border:1px solid #3a3a3a; color:#e0e0e0; padding:4px; }
        QMenu::item { padding:6px 24px; border-radius:3px; }
        QMenu::item:selected { background:#4a9eff; }
        QMenu::separator { height:1px; background:#3a3a3a; margin:4px 8px; }
        QGroupBox { color:#e0e0e0; border:1px solid #3a3a3a; border-radius:4px; margin-top:8px; padding-top:16px; font-weight:bold; }
        QGroupBox::title { subcontrol-origin:margin; left:10px; padding:0 6px; }
        QDialog { background:#1a1a1a; }
        QPushButton { background:#3a3a3a; color:#e0e0e0; border:1px solid #4a9eff; border-radius:4px; padding:6px 14px; }
        QPushButton:hover { background:#384657; border-color:#526b88; }
        QPushButton:pressed { background:#29415f; }
        QPushButton:disabled { background:#232323; color:#707070; border-color:#333; }
        QLineEdit { background:#1a1a1a; color:#e0e0e0; border:1px solid #3a3a3a; border-radius:4px; padding:6px 8px; }
        QLineEdit:focus { border-color:#4a9eff; }
        QLabel { color:#e0e0e0; }
        /* 下拉框和数字框以前没上过色，全靠 Fusion 的默认盘；设置框和教程框里都要用，
           顺手补上。⚠ **不要**去写 QComboBox::drop-down/::down-arrow —— 一旦给这两个
           子控件上样式，Qt 就改走样式表绘制，而没写的部分不会回落到 Fusion，
           箭头会直接消失。只给框本体上色，箭头交给 Fusion 自己画。 */
        QComboBox { background:#1a1a1a; color:#e0e0e0; border:1px solid #3a3a3a; border-radius:4px; padding:4px 8px; }
        QComboBox:hover { border-color:#526b88; }
        QComboBox:disabled { color:#707070; border-color:#333; }
        QComboBox QAbstractItemView { background:#1a1a1a; color:#e0e0e0; border:1px solid #3a3a3a;
                                      selection-background-color:#4a9eff; selection-color:#ffffff; outline:none; }
        QSpinBox { background:#1a1a1a; color:#e0e0e0; border:1px solid #3a3a3a; border-radius:4px; padding:4px 6px; }
        QSpinBox:focus { border-color:#4a9eff; }
        /* 教程正文：颜色主要来自文档里的行内样式，这里只管外框和默认字色（兜底）。 */
        QTextBrowser { background:#1a1a1a; color:#e0e0e0; border:1px solid #3a3a3a; border-radius:4px; padding:6px 10px; }
    )");
}

void apply(QApplication& app)
{
    app.setStyle(QStringLiteral("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window,          QColor(kDarkBg));
    p.setColor(QPalette::WindowText,      QColor(kTextPri));
    p.setColor(QPalette::Base,            QColor(kDarkSurf));
    p.setColor(QPalette::AlternateBase,   QColor(kDarkBg));
    p.setColor(QPalette::Text,            QColor(kTextPri));
    p.setColor(QPalette::Button,          QColor(kDarkSurf));
    p.setColor(QPalette::ButtonText,      QColor(kTextPri));
    p.setColor(QPalette::BrightText,      QColor("#ff5555"));
    p.setColor(QPalette::Link,            QColor(kAccent));
    p.setColor(QPalette::Highlight,       QColor(kAccent));
    p.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    // 占位提示（placeholderText）默认取自 disabled 那一档，深色底上几乎看不见；
    // QSS 管不到它，只能在调色板里给。
    p.setColor(QPalette::PlaceholderText, QColor(kTextSec));
    app.setPalette(p);

    app.setStyleSheet(styleSheet());
}

void applyNativeTitleBar(WId handle)
{
#ifdef Q_OS_WIN
    // Windows 10 1903+ 使用此 DWM 属性；旧版 SDK/MinGW 头文件尚未定义枚举值 20。
    constexpr DWORD kUseImmersiveDarkMode = 20;
    if (handle) {
        const BOOL enabled = TRUE;
        DwmSetWindowAttribute(reinterpret_cast<HWND>(handle), kUseImmersiveDarkMode,
                              &enabled, sizeof(enabled));
    }
#else
    Q_UNUSED(handle);
#endif
}

} // namespace DarkTheme

#pragma once

#include <QString>
#include <QStringList>

// 界面语言。中文是**源语言**，英文是一张"中文 → 英文"的查找表。
//
// 为什么不做成 Qt 的 tr()/.ts/.qm 那一套：
//   1. 那套要 lupdate/lrelease 两个工具链，还要把 .qm 打进包、在启动时装 QTranslator ——
//      对一个绿色免安装的单文件外壳来说是白加的依赖；
//   2. 更要紧的是**验收**：现有十几个无头测试是靠"报告里含某句中文"来判 PASS 的
//      （SelfCheck 的横幅文案、连接路径、体检结论……）。走 .qm 的话这些断言全得改成
//      查翻译表，而"改断言让它迁就新实现"正是测试失效的典型路径。
//   3. 中文原样返回 = zh 档下 t() 是恒等函数，**所有既有断言行为一字不变**。
//
// 所以这里只有一条规矩：**中文是键**。改中文原文 = 换了一个新键，
// 英文会静默退回中文（看得见，不是坏掉）。scripts/check_i18n.py 会把这种漏掉的键
// 在 verify.sh 里报出来。
namespace I18n {

enum class Lang { Zh, En };

// 当前语言。默认中文 —— 没设过语言的老配置、或者读不出系统区域时都落在这儿。
Lang    lang();
void    setLang(Lang l);

// 系统区域给的建议值：中文区域 → Zh，其余一律 En。
// 只在**第一次运行**（配置里还没有 lang 这个键）时用一次，之后就听用户自己选的。
Lang    systemLang();

QString code();                  // "zh" / "en"，写进配置用
QString code(Lang l);
Lang    fromCode(const QString& c);   // 认不出来 → Zh

// 把一句**界面文案**翻成当前语言。zh 档下原样返回（不查表）。
// ⚠ 只喂界面文案，**别喂**从配置/用户那儿读来的字符串 —— 见 tDynamic。
QString t(const QString& zh);

// 运行时才知道的字符串（比如地址簿里存着的联系人名）也过一遍表。
// 表里没有的原样返回：用户自己起的名字不该被"翻译"。
QString tDynamic(const QString& s);

// —— 下面是给 --uitest 的 I 段和 scripts/check_i18n.py 用的 ——

// 全部**中文键**。用来断言"没有重复的键""没有空键"。
QStringList keys();
// 查一条英文。没有返回空串（注意：和 t() 的"退回中文"不同 —— 这里要能分辨"没译"）。
QString englishFor(const QString& zh);
int     entryCount();

} // namespace I18n

// 书写上省一层括号。只接受**字面量**：check_i18n.py 就是靠正则扫 `TR("…")` 来核对
// 表里有没有漏的，写成变量它就看不见了。
#define TR(s) ::I18n::t(QStringLiteral(s))

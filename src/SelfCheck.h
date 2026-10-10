#pragma once

#include <QString>
#include <QVector>

// "准备情况"清单：告诉用户现在能连/能被连了吗，缺什么，缺的那项怎么补。
//
// 设计成**纯函数**（输入全从外面灌进来）是刻意的：判断逻辑里最怕"缺一项却显示全绿"，
// 而这种错只有在各分支都能无头断言的时候才拦得住。所以这里不碰 AppConfig、不碰 QProcess、
// 不继承 QObject —— 调用方负责把当前状态抓成 SelfCheckInputs。
namespace SelfCheck {

struct Inputs
{
    bool rustdeskPresent  = false;  // bin/rustdesk.exe 在不在
    bool hasId            = false;  // 本机 ID 读出来了没
    bool fixedPasswordSet = false;  // 设过固定密码没（没有就只能用一次性密码）
    bool relayPresent     = false;  // bin/pmrelay.exe 在不在
    bool tsnetAuthKeySet  = false;
    bool tsnetHostnameSet = false;

    // ⚠ 这里**曾经**有一个 hasContact（"地址簿里至少有一条"），已经删掉。
    // 理由：它是"连别人"的前置条件，而现在连接框是**填码直接连** —— 一条联系人都没有
    // 也能连上任何一台机器。留着它等于在主界面常年挂一条"还差 1 项"，而那一项
    // 其实什么都不缺。准备情况只该说"你能不能连上"，不该给一个可选项记缺勤。
};

struct Item
{
    QString key;         // 稳定标识（测试断言用，界面不显示）
    QString title;       // 一行短标题
    QString hint;        // 灰字说明：这项是干什么的 / 缺了怎么办
    bool    ok       = false;
    bool    fixable  = false;  // 缺了能不能"一键修"（能修的只有"填个值 / 点一下"这类）
    bool    tsnetOnly = false; // 只在 tsnet 档才有意义
};

// 顺序就是界面上的显示顺序。
QVector<Item> evaluate(const Inputs& in);

// 缺了几项。tsnetMode=false 时不计 tsnetOnly 的那几项 —— 走官方 ID 档的时候，
// 没装 pmrelay、没填 authkey 都不算"没准备好"，报出来只会吓人。
int missingCount(const QVector<Item>& items, bool tsnetMode);

// 主界面顶部那一行提示的文案。**一项不缺时返回空串**，调用方据此把整条横幅藏掉。
//
// 特意不返回"一切就绪"之类的字：横幅只在出问题时出现，常年挂一行"正常"就变成新的噪音，
// 而"复杂"正是我们要治的病。
QString bannerText(const QVector<Item>& items, bool tsnetMode);

// 横幅上那个按钮该干什么 —— 返回**第一个缺的**项的 key；那一项不可修、
// 或者压根没有缺项时返回空串（空串叫调用方改开教程）。
//
// 只给第一个是刻意的。旧版每个缺项各配一个「一键修」，5 个按钮里 3 个其实只是把光标
// 挪到输入框、什么也没"修"，名字与行为不符反而更让人糊涂。收敛成一个按钮之后，
// 语义变成"先补第一项"，按钮文案由调用方按"要进设置还是要开教程"来定。
//
// 注意"第一个缺的"而不是"第一个能修的"：顺序本身有意义，前面那项补不了的话，
// 后面的填了也连不上，这时候指向教程比给一个徒劳的按钮诚实。
QString firstFixableAction(const QVector<Item>& items, bool tsnetMode);

} // namespace SelfCheck

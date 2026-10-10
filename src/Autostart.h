#pragma once

#include <QString>

// 开机自启。用户原话是「万一人不在了，远程重启了」—— 绿色免安装的包没有服务、
// 没有计划任务，机器重启之后没人去双击，就是彻底连不上。这一块补的就是这个洞。
//
// 落地方式：**HKCU\Software\Microsoft\Windows\CurrentVersion\Run** 下的一条值，
// 值名固定 `PureRemote`，数据是 `"<exe 绝对路径>" --tray`。
//
// 为什么是 Run 键而不是"启动"文件夹里的快捷方式：
//   · 一句 QSettings(..., NativeFormat) 就够，**不需要 COM**（建 .lnk 要 COM）
//   · 用户级、不要管理员、不写 Program Files、不注册服务 —— 跟"纯绿色免安装"张力最小
//   · 关掉就是把**自己写的那一条**删掉，不碰同一个键里别人的值
//
// ⚠ 这是本项目**唯一一处有意为之的系统写入**。默认关闭；只有用户在设置里点开那一刻
//   才写。唯一的例外是"路径自愈"：自启**已经开着**时，启动会对齐一次路径
//   （绿色软件被挪目录是常态，否则那条值会变成死路径，开机静默失效）。
//   从来没开过的机器，启动过程一个字节都不写。
namespace Autostart {

// Run 键下那条值的名字。**只认这一个** —— 删除时也只删它。
QString valueName();

// 注册表键路径（测试里拿 reg query 对的就是这个）。
QString runKeyPath();

// 开机时要执行的那一行。路径**带空格必须加引号** —— 不加的话系统会把
// `C:\Program` 当程序名、`Files\...\PureRemote.exe` 当参数，开机什么都不会发生。
// `--tray` 是必须的：没有它自启会把主窗口糊到用户脸上。
QString runValue(const QString& exePath);

// 从 runValue 里抠出可执行文件路径。带引号走引号；没引号就取到第一个空白之前。
// 我们自己写的一律带引号，这里兜的是"用户手动改过注册表"。
QString exeFromRunValue(const QString& value);

// 两个路径算不算同一个可执行文件。Windows 路径**大小写不敏感**，`/` 和 `\` 等价 ——
// 直接拿字符串比会把 C:\X\a.exe 和 c:/x/A.EXE 判成不同，于是每次启动都白改一遍注册表。
bool sameExePath(const QString& a, const QString& b);

// 注册表里现在那条值（没有 = 空串）。
QString storedRunValue();

// 自启是不是开着（= 注册表里那条值非空）。
bool isEnabled();

// 「开着，但记的不是当前这个 exe」。纯函数，好断言。两种情况都要修：
//   · 路径对不上（被挪了目录 / 换了个解包位置）
//   · 那一行里没有 `--tray`（老版本写的，或者被人手动改过 —— 会弹窗）
// 空值**不算**需要修：那是"没开自启"，不是"坏了"。
bool needsRepair(const QString& storedValue, const QString& exePath);

// 写入 / 删除。返回是否成功，失败原因写进 errorOut。
// 两个都会**写完读回来比对** —— 注册表写失败（组策略、ACL、安全软件）不抛异常，
// 光看 status() 也会漏，只有读回来才知道到底进去没有。
bool enable(const QString& exePath, QString* errorOut = nullptr);
bool disable(QString* errorOut = nullptr);

} // namespace Autostart

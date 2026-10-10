#pragma once

// 「连接别的电脑」那一组里，几个按钮该不该亮 / 点了该连谁。
//
// 为什么抽成**纯函数**（照 SelfCheck 的先例：输入全从外面灌、不碰 AppConfig、
// 不碰 QProcess、不继承 QObject）：这几条规则是**曾经错过一次**的地方。
//
// 错在哪：原来的规则只用"列表里有没有选中一行"一个布尔去决定使能，而「新建」做的
// 恰恰是**取消选中**（`setCurrentRow(-1)`）。于是地址簿空着的时候，点「新建」→
// 仍然没选中 → 「保存修改」和「连接」**永远是灰的** → 用户一个人都加不进来。
//
// 抽出来的意义是把规则变成可以穷举断言的东西；但**光有它拦不住那个 bug** ——
// 规则可以全对，错的是 MainWindow 喂进来的输入。真正钉死它的是 --paneltest
// （造真控件断言），两边互补，缺一不可。见 MainWindow.h 里那段注释。
namespace ConnectPanel {

// MainWindow 从控件上抓下来的当前状态。**只有数据，没有控件**。
struct State
{
    int  bookCount     = 0;
    int  selectedRow   = -1;     // <0 = 列表没选中 = 正在"新建一条"，还没存
    bool targetFilled  = false;  // 当前档位下，输入框里能取到一个可连的地址
    bool editorValid   = false;  // 输入框里能凑出一条合法联系人（有 ID 或有主机名）
    bool sessionActive = false;  // 会话在跑（含正在建桥接/等对端）
};

struct Buttons
{
    bool save       = false;
    bool del        = false;
    bool connect    = false;
    bool disconnect = false;
};

// 按钮使能规则。
//
// ⚠ 四条规则里最要紧的是 `save` 和 `connect` **都跟"选没选中"无关**：
//   地址簿是一份"存过的常用设备"，不是"必须先建一条才能连"的门槛。
//   填了码就该能连（connect 看输入框），有东西可存就该能存（save 看输入框）。
//   `del` 才是真正只对"已存的某一条"有意义的那个。
Buttons buttons(const State& s);

// 是不是"新建态"（没选中任何一条）。决定「保存」按钮的文案：
// 选中一条时叫「保存修改」，新建态叫「存到列表」—— 文案不一样是因为用户的心智不一样，
// 使能条件**一样**（都看输入框里有没有东西可存）。
bool isNewMode(const State& s);

// 点「连接」时该拿谁当目标。
enum class Source
{
    None,    // 输入框里没有可连的地址 —— 调用方给一句提示，别发连接
    Saved,   // 跟列表里选中的那条一模一样 → 走 connectTo(row)，**这样才会记「上次连接」**
    Adhoc    // 输入框里的临时目标 → 走 connectToAdhoc()，不写地址簿、不记时间
};

// `selectionMatchesEditors`：选中的那条跟输入框里的值逐字段相等。
// 只有完全相等才当 Saved —— 语义是**所见即所连**：用户既然动手改了码，
// 连的就该是他改完的那个，而不是列表里那条旧的。
Source sourceFor(bool editorTargetFilled, bool hasSelection, bool selectionMatchesEditors);

} // namespace ConnectPanel

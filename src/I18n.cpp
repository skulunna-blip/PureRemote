#include "I18n.h"

#include <QHash>
#include <QLocale>
#include <QVector>

namespace {

I18n::Lang g_lang = I18n::Lang::Zh;      // 默认中文；启动时按配置覆盖

struct Pair
{
    const QString zh;
    const QString en;
};

// ─────────────────────────────────────────────────────────────────────────────
// 中文 → 英文。
//
// 规矩：
//   · **键就是中文原文**，连标点一起。改一个字 = 换了一个新键，旧英文作废 ——
//     所以改文案时记着同步改这里（check_i18n.py 会报"用到了但表里没有"）。
//   · `%1` `%2` 的顺序要和中文一致：QString::arg(a,b,…) 多参重载是按**出现顺序**
//     分配实参的，不是按编号 —— 编号跳号会让后面全部错位（这个坑踩过）。
//   · 表里没有的键，t() 退回中文（看得见），不会变成空字符串。
//   · 不翻译的东西：RustDesk 的配置键名、日志标记（"used to establish"）、
//     <b>/<span> 之类的 HTML 标签、路径、命令。
// ─────────────────────────────────────────────────────────────────────────────
const QVector<Pair>& table()
{
    static const QVector<Pair> t = {
        // ── 通用 ────────────────────────────────────────────────────────────
        { QStringLiteral("、"),                        QStringLiteral(", ") },
        { QStringLiteral("未命名"),                    QStringLiteral("Unnamed") },

        // ── 时长（MainWindow 的 humanDuration）─────────────────────────────
        { QStringLiteral("不到 1 分钟"),               QStringLiteral("less than a minute") },
        { QStringLiteral("%1 分钟"),                   QStringLiteral("%1 min") },
        { QStringLiteral("%1 小时"),                   QStringLiteral("%1 h") },
        { QStringLiteral("%1 小时 %2 分"),             QStringLiteral("%1 h %2 min") },
        { QStringLiteral("%1 天"),                     QStringLiteral("%1 d") },
        { QStringLiteral("%1 天 %2 小时"),             QStringLiteral("%1 d %2 h") },

        // ── SelfCheck：7 项准备情况 ────────────────────────────────────────
        { QStringLiteral("RustDesk 内核"),
          QStringLiteral("RustDesk core") },
        { QStringLiteral("随包的 bin/rustdesk.exe。找不到就重装这个绿色包 —— "
                         "外壳自己不实现远程桌面协议，全靠它。"),
          QStringLiteral("The bundled bin/rustdesk.exe. If it is missing, re-extract this "
                         "portable package — the shell does not implement the remote-desktop "
                         "protocol itself, it relies on this.") },
        { QStringLiteral("本机 ID 已读出"),
          QStringLiteral("This PC's ID was read") },
        { QStringLiteral("被别人连时对方要念的那串数字。读不到就点右边的“重新读取”。"),
          QStringLiteral("The digits the other side reads out to reach you. If it can't be "
                         "read, click \"Reload\" on the right.") },
        { QStringLiteral("已设固定密码"),
          QStringLiteral("A fixed password is set") },
        { QStringLiteral("没设的话只能用一次性密码，每次连之前都要重新问一次 —— "
                         "人不在电脑前就等于连不上。"),
          QStringLiteral("Without one, only one-time passwords are available and you have to "
                         "ask for a fresh one before every session — which means nobody can "
                         "reach this PC while you're away from it.") },
        { QStringLiteral("桥接程序 pmrelay.exe"),
          QStringLiteral("Bridge program pmrelay.exe") },
        { QStringLiteral("走 tailnet 档才需要它。它是纯用户态的中转程序，"
                         "不需要驱动、不需要管理员。"),
          QStringLiteral("Only needed in the tailnet mode. It is a pure user-space relay — "
                         "no driver, no administrator rights.") },
        { QStringLiteral("Tailscale 授权密钥"),
          QStringLiteral("Tailscale auth key") },
        { QStringLiteral("走 tailnet 档才需要。在 Tailscale 后台生成一个 "
                         "reusable、不过期的 authkey 粘进来。"),
          QStringLiteral("Only needed in the tailnet mode. Generate a reusable, "
                         "non-expiring auth key in the Tailscale admin console and paste it here.") },
        { QStringLiteral("本机 tailnet 主机名"),
          QStringLiteral("This PC's tailnet hostname") },
        { QStringLiteral("走 tailnet 档才需要。这是本机在 tailnet 上的名字，"
                         "对方靠它找到你。"),
          QStringLiteral("Only needed in the tailnet mode. This is this PC's name on the "
                         "tailnet; the other side uses it to find you.") },
        { QStringLiteral(" 等"),                       QStringLiteral(" etc.") },
        { QStringLiteral("还差 %1 项：%2"),             QStringLiteral("%1 item(s) still missing: %2") },

        // ── 主窗口：顶栏 / 本机 ────────────────────────────────────────────
        { QStringLiteral("PureRemote — 远程桌面"),      QStringLiteral("PureRemote — Remote Desktop") },
        { QStringLiteral("PureRemote — 设置"),          QStringLiteral("PureRemote — Settings") },
        { QStringLiteral("PureRemote — 图文教程"),      QStringLiteral("PureRemote — Guide") },
        { QStringLiteral("图文教程"),                   QStringLiteral("Guide") },
        { QStringLiteral("手把手教你换网络路径，带链接和示意图"),
          QStringLiteral("Step-by-step help for switching network paths, with links and diagrams") },
        { QStringLiteral("⚙ 设置"),                     QStringLiteral("⚙ Settings") },
        { QStringLiteral("线路 / 画质 / 密码 / 连接提示 / 界面语言"),
          QStringLiteral("Line / Quality / Password / Connection prompt / Interface language") },
        { QStringLiteral("去补上"),                     QStringLiteral("Fix it") },
        { QStringLiteral("本机"),                       QStringLiteral("This computer") },
        { QStringLiteral("连接码"),                     QStringLiteral("ID") },
        { QStringLiteral("复制"),                       QStringLiteral("Copy") },
        { QStringLiteral("重新读取"),                   QStringLiteral("Reload") },
        { QStringLiteral("新联系人"),                   QStringLiteral("New contact") },
        { QStringLiteral("看教程"),                     QStringLiteral("Open guide") },
        { QStringLiteral("去设置"),                     QStringLiteral("Open settings") },
        { QStringLiteral("把这个码告诉对方，他就能连过来"),
          QStringLiteral("Give this ID to the other person and they can connect to you") },
        { QStringLiteral("密码"),                       QStringLiteral("Password") },
        { QStringLiteral("显示"),                       QStringLiteral("Show") },
        { QStringLiteral("隐藏"),                       QStringLiteral("Hide") },
        { QStringLiteral("关闭"),                       QStringLiteral("Close") },
        { QStringLiteral("启动中…"),                    QStringLiteral("Starting…") },
        { QStringLiteral("读取中…"),                    QStringLiteral("Reading…") },
        { QStringLiteral("会话已结束"),                 QStringLiteral("Session ended") },
        // ConnectSession / NetworkProfile 两处共用的同一句（表里只留一条）
        { QStringLiteral("内部错误：没有 RustDesk 控制器"),
          QStringLiteral("Internal error: no RustDesk controller") },
        { QStringLiteral("已复制 ID：%1"),              QStringLiteral("ID copied: %1") },
        { QStringLiteral("已复制密码"),                 QStringLiteral("Password copied") },
        // 设置里那一页叫「线路与画质」，不叫「连接」：它跟"连谁"无关，
        // 而且跟主界面的「连接别的电脑」撞名会让人以为重了一份。
        // ⚠ 这里**不能写 `&`**：Qt 会把控件文字里的裸 & 当助记符，标签页上渲染成
        //   "Route _quality"（吃掉一个字符、下划线跑到 q 上）。截图里才看得见，
        //   断言全绿也拦不住 —— 所以 check_i18n.py 现在有一条专门查裸 & 的致命项。
        { QStringLiteral("线路与画质"),                 QStringLiteral("Route and quality") },
        { QStringLiteral("还没设固定密码，没有可复制的密码"),
          QStringLiteral("No fixed password is set, so there is nothing to copy") },
        { QStringLiteral("把密码复制到剪贴板，直接发给对方；不用先点「显示」"),
          QStringLiteral("Copy the password to the clipboard and send it to the other "
                         "person — no need to click \"Show\" first") },
        { QStringLiteral("没读到 ID —— 检查 bin/rustdesk.exe 是否在，或点“重新读取”"),
          QStringLiteral("No ID yet — check that bin/rustdesk.exe exists, or click \"Reload\"") },

        // ── 密码显示（本机那一组下面的小字）────────────────────────────────
        { QStringLiteral("固定密码"),                   QStringLiteral("Fixed password") },
        // 「关闭」模式下按那个按钮 = 删掉本机固定密码。按钮自己把话说清楚，
        // 真要删还会再弹一道确认 —— 这两句都不是装饰，见 MainWindow::onApplyClicked。
        { QStringLiteral("关闭固定密码"),               QStringLiteral("Turn off fixed password") },
        { QStringLiteral("要删掉本机的固定密码吗"),
          QStringLiteral("Delete this machine's fixed password?") },
        { QStringLiteral("这台机器现在是有固定密码的，「关闭」并应用会把它删掉。\n"
                         "删掉之后没人看着这台机器时就没人能连进来了 —— 只剩运行时的临时密码，"
                         "每次连接都得现场问一遍。\n"
                         "确定要删掉吗？"),
          QStringLiteral("This machine currently has a fixed password, and applying "
                         "\"Off\" will delete it.\n"
                         "After that, nobody can connect while this machine is "
                         "unattended — only the running one-time password is left, "
                         "which has to be asked for on every connection.\n"
                         "Delete it?") },
        { QStringLiteral("已取消，固定密码一个字节都没动"),
          QStringLiteral("Cancelled — the fixed password is untouched") },
        { QStringLiteral("定时轮换中，到点会自动换新密码"),
          QStringLiteral("Rotating on a schedule — a new password is generated automatically") },
        { QStringLiteral("限时有效中 —— 到点这个码就作废，"
                         "作废后只能用屏幕上的临时密码"),
          QStringLiteral("Time-limited — this password expires at the scheduled time, after "
                         "which only the on-screen one-time password works") },
        // ⚠ 原来叫「已设置（密文）」—— "密文"是行话，用户看不懂那行到底怎么了。
        //   2026-10-10 改成说人话：外壳**看不到内容**（RustDesk 加密存的）。
        { QStringLiteral("已设置（外壳看不到）"),     QStringLiteral("Set (shell can't see it)") },
        { QStringLiteral("用完即换 —— 有人连过一次、断开之后就自动换成新的"),
          QStringLiteral("Use once — a new password is generated after someone connects and "
                         "then disconnects") },
        // ⚠ 这一句是上面那条分支的正文本。以前它是"点「换成外壳能记住的新密码」"——
        //   那个按钮已经删了（首次运行改成自动接管）。现在出口写成**指路到「立即应用」**，
        //   那是设置里本来就有的按钮，代价（替换、原来那个作废）也已经说出口。
        { QStringLiteral("RustDesk 里有一个固定密码，但它是加密存的，外壳看不到内容（也没法看到）。"
                         "想让这里能看能复制，去「设置 → 别人连我」点<b>「立即应用」</b> —— "
                         "会用外壳生成的新密码替换它，原来那个立刻失效。"),
          QStringLiteral("RustDesk has a fixed password, but it is stored encrypted and this "
                         "shell cannot see its contents (there is no way to). To view and copy "
                         "it here, go to Settings → \"Others connect to me\" and click "
                         "<b>\"Apply now\"</b> — that replaces it with a new password generated "
                         "by this shell, and the old one stops working immediately.") },
        { QStringLiteral("要替换掉本机现在的固定密码吗"),
          QStringLiteral("Replace the fixed password on this machine?") },
        // ⚠ %1 = 是谁触发的（现在只有「立即应用」这一个入口）。**参数个数两边必须一致** ——
        //   少一个 %1 在英文档下就会显示成光秃秃的一句，中文档下永远看不见。
        { QStringLiteral("RustDesk 里现在有一个固定密码。它是<b>加密存的</b>，外壳读不出明文（也没法读）。<br><br>"
                         "%1会用外壳生成的新密码替换它：<br>"
                         "· 新密码会显示在本机那一行，可以看、可以复制<br>"
                         "· 原来那个立刻失效，手里拿着旧密码的人再也连不上<br><br>"
                         "确定要继续吗？"),
          QStringLiteral("RustDesk has a fixed password right now. It is <b>stored encrypted</b>, "
                         "and this shell can't read its plaintext (there is no way to).<br><br>"
                         "%1 will replace it with a new one generated by this shell:<br>"
                         "· the new password is shown on the \"This machine\" row — you can view "
                         "and copy it<br>"
                         "· the old one stops working immediately, so anyone holding it can no "
                         "longer connect<br><br>"
                         "Continue?") },
        { QStringLiteral("已取消，RustDesk 里那个固定密码一个字节都没动"),
          QStringLiteral("Cancelled — the fixed password inside RustDesk is byte-for-byte "
                         "untouched") },
        // ⚠ 「关闭」模式下必须换一句：那个按钮那时叫"关闭固定密码"，干的是**删密码**。
        //   原来只有上面那一句，指向的是一个当时不存在的按钮名 —— 用户照着找，最可能
        //   点中的就是旁边那个真会删密码的，正好踩上要防的那次事故。
        { QStringLiteral("RustDesk 里已有固定密码（外壳没记明文），别人现在能连进来。"
                         "别点「关闭固定密码」—— 那会把它删掉。"),
          QStringLiteral("RustDesk already has a fixed password (this shell doesn't know the "
                         "plaintext) and others can connect right now. Don't click \"Turn off "
                         "fixed password\" — that deletes it.") },
        { QStringLiteral("未设置"),                     QStringLiteral("Not set") },
        // 这两条是同一件事在**两处**的说法：主界面密码行的提示，和顶部那条红色横幅。
        // 刻意不共用一句话 —— 横幅得把后果（"没人能连进来"）摆在最前面，提示行
        // 空间小、得先说要做什么。要改就两处一起改，别只改一处留个半截。
        { QStringLiteral("现在还连不进来 —— 没有固定密码。RustDesk 的一次性密码"
                         "只显示在它自己的窗口里，外壳读不到；去「别人连我」设一个固定密码，"
                         "或者在那里点「打开 RustDesk 看一次性密码」。"),
          QStringLiteral("Nobody can connect in right now — there is no fixed password. "
                         "RustDesk's one-time password is only shown in its own window and this "
                         "shell can't read it; set a fixed password under \"Let others connect to "
                         "me\", or click \"Open RustDesk to see the one-time password\" there.") },
        { QStringLiteral("现在没人能连进来：本机还没有固定密码 —— RustDesk 的一次性密码"
                         "在这个界面上看不到。点右边的「去设置」设一个（会自动替你生成）。"),
          QStringLiteral("Nobody can connect to this machine right now: it has no fixed "
                         "password, and RustDesk's one-time password can't be seen in this "
                         "window. Click \"Open settings\" on the right to set one — one will "
                         "be generated for you.") },
        { QStringLiteral("第一次运行：已经自动设好一个固定密码（就在主界面上），"
                         "别人现在就能连进来。不想要就去「别人连我」关掉。"),
          QStringLiteral("First run: a fixed password has been set for you (it is shown on "
                         "the main screen), so others can connect now. If you don't want one, "
                         "turn it off under \"Let others connect to me\".") },
        // ⚠ 这是"首次运行**替换**掉了一个已有密码"时说的那句话 —— 凭据被换掉不该让用户
        //   事后自己发现。分支判据是 BootstrapInputs::tomlHasPassword（见 maybeBootstrapPassword）。
        { QStringLiteral("第一次运行：已经把本机的固定密码换成外壳管的这一个"
                         "（就在主界面上，能看能复制）。原来那个已备份到 RustDesk.toml.bak。"),
          QStringLiteral("First run: the fixed password on this machine has been replaced with "
                         "one managed by this shell (shown on the main screen — you can view and "
                         "copy it). The previous one is backed up to RustDesk.toml.bak.") },
        { QStringLiteral("自动设置固定密码失败：%1"),
          QStringLiteral("Couldn't set a fixed password automatically: %1") },

        // ── 设置：密码策略 ────────────────────────────────────────────────
        { QStringLiteral("密码策略"),                   QStringLiteral("Password policy") },
        { QStringLiteral("模式"),                       QStringLiteral("Mode") },
        // ⚠ 第 0 档原来是「关闭 —— 只用一次性密码」。那个名字在**暗示**"只是不用固定密码，
        //   一次性密码照样能用"，而 RustDesk 的一次性密码只显示在它自己的窗口里、外壳读不到，
        //   所以这一档的真实含义是**谁也连不进来**。英文侧同理：不能写成 "off"，
        //   要留住"nobody can connect in"这层后果。
        { QStringLiteral("不用固定密码 —— 别人连不进来"),
          QStringLiteral("No fixed password — nobody can connect in") },
        { QStringLiteral("固定密码 —— 一直用同一个"),
          QStringLiteral("Fixed password — always the same one") },
        { QStringLiteral("定时轮换 —— 到点换新密码"),   QStringLiteral("Rotate — new password on a schedule") },
        { QStringLiteral("限时有效 —— 到点作废"),       QStringLiteral("Time-limited — expires on schedule") },
        // 「用完即换」的英文里必须留住"**断开之后**才换"——写成 "after someone connects"
        // 会被读成"连上就换"，而那意味着把刚连上的人掐断。
        { QStringLiteral("用完即换 —— 连过的人断开后换新密码"),
          QStringLiteral("Use once — new password once the visitor disconnects") },
        { QStringLiteral("未应用"),                     QStringLiteral("Not applied") },
        { QStringLiteral("选中的这一档还没真正生效 —— 点右边的按钮才会落到 RustDesk 上。"),
          QStringLiteral("The mode you picked hasn't taken effect yet — click the button on the "
                         "right to write it into RustDesk.") },
        { QStringLiteral("打开 RustDesk 看一次性密码"),
          QStringLiteral("Open RustDesk to see the one-time password") },
        { QStringLiteral("RustDesk 的一次性密码只显示在它自己的窗口里，外壳读不到。"),
          QStringLiteral("RustDesk's one-time password is only shown in its own window; this "
                         "shell can't read it.") },
        { QStringLiteral("每"),                         QStringLiteral("Every") },
        { QStringLiteral("有效"),                       QStringLiteral("Valid for") },
        { QStringLiteral(" 小时"),                      QStringLiteral(" h") },
        { QStringLiteral("立即应用"),                   QStringLiteral("Apply now") },
        { QStringLiteral("换一个码"),                   QStringLiteral("New password") },
        { QStringLiteral("立刻生成一个新密码并重新计时 —— 之前把码给出去、现在想作废就用它"),
          QStringLiteral("Generates a new password right away and restarts the timer — use it "
                         "to invalidate a password you already handed out") },
        { QStringLiteral("当前不轮换。"),               QStringLiteral("Not rotating right now.") },
        { QStringLiteral("一直用这一个，不会自动变。"), QStringLiteral("Stays the same until you change it.") },
        { QStringLiteral("还没设固定密码 —— 别人现在连不进来。"),
          QStringLiteral("No fixed password yet — nobody can connect in right now.") },
        { QStringLiteral("用完即换：有人连过一次、对方断开之后自动换新密码。"
                         "换的瞬间会重启 RustDesk（大约几秒连不上），所以<b>不会</b>掐断正在连的人。"),
          QStringLiteral("Use once: after someone connects and then disconnects, a new password "
                         "is generated automatically. The swap restarts RustDesk (a few seconds "
                         "of downtime), which is why it <b>never</b> cuts off the person who is "
                         "currently connected.") },
        { QStringLiteral("这个码没有作废时间 —— 点「立即应用」重新计时。"),
          QStringLiteral("This password has no expiry — click \"Apply now\" to restart the clock.") },
        { QStringLiteral("已到期，正在作废…"),          QStringLiteral("Expired — invalidating…") },
        { QStringLiteral("这个码 %1 作废（还剩 %2）"),
          QStringLiteral("This password expires at %1 (in %2)") },
        { QStringLiteral("下次轮换：即将进行"),         QStringLiteral("Next rotation: any moment now") },
        { QStringLiteral("下次轮换：%1（%2 后）"),      QStringLiteral("Next rotation: %1 (in %2)") },

        // ── 设置：连接提示 ────────────────────────────────────────────────
        { QStringLiteral("有人连进来时"),               QStringLiteral("When someone connects in") },
        { QStringLiteral("方式"),                       QStringLiteral("Behaviour") },
        { QStringLiteral("不提示 —— 静默接受"),         QStringLiteral("No prompt — accept silently") },
        { QStringLiteral("托盘提示 —— 只告诉一声"),     QStringLiteral("Tray notice — just a heads-up") },
        { QStringLiteral("需要我确认 —— 不点不接受"),   QStringLiteral("Ask me — nothing gets through without a click") },
        { QStringLiteral("有人连进来会弹出 RustDesk 的连接窗口，你不点“接受”就连不上。"
                         "注意：这一条和“无人值守”是冲突的 —— 人不在电脑前就没人点接受。"),
          QStringLiteral("An incoming connection pops up RustDesk's own connection window and "
                         "nothing gets through until you click \"Accept\". Note this conflicts "
                         "with unattended access — if nobody is at the keyboard, nobody clicks.") },
        { QStringLiteral("有人连进来会在右下角弹一个气泡告诉你一声，但<b>不拦连接</b>，对方照样能进。"
                         "说明：被控端的日志里没有对方的 ID，所以提示只能说“有人连进来了”，说不出是谁。"),
          QStringLiteral("An incoming connection shows a balloon in the corner, but it "
                         "<b>doesn't block anything</b> — the other side still gets in. Note: "
                         "the host-side log carries no peer ID, so the notice can only say "
                         "\"someone connected\", never who.") },
        { QStringLiteral("对方有密码就直接连上，不弹任何东西。"),
          QStringLiteral("If the other side has the password they get straight in, with no prompt at all.") },

        // ── 设置：地址簿 ──────────────────────────────────────────────────
        { QStringLiteral("连接别的电脑"),               QStringLiteral("Connect to another computer") },
        { QStringLiteral("对方把<b>连接码</b>念给你，填在下面的框里，点「连接」就直接连 —— "
                         "不用先存。常连的可以点「存到列表」留下来，下次从上面的列表里挑。"),
          QStringLiteral("Ask the other person for their <b>ID</b>, type it below and click "
                         "\"Connect\" — no need to save it first. For a machine you reach "
                         "often, click \"Save to list\" and pick it from the list above next time.") },
        { QStringLiteral("名字"),                       QStringLiteral("Name") },
        { QStringLiteral("随便起，自己能认出来就行"),
          QStringLiteral("Anything you'll recognise later") },
        { QStringLiteral("对方连接码"),                 QStringLiteral("Their ID") },
        { QStringLiteral("对方念给你的那串数字（官方档用）"),
          QStringLiteral("The digits they read out to you (for the official mode)") },
        { QStringLiteral("tailnet 主机名"),             QStringLiteral("tailnet hostname") },
        { QStringLiteral("对方在 tailnet 上的名字（tailnet 档用）"),
          QStringLiteral("Their name on the tailnet (for the tailnet mode)") },
        { QStringLiteral("对方密码"),                   QStringLiteral("Their password") },
        { QStringLiteral("可留空 —— 留空就让 RustDesk 自己弹窗问"),
          QStringLiteral("Can be left empty — RustDesk will then ask you itself") },
        { QStringLiteral("新建"),                       QStringLiteral("New") },
        { QStringLiteral("保存修改"),                   QStringLiteral("Save changes") },
        { QStringLiteral("删除"),                       QStringLiteral("Delete") },
        { QStringLiteral("连接"),                       QStringLiteral("Connect") },
        { QStringLiteral("断开"),                       QStringLiteral("Disconnect") },
        { QStringLiteral("存到列表"),                   QStringLiteral("Save to list") },
        { QStringLiteral("粘贴"),                       QStringLiteral("Paste") },
        { QStringLiteral("已粘贴连接码"),               QStringLiteral("ID pasted") },
        { QStringLiteral("剪贴板里没有文字"),           QStringLiteral("Nothing on the clipboard") },
        { QStringLiteral("先填对方的连接码"),           QStringLiteral("Fill in their ID first") },
        { QStringLiteral("先填对方的 tailnet 主机名"),
          QStringLiteral("Fill in their tailnet hostname first") },
        // 「连接」现在直接吃输入框的值，所以提示也得说这件事 —— 老版这两句讲的是
        // "先建一条联系人再连"，那是被修掉的那个流程。
        { QStringLiteral("填上对方的连接码就能连 —— 想留着下次用，再点「存到列表」。"),
          QStringLiteral("Fill in their ID and you can connect — click \"Save to list\" "
                         "to keep it for next time.") },
        { QStringLiteral("点「连接」直接连。密码留空的话，RustDesk 会自己弹窗问。"),
          QStringLiteral("Click \"Connect\" to connect. Leave the password empty and "
                         "RustDesk will ask for it itself.") },
        { QStringLiteral("填上对方的连接码，点「连接」直接连；想留着就点「存到列表」"),
          QStringLiteral("Fill in their ID and click \"Connect\"; click \"Save to list\" "
                         "to keep it") },
        { QStringLiteral("没连过。"),                   QStringLiteral("Never connected.") },
        { QStringLiteral("上次连接：%1。"),             QStringLiteral("Last connected: %1.") },
        { QStringLiteral("这条在「%1」档下没有地址 —— %2"),
          QStringLiteral("This contact has no address in the \"%1\" mode — %2") },
        { QStringLiteral("官方 ID"),                    QStringLiteral("official ID") },
        { QStringLiteral("填一下它的 tailnet 主机名"),
          QStringLiteral("fill in its tailnet hostname") },
        { QStringLiteral("填一下它的对方连接码"),
          QStringLiteral("fill in its ID") },
        { QStringLiteral("会连到 %1。%2%3"),            QStringLiteral("Connecting to %1. %2%3") },
        { QStringLiteral(" 没填对方密码，RustDesk 会自己弹窗问。"),
          QStringLiteral(" No password stored, so RustDesk will ask for it.") },
        { QStringLiteral("「对方连接码」和「tailnet 主机名」至少得填一个"),
          QStringLiteral("Fill in at least one of \"Their ID\" and \"tailnet hostname\"") },
        { QStringLiteral("已保存"),                     QStringLiteral("Saved") },
        { QStringLiteral("保存失败"),                   QStringLiteral("Save failed") },
        { QStringLiteral("删除联系人"),                 QStringLiteral("Delete contact") },
        { QStringLiteral("确定删掉「%1」吗？"),         QStringLiteral("Delete \"%1\"?") },
        { QStringLiteral("已删除"),                     QStringLiteral("Deleted") },

        // ── 设置：画质与性能 ──────────────────────────────────────────────
        { QStringLiteral("画质与性能"),                 QStringLiteral("Quality and performance") },
        { QStringLiteral("这些值是<b>按联系人</b>存的 —— 改的是上面选中的那一个对端，"
                         "下次连他就用这个。"),
          QStringLiteral("These values are stored <b>per contact</b> — you are editing the peer "
                         "selected above, and they apply the next time you connect to it.") },
        { QStringLiteral("画质"),                       QStringLiteral("Quality") },
        { QStringLiteral("自定义值"),                   QStringLiteral("Custom value") },
        { QStringLiteral("帧率"),                       QStringLiteral("Frame rate") },
        { QStringLiteral(" 帧/秒"),                     QStringLiteral(" fps") },
        { QStringLiteral("编码器"),                     QStringLiteral("Codec") },
        { QStringLiteral("应用到选中联系人"),           QStringLiteral("Apply to selected contact") },
        { QStringLiteral("先在上面选一个联系人。"),     QStringLiteral("Select a contact above first.") },
        { QStringLiteral("RustDesk 还没为「%1」建过设置文件 —— 它是在<b>第一次连接</b>时才建的。<br>"
                         "先连一次这个联系人，再回来调画质。上面显示的是默认值。"),
          QStringLiteral("RustDesk hasn't created a settings file for \"%1\" yet — it only does "
                         "that on the <b>first connection</b>.<br>Connect to this contact once, "
                         "then come back to tune quality. The values above are the defaults.") },
        { QStringLiteral("⚠ 文件里存的是「%1」，不在可选的四档里（RustDesk 自己的界面也会往"
                         "这个文件写值）。上面显示的是默认档 —— 按「应用」会把它覆盖掉。"),
          QStringLiteral("⚠ The file stores \"%1\", which is not one of the four options here "
                         "(RustDesk's own UI writes to this file too). The value above is the "
                         "default — clicking \"Apply\" will overwrite it.") },
        { QStringLiteral("当前存在 %1.toml 里。改完点右边那个按钮才写盘。"),
          QStringLiteral("Currently stored in %1.toml. Nothing is written until you click the "
                         "button on the right.") },
        { QStringLiteral("先选一个联系人"),             QStringLiteral("Select a contact first") },
        { QStringLiteral("「%1」的画质已保存（下次连接生效）"),
          QStringLiteral("Quality settings for \"%1\" saved (they apply on the next connection)") },

        // ── 设置：网络路径 ────────────────────────────────────────────────
        { QStringLiteral("线路"),
          QStringLiteral("Line") },
        { QStringLiteral("档位"),                       QStringLiteral("Mode") },
        { QStringLiteral("信令服务器"),                 QStringLiteral("Rendezvous server") },
        { QStringLiteral("hbbs 地址，host 或 host:port"),
          QStringLiteral("hbbs address, host or host:port") },
        { QStringLiteral("中继服务器"),                 QStringLiteral("Relay server") },
        { QStringLiteral("hbbr 地址，跟信令同一台就填一样的"),
          QStringLiteral("hbbr address — the same as the rendezvous one if it's the same machine") },
        { QStringLiteral("API 服务器"),                 QStringLiteral("API server") },
        { QStringLiteral("可留空（只有要网页控制台时才需要）"),
          QStringLiteral("Can be left empty (only needed for the web console)") },
        { QStringLiteral("服务器公钥"),                 QStringLiteral("Server public key") },
        { QStringLiteral("hbbs 首次启动时打印的那串"),
          QStringLiteral("The string hbbs prints on its first start") },
        { QStringLiteral("本机主机名"),                 QStringLiteral("This PC's hostname") },
        { QStringLiteral("这台机器在 tailnet 上的名字"),
          QStringLiteral("This machine's name on the tailnet") },
        { QStringLiteral("授权密钥"),                   QStringLiteral("Auth key") },
        { QStringLiteral("Tailscale authkey（要 reusable、不过期）"),
          QStringLiteral("Tailscale auth key (must be reusable and non-expiring)") },
        { QStringLiteral("自检"),                       QStringLiteral("Verify") },
        { QStringLiteral("一键体检"),                   QStringLiteral("Run network check") },
        { QStringLiteral("两台机器组成一张私有网，RustDesk 的流量从里面走。"
                         "本机主机名两台必须不一样。"),
          QStringLiteral("The two machines form a private network and RustDesk's traffic runs "
                         "over it. The two hostnames must differ.") },
        { QStringLiteral("自己搭的 hbbs/hbbr，走 Cloudflare Workers。会自动打开 websocket。"),
          QStringLiteral("Your own hbbs/hbbr, reached through Cloudflare Workers. WebSocket is "
                         "switched on automatically.") },
        { QStringLiteral("自己搭的 hbbs/hbbr，有真 TCP 端口（VPS / frp / 端口映射）。"),
          QStringLiteral("Your own hbbs/hbbr with real TCP ports (VPS / frp / port forwarding).") },
        { QStringLiteral("用 RustDesk 官方服务器，什么都不用配。"
                         "先点「一键体检」看看信令通不通。"),
          QStringLiteral("Use RustDesk's official servers — nothing to configure. Run the "
                         "network check first to see whether signalling gets through.") },
        { QStringLiteral("已切换到「%1」，RustDesk 配置写好了"),
          QStringLiteral("Switched to \"%1\"; the RustDesk config is written") },
        { QStringLiteral("没写成：%1"),                 QStringLiteral("Couldn't write it: %1") },
        { QStringLiteral("自检通过 —— RustDesk 配置跟界面上的档位一致"),
          QStringLiteral("Verified — the RustDesk config matches the mode shown here") },
        { QStringLiteral("有 %1 处对不上（点“立即应用”重写一遍）：%2"),
          QStringLiteral("%1 mismatch(es) (click \"Apply now\" to rewrite): %2") },
        { QStringLiteral("还在体检中…"),                QStringLiteral("The network check is still running…") },
        { QStringLiteral("<span style='color:#999999;'>正在体检"
                         "（DNS / 延迟 / UDP / NAT / IPv6，最长约 1 分钟）…</span>"),
          QStringLiteral("<span style='color:#999999;'>Running the network check "
                         "(DNS / latency / UDP / NAT / IPv6, up to about a minute)…</span>") },
        { QStringLiteral("开始网络体检…"),              QStringLiteral("Starting the network check…") },
        { QStringLiteral("体检完成 —— 没有硬性问题，可以用"),
          QStringLiteral("Check finished — no hard problems, you're good to go") },
        { QStringLiteral("体检完成 —— 有 %1 项会影响远程，看下面红色的"),
          QStringLiteral("Check finished — %1 item(s) will affect remote access; see the red "
                         "lines below") },
        // 主界面那行「线路：官方服务器 · 直连」。**拆成两段**而不是一整句带 HTML ——
        // 颜色和「直连/中继」是运行时拼进去的，整句塞进译文里翻译时很容易被改坏。
        { QStringLiteral("线路：%1"),                   QStringLiteral("Line: %1") },
        { QStringLiteral("还没连过"),                   QStringLiteral("not connected yet") },
        { QStringLiteral("直连"),                       QStringLiteral("peer-to-peer") },
        { QStringLiteral("中继"),                       QStringLiteral("relayed") },
        { QStringLiteral("还没连过 —— 连上之后这里会显示走的是 P2P 直连还是中继。"),
          QStringLiteral("Not connected yet — once you are, this shows whether traffic went "
                         "peer-to-peer or through a relay.") },
        { QStringLiteral("换线路…"),                    QStringLiteral("Change line…") },
        { QStringLiteral("官方服务器 / 自建 / tailnet —— 官方那条慢就换一条"),
          QStringLiteral("Official / self-hosted / tailnet — switch if the official one is slow") },

        // ── 设置：标签页标题（第一页的「连接」复用上面连接按钮那条）──
        { QStringLiteral("别人连我"),                   QStringLiteral("Others reach me") },
        { QStringLiteral("界面"),                       QStringLiteral("Interface") },

        // ── 设置：语言 ────────────────────────────────────────────────────
        { QStringLiteral("界面语言"),                   QStringLiteral("Interface language") },
        // ── 设置：开机与后台 ──────────────────────────────────────────────
        // ⚠ 这里原来写的是 "Startup & background" —— 裸 & 会被 Qt 当助记符，
        //   界面上渲染成 "Startup _background"（吃掉一个字符 + 下划线跑到 b 上）。
        //   就是 check_i18n.py 那条致命项抓出来的。要用 "and"。
        { QStringLiteral("开机与后台"),                 QStringLiteral("Startup and background") },
        { QStringLiteral("开机自动运行，起来只待在托盘里"),
          QStringLiteral("Start automatically at startup, and stay in the tray") },
        // ⚠ 英文这句里"after you sign in"不能省：写进 HKCU 的 Run 键是**登录时**才跑的，
        //   机器设了登录密码的话，没人登录之前仍然是连不上的。这是这套方案最大的限制。
        { QStringLiteral("已经开着：系统<b>登录之后</b>会自动起来，只留一个托盘图标，窗口不会弹出来。"
                         "（如果开机要输密码才能进桌面，在你登录之前仍然是连不上的。）"
                         "关掉这一项会把注册表里那条删掉。"),
          QStringLiteral("On: it starts automatically <b>after you sign in</b>, leaves only a "
                         "tray icon, and never pops the window up. (If this machine asks for a "
                         "password to reach the desktop, it still can't be reached until you "
                         "sign in.) Turning this off deletes the entry from the registry.") },
        { QStringLiteral("关着。打开之后：系统<b>登录之后</b>会自动起来，只留一个托盘图标，"
                         "窗口不会弹出来。（如果开机要输密码才能进桌面，在你登录之前仍然是连不上的。）"),
          QStringLiteral("Off. When you turn it on: it starts automatically <b>after you sign "
                         "in</b>, leaves only a tray icon, and never pops the window up. (If "
                         "this machine asks for a password to reach the desktop, it still can't "
                         "be reached until you sign in.)") },
        { QStringLiteral("没设置成功：%1"),
          QStringLiteral("Couldn't set it up: %1") },
        { QStringLiteral("设置开机自启失败"),
          QStringLiteral("Couldn't set up start-on-login") },
        { QStringLiteral("已设为开机自动运行（下次登录后生效）"),
          QStringLiteral("Start-on-login is on (takes effect the next time you sign in)") },
        { QStringLiteral("已取消开机自动运行"),
          QStringLiteral("Start-on-login is off") },
        // Autostart.cpp 里那几句（写注册表失败时的原因）
        { QStringLiteral("找不到本程序自己的可执行文件（%1），没法写自启"),
          QStringLiteral("Can't find this program's own executable (%1), so the start-up entry "
                         "can't be written") },
        { QStringLiteral("写了注册表但读回来对不上（可能被安全软件或组策略挡了）"),
          QStringLiteral("The registry was written but reading it back doesn't match (security "
                         "software or group policy may be blocking it)") },
        { QStringLiteral("删不掉注册表里那条自启（可能被安全软件或组策略挡了）"),
          QStringLiteral("Couldn't delete that start-up entry from the registry (security "
                         "software or group policy may be blocking it)") },

        { QStringLiteral("改完要重启才生效"),           QStringLiteral("Takes effect after a restart") },
        { QStringLiteral("立即重启"),                   QStringLiteral("Restart now") },
        { QStringLiteral("重启后才能换成英文。点「立即重启」，软件会自己重开。"),
          QStringLiteral("English starts working after a restart. Click \"Restart now\" and the "
                         "app will reopen itself.") },
        { QStringLiteral("重启后才能换回中文。点「立即重启」，软件会自己重开。"),
          QStringLiteral("Chinese starts working after a restart. Click \"Restart now\" and the "
                         "app will reopen itself.") },

        // ── 状态栏 / 通知 ─────────────────────────────────────────────────
        { QStringLiteral("正在重启 RustDesk 以应用密码…"),
          QStringLiteral("Restarting RustDesk to apply the password…") },
        { QStringLiteral("正在应用密码…"),              QStringLiteral("Applying the password…") },
        { QStringLiteral("限时密码已生效 —— 到点自动作废"),
          QStringLiteral("Time-limited password is active — it expires on schedule") },
        { QStringLiteral("密码已应用"),                 QStringLiteral("Password applied") },
        { QStringLiteral("应用失败"),                   QStringLiteral("Apply failed") },
        { QStringLiteral("换一个连接码"),               QStringLiteral("Generate a new password") },
        { QStringLiteral("会立刻生成一个新密码。<br><br>"
                         "<b>正在连着的远程会话会断</b> —— 换密码必须重启 RustDesk"
                         "（大约几秒连不上），重新连需要用新密码。<br>"
                         "旧密码从此作废，之前把码发出去的人再连就连不上了。<br><br>要继续吗？"),
          QStringLiteral("A new password is generated right away.<br><br>"
                         "<b>Any remote session that is open right now will drop</b> — changing "
                         "the password requires restarting RustDesk (a few seconds of downtime), "
                         "and the other side has to reconnect with the new password.<br>"
                         "The old password stops working from then on, so anyone you gave it to "
                         "can no longer get in.<br><br>Continue?") },
        { QStringLiteral("立刻换一个新码（会断开当前的远程会话 —— RustDesk 要重启才认新密码）"),
          QStringLiteral("Generate a new password right away (this drops any open remote "
                         "session — RustDesk only reads a new password after a restart)") },
        { QStringLiteral("找不到 RustDesk（还没解包 / 路径不对），打不开它的窗口"),
          QStringLiteral("RustDesk not found (not unpacked yet, or the path is wrong) — can't "
                         "open its window") },
        { QStringLiteral("已经打开 RustDesk 窗口 —— 一次性密码显示在那里"),
          QStringLiteral("RustDesk's window is open — the one-time password is shown there") },
        { QStringLiteral("打不开 RustDesk 窗口"),
          QStringLiteral("Couldn't open RustDesk's window") },
        // 「立即应用」的**名字**，作为确认框里的 %1 用（引号一起翻）。
        // （以前还有第二个入口「换成外壳能记住的新密码」，那个按钮已删。）
        { QStringLiteral("「立即应用」"),               QStringLiteral("\"Apply now\"") },
        { QStringLiteral("用完即换：刚才那位已经断开，密码已换成新的"),
          QStringLiteral("Use once: the visitor has disconnected, so the password has been "
                         "changed") },
        { QStringLiteral("用完即换：换密码失败（%1）—— 下次有人连过再试"),
          QStringLiteral("Use once: changing the password failed (%1) — will retry after the "
                         "next visit") },
        { QStringLiteral("正在重启 RustDesk 以换新密码…"),
          QStringLiteral("Restarting RustDesk to change the password…") },
        { QStringLiteral("已换成新密码"),               QStringLiteral("New password is in place") },
        { QStringLiteral("换码失败"),                   QStringLiteral("Couldn't change the password") },
        { QStringLiteral("正在重启 RustDesk 以应用连接提示…"),
          QStringLiteral("Restarting RustDesk to apply the connection prompt…") },
        { QStringLiteral("正在应用连接提示…"),          QStringLiteral("Applying the connection prompt…") },
        { QStringLiteral("连接提示已应用"),             QStringLiteral("Connection prompt applied") },
        { QStringLiteral("有人连进来了"),               QStringLiteral("Someone connected in") },
        { QStringLiteral("有人正在远程控制这台电脑。\n"
                         "（不知道是谁 —— 被控端日志里没有对方的 ID）"),
          QStringLiteral("Someone is controlling this computer remotely.\n"
                         "(No idea who — the host-side log carries no peer ID.)") },
        { QStringLiteral("已最小化到托盘，仍在后台待连"),
          QStringLiteral("Minimised to the tray, still waiting for connections in the background") },

        // ── 托盘菜单 ──────────────────────────────────────────────────────
        { QStringLiteral("本机 ID：—"),                 QStringLiteral("This PC's ID: —") },
        { QStringLiteral("状态：启动中…"),              QStringLiteral("Status: starting…") },
        { QStringLiteral("显示主窗口"),                 QStringLiteral("Show main window") },
        { QStringLiteral("连接…"),                      QStringLiteral("Connect…") },
        { QStringLiteral("重新读取 ID"),                QStringLiteral("Reload ID") },
        { QStringLiteral("退出"),                       QStringLiteral("Quit") },
        { QStringLiteral("—"),                          QStringLiteral("—") },
        { QStringLiteral("本机 ID：%1"),                QStringLiteral("This PC's ID: %1") },
        { QStringLiteral("PureRemote\n本机 ID：%1\n%2"),
          QStringLiteral("PureRemote\nThis PC's ID: %1\n%2") },
        { QStringLiteral("状态：%1"),                   QStringLiteral("Status: %1") },

        // ── RustDeskController ────────────────────────────────────────────
        { QStringLiteral("正在拉起 RustDesk 常驻服务…"),
          QStringLiteral("Starting the RustDesk background service…") },
        { QStringLiteral("RustDesk 后台服务不在了，正在重新拉起…"),
          QStringLiteral("The RustDesk background service is gone; starting it again…") },
        { QStringLiteral("重新拉起 RustDesk 失败 —— 检查 %1 是否还在"),
          QStringLiteral("Couldn't restart RustDesk — check that %1 still exists") },
        { QStringLiteral("找不到 rustdesk.exe：%1"),
          QStringLiteral("rustdesk.exe not found: %1") },
        { QStringLiteral("首次运行：正在解包 RustDesk 运行时…"),
          QStringLiteral("First run: unpacking the RustDesk runtime…") },
        { QStringLiteral("正在读取本机 ID…"),           QStringLiteral("Reading this PC's ID…") },
        { QStringLiteral("就绪"),                       QStringLiteral("Ready") },
        { QStringLiteral("读不到 ID：RustDesk 未就绪"),
          QStringLiteral("No ID yet: RustDesk isn't ready") },
        { QStringLiteral("读不到"),                     QStringLiteral("unreadable") },
        { QStringLiteral("不是合法的 UTF-8（%1 字节，至少有一处坏字节）—— "
                         "不认它就不会动它"),
          QStringLiteral("Not valid UTF-8 (%1 bytes, at least one bad byte) — refusing to "
                         "touch it rather than mangle it") },
        { QStringLiteral("没有要写的配置项"),           QStringLiteral("No config entries to write") },
        { QStringLiteral("配置项名为空"),               QStringLiteral("Config entry name is empty") },
        { QStringLiteral("配置项 %1 的值里有单引号或反斜杠，会写出非法 TOML"),
          QStringLiteral("The value for %1 contains a single quote or backslash, which would "
                         "produce invalid TOML") },
        { QStringLiteral("读不到 %1（RustDesk 至少得跑过一次）"),
          QStringLiteral("Couldn't read %1 (RustDesk must have run at least once)") },
        { QStringLiteral("读不了 %1：%2"),              QStringLiteral("Couldn't read %1: %2") },
        { QStringLiteral("RustDesk2.toml 里没有 [options] 段 —— 不猜位置，宁可不写"),
          QStringLiteral("RustDesk2.toml has no [options] section — refusing to guess where "
                         "to write") },
        { QStringLiteral("备份 %1 失败，为安全起见没有动原文件"),
          QStringLiteral("Couldn't back up %1, so the original was left untouched") },
        { QStringLiteral("写不进 %1（或只写进去一部分）"),
          QStringLiteral("Couldn't write %1 (or only wrote part of it)") },
        { QStringLiteral("写入后复验 %1 没对上，已整体回滚"),
          QStringLiteral("Verification of %1 failed after writing; rolled everything back") },
        { QStringLiteral("写入后复验 %1 没对上，回滚也失败了 —— 备份在 %2"),
          QStringLiteral("Verification of %1 failed after writing, and the rollback failed too "
                         "— the backup is at %2") },
        { QStringLiteral("找不到 bin/rustdesk.exe"),
          QStringLiteral("bin/rustdesk.exe not found") },
        { QStringLiteral("没有可连的地址（ID 和 tailnet 主机名都是空的）"),
          QStringLiteral("Nothing to connect to (both the ID and the tailnet hostname are empty)") },
        { QStringLiteral("拉不起连接客户端：%1"),
          QStringLiteral("Couldn't start the connection client: %1") },

        // ── ConnectionWatcher ─────────────────────────────────────────────
        { QStringLiteral("读不到 APPDATA，没法盯 RustDesk 日志"),
          QStringLiteral("Can't read APPDATA, so RustDesk's log can't be watched") },

        // ── ConnectSession ────────────────────────────────────────────────
        { QStringLiteral("写不进桥接的对端文件 —— 程序目录可能是只读的"),
          QStringLiteral("Couldn't write the bridge's peer file — the program folder may be read-only") },
        { QStringLiteral("未连接"),                     QStringLiteral("Not connected") },
        { QStringLiteral("正在登录 tailnet…"),          QStringLiteral("Signing in to the tailnet…") },
        { QStringLiteral("正在等对方上线…"),            QStringLiteral("Waiting for the other side to come online…") },
        { QStringLiteral("正在发起连接…"),              QStringLiteral("Starting the connection…") },
        { QStringLiteral("已发起 —— 见 RustDesk 窗口"),
          QStringLiteral("Started — see the RustDesk window") },
        { QStringLiteral("已经在连接中了 —— 先断开再试"),
          QStringLiteral("A connection is already in progress — disconnect first") },
        { QStringLiteral("先在上面的列表里选一条联系人"),
          QStringLiteral("Pick a contact from the list above first") },
        { QStringLiteral("这条联系人没填 RustDesk ID —— 改成官方 ID 档就得把它补上，"
                         "或者切到 tailnet 档"),
          QStringLiteral("This contact has no RustDesk ID — either fill it in for the official "
                         "mode, or switch to the tailnet mode") },
        { QStringLiteral("没有可连的地址"),             QStringLiteral("There's no address to connect to") },
        { QStringLiteral("这条联系人没填 tailnet 主机名 —— 切到官方 ID 档，或把它补上"),
          QStringLiteral("This contact has no tailnet hostname — switch to the official mode, "
                         "or fill it in") },
        { QStringLiteral("tailnet 桥接没准备好（pmrelay.exe 没找到）"),
          QStringLiteral("The tailnet bridge isn't ready (pmrelay.exe not found)") },
        { QStringLiteral("还没填 Tailscale 授权密钥 —— 见下面的“准备情况”"),
          QStringLiteral("No Tailscale auth key yet — see the checklist below") },
        { QStringLiteral("还没填本机 tailnet 主机名 —— 见下面的“准备情况”"),
          QStringLiteral("This PC's tailnet hostname isn't set yet — see the checklist below") },
        { QStringLiteral("拉不起桥接程序 pmrelay.exe"),
          QStringLiteral("Couldn't start the bridge program pmrelay.exe") },
        { QStringLiteral("拉不起连接客户端"),           QStringLiteral("Couldn't start the connection client") },
        { QStringLiteral("连接客户端退出（代码 %1）"),
          QStringLiteral("The connection client exited (code %1)") },
        { QStringLiteral("对端上线了，但这条联系人没填 tailnet 主机名"),
          QStringLiteral("The other side came online, but this contact has no tailnet hostname") },
        { QStringLiteral("等了 %1 秒，对方还没在 tailnet 上出现 —— 最常见的原因是对方那台"
                         "没开着本程序（程序不开，它在 tailnet 上就不存在），其次是对方的 "
                         "tailnet 主机名填错了。%2"),
          QStringLiteral("Waited %1 seconds and the other side still hasn't appeared on the "
                         "tailnet — the usual cause is that their machine is not running this "
                         "app (with it closed they simply do not exist on the tailnet); the "
                         "next most common is a misspelled tailnet hostname. %2") },
        { QStringLiteral("（本机已经在 tailnet 上了）"),
          QStringLiteral("(this machine is already on the tailnet)") },
        { QStringLiteral("（本机还没登上 tailnet：%1）"),
          QStringLiteral("(this machine could not join the tailnet: %1)") },
        { QStringLiteral("（本机还在登录 tailnet）"),
          QStringLiteral("(this machine is still logging in to the tailnet)") },
        { QStringLiteral("这台机器没登上 tailnet（%1）—— 先在设置里把线路配好，"
                         "它登上去了对方才看得见"),
          QStringLiteral("This machine is not on the tailnet (%1) — fix the network path in "
                         "Settings first; the peer can only see this machine once it has "
                         "joined") },
        { QStringLiteral("%1：%2"),                     QStringLiteral("%1: %2") },
        { QStringLiteral("桥接断了"),                   QStringLiteral("The bridge went down") },

        // ── PasswordPolicy ────────────────────────────────────────────────
        { QStringLiteral("停不掉正在运行的 RustDesk，已放弃写入（否则会被它回写覆盖）"),
          QStringLiteral("Couldn't stop the running RustDesk, so the write was abandoned "
                         "(it would otherwise be overwritten by the process)") },
        { QStringLiteral("写 %1 失败（写前已备份到 RustDesk.toml.bak，失败会自动回滚）"),
          QStringLiteral("Couldn't write %1 (backed up to RustDesk.toml.bak beforehand; "
                         "failures roll back automatically)") },
        { QStringLiteral("外壳配置写不进去（%1）"),
          QStringLiteral("Couldn't save the shell's config (%1)") },
        { QStringLiteral("启动补轮换失败：%1"),
          QStringLiteral("Catching up on a missed rotation failed: %1") },
        { QStringLiteral("限时密码在关机期间已到期，已作废"),
          QStringLiteral("The time-limited password expired while the PC was off and has been "
                         "invalidated") },
        { QStringLiteral("限时密码已到期，但作废失败 —— 稍后会自动重试"),
          QStringLiteral("The time-limited password expired, but invalidating it failed — it "
                         "will be retried automatically") },
        { QStringLiteral("限时密码已到期，已作废 —— 现在只能靠屏幕上的一次性密码连"),
          QStringLiteral("The time-limited password expired and has been invalidated — only "
                         "the on-screen one-time password works now") },
        { QStringLiteral("限时密码到期了但作废失败，稍后重试"),
          QStringLiteral("The time-limited password expired but couldn't be invalidated; "
                         "retrying later") },
        { QStringLiteral("密码已轮换"),                 QStringLiteral("Password rotated") },
        { QStringLiteral("轮换失败：%1"),               QStringLiteral("Rotation failed: %1") },

        // ── NotifyPolicy ──────────────────────────────────────────────────
        { QStringLiteral("停不掉正在运行的 RustDesk，已放弃写入（否则会被它回读覆盖）"),
          QStringLiteral("Couldn't stop the running RustDesk, so the write was abandoned "
                         "(it would otherwise be read back and overwritten)") },
        { QStringLiteral("写 %1 的 [options] 失败（写前已备份，失败会自动回滚）"),
          QStringLiteral("Couldn't write the [options] section of %1 (backed up beforehand; "
                         "failures roll back automatically)") },
        { QStringLiteral("已开启连接确认：有人连进来要你点接受才连得上"),
          QStringLiteral("Connection confirmation enabled: you must click Accept before anyone "
                         "gets in") },
        { QStringLiteral("已开启连接提示：有人连进来会弹通知，不拦连接"),
          QStringLiteral("Connection notice enabled: you get a notification, but nothing is blocked") },
        { QStringLiteral("连接提示已关闭"),             QStringLiteral("Connection notice turned off") },

        // ── AppConfig：四个网络档位的名字 ─────────────────────────────────
        { QStringLiteral("tailnet（自建 tsnet 桥接）"),
          QStringLiteral("tailnet (self-hosted tsnet bridge)") },
        { QStringLiteral("Cloudflare Workers（自建）"),
          QStringLiteral("Cloudflare Workers (self-hosted)") },
        { QStringLiteral("自填地址（frp / 端口映射 / 自建 VPS）"),
          QStringLiteral("Custom address (frp / port forwarding / your own VPS)") },
        { QStringLiteral("官方服务器"),                 QStringLiteral("Official servers") },

        // ── NetworkProfile：换档时的校验错误 ──────────────────────────────
        { QStringLiteral("认不出来的网络档位：%1"),
          QStringLiteral("Unrecognised network mode: %1") },
        { QStringLiteral("还没填信令服务器地址。\n"
                         "自建服务器至少要有一个 hbbs 地址，否则客户端不知道该找谁。"),
          QStringLiteral("The rendezvous server address is still empty.\nA self-hosted setup "
                         "needs at least one hbbs address, otherwise clients don't know who to "
                         "talk to.") },
        { QStringLiteral("服务器地址里不能有单引号或反斜杠：%1"),
          QStringLiteral("Server addresses can't contain single quotes or backslashes: %1") },
        { QStringLiteral("tsnet 档要有 Tailscale 授权密钥（authkey）。\n"
                         "登录控制台 → Settings → Keys 生成一个 reusable 的。"),
          QStringLiteral("The tsnet mode needs a Tailscale auth key.\nSign in to the admin "
                         "console → Settings → Keys and generate a reusable one.") },
        { QStringLiteral("（没这个键）"),               QStringLiteral("(key absent)") },
        { QStringLiteral("%1：期望 %2，实际 %3"),       QStringLiteral("%1: expected %2, got %3") },

        // ── PeerPrefs ─────────────────────────────────────────────────────
        { QStringLiteral("省流（画质系数 0.5）"),       QStringLiteral("Data saver (quality factor 0.5)") },
        { QStringLiteral("平衡（0.67，默认）"),         QStringLiteral("Balanced (0.67, default)") },
        { QStringLiteral("清晰（1.5）"),                QStringLiteral("Sharp (1.5)") },
        { QStringLiteral("自定义（按下面那个数）"),     QStringLiteral("Custom (use the number below)") },
        { QStringLiteral("自动（让 RustDesk 自己挑）"),
          QStringLiteral("Automatic (let RustDesk choose)") },
        { QStringLiteral("H.264（兼容最好）"),          QStringLiteral("H.264 (best compatibility)") },
        { QStringLiteral("H.265 / HEVC（同画质更省带宽）"),
          QStringLiteral("H.265 / HEVC (same quality, less bandwidth)") },
        { QStringLiteral("VP9（软件编码，吃 CPU）"),    QStringLiteral("VP9 (software encoding, CPU-hungry)") },
        { QStringLiteral("AV1（最省带宽，最吃 CPU）"),
          QStringLiteral("AV1 (least bandwidth, heaviest on CPU)") },
        { QStringLiteral("VP8（老，基本不用）"),        QStringLiteral("VP8 (old, rarely useful)") },
        { QStringLiteral("画质档位「%1」不是 RustDesk 认的值（只能 low/balanced/best/custom）"),
          QStringLiteral("Quality \"%1\" is not a value RustDesk accepts "
                         "(only low/balanced/best/custom)") },
        { QStringLiteral("自定义画质要在 0–100 之间，现在是 %1"),
          QStringLiteral("The custom quality must be between 0 and 100; it is %1") },
        { QStringLiteral("帧率要在 1–240 之间，现在是 %1"),
          QStringLiteral("The frame rate must be between 1 and 240; it is %1") },
        { QStringLiteral("编码器「%1」不是 RustDesk 认的值（只能 auto/h264/h265/vp9/av1/vp8）"),
          QStringLiteral("Codec \"%1\" is not a value RustDesk accepts "
                         "(only auto/h264/h265/vp9/av1/vp8)") },
        { QStringLiteral("对端 ID 是空的"),             QStringLiteral("The peer ID is empty") },
        { QStringLiteral("对端 ID「%1」里有路径分隔符，看着不像真的 ID"),
          QStringLiteral("Peer ID \"%1\" contains a path separator, which doesn't look like a "
                         "real ID") },
        { QStringLiteral("RustDesk 还没为这个对端建过设置文件（%1）。\n"
                         "先连一次（建立过会话之后才有地方存画质），再回来调。"),
          QStringLiteral("RustDesk hasn't created a settings file for this peer yet (%1).\n"
                         "Connect once (settings can only be stored after a session exists), "
                         "then come back.") },
        { QStringLiteral("%1 里没有 [options] 段 —— 不猜位置，宁可不写"),
          QStringLiteral("%1 has no [options] section — refusing to guess where to write") },
        { QStringLiteral("写 %1 失败，而且还原也失败了 —— 备份在 %2"),
          QStringLiteral("Couldn't write %1, and restoring failed too — the backup is at %2") },
        { QStringLiteral("写 %1 失败（磁盘满？），已还原"),
          QStringLiteral("Couldn't write %1 (disk full?) — restored") },
        { QStringLiteral("复验没对上，回滚也失败了 —— 备份在 %1，"
                         "把它改名成 %2 就是原文件"),
          QStringLiteral("Verification failed and the rollback failed too — the backup is at "
                         "%1; rename it to %2 to get the original file back") },
        { QStringLiteral("写完后复验没对上，已整体回滚"),
          QStringLiteral("Verification failed after writing; rolled everything back") },
        { QStringLiteral("四个画质键有重复的，回滚也失败了 —— 备份在 %1"),
          QStringLiteral("The four quality keys are duplicated and the rollback failed too — "
                         "the backup is at %1") },
        { QStringLiteral("写完发现画质键各有 %1/%2/%3/%4 个（应该各 1 个）—— "
                         "重复键在 TOML 里非法，已整体回滚"),
          QStringLiteral("After writing, the quality keys appear %1/%2/%3/%4 times (1 each was "
                         "expected) — duplicate keys are invalid TOML, so everything was "
                         "rolled back") },
        { QStringLiteral("键落到别的表里去了，回滚也失败了 —— 备份在 %1，"
                         "把它改名成 %2 就是原文件"),
          QStringLiteral("Keys landed in the wrong table and the rollback failed too — the "
                         "backup is at %1; rename it to %2 to get the original file back") },
        { QStringLiteral("键落错了表（顶层=[%1][%2]、options=[%3][%4]）—— "
                         "文件结构有我没看懂的地方，已整体回滚"),
          QStringLiteral("Keys landed in the wrong table (top-level = [%1][%2], options = "
                         "[%3][%4]) — the file structure isn't what I expected, so everything "
                         "was rolled back") },

        // ── TsnetBridge ───────────────────────────────────────────────────
        { QStringLiteral("找不到 pmrelay.exe"),         QStringLiteral("pmrelay.exe not found") },
        { QStringLiteral("connect 模式必须指定对端"),
          QStringLiteral("connect mode requires a peer") },
        { QStringLiteral("pmrelay 启动失败"),           QStringLiteral("pmrelay failed to start") },
        { QStringLiteral("pmrelay 意外退出（退出码 0）"),
          QStringLiteral("pmrelay exited unexpectedly (exit code 0)") },
        { QStringLiteral("pmrelay 退出，代码 %1"),      QStringLiteral("pmrelay exited with code %1") },
        { QStringLiteral("tailnet %1:%2 → 本机 127.0.0.1:%3"),
          QStringLiteral("tailnet %1:%2 → local 127.0.0.1:%3") },
        { QStringLiteral("%1 → %2（经 tailnet）"),      QStringLiteral("%1 → %2 (over the tailnet)") },
        { QStringLiteral("%1 已在 tailnet 上（%2:%3）；本机 %4 → 对端"),
          QStringLiteral("%1 is on the tailnet (%2:%3); local %4 → peer") },

        // ── NetworkProbe：体检结论 ────────────────────────────────────────
        { QStringLiteral("连接路径：中继转发（打洞没成功，走服务器中转）"),
          QStringLiteral("Connection path: relayed (hole punching failed, going through a "
                         "server)") },
        { QStringLiteral("连接路径：P2P 直连（打洞成功）"),
          QStringLiteral("Connection path: peer-to-peer (hole punching worked)") },
        { QStringLiteral("短包（%1 字节）"),            QStringLiteral("Short packet (%1 bytes)") },
        { QStringLiteral("不是一个 Binding Success Response（type=0x%1）"),
          QStringLiteral("Not a Binding Success Response (type=0x%1)") },
        { QStringLiteral("没有 MAPPED-ADDRESS / XOR-MAPPED-ADDRESS 属性"),
          QStringLiteral("No MAPPED-ADDRESS / XOR-MAPPED-ADDRESS attribute") },
        { QStringLiteral("DNS 解析失败"),               QStringLiteral("DNS lookup failed") },
        { QStringLiteral("没有 IPv4 地址"),             QStringLiteral("No IPv4 address") },
        { QStringLiteral("发送失败"),                   QStringLiteral("Send failed") },
        { QStringLiteral("超时无应答（UDP 被拦，或该服务器不回）"),
          QStringLiteral("Timed out with no reply (UDP blocked, or this server doesn't answer)") },
        { QStringLiteral("解析不了官方服务器 %1"),
          QStringLiteral("Can't resolve the official server %1") },
        { QStringLiteral("DNS 查询失败：%1\n"
                         "→ 换 DNS（223.5.5.5 / 119.29.29.29）或者改用自建服务器。"),
          QStringLiteral("DNS lookup failed: %1\n"
                         "→ Switch DNS (223.5.5.5 / 119.29.29.29) or move to a self-hosted "
                         "server.") },
        { QStringLiteral("官方服务器 %1 解析正常"),
          QStringLiteral("Official server %1 resolves fine") },
        { QStringLiteral("官方信令端口连通（%1 ms）"),
          QStringLiteral("Official signalling port reachable (%1 ms)") },
        { QStringLiteral("%1:%2  最快 %3 / 中位 %4 / 最慢 %5 ms"),
          QStringLiteral("%1:%2  best %3 / median %4 / worst %5 ms") },
        { QStringLiteral("官方信令端口连不上"),
          QStringLiteral("Official signalling port unreachable") },
        { QStringLiteral("%1:%2  %3\n"
                         "→ 信令不通就建立不了连接，换自建服务器或检查防火墙。"),
          QStringLiteral("%1:%2  %3\n"
                         "→ Without signalling no connection can be established; switch to a "
                         "self-hosted server or check the firewall.") },
        { QStringLiteral("官方中继端口连通（%1 ms）"),
          QStringLiteral("Official relay port reachable (%1 ms)") },
        { QStringLiteral("%1:%2  中位 %3 ms"),
          QStringLiteral("%1:%2  median %3 ms") },
        { QStringLiteral("官方中继端口不可达"),
          QStringLiteral("Official relay port unreachable") },
        { QStringLiteral("%1:%2  %3\n"
                         "→ 只影响「打洞失败后走中继」这条退路；直连不受影响。"),
          QStringLiteral("%1:%2  %3\n"
                         "→ Only affects the fallback used when hole punching fails; direct "
                         "connections are unaffected.") },
        { QStringLiteral("%1（%2）"),                   QStringLiteral("%1 (%2)") },
        { QStringLiteral("UDP 出站没有收到任何回应"),
          QStringLiteral("No replies at all on UDP egress") },
        { QStringLiteral("逐个问过 %1 个 STUN 服务器，一个都没回：\n  %2\n"
                         "→ 打洞需要 UDP 出站。若是公司/校园网封了 UDP，"
                         "只能走中继或自建服务器。"),
          QStringLiteral("Asked %1 STUN servers one by one and not one replied:\n  %2\n"
                         "→ Hole punching needs UDP egress. If a corporate or campus network "
                         "blocks UDP, the only options are a relay or a self-hosted server.") },
        { QStringLiteral("NAT 类型：判不出来（UDP 不通）"),
          QStringLiteral("NAT type: can't tell (UDP is blocked)") },
        { QStringLiteral("UDP 出站被拦，NAT 行为无从观察。"),
          QStringLiteral("UDP egress is blocked, so NAT behaviour can't be observed.") },
        { QStringLiteral("UDP 出站正常（%1/%2 个 STUN 有回应）"),
          QStringLiteral("UDP egress is fine (%1/%2 STUN servers replied)") },
        { QStringLiteral("全部服务器都有回应。"),
          QStringLiteral("Every server replied.") },
        { QStringLiteral("没回的：\n  %1"),             QStringLiteral("No reply from:\n  %1") },
        { QStringLiteral("样本不足，判不了（只有这一个服务器从这个出口回话）"),
          QStringLiteral("Not enough samples to judge (only this one server replied from this "
                         "egress)") },
        { QStringLiteral("Cone（端口保持，利于打洞）"),
          QStringLiteral("Cone (port preserved, good for hole punching)") },
        { QStringLiteral("Symmetric（端口被重映射，打洞难）"),
          QStringLiteral("Symmetric (port remapped, hole punching is hard)") },
        { QStringLiteral("%1 → 映射端口 %2  ⇒ %3"),
          QStringLiteral("%1 → mapped port %2  ⇒ %3") },
        { QStringLiteral("NAT 类型：判不出来（每个出口都只有单个样本）"),
          QStringLiteral("NAT type: can't tell (every egress has only a single sample)") },
        { QStringLiteral("NAT 类型：混合（不同出口行为不一样）"),
          QStringLiteral("NAT type: mixed (different egresses behave differently)") },
        { QStringLiteral("NAT 类型：Symmetric（打洞成功率低）"),
          QStringLiteral("NAT type: Symmetric (hole punching often fails)") },
        { QStringLiteral("NAT 类型：Cone（利于打洞）"),
          QStringLiteral("NAT type: Cone (good for hole punching)") },
        { QStringLiteral("\n（同一个本地端口问不同目的地 → 映射端口变 = Symmetric）"),
          QStringLiteral("\n(one local port asking different destinations → mapped port "
                         "changes = Symmetric)") },
        { QStringLiteral("IPv6 可用"),                  QStringLiteral("IPv6 available") },
        { QStringLiteral("IPv6 不可用（只有链路本地）"),
          QStringLiteral("IPv6 unavailable (link-local only)") },
        { QStringLiteral("没有全局 IPv6 地址 —— 打洞只能用 IPv4 通道。\n"
                         "这只是个事实，不影响使用。"),
          QStringLiteral("No global IPv6 address — hole punching can only use the IPv4 "
                         "channel.\nJust a fact, doesn't affect usage.") },
        { QStringLiteral("自建信令"),                   QStringLiteral("Self-hosted rendezvous") },
        { QStringLiteral("自建中继"),                   QStringLiteral("Self-hosted relay") },
        { QStringLiteral("自建 API"),                   QStringLiteral("Self-hosted API") },
        { QStringLiteral("%1 %2:%3 可达（%4 ms）"),
          QStringLiteral("%1 %2:%3 reachable (%4 ms)") },
        { QStringLiteral("中位延迟 %1 ms"),             QStringLiteral("Median latency %1 ms") },
        { QStringLiteral("%1 %2:%3 连不上"),           QStringLiteral("%1 %2:%3 unreachable") },
        { QStringLiteral("%1\n→ 检查这台服务器的端口有没有放行、地址填得对不对。"),
          QStringLiteral("%1\n→ Check that this server's port is open and the address is "
                         "spelled correctly.") },

        // ── Guide：四个主题的标题 ─────────────────────────────────────────
        { QStringLiteral("先用起来（默认档，什么都不用配）"),
          QStringLiteral("Get started (default mode, nothing to configure)") },
        { QStringLiteral("进阶：让连接更快不卡（tailnet）"),
          QStringLiteral("Advanced: make connections faster and smoother (tailnet)") },
        { QStringLiteral("自建服务器（Cloudflare Workers）"),
          QStringLiteral("Self-hosted server (Cloudflare Workers)") },
        { QStringLiteral("自建服务器（自己的 VPS / frp / 端口映射）"),
          QStringLiteral("Self-hosted server (your own VPS / frp / port forwarding)") },
        { QStringLiteral("没有这个主题。"),             QStringLiteral("No such topic.") },
    };
    return t;
}

const QHash<QString, QString>& index()
{
    static const QHash<QString, QString> h = [] {
        QHash<QString, QString> out;
        out.reserve(table().size());
        for (const Pair& p : table())
            out.insert(p.zh, p.en);
        return out;
    }();
    return h;
}

} // namespace

I18n::Lang I18n::lang()
{
    return g_lang;
}

void I18n::setLang(Lang l)
{
    g_lang = l;
}

I18n::Lang I18n::systemLang()
{
    // 只认中文 —— 我们的英文表是给"不是中文用户"准备的兜底。
    // 简体/繁体都算中文（表里只有简体，但繁体用户看简体也比看英文近）。
    return QLocale::system().language() == QLocale::Chinese ? Lang::Zh : Lang::En;
}

QString I18n::code(Lang l)
{
    return l == Lang::En ? QStringLiteral("en") : QStringLiteral("zh");
}

QString I18n::code()
{
    return code(g_lang);
}

I18n::Lang I18n::fromCode(const QString& c)
{
    return c.compare(QLatin1String("en"), Qt::CaseInsensitive) == 0 ? Lang::En : Lang::Zh;
}

QString I18n::t(const QString& zh)
{
    // zh 档是恒等函数 —— **不查表**。这是"中文原文是键"这个设计的全部好处：
    // 既有的一堆"报告里含某句中文"的无头断言，在中文档下一个字节都不会变。
    if (g_lang == Lang::Zh)
        return zh;
    const auto it = index().constFind(zh);
    return it == index().constEnd() ? zh : it.value();
}

QString I18n::tDynamic(const QString& s)
{
    if (g_lang == Lang::Zh || s.isEmpty())
        return s;
    const auto it = index().constFind(s);
    return it == index().constEnd() ? s : it.value();
}

QStringList I18n::keys()
{
    QStringList out;
    out.reserve(table().size());
    for (const Pair& p : table())
        out << p.zh;
    return out;
}

QString I18n::englishFor(const QString& zh)
{
    const auto it = index().constFind(zh);
    return it == index().constEnd() ? QString() : it.value();
}

int I18n::entryCount()
{
    return table().size();
}

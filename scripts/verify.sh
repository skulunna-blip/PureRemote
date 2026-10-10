#!/usr/bin/env bash
# PureRemote 无头验收。十七项都要绿才算过。
#
#   1. --print-id 读到的 ID == 随包运行时 --get-id 的输出
#      （断言的是外壳的 QProcess 管道与解析忠实复现了 CLI 输出）
#   2. --screenshot 三张图：主窗口 / 设置对话框 / 图文教程（tsnet 那篇）。
#      面板类 bug 只能靠真实渲染暴露 —— 而主窗口现在只剩两组，
#      另外四组（密码策略/连接提示/画质/网络路径）搬进对话框了，不补那两张
#      等于验收覆盖静默下降（本步判据只是"PNG 非空"，少拍不会报错）
#      ⚠ 设置框分了三页之后，一张图只拍得到当前那一页。`--screenshot-dialog settings:<n>`
#        可以指定抓第 n 页（0=线路与画质 / 1=别人连我 / 2=界面）——
#        新加的「开机与后台」在「界面」页，不切页根本拍不着。
#   3. 二次启动转发给热实例后自己退出，进程数保持 1 且 PID 不变
#   4. 跑完全程 %LOCALAPPDATA%/rustdesk 仍是空的（零系统写入）
#      ⚠ 全项目**唯一**一处有意为之的系统写入是「开机自启」（写 HKCU 的 Run 键一条值）。
#        它默认关闭、**只在用户点那个开关时**才写 —— 本脚本、启动流程、截图流程一律不碰，
#        所以这一条照旧成立。对应的实测是 `--autostarttest`，它故意**不在**下面这 17 项里
#        （那批的前提就是零系统写入），只能手动跑一次：先记原状 → 开关各走一遍 → 还原。
#   5. --pwtest：RustDesk.toml 顶层 password 的读写（17 项断言）
#      ⚠ 这两项跑的是真 RustDesk.toml 的**副本**，用户那份逐字节不会被动（第 5 步末尾有闸门）。
#        以前是拿真文件跑、末尾再写回 —— 中途被打断就永久丢密码，而下次正常启动会**静默**
#        补一个机器生成的新的（用户既连不上也拿不到），2026-10-10 真发生过。
#   6. --poltest：密码策略（Off/Fixed/Rotate/Timed/Once **五**模式 + 轮换 + 限时作废，78 项断言）
#      外加 P4a-P5h：删密码前的确认判据 + **首次运行自动补固定密码**那条纯函数
#      （七个条件逐条钉；P5e 就是那次"把用户原生加密密码当成没有、直接覆盖掉"的事故现场）。
#      新增 P9 系列：`needsApply()` 四种情形（界面那句「未应用」凭什么这么说）、
#      P5b/P5b2：**首次运行 + RustDesk 里已有加密固定密码 → 照样接管**（2026-10-10 改的：
#      以前这一条是"别碰别人的密码"，结果是新用户第一次打开只能看到「已设置（外壳看不到）」
#      + 一个看不懂的按钮；现在开箱就是外壳管着的那一个，能看能复制）、
#      `shouldRotateAfterSession()`
#      （「用完即换」的触发判据：没连过不换 / 刚换过 30 s 内不换 / 正常换），
#      以及 Autostart 那几个**纯函数**（Run 值构造/解析/漂移判定，含带空格路径的引号）。
#      注意 --poltest 全程 manageProcess=false，不会去停/起真 RustDesk。
#      ⚠ 跑之前必须把真 RustDesk 停掉，否则写盘那段会 SKIP（不报失败，但也没验到）。
#   7. --conntest：连接提示 + **会话结束标记**（模式持久化 + approve-mode 落盘 + 日志监听 36 项断言）
#      E 段拿**真日志的副本**逐字节喂：连通标记与 `Connection closed:` 各认一次；
#      只有 `connection manager` 关门的那份**不许**报"对端走了"（那是 RustDesk 自己关停）；
#      另造一份"轮转后新文件比旧游标还大"的场景，钉死那个会被跳标记的坑
#      （RustDesk 重启 = 改名 CURRENT + 建新 CURRENT，新文件长得比旧 pos 快就从中间开读）。
#      日志监听那部分走临时文件，完全不碰真日志；approve-mode 部分同样 manageProcess=false
#   8. RustDesk 常驻：外壳用 `--server`（无窗口）拉起被控端，掉了自动重拉
#      ① --servertest 的 9 项纯解析断言：判据是"命令行里有独立的 `--server`"，喂的是
#         **真机抓下来的 wmic 原文**（含一个反例：只有 --tray 时**必须**判"不在"）。
#      ② 真的停/起一遍：启动后要有 --server；把它全杀掉、只留一个 `--tray` 界面进程
#         （2026-10-10 事故现场）→ 看门狗必须照样拉回来。
#      ⚠ 判据为什么不能是"有没有叫 rustdesk.exe 的进程"：RustDesk **自己的界面进程也叫这个
#        名字**，而且它**不提供服务**（只是 IPC 客户端，现场每秒往 log\tray\ 刷
#        `ipc connection closed: reset by the peer`）。旧判据会让"服务端死了、界面还在"
#        这种状态被判成一切正常 —— 机器静默连不进来，连它自己都报"目标设备离线"。
#      ③ 判据是 log\server\ 那份日志被写 —— 只有 --server 会写那里，等于间接证明了无窗口启动
#   9. --tsnettest：tsnet 桥接（20 项断言：协议解析 T1-T12 + 真两端互连 B1-B8）
#      需要 Tailscale authkey：优先环境变量 TS_AUTHKEY，其次 config/puremote.json 的
#      tsnet_authkey。都没有就 SKIP（不计失败，**也不冒充全绿**）。
#      ⚠ 会在你的 tailnet 里注册两个测试设备（puremote-test-a / -b），不会自动注销 ——
#        嫌乱去 console.tailscale.com/admin/machines 删掉纯测试设备即可。
#      ⚠ B 段固定走中继（hkg）：单机上两个 tsnet 节点会挑到一条走不通的假"直连"
#        （本机自己的 LAN 地址），跨机真 P2P 需要两台机器，仍未验证。
#  10. --booktest：地址簿 / 连接命令 / 时序状态机 / 准备情况（84 项断言）
#      A 段真读写 config/puremote.json（跑完按字节还原）、C 段是 dry-run（**不起真进程**，
#      验的是时序与参数构造，不证明 RustDesk 真会按这个参数连上）、D 段纯函数。
#      E 段要 authkey（同上取法，没有就 SKIP 并**写明这段没验证**）：起一个真 Connect 桥接、
#      对端用一个不存在的 tailnet 主机名，证明"对端不上线就不放行"这条闸门在真 pmrelay 上成立。
#      ⚠ 会在你的 tailnet 里登记 puremote-test-b 这个测试设备（复用第 9 项的，不新增）。
#  11. --netcheck：网络体检 + 连接路径（29 项断言）
#      A 段离线可判（路径标记解析 + tail 循环的"只处理完整行"），永远该绿；
#      B 段是**真体检**（DNS / TCP 延迟 / UDP 出站 / NAT 类型 / IPv6），结论取决于本机网络。
#      报告里的「注意」行是软项（环境事实：IPv6 不可用、官方中继端口不通），不算失败。
#  12. --proftest：四档网络路径 → RustDesk2.toml [options] 的映射（44 项断言）
#      A 段纯函数（四档各写哪些键 / 校验 / 引导文案）；B 段真写 toml，验**事务语义**：
#      一组里有一个坏值就整组不写。B 段要 RustDesk 停着，所以前面先 kill_rustdesk。
#  13. --qtest：画质与性能（按联系人），写 %APPDATA%\RustDesk\config\peers\<对端ID>.toml
#      而不是 RustDesk2.toml 的全局 [options]（后者实测完全无效，见 PeerPrefs.h 类注释）。
#      全程跑在 %TEMP% 的仿真 peers 文件上（A 段纯函数 / B 段真读写仿真文件 / C 段只读真文件），
#      一个字节都不碰真配置。
#      ⚠ 它证明的是"写到哪、写得对、非法值被挡"；**不证明** RustDesk 会按这个档位出画
#        —— 那要真会话，本机只有一台机器，验不了。
#  14. 绿色包结构（dist/PureRemote）：qt.conf / 平台插件 / 内核真身齐全，
#      且没把开发机的 config/puremote.json（含联系人、tailnet 授权密钥）打进去。
#      没打包过就 SKIP。**真启动那一步不在这里** —— 那是 deploy.ps1 -Smoke 干的。
#  15. --uitest：样式表 / 图文教程正文 / 横幅文案 / 翻译表（不起窗口）。
#      拦四类静默错误：样式表占位符漏参数（控件颜色不对但界面照开）、
#      教程链接写错域名、图片没进 qrc（图片框空着，要点开那一页才看得见）、
#      翻译表自相矛盾（英文列里还留着汉字 = 忘翻了；zh 档 t() 不再恒等 = 老断言的地基塌了）。
#      ⚠ 不改渲染结果、只改**看起来**的那两类，全部由 scripts/check_i18n.py 兜（第 17a 步）：
#        裸 `&`（Qt 当助记符吃掉一个字符）、markdown `**`（Qt 不认，星号连字一起画出来）、
#        以及同一句里既写 `<b>` 又写 `\n`（富文本下 \n 不换行，整段挤成一行）。
#        这三种的共同点：长度没变、不报错、断言照绿、**只有看图才发现**。
#  16. --paneltest：真控件状态。全项目**唯一**真的造一个 MainWindow 来读控件状态的测试
#      —— 纯函数测试拦不住的一类是"规则全对，错的是界面喂给规则的输入"，
#      实际发生过的那个死路 bug 正是如此。顺带量三件只能靠尺寸/控件数发现的事：
#      设置框会不会比一屏高（原来 1061px > 1080p 可用高度，底部按钮在屏幕外还没滚动条，
#      而截图判据只是"PNG 非空"、根本拦不住）、标签页有没有空页（漏挂一组只表现为
#      某页空白、不报错）、主界面那行「线路」会不会跟着档位变。
#      ⚠ 这个测试**只在最后 P45/P46 那两块真的 show() 一次**（量完就 hide）：
#        主界面宽度是靠 LayoutRequest 撑起来的，没显示过的窗口收不到那个事件，
#        构造期量到的数偏小（英文档实测 588 vs 真渲染 771），拿它做断言等于自证。
#        P45 量中文档、**P46 另开一个 MainWindow 量英文档** —— 语言是构造时 TR()
#        烤进控件里的，中途 setLang() 不会重译已有控件。**原先只有中文那半**，
#        而中文恰好塞得下，于是"英文档最右边那列按钮被切掉"整个漏过去了
#        （英文档普遍比中文长，量出来 771 vs 中文 628）。
#      另外 P26-P31 读的是**真控件**：没有固定密码时横幅是不是真红了、红的文案里有没有
#      「现在没人能连进来」这句后果、红横幅上的「去设置」是不是钉在密码那一项上、
#      「本机」密码那行的提示红没红、以及那条提示跟当前档位的按钮名对不对得上
#      （对不上 = 文案在骗用户，这是"删掉密码"那类事故的前置条件）。
#      P32-P41 是密码策略那一格的真控件状态：档位下拉有 5 项（含后加的「用完即换」）、
#      「未应用」标记在"选着固定密码而文件里没有"时亮/在"两边逐字一致"时灭（**常亮等于没有**，
#      所以反面也钉了）、RustDesk 里是加密密码而外壳手上没有明文时的提示**必须指向
#      「设置 → 立即应用」并说清代价**（P31：那句提示是把用户往"替换掉原密码"这条路上引的，
#      文案里少了"失效"或者多出「关闭固定密码」= 在骗人）、
#      「不用固定密码」档下「打开 RustDesk 看一次性密码」露面（那是这一档唯一的逃生口）、
#      「用完即换」那行说明写的是**断开**这个时机（写成"连上就换"会把刚连上的人掐断）。
#      P42-P44 是开机自启：全新配置默认 false（绝不凭空装启动项）、勾选框状态与**注册表实际
#      状态**一致（显示"开着"而注册表里没有是最坏的失败方式）、提示里必须出现「登录之后」。
#      ⚠ 这几项一个字节的注册表都不写 —— 真写真的就交给手动跑的 `--autostarttest`。
#  17. 中英双语：
#      a) scripts/check_i18n.py 静态扫源码里每个 TR("…")，核对翻译表 —— 这一类
#         **只有静态扫得出来**：表里多了个没人调用的键、或者源码里用了个表里没有的键，
#         运行时都看不见（前者永不触发，后者在 zh 档下表现完全正常）。
#      b) 三张 --lang en 的英文截图（主窗口 / 设置 / 图文教程）。判据只是"非空"，
#         但它补的是**英文特有的**风险：英文串普遍比中文长，固定宽度的标签会溢出，
#         而这种问题只有看图才发现 —— 中文那三张图照不到。
#
# 用法：bash scripts/verify.sh
set -u

# ROOT 必须先于下面那些"从本机环境推出来的路径"算出来 —— 它们要读 $ROOT/CMakePresets.json。
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXE="$ROOT/build/PureRemote.exe"
RD_CLI="$ROOT/bin/rustdesk.exe"

# Qt 工具链目录，三处依次找，**绝不写死某个人的绝对路径** —— 那种路径在别人机器上必然不存在，
# 却因为前面几项还能跑而"看起来是好的"，比直接报错更坏：
#   ① 环境变量 QT_BIN
#   ② 本机那份（未入库的）CMakePresets.json 里的 CMAKE_PREFIX_PATH
#   ③ PATH 里能找到的 qmake
QT_BIN="${QT_BIN:-}"
if [ -z "$QT_BIN" ] && [ -f "$ROOT/CMakePresets.json" ]; then
    _prefix="$(sed -n 's/.*"CMAKE_PREFIX_PATH"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        "$ROOT/CMakePresets.json" | head -1)"
    [ -n "$_prefix" ] && QT_BIN="$_prefix/bin"
fi
if [ -z "$QT_BIN" ]; then
    _qmake="$(command -v qmake 2>/dev/null || true)"
    [ -n "$_qmake" ] && QT_BIN="$(dirname "$_qmake")"
fi
# ⚠ 必须转成 POSIX 形式再进 PATH。CMakePresets.json 里是 Windows 风格（`C:/…`），
#   直接塞进 PATH 的话**本土 exe 一个都起不来** —— 而且一个字节的报错都没有，症状是
#   "脚本里所有断言都失败、手工敲同一条命令却成功"。实测：`C:/…` 失败、`/c/…` 成功。
QT_BIN="$(cygpath -u "$QT_BIN" 2>/dev/null || echo "$QT_BIN")"
if [ -z "$QT_BIN" ] || [ ! -d "$QT_BIN" ]; then
    echo "找不到 Qt 工具链目录。" >&2
    echo "  设 QT_BIN 指向 <Qt>/<版本>/mingw_64/bin 再跑；" >&2
    echo "  或把 CMakePresets.json.example 复制成 CMakePresets.json 填上本机路径。" >&2
    exit 2
fi

TMP="${TEMP:-${TMPDIR:-/tmp}}"
TMP_U="$(cygpath -u "$TMP" 2>/dev/null || echo "$TMP")"
# 官方便携 exe 解包会落到这里 —— 第 4 项要断言它**没被创建**，所以路径从环境变量拼。
LOCAL_RD="$(cygpath -u "${LOCALAPPDATA:-$HOME/AppData/Local}" 2>/dev/null \
            || echo "${LOCALAPPDATA:-$HOME/AppData/Local}")/rustdesk"

export PATH="$ROOT/build:$QT_BIN:$PATH"
fails=0
skips=0
ok()  { echo "  PASS  $1"; }
bad() { echo "  FAIL  $1"; fails=$((fails + 1)); }
skip() { echo "  SKIP  $1"; skips=$((skips + 1)); }

# 停 RustDesk 并**等它真的消失**再往下走。外壳开了常驻看门狗，只要有一个 PureRemote
# 还活着就会把 --server 拉回来；写盘类测试必须在它完全停住的时候跑，否则会被进程内的
# 旧值覆盖。所以不能只 taskkill + sleep 2 就冲进去（踩过一次，poltest 静默变 SKIP）。
kill_rustdesk() {
    taskkill //F //IM rustdesk.exe > /dev/null 2>&1
    for _ in $(seq 1 20); do
        [ "$(tasklist //FI 'IMAGENAME eq rustdesk.exe' //NH 2>/dev/null | grep -ci rustdesk)" = "0" ] && return 0
        sleep 0.5
    done
    echo "  WARN  等了 10 秒 rustdesk.exe 还没退干净"
    return 1
}

if [ ! -x "$EXE" ]; then
    echo "找不到 $EXE —— 先跑 cmake --build --preset release"
    exit 2
fi

echo "[1/17] ID 读取"
rm -f "$TMP_U/puremote-lastid.txt" "$TMP_U/puremote-lastid-reread.txt"
timeout 70 "$EXE" --print-id > /dev/null 2>&1
got="$(tr -d '\r\n' < "$TMP_U/puremote-lastid.txt" 2>/dev/null)"
want="$("$RD_CLI" --get-id 2>/dev/null | tr -d '\r\n')"
echo "        外壳读到: '$got'   基线: '$want'"
if [ -n "$got" ] && [ "$got" = "$want" ]; then
    ok "ID 一致"
else
    bad "ID 不一致或为空"
fi
# 第二次读（值没变）也必须收尾。idChanged 只在**值变了**才发，界面靠它收尾的话
# 第二次点「重新读取」就永远停在「读取中…」（用户真撞到过）。
reread="$(tr -d '\r\n' < "$TMP_U/puremote-lastid-reread.txt" 2>/dev/null)"
if [ "$reread" = "OK" ]; then
    ok "第二次读取也收尾了（没借 idChanged 当'读完了'）"
else
    bad "第二次读取没收尾（reread='$reread'）—— 界面会卡在「读取中…」"
fi

echo "[2/17] 真实窗口渲染"
# 三张图，缺一不可：主窗口只剩「本机 / 连接别的电脑」两组，密码策略、连接提示、
# 画质与性能、网络路径这四组全搬进了「设置」对话框。只抓主窗口的话，
# 那四组的布局错误**再也没人看**了 —— 而这一步的判据只是"PNG 非空"，不会报错。
shot_main="$TMP_U/puremote-verify-main.png"
shot_set="$TMP_U/puremote-verify-settings.png"
shot_gd="$TMP_U/puremote-verify-guide.png"
rm -f "$shot_main" "$shot_set" "$shot_gd"
timeout 40 "$EXE" --screenshot "$shot_main" > /dev/null 2>&1
timeout 40 "$EXE" --screenshot "$shot_set"  --screenshot-dialog settings  > /dev/null 2>&1
timeout 40 "$EXE" --screenshot "$shot_gd"   --screenshot-dialog guide:tsnet > /dev/null 2>&1
for pair in "主窗口:$shot_main" "设置:$shot_set" "图文教程:$shot_gd"; do
    name="${pair%%:*}"; f="${pair#*:}"
    if [ -s "$f" ]; then
        ok "$name 截到 $(stat -c%s "$f") 字节"
    else
        bad "$name 没截到图（窗口/对话框没起来，或渲染失败）"
    fi
done

echo "[3/17] 单例复用"
# 判据是 **PID 不变**，不是耗时。耗时在这台机器上抖得厉害（实测 1.4s / 2.2s / 4.4s）：
# 冷进程要把 Qt6 那堆 DLL 从 Documents 底下加载起来，而那里挂着 OneDrive/Defender，
# 抖动是环境给的，跟单例逻辑无关。光看"进程数 1 -> 1"也不够——第一个实例死了、
# 第二个顶上来同样是 1 -> 1；PID 前后一致才能证明是**原来那个热实例**接住的。
# （M5 windeployqt 把 DLL 拷到 exe 旁边之后冷启动会好很多，那时再考虑加耗时门槛。）
pid_of() {
    tasklist //FI "IMAGENAME eq PureRemote.exe" //NH //FO CSV 2>/dev/null \
        | head -1 | cut -d, -f2 | tr -d '"\r'
}
taskkill //F //IM PureRemote.exe > /dev/null 2>&1
sleep 1
"$EXE" > /dev/null 2>&1 &
sleep 4
n1="$(tasklist //FI "IMAGENAME eq PureRemote.exe" //NH 2>/dev/null | grep -c PureRemote)"
pid1="$(pid_of)"
start=$(date +%s%N)
# 用 --show 而不是 --connect：M2 之后 --connect 会**真的发一次连接请求**
# （弹 RustDesk 客户端窗口、可能长挂），作为"单例接住了吗"的探针就太长太吵了。
# --show 无副作用，语义一样是"再来一次启动"。
timeout 20 "$EXE" --show > /dev/null 2>&1
elapsed=$(( ($(date +%s%N) - start) / 1000000 ))
n2="$(tasklist //FI "IMAGENAME eq PureRemote.exe" //NH 2>/dev/null | grep -c PureRemote)"
pid2="$(pid_of)"
taskkill //F //IM PureRemote.exe > /dev/null 2>&1
echo "        进程数 $n1 -> $n2，PID $pid1 -> $pid2，二次启动耗时 ${elapsed}ms"
if [ "$n1" = "1" ] && [ "$n2" = "1" ] && [ -n "$pid1" ] && [ "$pid1" = "$pid2" ]; then
    ok "热实例接住了第二次启动（还是同一个 PID）"
else
    bad "单例行为不对（进程数或 PID 变了）"
fi

# 选"随包发解包后的运行时"就是为了这一项：便携 launcher 会往 %LOCALAPPDATA%/rustdesk/
# 解 80MB 出来，是整套方案里唯一一处系统写入。跑完上面三项之后这里必须还是空的。
echo "[4/17] 零系统写入"
# 先确认这个"不该存在的目录"的**父目录**在不在：父目录都不在的话，下面那条断言是空的
# （永远 PASS），比 FAIL 更能骗人。环境变量拼出来的路径尤其要防这一手。
if [ ! -d "$(dirname "$LOCAL_RD")" ]; then
    skip "找不到 $(dirname "$LOCAL_RD")，这项查不了（LOCALAPPDATA 没设？）"
elif [ -e "$LOCAL_RD" ]; then
    bad "LOCALAPPDATA/rustdesk 被创建了（$(du -sh "$LOCAL_RD" 2>/dev/null | cut -f1)）"
else
    ok "没往 LOCALAPPDATA 写任何东西"
fi
# 便携服务（--run-as-system，SYSTEM 身份）会在 %ProgramData%\RustDesk\portable_service_shmem\
# 建共享内存，但它**只在提权启动时才被拉起**：实测非提权启动（explorer 拉起）完全不创建。
# 这里只做提示不断言——因为本脚本自己是从提权 shell 跑的，会把这个结论污染成假阳性。
if [ -e "/c/ProgramData/RustDesk" ]; then
    echo "  NOTE  存在 ProgramData/RustDesk（本脚本从提权 shell 跑，预期如此；"
    echo "        非提权启动不会创建它，见 rustdesk-toml-password-scope 的姊妹结论）"
else
    echo "  NOTE  ProgramData/RustDesk 也不存在（本次是非提权跑的）"
fi

# 密码机制：C++ 写 RustDesk.toml 顶层 password 必须落在顶层、不破坏 TOML、不动 enc_id。
# 这是踩过大坑的地方——写坏 TOML 会让 RustDesk 丢弃身份重新注册、把 ID 换掉。
# --pwtest 自己跑 15 项断言，结束时恢复原配置。
kill_rustdesk

echo "[5/17] 密码写入（RustDesk.toml 顶层 password）"
# ⚠ 闸门：接下来这两项曾经直接改**用户真实的** RustDesk.toml（顶层 password 是这台机器上
#   唯一一份固定密码，RustDesk 只存加密串、谁都读不回来）。现在的实现把测试指到**副本**上，
#   真文件一个字节都不该动 —— 这里从外面钉死这一点，代码以后退回老做法就当场红。
#   （踩过一次：2026-10-10 真文件里被塞进一个机器生成的 10 位密码。）
RD_TOML="$APPDATA/RustDesk/config/RustDesk.toml"
rd_before=$(md5sum "$RD_TOML" 2>/dev/null | cut -d' ' -f1)
rm -f "$TMP_U/puremote-pwtest.txt"
timeout 60 "$EXE" --pwtest > /dev/null 2>&1
if grep -q "全绿" "$TMP_U/puremote-pwtest.txt" 2>/dev/null; then
    ok "$(grep -c '\[PASS\]' "$TMP_U/puremote-pwtest.txt") 项断言全绿"
else
    bad "密码读写断言没过"
    sed 's/^/        /' "$TMP_U/puremote-pwtest.txt" 2>/dev/null
fi

# 密码策略：固定密码、定时轮换、限时作废都要能落到同一处（RustDesk.toml 顶层 password）。
# --poltest 跑 37 项断言（模式持久化 / 到期判定 / 轮换换新 / 限时作废 / Off 清空 / 配置恢复）。
# 它刻意用 manageProcess=false，所以不会停掉正在跑的 RustDesk —— 真·停起链路
# 靠人工验证（改一次配置后 --screenshot 走 GUI 路径触发补轮换）。
kill_rustdesk

echo "[6/17] 密码策略（固定 / 定时轮换 / 限时作废）"
rm -f "$TMP_U/puremote-poltest.txt"
timeout 60 "$EXE" --poltest > /dev/null 2>&1
if grep -q "全绿" "$TMP_U/puremote-poltest.txt" 2>/dev/null; then
    ok "$(grep -c '\[PASS\]' "$TMP_U/puremote-poltest.txt") 项断言全绿"
else
    bad "密码策略断言没过"
    sed 's/^/        /' "$TMP_U/puremote-poltest.txt" 2>/dev/null
fi

# 闸门的另一半：真 RustDesk.toml 逐字节没变（见第 5 步开头那段说明）
rd_after=$(md5sum "$RD_TOML" 2>/dev/null | cut -d' ' -f1)
if [ -n "$rd_before" ] && [ "$rd_before" = "$rd_after" ]; then
    ok "真 RustDesk.toml 逐字节未被改动（${rd_before:0:12}…）—— 密码那两项跑的是副本"
else
    bad "真 RustDesk.toml 被改了！$rd_before -> $rd_after（这台机器上唯一一份固定密码）"
fi

# 连接提示：被控端"有人连进来了"靠盯 RustDesk 日志实现（IPC 对第三方是关死的）。
# --conntest 跑 28 项断言（三模式持久化 / approve-mode 写进 [options] / 日志增量读 +
# 源文件路径限定 + 半行与轮转的边界）。RustDesk 在跑时写盘段会 SKIP 而非假阳性。
kill_rustdesk

echo "[7/17] 连接提示（托盘提示 / 需要确认）"
rm -f "$TMP_U/puremote-conntest.txt"
timeout 60 "$EXE" --conntest > /dev/null 2>&1
if grep -q "全绿" "$TMP_U/puremote-conntest.txt" 2>/dev/null; then
    ok "$(grep -c '\[PASS\]' "$TMP_U/puremote-conntest.txt") 项断言全绿"
else
    bad "连接提示断言没过"
    sed 's/^/        /' "$TMP_U/puremote-conntest.txt" 2>/dev/null
fi


# 常驻：绿色免安装没有"开机自启的服务"，人不在电脑前没人去双击 RustDesk，
# 机器重启后就是彻底连不上 —— 所以外壳自己拉起并看住它。
# 用 --server（源码 src/core_main.rs:397 → start_server(true,false)）而不是裸 exe：
# 裸 exe 会弹 RustDesk 主窗口，托盘外壳里很突兀。
# "无窗口"不好无头断言，改看 log\server\ 那份日志有没有被写 —— 只有 --server 写那里。
echo "[8/17] RustDesk 常驻（--server + 看门狗）"

# ① 判据本身：喂真机抓的 wmic 原文，逐条断言"什么算在、什么算不在"。
rm -f "$TMP_U/puremote-servertest.txt"
timeout 60 "$EXE" --servertest > /dev/null 2>&1
if grep -q "全绿" "$TMP_U/puremote-servertest.txt" 2>/dev/null; then
    ok "被控端判据（--servertest）$(grep -c '\[PASS\]' "$TMP_U/puremote-servertest.txt") 项断言全绿"
else
    bad "被控端判据断言没过"
    sed 's/^/        /' "$TMP_U/puremote-servertest.txt" 2>/dev/null
fi

# ② 端到端。数的是"带 --server 的 rustdesk.exe"而不是"叫 rustdesk.exe 的进程" ——
#    后者数不出 --tray 和 --server 的区别，2026-10-10 那次事故就是从这儿溜过去的。
rd_server_count() {
    local out
    out=$(wmic process where "name='rustdesk.exe'" get commandline //format:list 2>/dev/null)
    printf '%s\n' "$out" \
        | tr -d '\r' \
        | awk '{ for (i = 1; i <= NF; i++) if ($i == "--server") { n++; break } } END { print n + 0 }'
}
rd_proc_count() {
    tasklist //FI "IMAGENAME eq rustdesk.exe" //NH 2>/dev/null | grep -ci rustdesk
}

if ! command -v wmic > /dev/null 2>&1; then
    skip "这台机器上没有 wmic —— 第 8 步没法从外面核对「带 --server 的那个进程」到底在不在"
else
    taskkill //F //IM PureRemote.exe > /dev/null 2>&1
    kill_rustdesk
    rm -rf "$APPDATA/RustDesk/log/server"
    "$EXE" > /dev/null 2>&1 &
    sleep 12
    n_start=$(rd_server_count)
    echo "        启动后带 --server 的进程 $n_start 个"
    if [ "$n_start" -ge 1 ]; then
        ok "外壳一起来就把被控端拉起来了（--server）"
    else
        bad "常驻没生效（启动后 --server $n_start 个）"
    fi

    # 事故现场：进程全灭之后**只补一个 --tray 界面进程**。此刻机器是连不进来的，
    # 但按进程名看"rustdesk 还在" —— 旧判据就是在这儿骗过看门狗的。
    # 看门狗 10s 一跳，可能抢先拉起，所以重试几轮直到真的造出"只有界面进程"那个状态。
    scene=0
    for _ in 1 2 3; do
        taskkill //F //IM rustdesk.exe > /dev/null 2>&1
        "$RD_CLI" --tray > /dev/null 2>&1 &
        sleep 3
        n_proc=$(rd_proc_count)
        n_srv=$(rd_server_count)
        if [ "$n_proc" -ge 1 ] && [ "$n_srv" -eq 0 ]; then
            scene=1
            break
        fi
    done
    if [ "$scene" -eq 1 ]; then
        ok "造出了事故现场：有 rustdesk.exe 进程 $n_proc 个，但没有一个是 --server"
    else
        bad "没能造出「只有 --tray 界面进程」的状态（进程 $n_proc，--server $n_srv）"
    fi

    sleep 18
    n_back=$(rd_server_count)
    echo "        只剩界面进程时，看门狗拉回带 --server 的进程 $n_back 个"
    if [ "$n_back" -ge 1 ]; then
        ok "服务端不在、只剩界面进程时，看门狗照样把它拉回来（旧判据在这里会一声不吭）"
    else
        bad "只剩界面进程时看门狗没反应（--server $n_back 个）—— 机器会一直连不进来"
    fi
    taskkill //F //IM PureRemote.exe > /dev/null 2>&1
    kill_rustdesk
fi

if [ -s "$APPDATA/RustDesk/log/server/rustdesk_rCURRENT.log" ]; then
    ok "是 --server 无窗口拉起的（log/server 有日志）"
else
    bad "没看到 log/server 日志 —— 可能用了会弹窗口的裸 exe"
fi

# tsnet 桥接：被控端、控制端各拉一个真的 pmrelay.exe，经 tailnet 互连并把一段字节
# 原样搬回来。这是唯一一项"真跨进程 + 真网络"的断言，也是 M4.4 的验收。
# 它不需要管理员、不需要驱动 —— 这正是选 tsnet 的全部理由，所以这一项过了就等于
# 证明了"纯用户态能跑通"。
echo "[9/17] tsnet 桥接（pmrelay 两端互连）"
AUTHKEY="${TS_AUTHKEY:-}"
if [ -z "$AUTHKEY" ] && [ -f "$ROOT/build/config/puremote.json" ]; then
    # 从 config/puremote.json 里抠 tsnet_authkey（没有就还是空）
    AUTHKEY="$(sed -n 's/.*"tsnet_authkey"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        "$ROOT/build/config/puremote.json" 2>/dev/null | head -1)"
fi
if [ -z "$AUTHKEY" ]; then
    skip "没给 authkey（设 TS_AUTHKEY 环境变量，或在配置里填 tsnet_authkey）"
    echo "        注意：跳过 ≠ 通过 —— B 段（真两端互连）这一项没跑"
else
    rm -f "$TMP_U/puremote-tsnettest.txt"
    # 单机上两个节点最初互连 + 各自登录 tailnet，实测要 ~40 秒；给足 8 分钟余量
    TS_AUTHKEY="$AUTHKEY" timeout 480 "$EXE" --tsnettest > /dev/null 2>&1
    if grep -q "全绿" "$TMP_U/puremote-tsnettest.txt" 2>/dev/null; then
        ok "$(grep -c '\[PASS\]' "$TMP_U/puremote-tsnettest.txt") 项断言全绿"
    else
        bad "tsnet 桥接断言没过"
        sed 's/^/        /' "$TMP_U/puremote-tsnettest.txt" 2>/dev/null
    fi
fi

# 地址簿 / 连接命令 / 时序状态机 / 准备情况。全程不碰 RustDesk 进程（E 段只起 pmrelay），
# 所以不用 kill_rustdesk。E 段需要 authkey，取法跟第 9 项完全一样。
echo "[10/17] 地址簿与连接编排（--booktest）"
AUTHKEY="${TS_AUTHKEY:-}"
if [ -z "$AUTHKEY" ] && [ -f "$ROOT/build/config/puremote.json" ]; then
    AUTHKEY="$(sed -n 's/.*"tsnet_authkey"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        "$ROOT/build/config/puremote.json" 2>/dev/null | head -1)"
fi
rm -f "$TMP_U/puremote-booktest.txt"
# E 段最长等 180 秒（桥接 ready），加上其余各段，给 5 分钟余量
TS_AUTHKEY="$AUTHKEY" timeout 300 "$EXE" --booktest > /dev/null 2>&1
if grep -q "全绿" "$TMP_U/puremote-booktest.txt" 2>/dev/null; then
    ok "$(grep -c '\[PASS\]' "$TMP_U/puremote-booktest.txt") 项断言全绿"
else
    bad "地址簿 / 连接编排断言没过"
    sed 's/^/        /' "$TMP_U/puremote-booktest.txt" 2>/dev/null
fi
# E 段是唯一一项"要真网络"的，跳过 ≠ 通过，单独说一声
if grep -q "SKIP" "$TMP_U/puremote-booktest.txt" 2>/dev/null; then
    skip "E 段（真桥接闸门）没跑 —— 对端不上线时到底放不放行，这次没验证"
fi

# 网络体检 + 路径探测。不碰 RustDesk 进程（纯 QHostInfo/QTcpSocket/QUdpSocket），
# 所以不用 kill_rustdesk。B 段要真网络，最长 90 秒，给 3 分钟余量。
echo "[11/17] 网络体检与连接路径（--netcheck）"
rm -f "$TMP_U/puremote-netcheck.txt"
timeout 180 "$EXE" --netcheck > /dev/null 2>&1
if grep -q "全绿" "$TMP_U/puremote-netcheck.txt" 2>/dev/null; then
    ok "$(grep -c '\[PASS\]' "$TMP_U/puremote-netcheck.txt") 项断言全绿"
    # 软项不平，但值得知道 —— 尤其"UDP 出站不通"这种会直接毁掉打洞的。
    # 用普通 echo 不用 skip：它们不是"没跑的项"，计进 skips 会让结尾的
    # "全绿（N 项跳过）"看着像有东西没验。
    ntips="$(grep -c '\[注意\]' "$TMP_U/puremote-netcheck.txt" 2>/dev/null)"
    if [ "${ntips:-0}" -gt 0 ]; then
        # 一行一条。用 paste 拼分隔符会把多字节的「、」按字节切开，输出成乱码。
        echo "        环境提示（非失败）："
        grep '\[注意\]' "$TMP_U/puremote-netcheck.txt" | sed 's/.*：//; s/^/          · /'
    fi
else
    bad "网络体检断言没过"
    sed 's/^/        /' "$TMP_U/puremote-netcheck.txt" 2>/dev/null
fi

# 四档映射 + 事务语义。B 段要写 RustDesk2.toml，必须在 RustDesk 完全停住时跑
# （运行中的实例退出时会把内存里的 CONFIG2 回写，把我们的值盖掉）。
echo "[12/17] 网络档位映射（--proftest）"
kill_rustdesk
rm -f "$TMP_U/puremote-proftest.txt"
timeout 120 "$EXE" --proftest > /dev/null 2>&1
if grep -q "全绿" "$TMP_U/puremote-proftest.txt" 2>/dev/null; then
    ok "$(grep -c '\[PASS\]' "$TMP_U/puremote-proftest.txt") 项断言全绿"
else
    bad "网络档位映射断言没过"
    sed 's/^/        /' "$TMP_U/puremote-proftest.txt" 2>/dev/null
fi
if grep -q "SKIP" "$TMP_U/puremote-proftest.txt" 2>/dev/null; then
    skip "B 段（真写盘 / 事务语义）没跑 —— 要么 RustDesk 没停住，要么读不到 RustDesk2.toml"
fi

# 画质与性能（按联系人）。画质**不写** RustDesk2.toml 的全局 [options]（实测无效），
# 而是写 %APPDATA%\RustDesk\config\peers\<对端ID>.toml。这一段全跑在临时的仿真文件上，
# 一个字节都不碰真配置。
# ⚠ 报告里写明了：它证明的是文件读写位置与校验；**不证明** RustDesk 会按此出画
#   （那要真会话，本机只有一台机器，验不了）。
echo "[13/17] 画质与性能（--qtest）"
rm -f "$TMP_U/puremote-qtest.txt"
timeout 60 "$EXE" --qtest > /dev/null 2>&1
if grep -q "全绿" "$TMP_U/puremote-qtest.txt" 2>/dev/null; then
    ok "$(grep -c '\[PASS\]' "$TMP_U/puremote-qtest.txt") 项断言全绿"
else
    bad "画质断言没过"
    sed 's/^/        /' "$TMP_U/puremote-qtest.txt" 2>/dev/null
fi

# 绿色包结构（M5）。只查结构不查运行 —— 真启动那一步是 deploy.ps1 -Smoke 干的，
# 这里再跑一遍要多花一分半。结构查的是**几个"缺了必死"的文件**：
#   qt.conf + plugins/platforms/qwindows.dll  缺任一 = 一启动就 "no Qt platform plugin"
#   bin/rustdesk.exe + bin/librustdesk.dll    缺任一 = 内核找不到，
#                                             cliExePath() 会回落到 %LOCALAPPDATA%
# 没打包过就 SKIP（不算失败）—— 不是每次都要出包。
echo "[14/17] 绿色包结构（dist/PureRemote）"
DIST="$ROOT/dist/PureRemote"
if [ ! -d "$DIST" ]; then
    skip "还没打包（跑 scripts/deploy.ps1 生成）"
else
    pkg_fails=0
    for f in "PureRemote.exe" "qt.conf" "plugins/platforms/qwindows.dll" \
             "bin/rustdesk.exe" "bin/librustdesk.dll"; do
        if [ ! -e "$DIST/$f" ]; then
            printf '        缺 %s\n' "$f"
            pkg_fails=$((pkg_fails + 1))
        fi
    done
    # 私货检查：开发机的配置和 tailnet 授权密钥绝不能被打进包
    if [ -e "$DIST/config/puremote.json" ]; then
        printf '        包里有 config/puremote.json（开发机配置被带进去了，里面有联系人和授权密钥）\n'
        pkg_fails=$((pkg_fails + 1))
    fi
    if [ "$pkg_fails" -eq 0 ]; then
        ok "关键文件齐全，且没把开发机配置打进去"
    else
        bad "$pkg_fails 处不对"
    fi
fi

echo "[15/17] 界面资源（--uitest）"
# 样式表 / 图文教程正文 / 横幅文案。**不起窗口**：这一段管"字和资源对不对"，
# 长什么样由第 2 步那三张图人眼看。它拦的是三类**静默**错误：
# 样式表占位符漏参数（某控件颜色不对，界面照开）、教程链接写错域名、
# 图片没进 qrc（图片框空着，只有点开那一页才看得见）。
rm -f "$TMP_U/puremote-uitest.txt"
timeout 60 "$EXE" --uitest > /dev/null 2>&1
if grep -q "全绿" "$TMP_U/puremote-uitest.txt" 2>/dev/null; then
    ok "$(grep -c '\[PASS\]' "$TMP_U/puremote-uitest.txt") 项断言全绿"
else
    bad "有断言没过（详细：%TEMP%\\puremote-uitest.txt）"
    grep '\[FAIL\]' "$TMP_U/puremote-uitest.txt" 2>/dev/null | head -5
fi

echo "[16/17] 真控件状态（--paneltest）"
# 全项目**唯一**真的造一个 MainWindow 来读控件状态的测试。它拦的是纯函数
# 测试永远拦不住的一类：**规则可以全对，错的是界面喂给规则的输入**。实际发生过的那个
# 死路 bug 就是这样 —— 地址簿空着时点「新建」，保存/连接都该亮却全灰，因为旧代码只拿
# "列表选中没选中"这一个布尔去喂规则，把"正在填一条新的"这个状态整个丢了。
#
# ⚠ 最后 P45/P46 两块会**真的 show() 一次**（量完立刻 hide）：主界面宽度靠 LayoutRequest
#   撑起来，没显示过的窗口收不到，构造期量到的数偏小。P46 是**另开一个英文档窗口**量的
#   —— 中文档恰好塞得下，只量中文就会把"英文档最右边被切"整个漏过去（已经漏过一次）。
#
# 顺带量三件**只能靠尺寸/控件数发现**的事：
#   · 设置框原来五组一字排开有 1061px，比 1080p 屏的可用高度还高 —— 底部「关闭」和整组
#     「界面语言」落在屏幕外，且全程没有滚动条。这一步的兄弟判据（截图）只是"PNG 非空"，
#     裁到框外照样是张好图，**拦不住这种错**，只能量 sizeHint。
#   · 标签页有没有空页（漏挂一组只表现为"某页空白"，不报错）。
#   · 主界面那行「线路」会不会跟着档位变（不跟着变比没有更误导）。
# ⚠ 只跑 build/ 下这份、**别拿打包产物跑**：AppConfig::configFilePath() 是 static，
#   它会读 <exe目录>/config/puremote.json。这段测试只在内存里改，不写盘。
rm -f "$TMP_U/puremote-paneltest.txt"
timeout 60 "$EXE" --paneltest > /dev/null 2>&1
if grep -q "全绿" "$TMP_U/puremote-paneltest.txt" 2>/dev/null; then
    ok "$(grep -c '\[PASS\]' "$TMP_U/puremote-paneltest.txt") 项断言全绿"
else
    bad "有断言没过（详细：%TEMP%\puremote-paneltest.txt）"
    grep '\[FAIL\]' "$TMP_U/puremote-paneltest.txt" 2>/dev/null | head -5
fi

echo "[17/17] 中英双语"
# a) 静态核对：源码里每个 TR("…") 的键都在表里，且表里没有重复/空值/漏翻。
#    为什么不能只靠第 15 步那段 I 段 —— 它跑在运行时，**看不见没被调用到的键**。
#    两边互补，缺一个都会漏掉一整类（详见 check_i18n.py 顶部注释）。
i18n_out="$(python -I "$ROOT/scripts/check_i18n.py" --root "$ROOT" 2>&1)"
i18n_rc=$?
if [ "$i18n_rc" -eq 0 ]; then
    ok "$(echo "$i18n_out" | head -1)"
else
    bad "翻译表对不上（详细见下）"
    echo "$i18n_out" | sed 's/^/        /'
fi

# b) 英文截图。--lang 只影响这一次进程，**不写盘**，所以不会把用户的配置改成英文。
shot_en_main="$TMP_U/puremote-verify-en-main.png"
shot_en_set="$TMP_U/puremote-verify-en-settings.png"
shot_en_gd="$TMP_U/puremote-verify-en-guide.png"
rm -f "$shot_en_main" "$shot_en_set" "$shot_en_gd"
timeout 40 "$EXE" --lang en --screenshot "$shot_en_main" > /dev/null 2>&1
timeout 40 "$EXE" --lang en --screenshot "$shot_en_set" --screenshot-dialog settings > /dev/null 2>&1
timeout 40 "$EXE" --lang en --screenshot "$shot_en_gd"  --screenshot-dialog guide:tsnet > /dev/null 2>&1
for pair in "英文主窗口:$shot_en_main" "英文设置:$shot_en_set" "英文图文教程:$shot_en_gd"; do
    name="${pair%%:*}"; f="${pair#*:}"
    if [ -s "$f" ]; then
        ok "$name 截到 $(stat -c%s "$f") 字节"
    else
        bad "$name 没截到图（--lang en 路径坏了，或英文渲染失败）"
    fi
done
# ⚠ 这几张图**只能人眼看**：英文串比中文长，固定宽度的标签/按钮会溢出或截断，
#   而"PNG 非空"永远为真。位置在 %TEMP%\puremote-verify-en-*.png。

echo
if [ "$fails" -eq 0 ]; then
    if [ "$skips" -gt 0 ]; then
        echo "全绿（$skips 项跳过 —— 跳过的那几项没验证过，别当成通过）"
    else
        echo "全绿"
    fi
    exit 0
fi
echo "有 $fails 项没过"
exit 1

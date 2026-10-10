#include "Guide.h"
#include "I18n.h"

namespace Guide {

QStringList topics()
{
    // 顺序 = AppConfig::netModes()：官方在最前，因为它就是默认档。
    return { QStringLiteral("official"), QStringLiteral("tsnet"),
             QStringLiteral("cloudflare"), QStringLiteral("custom") };
}

QString topicTitle(const QString& key)
{
    // 4 条都是**界面文案**，走 TR 才能在英文档下查表（键=中文原文，见 I18n.h）。
    if (key == QLatin1String("tsnet"))
        return TR("进阶：让连接更快不卡（tailnet）");
    if (key == QLatin1String("cloudflare"))
        return TR("自建服务器（Cloudflare Workers）");
    if (key == QLatin1String("custom"))
        return TR("自建服务器（自己的 VPS / frp / 端口映射）");
    return TR("先用起来（默认档，什么都不用配）");
}

QString topicForNetMode(const QString& netMode)
{
    return topics().contains(netMode) ? netMode : QStringLiteral("official");
}

// 英文正文。中文那份是**源语言**（I18n.h：中文即键），这里只做一份逐段对应的翻译 ——
// 单独成函数、而不是把中文分支塞进 if/else，是为了让下面 html() 里那份中文正文
// 维持"一个字节都不动"：改英文永远碰不到它。
// 图片一律用 -en 后缀的那套（另一份资源），中文正文继续用不带后缀的。
static QString htmlEn(const QString& key)
{
    // ------------------------------------------------------------- Get started
    if (key == QLatin1String("official")) {
        return QStringLiteral(R"(
<div style='color:#e0e0e0;'>
<h2>Get started</h2>
<p>This is the default mode: use RustDesk's own public servers. <b>Nothing to configure,
no sign-up, no address to enter</b> — it works as soon as it is installed.</p>

<h3>What each machine does</h3>
<p><b>Controlled side (the machine that gets connected to)</b></p>
<ol>
  <li>Send the other person your <b>connection code</b> and <b>connection password</b> from
      the "This device" panel. If you have never set a password, go to
      "Settings → Password policy" and set a <b>fixed password</b>.</li>
  <li>Make sure "When someone connects" is set to "Don't ask" — that way people can get in
      even when you are away from the computer.</li>
</ol>
<p><b>Controlling side (your own machine)</b></p>
<ol>
  <li>Under "Connect to another computer", enter the peer's <b>connection code</b> and
      <b>connection password</b>, then click "Connect".</li>
  <li>If retyping them every time is a hassle, click "New" to save this as a contact;
      after that, select it and click once.</li>
</ol>

<h3>When to switch away from this mode</h3>
<p>The official server has only one fallback node on our side (in New York). Measured on
this machine: signaling <b>220 ms</b>, with the relay in France — when hole punching fails
it falls back to the France relay and the picture visibly stutters.</p>
<p>Click "One-tap checkup" first to see whether signaling works: <b>if it works, keep
using this mode</b>; if it fails, or it stutters often, read the other three topics.</p>
)" R"(
<img src=":/guide/net-modes-en.png" width="560"/>
)" R"(
<p style='color:#999999; font-size:12px;'>Schematic only — the real UI may differ.</p>
</div>)");
    }

    // ------------------------------------------------------------------ tsnet
    if (key == QLatin1String("tsnet")) {
        return QStringLiteral(R"(
<div style='color:#e0e0e0;'>
<h2>Advanced: make connections faster and smoother (tailnet)</h2>
<p>This path goes <b>through no third-party server</b>: the two machines use Tailscale to
form a private network (called a tailnet), and RustDesk's traffic travels over it.</p>
)" R"(
<img src=":/guide/tsnet-net-en.png" width="560"/>
)" R"(
<p style='color:#999999; font-size:12px;'>Schematic only — the real UI may differ.</p>
<p>Measured on this machine: Tailscale's Hong Kong relay is <b>43–68 ms</b>, while RustDesk's
official node in New York is <b>220 ms</b> — more than three times slower.</p>

<h3>Step 1: sign up for Tailscale (free)</h3>
<p>Open )" R"(<a href="https://login.tailscale.com/start">https://login.tailscale.com/start</a>)" R"(
to register. The personal plan is <b>$0 "Free forever"</b> with unlimited devices; for
pricing see
)" R"(<a href="https://tailscale.com/pricing">https://tailscale.com/pricing</a>)" R"(.</p>
<p>⚠ You must sign in online at least once — <b>there is no anonymous mode</b>. There is no
way around this step.</p>

<h3>Step 2: generate an auth key</h3>
<p>Open )" R"(<a href="https://login.tailscale.com/admin/settings/keys">https://login.tailscale.com/admin/settings/keys</a>)" R"(
and click <b>Generate auth key</b>.</p>
)" R"(
<img src=":/guide/tsnet-keys-en.png" width="560"/>
)" R"(
<p style='color:#999999; font-size:12px;'>Schematic only — the real UI may differ.</p>
<p style='color:#ffb454;'><b>⚠ The next three come from real experience — choose wrong and you will not be able to connect:</b></p>
<ul>
  <li><b>Always check Reusable</b> — if you do not, the key is <b>single-use</b>, and the
      second machine can never join.</li>
  <li><b>Never check Ephemeral</b> — once checked, the node is <b>deleted</b> by Tailscale
      the moment it goes offline; when the controlled side comes back it is handed a
      <b>new address</b> and you will <b>never connect again</b>.</li>
  <li><b>Set the expiry to No expiry</b> — if you set an expiry, new machines cannot join
      after it lapses, and that is usually exactly when you are away waiting to connect
      back.</li>
</ul>
<p>After generating, copy the string that starts with <span style='font-family:Consolas,monospace;color:#4a9eff;'>tskey-auth-</span>.
The official docs are at
)" R"(<a href="https://tailscale.com/kb/1085/auth-keys">https://tailscale.com/kb/1085/auth-keys</a>)" R"(.</p>

<h3>Step 3: paste the key in</h3>
<p>Open "Settings" → "Network path", choose the <b>tailnet</b> mode, then fill in two fields:</p>
<ul>
  <li><b>Local hostname</b> — this machine's name on the tailnet; pick anything, e.g. )" R"(<span style='font-family:Consolas,monospace;color:#4a9eff;'>my-pc</span>)" R"(</li>
  <li><b>Auth key</b> — paste the string you copied in the previous step</li>
</ul>
<p><b>Fill it in on both machines, and the hostnames must differ</b> (duplicates knock each
other out). Then click "Apply now" to write it in.</p>

<h3>Step 4: connect</h3>
<p>Go back to "Connect to another computer", create a contact, enter the <b>peer's tailnet
hostname</b>, and click "Connect".</p>

<hr/>
<p style='color:#999999; font-size:12px;'>
No admin rights, no drivers, no registry writes — tsnet runs entirely in user space.<br/>
What tsnet is: )" R"(<a href="https://tailscale.com/kb/1244/tsnet">https://tailscale.com/kb/1244/tsnet</a>)" R"(
</p>
</div>)");
    }

    // ------------------------------------------------------------- cloudflare
    if (key == QLatin1String("cloudflare")) {
        return QStringLiteral(R"(
<div style='color:#e0e0e0;'>
<h2>Self-hosted server (Cloudflare Workers)</h2>
<p>Use Cloudflare Workers' free allowance to run your own hbbs/hbbr — RustDesk's
"signaling server + relay server". Once it is up, connections no longer pass through the
official New York node.</p>

<h3>What to do</h3>
<ol>
  <li>Deploy hbbs/hbbr to Cloudflare Workers following the community solution.
      What Workers is: )" R"(<a href="https://developers.cloudflare.com/workers/">https://developers.cloudflare.com/workers/</a>)" R"(,
      the server itself: )" R"(<a href="https://github.com/rustdesk/rustdesk-server">https://github.com/rustdesk/rustdesk-server</a>)" R"(.</li>
  <li>⚠ <b>Always set Workers' locationHint to <code>apac</code></b> — that is what lands
      you on the Hong Kong/Japan nodes, the batch measured at 43–101 ms; the default drifts
      to Europe/America, and then the whole thing is wasted.</li>
  <li>Fill the domain you get into the three address fields under "Network path", and the
      public key into "Server public key".</li>
</ol>

<h3>Why this mode turns websocket on automatically</h3>
<p>Cloudflare can only proxy HTTP/WebSocket — <b>it cannot proxy raw TCP</b>. So when you
pick this mode, the shell enables RustDesk's allow-websocket for you; there is nothing to
edit by hand.</p>
<p style='color:#999999; font-size:12px;'>The latency number for this mode is an
<b>expected value</b> inferred from Tailscale nodes in the same region, not a measured
Cloudflare figure. Click "One-tap checkup" to measure it directly.</p>
</div>)");
    }

    // ----------------------------------------------------------------- custom
    if (key == QLatin1String("custom")) {
        return QStringLiteral(R"(
<div style='color:#e0e0e0;'>
<h2>Self-hosted server (your own VPS / frp / port forwarding)</h2>
<p>You have a machine reachable from the outside — a VPS, a company intranet, an frp
tunnel, or a home router with port forwarding — run hbbs + hbbr on it and fill in the
address. This path is the fastest, because the server sits closest to you.</p>

<h3>What to run on the server</h3>
<ul>
  <li><b>hbbs</b> (signaling) listening on <code>21116/tcp</code> + <code>21116/udp</code>
      → fill its address into "Signaling server"</li>
  <li><b>hbbr</b> (relay) listening on <code>21117/tcp</code>
      → fill its address into "Relay server" (it can be the same machine as signaling)</li>
  <li>hbbs <b>prints a public key on its first start</b> → paste it into
      "Server public key"</li>
</ul>
<p>Official deployment docs: )" R"(<a href="https://rustdesk.com/docs/en/self-host/">https://rustdesk.com/docs/en/self-host/</a>)" R"(</p>

<h3>How to write the address</h3>
<p>Write either <code>host</code> or <code>host:port</code> — <b>you only need the port when
it differs from the default</b>. The address <b>must not contain single quotes or
backslashes</b> (they corrupt RustDesk's config file so it fails to load entirely, and even
your local ID gets swapped).</p>
<p>When done, click "Apply now", then click "One-tap checkup" to verify the server really
answers.</p>
</div>)");
    }

    // 认不出来的 key：英文档也返回一段不会崩的兜底文案（对应中文那份的写法）。
    // 这句**走翻译表**而不是写死英文：表里那条 `没有这个主题。` 只有这里用得上，
    // 写死英文的话它就成了一个永远查不到的孤儿条目。
    return QStringLiteral("<div style='color:#e0e0e0;'><p>%1</p></div>")
        .arg(TR("没有这个主题。"));
}

QString html(const QString& key)
{
    // 英文档走英文正文；其余（含默认的中文档）走下面这份源语言正文，一字未动。
    if (I18n::lang() == I18n::Lang::En)
        return htmlEn(key);

    // ---------------------------------------------------------------- 官方档
    if (key == QLatin1String("official")) {
        return QStringLiteral(R"(
<div style='color:#e0e0e0;'>
<h2>先用起来</h2>
<p>默认就是这一档：直接用 RustDesk 自带的官方服务器。<b>什么都不用配、不用注册、
不用填地址</b>，装好就能连。</p>

<h3>两台机器各自要做的</h3>
<p><b>被控端（要被连的那台）</b></p>
<ol>
  <li>把「本机」里的<b>连接码</b>和<b>连接密码</b>发给对方。
      没设过密码就去「设置 → 密码策略」设一个<b>固定密码</b>。</li>
  <li>确认「有人连进来时」是「不提示」—— 这样人不在电脑前也能连进来。</li>
</ol>
<p><b>控制端（自己这台）</b></p>
<ol>
  <li>在「连接别的电脑」里填对方的<b>连接码</b>和<b>连接密码</b>，点「连接」。</li>
  <li>觉得每次都填麻烦，就点「新建」把这条存进地址簿，以后选中点一下就行。</li>
</ol>

<h3>什么时候该换掉这一档</h3>
<p>官方服务器在我们这边只有一个兜底节点（在纽约）。本机实测信令 <b>220 ms</b>、
中继在法国 —— 打洞失败时会走法国中继，画面会明显卡。</p>
<p>先点「一键体检」看看信令通不通：<b>通就继续用</b>，不通或者经常卡，
再去看另外三个主题。</p>
)" R"(
<img src=":/guide/net-modes.png" width="560"/>
)" R"(
<p style='color:#999999; font-size:12px;'>上图为示意图，界面以软件实际显示为准。</p>
</div>)");
    }

    // ------------------------------------------------------------------ tsnet
    if (key == QLatin1String("tsnet")) {
        return QStringLiteral(R"(
<div style='color:#e0e0e0;'>
<h2>进阶：让连接更快不卡（tailnet）</h2>
<p>这条路<b>不经过任何第三方服务器</b>：两台机器用 Tailscale 组成一张私有网
（叫 tailnet），RustDesk 的流量从这张网里走。</p>
)" R"(
<img src=":/guide/tsnet-net.png" width="560"/>
)" R"(
<p style='color:#999999; font-size:12px;'>上图为示意图，界面以软件实际显示为准。</p>
<p>本机实测：tailnet 的香港中继节点 <b>43–68 ms</b>，而 RustDesk 官方在纽约
<b>220 ms</b> —— 差三倍多。</p>

<h3>第 1 步：注册 Tailscale（免费）</h3>
<p>打开 )" R"(<a href="https://login.tailscale.com/start">https://login.tailscale.com/start</a>)" R"(
注册。个人版是 <b>$0「Free forever」</b>，设备不限；价格说明见
)" R"(<a href="https://tailscale.com/pricing">https://tailscale.com/pricing</a>)" R"(。</p>
<p>⚠ 必须联网登录一次，<b>没有匿名模式</b>。这一步绕不过去。</p>

<h3>第 2 步：生成授权密钥（authkey）</h3>
<p>打开 )" R"(<a href="https://login.tailscale.com/admin/settings/keys">https://login.tailscale.com/admin/settings/keys</a>)" R"(
，点 <b>Generate auth key</b>。</p>
)" R"(
<img src=":/guide/tsnet-keys.png" width="560"/>
)" R"(
<p style='color:#999999; font-size:12px;'>上图为示意图，界面以软件实际显示为准。</p>
<p style='color:#ffb454;'><b>⚠ 下面三条是踩过坑的，选错了以后连不上：</b></p>
<ul>
  <li><b>Reusable 一定要勾</b> —— 不勾的话这个密钥<b>只能用一次</b>，
      第二台机器就加不进来了。</li>
  <li><b>Ephemeral 千万别勾</b> —— 勾了以后节点一下线就被 Tailscale 删掉，
      被控端重新上线会拿到<b>新地址</b>，你会<b>再也连不上</b>。</li>
  <li><b>过期时间选不限制（No expiry）</b> —— 设了过期，到期以后新机器加不进来，
      而那时候你多半正在外面等着连回去。</li>
</ul>
<p>生成后复制那串以 <span style='font-family:Consolas,monospace;color:#4a9eff;'>tskey-auth-</span>
开头的字符串。官方说明见
)" R"(<a href="https://tailscale.com/kb/1085/auth-keys">https://tailscale.com/kb/1085/auth-keys</a>)" R"(。</p>

<h3>第 3 步：把密钥粘进来</h3>
<p>打开「设置」→「网络路径」，档位选 <b>tailnet</b>，然后填两格：</p>
<ul>
  <li><b>本机主机名</b> —— 这台机器在 tailnet 上的名字，随便起，比如 )" R"(<span style='font-family:Consolas,monospace;color:#4a9eff;'>my-pc</span>)" R"(</li>
  <li><b>授权密钥</b> —— 粘上一步复制的那一串</li>
</ul>
<p><b>两台机器都要填，而且主机名必须不一样</b>（重名会互相顶掉）。然后用「立即应用」写进去。</p>

<h3>第 4 步：连</h3>
<p>回到「连接别的电脑」建一条联系人，把<b>对方的 tailnet 主机名</b>填进去，
点「连接」。</p>

<hr/>
<p style='color:#999999; font-size:12px;'>
不需要管理员、不装驱动、不写注册表 —— tsnet 全在用户态。<br/>
tsnet 是什么：)" R"(<a href="https://tailscale.com/kb/1244/tsnet">https://tailscale.com/kb/1244/tsnet</a>)" R"(
</p>
</div>)");
    }

    // ------------------------------------------------------------- cloudflare
    if (key == QLatin1String("cloudflare")) {
        return QStringLiteral(R"(
<div style='color:#e0e0e0;'>
<h2>自建服务器（Cloudflare Workers）</h2>
<p>用 Cloudflare Workers 的免费额度自己搭一台 hbbs/hbbr —— 也就是 RustDesk 的
「信令服务器 + 中继服务器」。搭好之后连接不再经过官方的纽约节点。</p>

<h3>要做的事</h3>
<ol>
  <li>按社区方案把 hbbs/hbbr 部署到 Cloudflare Workers。
      Workers 是什么见 )" R"(<a href="https://developers.cloudflare.com/workers/">https://developers.cloudflare.com/workers/</a>)" R"(，
      服务端本体见 )" R"(<a href="https://github.com/rustdesk/rustdesk-server">https://github.com/rustdesk/rustdesk-server</a>)" R"(。</li>
  <li>⚠ <b>Workers 的 locationHint 一定要设 <code>apac</code></b> ——
      这样才会落到香港/日本节点，也就是实测 43–101 ms 的那一批；默认会飘到欧美，
      那就白搭了。</li>
  <li>把拿到的域名填进「网络路径」里的三个地址，公钥填到「服务器公钥」。</li>
</ol>

<h3>为什么这一档会自动打开 websocket</h3>
<p>Cloudflare 只能代理 HTTP/WebSocket，<b>代理不了裸 TCP</b>。所以选这一档时，
外壳会自动帮你把 RustDesk 的 allow-websocket 打开，不用你手动改。</p>
<p style='color:#999999; font-size:12px;'>这一档的延迟数字是从 Tailscale 同区域节点
推出来的<b>预期值</b>，不是 Cloudflare 实测值。点「一键体检」可以直接量出来。</p>
</div>)");
    }

    // ----------------------------------------------------------------- custom
    if (key == QLatin1String("custom")) {
        return QStringLiteral(R"(
<div style='color:#e0e0e0;'>
<h2>自建服务器（自己的 VPS / frp / 端口映射）</h2>
<p>你自己有一台能被外面访问到的机器 —— VPS、公司内网、frp 打洞、或者家里路由器
做了端口映射 —— 在上面跑 hbbs + hbbr，把地址填进来。这条路最快，因为服务器
离你最近。</p>

<h3>服务器上要跑的</h3>
<ul>
  <li><b>hbbs</b>（信令）监听 <code>21116/tcp</code> + <code>21116/udp</code>
      → 地址填「信令服务器」</li>
  <li><b>hbbr</b>（中继）监听 <code>21117/tcp</code>
      → 地址填「中继服务器」（可以和信令同一台机器）</li>
  <li>hbbs <b>首次启动时会打印一串公钥</b> → 填到「服务器公钥」</li>
</ul>
<p>官方部署文档：)" R"(<a href="https://rustdesk.com/docs/en/self-host/">https://rustdesk.com/docs/en/self-host/</a>)" R"(</p>

<h3>地址怎么写</h3>
<p>写 <code>host</code> 或者 <code>host:port</code> 都行 —— <b>只有端口和默认值不一样时
才需要写端口</b>。地址里<b>不能有单引号或反斜杠</b>（会写坏 RustDesk 的配置文件，
导致它整个加载失败、连本机 ID 都会被换掉）。</p>
<p>填完点「立即应用」，再点「一键体检」验一下服务器真的通。</p>
</div>)");
    }

    // 认不出来的 key：返回一段不会崩的兜底文案，而不是空串 ——
    // 空串会让 QTextBrowser 显示一片空白，看着像功能坏了。
    return QStringLiteral(
        "<div style='color:#e0e0e0;'><p>没有这个主题。</p></div>");
}

} // namespace Guide

#include "PasswordPolicy.h"

#include "AppConfig.h"
#include "ConnectionWatcher.h"
#include "I18n.h"
#include "RustDeskController.h"

#include <QRandomGenerator>
#include <QTimer>

namespace {
// 去掉 0/O、1/l/I 这些念出来会听错、抄下来会看错的字符 —— 这个密码是要口头/微信
// 传给对方的，易混字符是实打实的麻烦。
const char kCharset[] = "abcdefghjkmnpqrstuvwxyz"
                        "ABCDEFGHJKLMNPQRSTUVWXYZ"
                        "23456789";

// 定时器上限：系统睡眠/休眠会让长定时器不准，所以最多睡 10 分钟就醒来重算一次。
constexpr qint64 kMaxSleepMs = 10 * 60 * 1000;

// 首次补密码时"等 RustDesk.toml 生成"的重试节奏与次数上限（3 秒 × 5 ≈ 15 秒）。
// 内核是被外壳在启动时拉起来的，它落地自己的配置通常在一两秒内。
constexpr int    kMaxBootstrapTries = 5;
constexpr int    kBootstrapRetryMs  = 3000;

// 「用完即换」的宽限期：对端走了之后先等这么久，期间**只要又有人连进来就取消**。
// 为什么必须有：客户端网络抖一下、或者用户自己手滑断一下再点连接，都会写成
// "Connection closed" —— 那种情况下人其实还在，换密码只会把对方挡在门外。
constexpr int kOnceGraceMs = 10000;

// 两次"用完即换"之间的最小间隔。防的是客户端反复掉线重连：每一次都换一次密码
// 就等于把 RustDesk 反复重启，而那几秒这台机器是连不上的。
constexpr int kOnceMinGapSeconds = 30;
} // namespace

PasswordPolicy::PasswordPolicy(AppConfig* config, RustDeskController* rd,
                               ConnectionWatcher* watcher, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_rd(rd)
    , m_watcher(watcher ? watcher : new ConnectionWatcher(this))
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &PasswordPolicy::onTick);

    m_bootstrapTimer = new QTimer(this);
    m_bootstrapTimer->setSingleShot(true);
    connect(m_bootstrapTimer, &QTimer::timeout, this, &PasswordPolicy::onBootstrapTick);

    m_onceTimer = new QTimer(this);
    m_onceTimer->setSingleShot(true);
    connect(m_onceTimer, &QTimer::timeout, this, &PasswordPolicy::onOnceGraceElapsed);

    connect(m_watcher, &ConnectionWatcher::remoteConnected, this,
            &PasswordPolicy::onRemoteConnected);
    connect(m_watcher, &ConnectionWatcher::remoteDisconnected, this,
            &PasswordPolicy::onRemoteDisconnected);
}

PasswordPolicy::Mode PasswordPolicy::mode() const
{
    const QString s = m_config->passwordMode();
    if (s == QLatin1String("fixed"))
        return Mode::Fixed;
    if (s == QLatin1String("rotate"))
        return Mode::Rotate;
    if (s == QLatin1String("timed"))
        return Mode::Timed;
    if (s == QLatin1String("once"))
        return Mode::Once;
    if (s == QLatin1String("off"))
        return Mode::Off;
    // ⚠ 「关闭」必须是配置里**明写**的 "off"，不能靠"没配过"来默认。
    //   一台刚解压、配置里还没有 password_mode 的机器真正该走的是「固定密码」这条主线
    //   （首次运行还会自动补一个，见 start()），界面上再按 needsApply 打「未应用」。
    //   把"没配过"当成「关闭」，等于一进门就告诉用户"这台机器谁也连不进来"。
    return Mode::Fixed;
}

void PasswordPolicy::setMode(Mode m)
{
    switch (m) {
    case Mode::Fixed:  m_config->setPasswordMode(QStringLiteral("fixed"));  break;
    case Mode::Rotate: m_config->setPasswordMode(QStringLiteral("rotate")); break;
    case Mode::Timed:  m_config->setPasswordMode(QStringLiteral("timed"));  break;
    case Mode::Once:   m_config->setPasswordMode(QStringLiteral("once"));   break;
    case Mode::Off:    m_config->setPasswordMode(QStringLiteral("off"));    break;
    }
    persist();
    // 换档位会让"谁需要盯日志"跟着变（只有「用完即换」需要）。
    syncWatcher();
    scheduleNext();
}

int PasswordPolicy::rotateHours() const
{
    const int h = m_config->rotateHours();
    return h > 0 ? h : 3;
}

void PasswordPolicy::setRotateHours(int hours)
{
    m_config->setRotateHours(hours > 0 ? hours : 3);
    persist();
    scheduleNext();
}

QString PasswordPolicy::currentPassword() const
{
    return m_config->password();
}

QDateTime PasswordPolicy::nextRotationAt() const
{
    switch (mode()) {
    case Mode::Rotate: {
        const QDateTime last = m_config->passwordRotatedAt();
        if (!last.isValid())
            return QDateTime::currentDateTime();   // 从没轮换过 → 立刻该轮换
        return last.addSecs(qint64(rotateHours()) * 3600);
    }
    case Mode::Timed:
        return m_config->passwordExpiresAt();      // 到点作废
    default:
        return QDateTime();
    }
}

QDateTime PasswordPolicy::expiresAt() const
{
    return mode() == Mode::Timed ? m_config->passwordExpiresAt() : QDateTime();
}

QString PasswordPolicy::generatePassword(int length)
{
    if (length < 4)
        length = 4;
    const int n = int(sizeof(kCharset) - 1);
    QString   out;
    out.reserve(length);
    for (int i = 0; i < length; ++i)
        out.append(QChar::fromLatin1(kCharset[QRandomGenerator::system()->bounded(n)]));
    return out;
}

bool PasswordPolicy::offModeWipesExisting(const QString& shellPlain, const QString& tomlRaw)
{
    // 两边都空才叫"本来就没有固定密码"，那种情况点「立即应用」是空操作，不用吓唬人。
    // 只要有一边有值，关闭模式就会把它删掉 —— **包括外壳读不回明文的原生加密串**，
    // 那是最容易被误删的一种（界面上它显示成"已设置（密文）"，看着不像能删的东西）。
    return !shellPlain.isEmpty() || !tomlRaw.isEmpty();
}

bool PasswordPolicy::needsApply(Mode m, const QString& shellPlain, const QString& tomlRaw)
{
    if (m == Mode::Off)
        return !tomlRaw.isEmpty();   // 要的是"没有密码"，可文件里还留着一个（含加密串）
    if (tomlRaw.isEmpty())
        return true;                 // 要的是"有密码"，可文件里什么都没有
    if (shellPlain.isEmpty())
        return true;                 // 文件里有串，外壳却没有对应的明文 → 不是我们落的那份
    // 两边都有：只有**逐字相同**才算我们落地成功。不同就是漂移（比如 RustDesk 自己
    // 把明文加密了、或者用户拿别的工具改过）。
    return tomlRaw != shellPlain;
}

bool PasswordPolicy::canTakeOverPassword(Mode m, const QString& shellPlain, const QString& tomlRaw)
{
    return m != Mode::Off && shellPlain.isEmpty() && !tomlRaw.isEmpty();
}

bool PasswordPolicy::shouldRotateAfterSession(bool hasSessionBeenSeen,
                                              const QDateTime& lastRotatedAt,
                                              const QDateTime& now, int minGapSeconds)
{
    if (!hasSessionBeenSeen)
        return false;
    if (!lastRotatedAt.isValid())
        return true;                 // 从来没换过
    return lastRotatedAt.secsTo(now) >= minGapSeconds;
}

bool PasswordPolicy::bootstrapPending(const BootstrapInputs& in)
{
    // 前提跟 shouldBootstrapPassword 完全同源，只差最后那一项：
    //   · 文件**在**、但读不到 → 不是 pending（那是"不知道"，再看几眼也还是不知道，
    //     而且这种机器多半不是全新机器，别拿它的文件开玩笑）
    //   · 文件在、也确实没密码 → 不是 pending（该由 shouldBootstrapPassword 直接判补）
    return in.freshConfig
           && !in.alreadyBootstrapped
           && (in.mode == Mode::Off || in.mode == Mode::Fixed)
           && in.shellPlain.isEmpty()
           && !in.tomlExists;
}

bool PasswordPolicy::shouldBootstrapPassword(const BootstrapInputs& in)
{
    // ① 用户（或上一次启动）已经做过决定了，别插手。
    //    「关闭」也是一次决定 —— 少了这条，用户刚删掉的密码下次启动就自己回来了。
    if (in.alreadyBootstrapped)
        return false;

    // ② **必须是真·首次运行。** 这条是后加的，也是这次事故唯一能挡住的那一条：
    //    光看"外壳觉得自己没做过决定"不够 —— 判据一旦落到"RustDesk.toml 里现在没有
    //    password 这一行"上，就等于把**别人的文件**在某一个瞬间的内容当成了事实。
    //    RustDesk 自己会在启动/退出时整份重写这个文件，那一瞬间读到的可能正好是
    //    "没有密码"（也可能是读了一半）。实测真发生过：开发机上跑验收，外壳把用户
    //    原生的加密固定密码当成"没有"，直接写了一个自己生成的新密码进去。
    //    改成要求"外壳的配置文件夹里连文件都没有"之后，这台机器（跑过外壳的机器）
    //    就再也不可能命中这条路径了 —— 判据落在**我自己的文件**上，而不是别人的。
    if (!in.freshConfig)
        return false;

    // ③ Rotate / Timed 各有自己的到点逻辑（start() 里那段），这里只管"根本没配过"的两种：
    //    Off（默认）和 Fixed（配置说"要固定密码"但压根没有密码 —— 半拉子状态，也该补）。
    if (in.mode != Mode::Off && in.mode != Mode::Fixed)
        return false;

    // ④ 文件得**在**，而且**读得到**。
    //    · 文件不在 = 内核还没把配置落下来 → 不是"没有密码"，是"还没到时候"
    //      （写也写不进去：writePasswordPlain 要求文件已存在），交给重试。
    //    · 文件在但读不到 = "不知道"，不是"没有" —— 不知道就别动别人的东西。
    if (!in.tomlExists || !in.tomlReadOk)
        return false;

    // ⑤ 最后一道：**外壳手上一份明文都没有**。
    //
    //    ⚠ 这里以前写的是 `&& !in.tomlHasPassword` —— 也就是"RustDesk 里本来就没有
    //      密码"才补，反过来"机器上已经有一个外壳读不出明文的固定密码"就一个字都不碰。
    //      那条把**新用户第一次打开最常撞上的状态**排除在外了：本机那行只有
    //      「已设置（密文）」+ 一个没人看得懂的按钮，用户得先做一次选择才能用 ——
    //      用户实机就是这么被卡住的（"大部分人看不懂"）。
    //      现在改成**首次运行一律接管**：不管 RustDesk 里有没有、读不读得出来，
    //      首次运行都把密码落成外壳自己管的这一个，界面上直接能看能复制。
    //
    //      顺带把很久以前那次事故的路径**从根上消掉**：判据不再去看别人的文件此刻
    //      写了什么（那正是当年读到"没有密码"、把用户原生加密密码覆盖掉的原因），
    //      只看"**我自己的**配置文件在不在"这一个本进程内不会变的事实。
    //
    //      代价说清楚：原来那个固定密码**会被替换掉**（写前自动备份到 RustDesk.toml.bak）。
    //      只发生一次 —— 落地成功后 alreadyBootstrapped 置位，之后永远走不到这条。
    return in.shellPlain.isEmpty();
}

bool PasswordPolicy::applyNow(QString* errorOut, bool manageProcess)
{
    // 谁在"落地当前设置"时该沿用已存的密码：固定、限时，以及「用完即换」。
    //   · 固定 / 限时：换掉会把已经拿到码的人挡在外面
    //   · 用完即换：它的"换"有自己的时机（人走了之后）—— 用户点「立即应用」只是
    //     想把这档生效，**不该顺手作废他刚发出去的那个码**
    // 只有「定时轮换」按「立即应用」= 立刻换新的。
    const bool reuse = (mode() != Mode::Rotate);
    return applyInternal(reuse, errorOut, manageProcess);
}

bool PasswordPolicy::regenerate(QString* errorOut, bool manageProcess)
{
    return applyInternal(/*reuseExisting=*/false, errorOut, manageProcess);
}

bool PasswordPolicy::applyInternal(bool reuseExisting, QString* errorOut, bool manageProcess)
{
    // 看门狗在这段里必须闭嘴：下面要"停 → 写 → 起"，它要是抢在中间把进程拉起来，
    // 写进去的密码会被进程内存里的旧值覆盖。
    RustDeskController::Maintenance guard(m_rd);

    const Mode m = mode();

    QString plain = m_config->password();
    if (m == Mode::Rotate || (m == Mode::Timed && !reuseExisting)) {
        plain = generatePassword();
    } else if (m == Mode::Timed && plain.isEmpty()) {
        plain = generatePassword();          // 限时但还没设过密码：生成一个
    } else if (m == Mode::Fixed && plain.isEmpty()) {
        plain = generatePassword();          // 首次开固定密码：生成一个
    } else if (m == Mode::Once && (plain.isEmpty() || !reuseExisting)) {
        // 「用完即换」：首次开（还没密码）要生成一个；到点换的时候自然也是新的。
        // ⚠ 这个 `!reuseExisting` 不能省 —— applyNow() 算出来的 reuse 对 Once 是 false，
        //   所以「立即应用」和"到点自动换"都走这里；少了它就会**复用旧密码**，
        //   表现是"显示说换过了，密码其实一个字没变"。
        plain = generatePassword();
    } else if (m == Mode::Off) {
        plain.clear();
    }

    // 限时：作废时刻从这次落地算起。其余模式清掉它（免得留下过期的残值）。
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime expiry =
        (m == Mode::Timed) ? now.addSecs(qint64(rotateHours()) * 3600) : QDateTime();

    const bool wasRunning = m_rd->isRunning();

    if (manageProcess) {
        if (wasRunning && !m_rd->stop()) {
            if (errorOut)
                *errorOut = TR("停不掉正在运行的 RustDesk，已放弃写入（否则会被它回写覆盖）");
            return false;
        }
    }

    bool ok = false;
    if (m == Mode::Off)
        ok = m_rd->clearPassword();
    else
        ok = m_rd->writePasswordPlain(plain);

    if (!ok) {
        if (errorOut)
            *errorOut = TR("写 %1 失败（写前已备份到 RustDesk.toml.bak，失败会自动回滚）")
                            .arg(RustDeskController::mainConfigPath());
        if (manageProcess && wasRunning)
            m_rd->start();                    // 写失败也要把服务拉回来
        return false;
    }

    m_config->setPassword(m == Mode::Off ? QString() : plain);
    m_config->setPasswordRotatedAt(now);
    m_config->setPasswordExpiresAt(expiry);
    // ⚠ **每落地一次就算"已经做过决定"**，不管是设还是删。
    //   少了这一行会有一个很难发现的回退：用户主动选「关闭」把密码删掉（那是有意的），
    //   而 password_bootstrapped 一直是 false（因为这机器本来有密码，补密码那步从没
    //   触发过）—— 于是**下次启动"首次运行补密码"又把它装回去了**，用户会以为
    //   「关闭」这一档坏了。删也是决定的一种，这里必须一并置位。
    m_config->setPasswordBootstrapped(true);
    persist();

    // 常驻开着的时候无论如何都要起回来 —— 被控端不能因为改个密码就下线
    if (manageProcess && (wasRunning || m_rd->keepAlive()))
        m_rd->start();

    emit passwordChanged(m_config->password());
    scheduleNext();
    return true;
}

// 限时到点：把密码清掉，模式退回 Off。清掉之后就只剩 RustDesk 的一次性密码了 ——
// 人不在机器前面就拿不到，这正是"作废"想要的效果。
bool PasswordPolicy::expireTimed()
{
    RustDeskController::Maintenance guard(m_rd);

    const bool wasRunning = m_rd->isRunning();
    if (wasRunning && !m_rd->stop()) {
        // 停不掉就先不动模式，等下一跳重试 —— 硬写会被进程内的旧值覆盖回写
        return false;
    }

    if (!m_rd->clearPassword()) {
        if (wasRunning)
            m_rd->start();
        return false;
    }

    m_config->setPassword(QString());
    m_config->setPasswordExpiresAt(QDateTime());
    m_config->setPasswordMode(QStringLiteral("off"));
    // 同 applyInternal：作废也是一次"落地"，也得算"已经做过决定"。否则一台老配置
    // （缺 password_bootstrapped 标记）走到"限时到期"之后，下次启动会被当成首次运行
    // 再补一个新密码 —— 用户明明要的是"到点就作废"，结果码又回来了。
    m_config->setPasswordBootstrapped(true);
    persist();

    if (wasRunning || m_rd->keepAlive())
        m_rd->start();

    emit passwordChanged(QString());
    syncWatcher();          // 模式退回「关闭」→ 不再需要盯着日志
    scheduleNext();
    return true;
}

// ---- 「用完即换」----
//
// 这一档的"换"发生在**对端离开之后**，所以它和别的档有一个本质区别：
// 换密码必须重启 RustDesk，而那一刻会话已经结束了，**不会掐断任何人**。
// （别的档（定时/手动）换的时候可能正有人连着，界面上都有断线提示。）
//
// 为什么不能"有人连上来就换"：重启 → 那个刚连上的人立刻掉线，而且他手里的码
// 已经作废了，等于永远连不上。用户要的"一次性"，正确的落点是"用完"而不是"开始用"。

void PasswordPolicy::onRemoteConnected()
{
    m_sawSession = true;
    if (m_onceTimer->isActive())
        m_onceTimer->stop();     // 人又连上了 → 宽限期作废，这次不换
}

void PasswordPolicy::onRemoteDisconnected()
{
    if (mode() != Mode::Once)
        return;
    if (!m_sawSession)
        return;                  // 只连上又立刻掉（认证失败、探测）不算"用了一次"
    m_sawSession = false;

    if (!shouldRotateAfterSession(true, m_config->passwordRotatedAt(),
                                  QDateTime::currentDateTime(), kOnceMinGapSeconds))
        return;                  // 刚换过（30 秒内）→ 这一轮不换，免得反复重启

    m_onceTimer->start(kOnceGraceMs);
}

void PasswordPolicy::onOnceGraceElapsed()
{
    if (mode() != Mode::Once)
        return;
    if (m_sawSession)
        return;                  // 宽限期内有人回来了 → 说明人还在，不换

    QString err;
    if (regenerate(&err))
        emit statusMessage(TR("用完即换：刚才那位已经断开，密码已换成新的"));
    else
        emit statusMessage(TR("用完即换：换密码失败（%1）—— 下次有人连过再试").arg(err));
}

void PasswordPolicy::syncWatcher()
{
    if (!m_watcher)
        return;
    // 只有「用完即换」需要盯日志。别的档白占着轮询没意义，而且会让"提示"那一档
    // 看起来像是被别人偷偷打开了。
    m_watcher->requestEnabled(this, mode() == Mode::Once);
}

void PasswordPolicy::start()
{
    // 0) 首次运行：**一个密码都没有**的时候自动补一个固定密码。
    //
    //    为什么非补不可：绿色包默认就是「关闭」，而 RustDesk 的一次性密码根本读不出来
    //    —— 命令行只有 --get-id / --set-id / --config，没有"把临时密码打出来"这条路
    //    （已 grep librustdesk.dll 确认）。也就是说开箱之后这台机器**谁都连不进来**，
    //    而且用户在这个界面上连临时密码是多少都看不到，会一直以为是软件坏了。
    //    —— 用户实机就是这么撞上的："密码未设置的时候为啥连接也要密码"。
    //
    //    ⚠ 这一步会**写盘**，所以边界必须收紧到只有一个方向：机器上已经有密码就
    //      一个字都不碰（含外壳读不回明文的原生加密串）；用户自己选过「关闭」之后
    //      （bootstrapped 已置位）也不再补，否则「关闭」就成了摆设，还会每次启动
    //      把用户刚删掉的密码偷偷复活。
    maybeBootstrapPassword();

    // 启动时补一次：机器关机期间错过的轮换 / 到期的限时密码都不能丢。
    const Mode      m   = mode();
    const QDateTime due = nextRotationAt();

    if (m == Mode::Rotate && due.isValid() && due <= QDateTime::currentDateTime()) {
        QString err;
        if (!applyNow(&err))
            emit statusMessage(TR("启动补轮换失败：%1").arg(err));
    } else if (m == Mode::Timed && due.isValid() && due <= QDateTime::currentDateTime()) {
        if (expireTimed())
            emit statusMessage(TR("限时密码在关机期间已到期，已作废"));
        else
            emit statusMessage(TR("限时密码已到期，但作废失败 —— 稍后会自动重试"));
    }
    // 「用完即换」：开机时**不该换** —— 换的时机只有"人走了"。这里只把状态对齐：
    // 谁需要盯日志（只有这一档），以及"这一段还没人来过"。
    m_sawSession = false;
    if (m != Mode::Once)
        m_onceTimer->stop();
    syncWatcher();
    scheduleNext();
}

// 首次运行补一个固定密码。start() 和重试定时器都走这里 —— **判据只有一份**，
// 免得"启动时"和"重试时"各写一套，慢慢漂移成两个不同的判断。
void PasswordPolicy::maybeBootstrapPassword()
{
    // ⚠ 判据里 freshConfig 取的是**本轮启动时算出来的**那个值（load() 那一刻文件在不在），
    //   本进程内不会再变。这是刻意的：一旦让它每次重算，就等于又回到"拿别人的文件在
    //   此刻的内容当事实"那套判据上了 —— 而这次真出事的就是那个判据。
    const RustDeskController::PasswordProbe probe = m_rd->probePassword();
    BootstrapInputs in;
    in.mode                = mode();
    in.freshConfig         = m_config->freshConfigAtLoad();
    in.shellPlain          = m_config->password();
    in.tomlExists          = probe.exists;
    in.tomlReadOk          = probe.readOk;
    in.tomlHasPassword     = probe.hasPassword;
    in.alreadyBootstrapped = m_config->passwordBootstrapped();

    if (!shouldBootstrapPassword(in)) {
        // 只差"RustDesk.toml 还没生成"这一项 → 是"还没到时候"，不是"不该补"。
        // 全新机器上内核刚被拉起来，这个文件要过几秒才出现；判死在这儿等于
        // "开箱即用"永远不生效（下次启动配置文件夹里已经有 puremote.json 了）。
        if (bootstrapPending(in) && m_bootstrapTries < kMaxBootstrapTries) {
            ++m_bootstrapTries;
            m_bootstrapTimer->start(kBootstrapRetryMs);
        }
        return;
    }

    m_bootstrapTimer->stop();
    m_config->setPasswordMode(QStringLiteral("fixed"));
    QString err;
    if (applyNow(&err)) {
        // 置位在 applyInternal 里做（落地成功才置）—— 失败了下次还能再试。
        // 否则一次偶发的"停不掉进程"就把这台机器**永久**留在"谁都连不进来"的状态里，
        // 而那恰恰是最需要它自己能恢复的一种状态。
        //
        // ⚠ 两种开场要分开说。`tomlHasPassword` 就是为这句话留的：机器上原本已经有一个
        //   固定密码（外壳读不出明文的那种）时，这次落地是**替换**，不是"从无到有" ——
        //   凭据被换掉不该让用户事后自己发现。
        emit statusMessage(in.tomlHasPassword
            ? TR("第一次运行：已经把本机的固定密码换成外壳管的这一个"
                 "（就在主界面上，能看能复制）。原来那个已备份到 RustDesk.toml.bak。")
            : TR("第一次运行：已经自动设好一个固定密码（就在主界面上），"
                 "别人现在就能连进来。不想要就去「别人连我」关掉。"));
    } else {
        emit statusMessage(TR("自动设置固定密码失败：%1").arg(err));
    }
}

void PasswordPolicy::onBootstrapTick()
{
    maybeBootstrapPassword();
}

void PasswordPolicy::scheduleNext()
{
    m_timer->stop();

    const QDateTime due = nextRotationAt();
    if (!due.isValid()) {
        emit nextRotationChanged(QDateTime());
        return;
    }

    qint64 ms = QDateTime::currentDateTime().msecsTo(due);
    if (ms < 1000)
        ms = 1000;
    m_timer->start(int(qMin(ms, kMaxSleepMs)));
    emit nextRotationChanged(due);
}

void PasswordPolicy::onTick()
{
    const Mode m = mode();
    if (m != Mode::Rotate && m != Mode::Timed) {
        scheduleNext();
        return;
    }

    const QDateTime due = nextRotationAt();
    if (!due.isValid() || due > QDateTime::currentDateTime()) {
        scheduleNext();                       // 还没到点（长定时器被截断过），继续等
        return;
    }

    if (m == Mode::Timed) {
        if (expireTimed())
            emit statusMessage(TR("限时密码已到期，已作废 —— 现在只能靠屏幕上的一次性密码连"));
        else
            emit statusMessage(TR("限时密码到期了但作废失败，稍后重试"));
        return;
    }

    QString err;
    if (applyNow(&err))
        emit statusMessage(TR("密码已轮换"));
    else
        emit statusMessage(TR("轮换失败：%1").arg(err));
}

void PasswordPolicy::persist() const
{
    if (!m_config->save())
        emit const_cast<PasswordPolicy*>(this)->statusMessage(
            TR("外壳配置写不进去（%1）").arg(AppConfig::configFilePath()));
}

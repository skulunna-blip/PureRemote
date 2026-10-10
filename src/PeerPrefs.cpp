#include "PeerPrefs.h"

#include "I18n.h"
#include "RustDeskController.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStringConverter>
#include <QStringList>

namespace {

QString g_dirOverride;      // 空 = 走真实 config 目录

// 字节级读写 / 行尾策略**不在这里实现** —— 见 RustDeskController.h 里那四条约定的说明。
// 收在一处是因为"写盘去掉了 Text、读的时候还留着"这种半拉子修复会互相掩盖：
// 读的时候 \r 被吃掉，于是"整份文件被改行尾"在比较里根本看不见，测试全绿而文件早改了。
// 本文件里旧名字的薄包装，免得把一堆调用点改得满屏限定名
// （`using RustDeskController::xxx;` 在命名空间作用域里对类成员是**不允许**的，
//   GCC 直接报 "using-declaration for member at non-class scope"）
inline bool restoreFromBytes(const QString& path, const QByteArray& original)
{
    return RustDeskController::writeConfigBytes(path, original);
}
inline bool readBytes(const QString& path, QByteArray* out, QString* why)
{
    return RustDeskController::readConfigBytes(path, out, why);
}
inline QStringList splitLines(const QByteArray& bytes)
{
    return RustDeskController::splitConfigLines(bytes);
}
inline QString detectEol(const QByteArray& bytes)
{
    return RustDeskController::detectEol(bytes);
}
inline bool writeConfigBytes(const QString& path, const QByteArray& bytes)
{
    return RustDeskController::writeConfigBytes(path, bytes);
}


// peers 根目录（真实）。**不建目录** —— 建了也没用，写操作本来就要求文件已存在。
QString realDirPath()
{
    if (!g_dirOverride.isEmpty())
        return g_dirOverride;
    return QDir(RustDeskController::configDir()).filePath(QStringLiteral("peers"));
}

// ---- 作用域判定 ----------------------------------------------------------
//
// 这个文件里**有多行数组**（`size = [\n 0,\n 0,\n …]`、`size_ft`、`size_pf`），
// 所以「行首是 `[` 就是表头」这套判法在这里必错 —— 数组元素行也可能以 `[` 或 `]` 开头。
// 一律按括号深度判。
//
// ⚠ **一次扫描出"每行开始时的深度"，三个判定函数共用这一份结果。**
//   原来那三个函数各抄了一遍同样的循环，那是"判定会漂移"的温床：同一个文件，
//   firstTableHeader 认它是表头而 tableEnd 不认，就会出现"键被写到别的表里、
//   而复验用的又是同一套判断、什么都发现不了"。对抗性复查（编译出来真跑）抓到过
//   这类静默错位的具体样例，所以这里收成一份。
QVector<int> scanDepths(const QStringList& lines)
{
    QVector<int> out(lines.size());
    int          depth = 0;
    for (int i = 0; i < lines.size(); ++i) {
        out[i] = depth;                       // 这一行**开始**时的深度
        const QString& s = lines.at(i);
        bool inSingle = false;                // '...' 字面串
        bool inDouble = false;                // "..." 基本串
        for (int k = 0; k < s.size(); ++k) {
            const QChar c = s.at(k);
            if (inSingle) {
                if (c == QLatin1Char('\''))
                    inSingle = false;
                continue;
            }
            if (inDouble) {
                if (c == QLatin1Char('\\')) { ++k; continue; }   // 转义，跳过下一个字符
                if (c == QLatin1Char('"'))
                    inDouble = false;
                continue;
            }
            // ⚠ 注释必须在算括号**之前**判掉：# 后面到行尾的内容（包括 `[` `]` `'` `"`）
            //   全是注释文字。原来没有这一步，`# TODO 修一下 [这个` 这种注释会让深度永远
            //   回不到 0，于是 [options] 之后的表头全都认不出来、键被追加到文件末尾（落进
            //   最后一个表里）。**而按同一个错误判断做的复验照样是绿的。**
            if (c == QLatin1Char('#'))
                break;                        // 注释到行尾，后面的字符都不算
            if (c == QLatin1Char('\'')) { inSingle = true; continue; }
            if (c == QLatin1Char('"'))  { inDouble = true; continue; }
            if (c == QLatin1Char('['))
                ++depth;
            else if (c == QLatin1Char(']'))
                --depth;
        }
    }
    return out;
}

bool isTableHeader(const QString& trimmed)
{
    return trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'))
           && !trimmed.contains(QLatin1Char('='));
}

// 第一个顶层表头的行号；没有则返回 lines.size()。顶层键都在这之前。
int firstTableHeader(const QStringList& lines)
{
    const QVector<int> d = scanDepths(lines);
    for (int i = 0; i < lines.size(); ++i)
        if (d.at(i) == 0 && isTableHeader(lines.at(i).trimmed()))
            return i;
    return lines.size();
}

// 名为 name 的表头的行号；-1 = 没有。
int tableHeaderLine(const QStringList& lines, const QString& name)
{
    const QVector<int> d = scanDepths(lines);
    for (int i = 0; i < lines.size(); ++i)
        if (d.at(i) == 0 && lines.at(i).trimmed() == name)
            return i;
    return -1;
}

// 从 from 行（不含）起，下一个顶层表头的位置；没有则 lines.size()。
int tableEnd(const QStringList& lines, int from)
{
    const QVector<int> d = scanDepths(lines);
    for (int i = from + 1; i < lines.size(); ++i)
        if (d.at(i) == 0 && isTableHeader(lines.at(i).trimmed()))
            return i;
    return lines.size();
}

// **独立**复验：某个键往上找最近的一个表头，看它落在哪个表里。返回表名（顶层 = ""）。
// 关键在"独立"——它不看 scanDepths 的判断，用的是"整行就是个 [表名]"这个字形判据。
// 用同一套判断去复验，等于让扫描器自己给自己判卷：它错两次就互相抵消。
// （这个判据在本文件里很干净：多行数组的元素行要么是 `0,` 要么是 `]`，都不是"整行 [名字]"。）
QString keyTable(const QStringList& lines, const QString& key)
{
    const QRegularExpression re(
        QStringLiteral(R"(^[ \t]*%1[ \t]*=)").arg(QRegularExpression::escape(key)));
    const QRegularExpression hdr(QStringLiteral(R"(^\[[^\[\]]*\][ \t]*$)"));
    for (int i = 0; i < lines.size(); ++i) {
        if (!re.match(lines.at(i)).hasMatch())
            continue;
        for (int k = i - 1; k >= 0; --k) {
            const QString t = lines.at(k).trimmed();
            if (hdr.match(t).hasMatch())
                return t == QLatin1String("[]") ? QString() : t;
        }
        return QString();                     // 上面没有表头 = 顶层
    }
    return QStringLiteral("<没有这个键>");
}

// 在 [begin, end) 里找形如 `key = …` 的行，返回行号；-1 = 没有。
// 限定范围是必须的：`[options]` 里和顶层可能有同名键，整文件替换会写串。
int findKey(const QStringList& lines, int begin, int end, const QString& key)
{
    const QRegularExpression re(
        QStringLiteral(R"(^[ \t]*%1[ \t]*=)").arg(QRegularExpression::escape(key)));
    for (int i = begin; i < end; ++i) {
        if (re.match(lines.at(i)).hasMatch())
            return i;
    }
    return -1;
}

// 在 [begin, end) 里取 `key = 'xxx'` 的 xxx（不带引号）。取不到返回空串。
QString readKey(const QStringList& lines, int begin, int end, const QString& key)
{
    const int at = findKey(lines, begin, end, key);
    if (at < 0)
        return QString();
    const QString line = lines.at(at);
    const int     eq   = line.indexOf(QLatin1Char('='));
    QString       v    = line.mid(eq + 1).trimmed();
    if (v.size() >= 2 && v.startsWith(QLatin1Char('\'')) && v.endsWith(QLatin1Char('\'')))
        v = v.mid(1, v.size() - 2);
    return v;
}

// 把一行 `key = 'value'` 落到 [begin, end) 里的正确位置：有就替换，没有就插在表尾
// （跳过尾随空行，免得把键插到下一个表头后面去）。
void putKey(QStringList& lines, int begin, int end, const QString& key, const QString& newLine)
{
    const int found = findKey(lines, begin, end, key);
    if (found >= 0) {
        lines[found] = newLine;
        return;
    }
    int at = end;
    while (at > begin + 1 && lines.at(at - 1).trimmed().isEmpty())
        --at;
    lines.insert(at, newLine);
}

// `custom_image_quality` 在这个文件里是**数组**（`[50]`），不是字符串。
QString arrayLine(const QString& key, int v)
{
    return QStringLiteral("%1 = [%2]").arg(key).arg(v);
}

QString stringLine(const QString& key, const QString& v)
{
    return QStringLiteral("%1 = '%2'").arg(key, v);
}

} // namespace

// ---- 合法值 --------------------------------------------------------------

QStringList PeerPrefs::imageQualities()
{
    // 实测被控端画质系数（--qtest 里有对应断言，别改顺序/取值而不改注释）：
    //   low → 0.5   balanced → 0.67（默认）   custom → 1.0   best → 1.5
    // `original` / `adaptive_ab` **不在**这一档里 —— 它们出现在二进制的同一串字符里，
    // 看着像，但实测写进去等于默认档（0.67），是别的东西的值。
    return { QStringLiteral("low"), QStringLiteral("balanced"),
             QStringLiteral("best"), QStringLiteral("custom") };
}

QStringList PeerPrefs::codecs()
{
    return { QStringLiteral("auto"), QStringLiteral("h264"), QStringLiteral("h265"),
             QStringLiteral("vp9"), QStringLiteral("av1"), QStringLiteral("vp8") };
}

QString PeerPrefs::imageQualityTitle(const QString& v)
{
    if (v == QLatin1String("low"))      return TR("省流（画质系数 0.5）");
    if (v == QLatin1String("best"))     return TR("清晰（1.5）");
    if (v == QLatin1String("custom"))   return TR("自定义（按下面那个数）");
    return TR("平衡（0.67，默认）");
}

QString PeerPrefs::codecTitle(const QString& v)
{
    if (v == QLatin1String("h264")) return TR("H.264（兼容最好）");
    if (v == QLatin1String("h265")) return TR("H.265 / HEVC（同画质更省带宽）");
    if (v == QLatin1String("vp9"))  return TR("VP9（软件编码，吃 CPU）");
    if (v == QLatin1String("av1"))  return TR("AV1（最省带宽，最吃 CPU）");
    if (v == QLatin1String("vp8"))  return TR("VP8（老，基本不用）");
    return TR("自动（让 RustDesk 自己挑）");
}

PeerPrefs::Quality PeerPrefs::defaults()
{
    // 与 RustDesk 自己写进新 peers 文件的值一致（见 peers/<id>.toml 初始内容）
    Quality q;
    q.imageQuality  = QStringLiteral("balanced");
    q.customQuality = 50;
    q.fps           = 30;
    q.codec         = QStringLiteral("auto");
    return q;
}

bool PeerPrefs::validate(const Quality& q, QString* err)
{
    auto fail = [err](const QString& m) {
        if (err)
            *err = m;
        return false;
    };
    if (!imageQualities().contains(q.imageQuality))
        return fail(TR("画质档位「%1」不是 RustDesk 认的值（只能 low/balanced/best/custom）")
                        .arg(q.imageQuality));
    if (q.customQuality < 0 || q.customQuality > 100)
        return fail(TR("自定义画质要在 0–100 之间，现在是 %1").arg(q.customQuality));
    // 上限给 240：RustDesk 没做校验，写个荒唐的帧率它照样收下，然后画面行为没法预期。
    if (q.fps < 1 || q.fps > 240)
        return fail(TR("帧率要在 1–240 之间，现在是 %1").arg(q.fps));
    if (!codecs().contains(q.codec))
        return fail(TR("编码器「%1」不是 RustDesk 认的值（只能 auto/h264/h265/vp9/av1/vp8）")
                        .arg(q.codec));
    return true;
}

// ---- 路径 ----------------------------------------------------------------

QString PeerPrefs::dirPath() { return realDirPath(); }

QString PeerPrefs::filePath(const QString& peerId)
{
    return QDir(dirPath()).filePath(peerId + QStringLiteral(".toml"));
}

bool PeerPrefs::exists(const QString& peerId)
{
    return !peerId.isEmpty() && QFile::exists(filePath(peerId));
}

void PeerPrefs::setDirForTest(const QString& dir) { g_dirOverride = dir; }
void PeerPrefs::clearDirForTest() { g_dirOverride.clear(); }

// ---- 读 ------------------------------------------------------------------

PeerPrefs::Quality PeerPrefs::read(const QString& peerId)
{
    Quality q = defaults();

    QByteArray bytes;
    if (!readBytes(filePath(peerId), &bytes, nullptr))
        return q;
    const QStringList lines = splitLines(bytes);

    const int topEnd = firstTableHeader(lines);

    // image_quality / custom_image_quality 在**顶层**
    const QString iq = readKey(lines, 0, topEnd, QStringLiteral("image_quality"));
    if (!iq.isEmpty())
        q.imageQuality = iq;

    // custom_image_quality 是数组形态 `[50]`，readKey 取不出数字，单独解
    {
        const int at = findKey(lines, 0, topEnd, QStringLiteral("custom_image_quality"));
        if (at >= 0) {
            const QString line = lines.at(at);
            // 负号要跟进来：只认 \d+ 的话 `[-50]` 会被读成 **50**，
            // 界面就会把一个负数安静地显示成正数（validate 根本没机会说话）。
            const QRegularExpression num(QStringLiteral(R"((-?\d+))"));
            const auto m = num.match(line.mid(line.indexOf(QLatin1Char('='))));
            if (m.hasMatch())
                q.customQuality = m.captured(1).toInt();
        }
    }

    // custom-fps / codec-preference 在 [options] 里
    const int optHdr = tableHeaderLine(lines, QStringLiteral("[options]"));
    if (optHdr >= 0) {
        const int optEnd = tableEnd(lines, optHdr);
        const QString fps = readKey(lines, optHdr + 1, optEnd, QStringLiteral("custom-fps"));
        if (!fps.isEmpty()) {
            bool ok = false;
            const int n = fps.toInt(&ok);
            if (ok)
                q.fps = n;
        }
        const QString cd = readKey(lines, optHdr + 1, optEnd, QStringLiteral("codec-preference"));
        if (!cd.isEmpty())
            q.codec = cd;
    }

    return q;
}

// ---- 写 ------------------------------------------------------------------

bool PeerPrefs::write(const QString& peerId, const Quality& q, QString* err)
{
    auto fail = [err](const QString& m) {
        if (err)
            *err = m;
        return false;
    };

    if (!validate(q, err))
        return false;
    if (peerId.isEmpty())
        return fail(TR("对端 ID 是空的"));
    // 这个 ID 是用户填的（RustDesk ID 或 tailnet 主机名），直接拼进路径。
    // 正常值是纯数字/字母/短横线，出现分隔符或 ".." 就说明不是正常值，别让它跑到 peers/ 外面去。
    if (peerId.contains(QLatin1Char('/')) || peerId.contains(QLatin1Char('\\'))
        || peerId.contains(QLatin1String("..")))
        return fail(TR("对端 ID「%1」里有路径分隔符，看着不像真的 ID").arg(peerId));

    const QString path = filePath(peerId);

    // **不建文件**。实测：极简的（只有画质几个键）peers 文件会被 RustDesk 在会话开始
    // 那一刻整个重置成默认值，写进去的东西一点不留。所以宁可拒绝，也别写一个"看起来
    // 存上了、连一次就没了"的假设置。文件是 RustDesk 第一次连这个对端时自己建的。
    if (!QFile::exists(path))
        return fail(TR("RustDesk 还没为这个对端建过设置文件（%1）。\n先连一次（建立过会话之后才有地方存画质），再回来调。")
                        .arg(path));

    QByteArray original;
    QString    why;
    if (!readBytes(path, &original, &why))
        return fail(TR("读不了 %1：%2").arg(path, why));

    QStringList   lines = splitLines(original);
    const QString eol   = detectEol(original);

    const int optHdr = tableHeaderLine(lines, QStringLiteral("[options]"));
    if (optHdr < 0)
        return fail(TR("%1 里没有 [options] 段 —— 不猜位置，宁可不写").arg(path));

    // ---- 全部改在内存里 ----
    // 顶层：image_quality（字符串）、custom_image_quality（数组）
    // [options]：custom-fps、codec-preference
    {
        const int topEnd = firstTableHeader(lines);
        putKey(lines, 0, topEnd, QStringLiteral("image_quality"),
               stringLine(QStringLiteral("image_quality"), q.imageQuality));
        putKey(lines, 0, firstTableHeader(lines), QStringLiteral("custom_image_quality"),
               arrayLine(QStringLiteral("custom_image_quality"), q.customQuality));
    }
    {
        // 上一段可能动过行数，[options] 的位置要重取
        const int hdr = tableHeaderLine(lines, QStringLiteral("[options]"));
        const int end = tableEnd(lines, hdr);
        putKey(lines, hdr + 1, end, QStringLiteral("custom-fps"),
               stringLine(QStringLiteral("custom-fps"), QString::number(q.fps)));
        putKey(lines, hdr + 1, tableEnd(lines, hdr), QStringLiteral("codec-preference"),
               stringLine(QStringLiteral("codec-preference"), q.codec));
    }

    // ---- 备份一次 → 一次写盘 ----
    const QString backup = path + QStringLiteral(".bak");
    QFile::remove(backup);
    if (!QFile::copy(path, backup))
        return fail(TR("备份 %1 失败，为安全起见没有动原文件").arg(path));

    // ⚠ **不要加 QIODevice::Text**。它在 Windows 上会把每个 \n 写成 \r\n，于是**整份文件
    //   每一行都被改掉** —— RustDesk 自己写的是 LF，改完就成了 CRLF。这个坑被 B19 抓到过：
    //   之前的 B18"其余行序一字未变"是绿的，因为读文件的辅助函数带 Text、把 \r 吃掉了，
    //   等于拿带滤镜的眼睛去比。TOML 解析器两种都收，所以不崩，但没理由动别人的行尾。
    //   至于原文本来就是 CRLF 的（别的工具写的）：用原风格拼回去，别让它一半 CRLF 一半 LF。
    const QByteArray out = lines.join(eol).toUtf8();
    if (!writeConfigBytes(path, out)) {
        // 写失败（磁盘满 / 被锁）也必须还原：writeConfigBytes 拒绝"只写进去一部分"，
        // 但那一部分已经在盘上了，不还原就留个半截文件。
        if (!writeConfigBytes(path, original))
            return fail(TR("写 %1 失败，而且还原也失败了 —— 备份在 %2")
                            .arg(path, backup));
        return fail(TR("写 %1 失败（磁盘满？），已还原").arg(path));
    }

    // ---- 复验一：值读回来对不对 ----
    const Quality back = read(peerId);
    if (back.imageQuality != q.imageQuality || back.customQuality != q.customQuality
        || back.fps != q.fps || back.codec != q.codec) {
        // 别说"已回滚"而没确认 —— 回滚失败的话文件是**坏的/没了**的，用户得知道
        // 备份在哪、怎么救回来，否则他只是看到一句平静的"已回滚"。
        if (!restoreFromBytes(path, original))
            return fail(TR("复验没对上，回滚也失败了 —— 备份在 %1，把它改名成 %2 就是原文件")
                            .arg(backup, path));
        return fail(TR("写完后复验没对上，已整体回滚"));
    }

    // ---- 复验二：**键落对了表没有** ----
    // 上面那条复验用的是 read()，而 read() 用的是**同一套** scanDepths 判定。
    // 判定错两次就互相抵消：文件里 `# 备注 [这里有个方括号` 这种注释会让深度回不到 0，
    // [options] 之后所有表头都认不出来，custom-fps 就被追加到文件末尾、落进最后一个表
    // （[transfer] 里）。而按同一个错判据做的复验照样读得到值、照样绿。
    // 所以这里换一把**尺子**：不查深度，只查"这个键往上最近的一个整行 [表名] 是什么"。
    {
        QByteArray after;
        if (readBytes(path, &after, nullptr)) {
            const QStringList ls  = splitLines(after);
            const QString     top = keyTable(ls, QStringLiteral("image_quality"));
            const QString     ciq = keyTable(ls, QStringLiteral("custom_image_quality"));
            const QString     fps = keyTable(ls, QStringLiteral("custom-fps"));
            const QString     cod = keyTable(ls, QStringLiteral("codec-preference"));
            // 每个键必须**恰好一次**。TOML 里同一个表内出现两个同名键是**非法**的，
            // 而 RustDesk 解析失败会把整份配置回落到默认值（连别的东西一起丢）。
            // 这条路我们自己的代码走不到（putKey 是"有就替换"），所以它一旦出现，
            // 说明写入逻辑出了新花样 —— 那正是要拦住的时候。
            const QRegularExpression once(
                QStringLiteral(R"(^[ \t]*%1[ \t]*=)").arg(QStringLiteral("image_quality")));
            const QRegularExpression once2(
                QStringLiteral(R"(^[ \t]*%1[ \t]*=)").arg(QStringLiteral("custom_image_quality")));
            const QRegularExpression once3(
                QStringLiteral(R"(^[ \t]*%1[ \t]*=)").arg(QStringLiteral("custom-fps")));
            const QRegularExpression once4(
                QStringLiteral(R"(^[ \t]*%1[ \t]*=)").arg(QStringLiteral("codec-preference")));
            int n[4] = { 0, 0, 0, 0 };
            for (const QString& l : ls) {
                if (once.match(l).hasMatch())  ++n[0];
                if (once2.match(l).hasMatch()) ++n[1];
                if (once3.match(l).hasMatch()) ++n[2];
                if (once4.match(l).hasMatch()) ++n[3];
            }
            if (n[0] != 1 || n[1] != 1 || n[2] != 1 || n[3] != 1) {
                if (!restoreFromBytes(path, original))
                    return fail(TR("四个画质键有重复的，回滚也失败了 —— 备份在 %1").arg(backup));
                return fail(TR("写完发现画质键各有 %1/%2/%3/%4 个（应该各 1 个）—— 重复键在 TOML 里非法，已整体回滚")
                                .arg(n[0]).arg(n[1]).arg(n[2]).arg(n[3]));
            }
            if (!top.isEmpty() || !ciq.isEmpty() || fps != QLatin1String("[options]")
                || cod != QLatin1String("[options]")) {
                if (!restoreFromBytes(path, original))
                    return fail(TR("键落到别的表里去了，回滚也失败了 —— 备份在 %1，把它改名成 %2 就是原文件").arg(backup, path));
                return fail(TR("键落错了表（顶层=[%1][%2]、options=[%3][%4]）—— 文件结构有我没看懂的地方，已整体回滚")
                                .arg(top, ciq, fps, cod));
            }
        }
    }
    return true;
}

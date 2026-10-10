#pragma once

#include <QString>
#include <QStringList>

// 每个对端一份的**会话设置**：%APPDATA%\RustDesk\config\peers\<对端ID>.toml
//
// ⚠ **为什么画质不能写全局 RustDesk2.toml**（这条是实测踩出来的，别再试回去）：
//   2026-10-09 做了两次独立的自连 A/B —— 先往 RustDesk2.toml 的 [options] 里写
//   image_quality='best'/custom_image_quality=85，再写 image_quality='low'/
//   custom_image_quality=10/custom-fps=15/codec-preference='vp9'。两次会话日志里的
//   `OptionMessage{image_quality, custom_image_quality, custom_fps}` 和被控端的
//   `initial quality` **逐字节相同**（30 / 0 / NotSet / 0.67），跟什么都没写一样。
//   真正的存储是**每个对端一个文件**；写这里立刻就生效（实测 low→0.5、balanced→0.67、
//   custom→1.0、best→1.5，codec-preference='vp9' 让协商里的 prefer 从 Auto 变 VP9）。
//
// ⚠ **RustDesk 完全不校验这些值**：写 `image_quality='zzzbogus'` 它照单全收，然后静默
//   按默认档（0.67）跑。所以合法性只能由外壳自己把关 —— 这就是 validate() 存在的理由。
//
// ⚠ **只认"完整"的 peers 文件**：拿一个只含 4 个画质键的极简文件去试，RustDesk 在会话
//   开始那一刻把整个文件**重置成默认**（连 mtime 都对得上）。所以这里所有写操作都要求
//   文件已存在 —— 也就是这个联系人至少被连过一次。
class PeerPrefs
{
public:
    // 成员默认值**必须**等于 defaults()：默认构造出来的 Quality 要是合法的，
    // 否则调用方漏填一个字段就会撞上 validate 的报错，而且报的是"另一个字段"的错
    // —— --qtest 第一版就栽在这上面（忘了填 codec，报的却是它的错，把
    // "文件不存在"那条断言测成了假绿）。
    struct Quality
    {
        QString imageQuality  = QStringLiteral("balanced");   // low | balanced | best | custom
        int     customQuality = 50;                           // 0..100，只有 custom 档用得上
        int     fps           = 30;                           // custom-fps
        QString codec         = QStringLiteral("auto");       // auto|h264|h265|vp9|av1|vp8
    };

    // 合法值的**唯一**来源。顺序就是界面下拉的顺序。
    static QStringList imageQualities();
    static QStringList codecs();
    static QString     imageQualityTitle(const QString& v);
    static QString     codecTitle(const QString& v);

    // RustDesk 自己的默认值（实测：balanced / 50 / 30 / auto）。
    static Quality defaults();

    // 校验：任一字段不在合法集合内（或数值越界）就返回 false 并填 err。
    // **必须调**：RustDesk 收到非法值不报错、静默回落默认档，用户会以为设置生效了。
    static bool validate(const Quality& q, QString* err = nullptr);

    static QString dirPath();
    static QString filePath(const QString& peerId);
    static bool    exists(const QString& peerId);

    // 读。文件不存在或字段缺失 → 该字段取 defaults()。读之前不校验（照原样读回来，
    // 好让界面显示"文件里此刻到底是什么"）。
    static Quality read(const QString& peerId);

    // 写。事务语义，和 RustDeskController::setServerProfile 同一条路子：
    // 校验 → 备份一次 → 全部改在内存里 → 一次写盘 → 整体复验 → 失败整体回滚。
    // 文件不存在 → 直接失败（见类注释：极简/自造的文件会被 RustDesk 重置）。
    static bool write(const QString& peerId, const Quality& q, QString* err = nullptr);

    // 测试用：把 peers 根目录指到别处（--qtest 跑在临时目录上，不碰真配置）。
    static void setDirForTest(const QString& dir);
    static void clearDirForTest();
};

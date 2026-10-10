#pragma once

#include <QAtomicInt>
#include <QObject>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVector>

class AppConfig;
class QThread;
class QTimer;

// 网络体检（M4.1）+ 连接路径探测（M4.3）。
//
// **两件不相干的事，只是都属于"网络"这一摊**：
//
//  1. 体检：DNS / TCP 延迟 / UDP 出站 / NAT 类型 / IPv6 / 自建服务器可达性。
//     探测全是**阻塞式**的（DNS、TCP connect、STUN），单次几秒。放主线程会把界面冻住，
//     所以整轮跑在**工作线程**里，结果一次性回主线程（queued 调用），途中不发任何
//     跨线程信号。这样界面上只看到一个"正在体检…"，结束后一次性刷新。
//
//  2. 路径探测：从 RustDesk 日志里读出"这次连接走的是 P2P 直连还是中继"。
//     这是 tail 日志的活，跑在主线程（跟 ConnectionWatcher 一个套路）。
//     ⚠ 逻辑是**照抄** ConnectionWatcher 的 tail 循环，不是复用它 —— 那个文件已经交付、
//       M1/M3 都靠它，为了加一条正则去改它会污染既有行为。宁可复制二十行。
class NetworkProbe : public QObject
{
    Q_OBJECT

public:
    struct Item
    {
        QString key;        // 稳定标识（测试断言用）
        QString title;      // 一行结论
        QString detail;     // 多行细节（缩进由调用方负责）
        bool    ok   = false;
        bool    hard = true;   // false = 只是个环境事实/提示，不算"没准备好"
    };

    explicit NetworkProbe(QObject* parent = nullptr);
    ~NetworkProbe() override;

    // 跑一轮体检。**异步**：立刻返回，跑完发 finished()。
    // 已经在跑就忽略这一次调用（界面按钮连点不该排出一队探测）。
    void run(const AppConfig& cfg);
    bool isRunning() const { return m_running; }

    QVector<Item> items() const { return m_items; }
    int badCount() const;      // hard 且 !ok 的项数

    // ---- 连接路径（M4.3）----
    // 开一个 tail 循环盯 RustDesk 日志，读到路径标记就更新 pathSummary 并发 pathChanged。
    void startPathWatch();
    void stopPathWatch();
    QString pathSummary() const { return m_pathSummary; }
    // 路径判定（直连 / 中继）。标签颜色一类的事按这个走，别去 contains 中文字面量 ——
    // 摘要一旦翻成英文，那种判定就静默失效了。
    bool pathIsDirect() const { return m_pathDirect; }

    // 测试用：直接喂一行日志给解析器（不起进程、不碰真日志）。
    bool feedLogLineForTest(const QString& line);
    // 测试用：把要盯的日志换成临时文件（真跑时永远看 RustDesk 自己的）。
    void setPathLogPathsForTest(const QStringList& paths);

    // 真跑时盯哪几个日志文件。公开出来是给无头验收用的 —— 它要拿**真日志里的字节**
    // 去喂解析器；只喂自己编的字符串等于自证，漏掉真实变体也发现不了。
    static QStringList rustdeskLogPaths();

signals:
    void finished();
    void pathChanged(const QString& summary, bool direct);

private:
    void setItems(const QVector<Item>& v);
    void pollPath();

    QVector<Item> m_items;
    bool          m_running = false;
    QThread*      m_thread  = nullptr;

    // 工作线程回主线程前要先看这个标志：对象如果已经被销毁，往它身上 post 事件是 UB。
    // 析构里是"置 0 → quit → wait"，wait 返回前对象还活着，所以这个检查是有效的。
    QSharedPointer<QAtomicInt> m_alive;

    struct PathTail
    {
        QString path;
        qint64  pos = -1;
    };
    QTimer*            m_pathTimer = nullptr;
    QVector<PathTail>  m_pathTails;
    QStringList        m_overridePaths;      // 非空 = 用这些（测试）
    QString            m_pathSummary;
    bool               m_pathDirect = false; // 摘要对应的判定，和 m_pathSummary 同步更新
};

#pragma once

#include <QDateTime>
#include <QMainWindow>
#include <QVector>

#include "NetworkProbe.h"
#include "SelfCheck.h"

struct Contact;

class AddressBook;
class AppConfig;
class RustDeskController;
class ConnectSession;
class PasswordPolicy;
class NotifyPolicy;

class QCheckBox;
class QComboBox;
class QDialog;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTextBrowser;
class QVBoxLayout;

// 主界面：把本机 ID 和密码摆出来，让人能念给对方 / 复制发出去。
// 密码策略（固定 / 定时轮换）由 PasswordPolicy 执行，连接提示由 NotifyPolicy 执行，
// 一次连接的编排由 ConnectSession 执行，这里只做界面。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(AppConfig* config, RustDeskController* rd, PasswordPolicy* policy,
               NotifyPolicy* notify, AddressBook* book, ConnectSession* session,
               NetworkProbe* probe, QWidget* parent = nullptr);

public slots:
    void refresh();
    void focusAddressBook();     // 托盘「连接…」用：把窗口叫出来并聚焦到地址簿

    // 主界面上现在只剩「本机」和「连接别的电脑」两组，其余全在这两个对话框里。
    // 做成 public 是为了让 main.cpp 的 --screenshot-dialog 能无头打开它们抓图 ——
    // 那四组设置搬走之后，主窗口截图就照不到它们了，不补这两张等于验收覆盖静默下降。
    void openSettingsDialog();
    void openGuideDialog(const QString& topic = QString());

    // 给 --screenshot-dialog 用：要抓的那个"整份内容"控件（设置框比屏幕还高，
    // 抓窗口那点可见区只会拍到前两组）。
    QWidget* settingsShotTarget() const;
    QWidget* guideShotTarget() const;

    // —— --paneltest 的无头钩子（照 ConnectSession 的 *ForTest 惯例）——
    //
    // 全项目只有那一段会**真的造一个 MainWindow**（不 show）来断言控件状态。
    // 为什么非造不可：纯函数能把"按钮该不该亮"的规则测得很透，但拦不住真正的回归 ——
    // **规则可以全对，错的是 MainWindow 喂给规则的输入**。实际发生的那个 bug 正是如此：
    // 地址簿空着时点「新建」，保存/连接两个按钮都该亮却全灰，因为旧代码只拿
    // "列表选没选中"这一个布尔去喂规则，把"正在填一条新的"这个状态整个丢了。
    // 那种错纯函数测试永远看不见，只有从真控件上读状态才复现得出来。
    void newContactForTest();                         // 等价于点「新建」
    void setContactFieldsForTest(const QString& id, const QString& host,
                                 const QString& pw);
    void setNetModeForTest(const QString& mode);      // 只改内存 + 重算按钮态，不写盘
    bool saveButtonEnabledForTest() const;
    bool connectButtonEnabledForTest() const;
    bool deleteButtonEnabledForTest() const;

    // 「本机」那组密码行的复制按钮。**只有读真控件才看得见这个 bug** ——
    // 用户实机反馈的是"只有显示、没有复制"，而没有任何纯函数/文案断言会注意到
    // 少了一个按钮（它不是文案，也不是规则）。而且要证明"真接上了"，
    // 必须拿真密码去撞剪贴板：断言按钮亮着只能说明它该亮。
    bool copyPasswordEnabledForTest() const;
    void copyPasswordForTest();                       // ⚠ 这一个真会改剪贴板
    void setLocalPasswordForTest(const QString& pw);  // 只改内存 + 重算显示，不写盘

    // 「现在没人能连进来」这条警告。**它只有真控件看得见** —— 判据是"横幅红没红、
    // 红的文案里有没有那句后果"，而 prepare/banner 的纯函数只算得出"缺几项"，
    // 算不出我们把它说成了什么。用户原话："密码未设置的时候为啥连接也要密码"，
    // 根子就是这条后果没人说出口。
    bool    bannerIsDangerForTest() const { return m_bannerIsDanger; }
    QString bannerTextForTest() const;                 // 横幅纯文本（含 ⚠ 前缀）
    bool    passwordHintIsWarningForTest() const;      // 「本机」密码那行的提示是不是红的
    QString passwordHintForTest() const;               // 「本机」密码那行的提示原文
    bool    policyIsOffModeForTest() const;            // 当前是不是「关闭」档（按钮那时叫「关闭固定密码」）

    // 「密码策略」那一格的控件态。**全是"只有真控件看得见"的错**：
    //   · 「未应用」小字该亮不亮 = 界面在替用户假装档位已经生效了
    //   · 「打开 RustDesk 看一次性密码」出现在别的档 = 给了一个那里并不需要的逃生口
    // setPolicyModeForTest **只改内存，绝不写盘**（setMode 会 persist()，
    // 在验收里那就是把用户真实的档位改掉 —— 跟 setLocalPasswordForTest 同一个道理）。
    void    setPolicyModeForTest(const QString& mode);
    void    restorePolicyModeForTest();                 // 把档位放回测试前的原值
    int     policyModeCountForTest() const;
    bool    notAppliedVisibleForTest() const;
    bool    openRdVisibleForTest() const;
    QString nextPolicyTextForTest() const;

    // 开机自启那个勾选框。**它必须与注册表实际状态一致** —— 控件显示"开着"而注册表里
    // 没有（或反过来），是这一项最坏的失败方式：真到机器重启那天才发现什么都没发生。
    // 不断言"默认关"（那取决于这台机器当前的状态），断言"跟注册表一致"。
    bool    autostartCheckedForTest() const;
    QString autostartHintForTest() const;

    // ⚠ 只有 --paneltest 用。伪造"配置文件里那份 password"（真值来自**本机真实的
    //   RustDesk.toml**，一定有内容，不伪造就永远测不到"一个密码都没有"这条分支）。
    //   **只影响显示判断**，不碰 RustDeskController —— 那边的 readPasswordRaw 还兼着
    //   "写完复验"的职责（rewriteTopPassword 里拿它比对刚写进去的值），伪造它等于
    //   让写校验自己判自己卷。删密码前那道确认（onApplyClicked）也照旧读真值。
    void setRawPasswordOverrideForTest(const QString& raw);
    void clearRawPasswordOverrideForTest();

    // 设置框分页 + 高度。**这三条是治"设置太复杂"的回归护栏**：
    // 原来五组一字排开，实测 580×1061，比 1080p 屏的可视高度还高 —— 底部的「关闭」
    // 和整组「界面语言」直接落在屏幕外面，而且全程没有滚动条。这种错**没有任何一条
    // 断言看得见**（截图只存 PNG 不做判断），只能靠量尺寸钉住。
    int  settingsTabCountForTest() const;
    void selectSettingsTabForTest(int idx);         // 抓图用：只切当前页，不改任何设置
    int  settingsTabMinGroupCountForTest() const;   // 最空的那一页挂了几组（0 = 有空页）
    int  settingsHeightForTest() const;             // sizeHint 高度（不受 resize 影响）
    QString routeTextForTest() const;               // 主界面那行「线路」的纯文本
    int  mainContentMinWidthForTest() const;        // 主界面内容至少要多宽（超窗口宽 = 右边被切）
    int  windowWidthForTest() const;                // 窗口当前宽度（跟上面那个比）
    QString mainWidthBreakdownForTest() const;      // 哪一组最宽（超宽时用来定位）

    // 教程左侧目录（M8 的「看哪一段」下拉改成了目录列表）
    int     guideTopicCountForTest() const;
    QString guideTopicKeyForTest(int row) const;    // 列表里第 row 项对应的主题 key
    QString guideCurrentTopicForTest() const;       // 当前选中的主题 key

signals:
    // 「立即重启」。**必须由 main.cpp 来接**：它在那边才拿得到 QLocalServer 的引用，
    // 而重启前得先把单例口让出去 —— 否则新进程一起来就探到老实例、把参数转过来自己退出，
    // 用户看到的是"点了重启，窗口闪一下没了"。
    void restartRequested();

protected:
    void closeEvent(QCloseEvent* e) override;
    bool event(QEvent* e) override;      // 只为接 LayoutRequest → 重量主界面宽度

private slots:
    void onIdChanged(const QString& id);
    void onIdRefreshFinished();
    void onStatus(const QString& msg);
    void onCopyId();
    void onCopyPassword();       // 密码旁边那个「复制」
    void onTogglePassword();

    void onModeChanged(int index);
    void onRotateHoursChanged(int hours);
    void onApplyClicked();
    void onRegenerateClicked();
    void onOpenRustDeskClicked();       // 「打开 RustDesk 看一次性密码」（那一档唯一的逃生口）
    void onPolicyPasswordChanged(const QString& plain);
    void onNextRotationChanged(const QDateTime& when);

    void onNotifyModeChanged(int index);
    void onNotifyApplyClicked();
    void onRemoteConnected();

    // ---- 连接 ----
    void onNetModeChanged(int index);
    void onTsnetConfigEdited();          // 本机主机名 / 授权密钥 改完
    void onServerConfigEdited();         // 自建服务器四个地址改完
    void onNetApplyClicked();            // 把档位写进 RustDesk2.toml
    void onNetVerifyClicked();           // 自检：界面档位 vs toml 实际值
    void onNetCheckClicked();            // 一键体检
    void onProbeFinished();
    // ⚠ 第二个参数是"这条路径是不是 P2P 直连"。**别改回"拿 summary 里找关键字判色"** ——
    //   切到英文之后 summary 里就没有「直连」两个字了，颜色会静默退回中继色（不报错、
    //   界面看着也正常，只是判断反了）。判定必须由 NetworkProbe 那边给。
    void onPathChanged(const QString& summary, bool direct);
    void onBookChanged();
    void onContactRowChanged(int row);
    void onContactEdited();              // 只用来更新按钮态，不自动保存
    void onContactNew();
    void onContactSave();
    void onContactDelete();
    void onConnectClicked();
    void onPastePeerId();                // 「对方连接码」旁边那个「粘贴」
    void onTogglePeerPassword();         // 「对方密码」的显示/隐藏
    void onDisconnectClicked();
    void onSessionPhase(int phase, const QString& text);
    void onSessionFailed(const QString& msg);

    // ---- 画质与性能（按联系人）----
    void onQualityEdited();
    void onQualityApplyClicked();

    // ---- 界面语言 ----
    void onLangChanged(int index);       // 只写盘 + 记 m_pendingLang，不动界面
    void onRestartClicked();             // 转发成 restartRequested()

    // ---- 开机自启 ----
    void onAutostartToggled(bool on);    // 真写/删注册表那条值；失败会把勾推回去

private:
    void buildUi();
    void buildSettingsDialog();      // ⚠ 必须在 buildUi() **之前**调用，理由见 .cpp
    void buildGuideDialog();
    void updatePasswordDisplay();
    void updatePolicyUi();
    void updateNotifyUi();
    void updateConnectUi();
    void updateNetUi();
    void updatePathLabel();
    void updateRouteLabel();         // 主界面那行「线路：…」
    void updateBanner();             // 前身是 updateChecklist()：7 行清单收成一行横幅
    void applyContentWidth();        // 主界面宽度按内容算（只涨不缩），见 .cpp 里的长注释
    void updateQualityUi();
    void refreshAutostartUi();       // 勾选框/hint 按**注册表实际状态**重算（不写盘）

    // 「要替换掉 RustDesk 里那个读不出明文的固定密码吗」的统一确认框。
    // 两个入口共用（设置里的「立即应用」+ 本机密码那行的接管按钮），文案里嵌 %1 = 是谁点的。
    // 返回 true 才继续。**它自己不碰任何东西**（纯确认）。
    bool confirmReplaceUnknownPassword(const QString& actionLabel);

    Contact contactFromEditors() const;
    void    loadContactIntoEditors(int row);
    void    rebuildContactList(int selectRow);

    // 当前选中联系人对应的 peers 文件名（不含 .toml）。官方档用 RustDesk ID，
    // 没有 ID 时回落到 tailnet 主机名；都没选就返回空串。
    QString selectedPeerId() const;

    AppConfig*          m_config = nullptr;
    RustDeskController* m_rd = nullptr;
    PasswordPolicy*     m_policy = nullptr;
    NotifyPolicy*       m_notify = nullptr;
    AddressBook*        m_book = nullptr;
    ConnectSession*     m_session = nullptr;
    NetworkProbe*       m_probe = nullptr;

    QLabel*      m_idLabel = nullptr;
    QLabel*      m_idHint = nullptr;
    QPushButton* m_copyBtn = nullptr;
    QPushButton* m_refreshBtn = nullptr;

    QLabel*      m_pwLabel = nullptr;
    QLabel*      m_pwHint = nullptr;
    QPushButton* m_pwToggleBtn = nullptr;
    // 连接码旁边一直有「复制」，密码旁边只有「显示」—— 用户实机反馈才发现。
    // 实话说：密码多半是念给对方听的，但"复制发过去"同样是常用路径，
    // 而且比念更不容易错。没明文可复制时（未设置 / 只有密文）置灰。
    QPushButton* m_pwCopyBtn   = nullptr;

    QComboBox*   m_modeBox = nullptr;
    QLabel*      m_hoursLabel = nullptr;   // "每" / "有效"，跟着模式变
    QSpinBox*    m_hoursBox = nullptr;
    QLabel*      m_nextLabel = nullptr;
    QPushButton* m_applyBtn = nullptr;
    QPushButton* m_regenBtn = nullptr;

    // 「立即应用」旁边那个「未应用」小字（琥珀色）。档位只写外壳配置，真正落地要走
    // 「停 → 写 → 起」；中间那段时间界面显示的档位跟机器上实际生效的东西**是两回事**。
    // 这个标记把差距明说出来，判据是纯函数 PasswordPolicy::needsApply()。
    QLabel*      m_notAppliedLabel = nullptr;

    // 「不用固定密码」那一档的唯一逃生口：RustDesk 的一次性密码只显示在它自己窗口里
    // （值不落盘、IPC 又对第三方关死），只能把它叫出来看。
    QPushButton* m_openRdBtn = nullptr;

    // 限时模式要把"还剩多久"显示出来，得自己走一个慢速心跳刷新文案
    class QTimer* m_uiTick = nullptr;

    QComboBox*   m_notifyBox = nullptr;
    QLabel*      m_notifyHint = nullptr;
    QPushButton* m_notifyApplyBtn = nullptr;

    bool m_pwVisible = false;

    // ---- 远程连接（地址簿，就地内联编辑，不做弹窗）----
    QListWidget* m_list = nullptr;
    QLineEdit*   m_cNameEdit = nullptr;
    QLineEdit*   m_cIdEdit   = nullptr;
    QLineEdit*   m_cHostEdit = nullptr;
    QLineEdit*   m_cPwEdit   = nullptr;
    QLabel*      m_cHint     = nullptr;
    QPushButton* m_cNewBtn   = nullptr;
    QPushButton* m_cSaveBtn  = nullptr;
    QPushButton* m_cDelBtn   = nullptr;
    QPushButton* m_cConnectBtn = nullptr;
    QPushButton* m_cDisconnBtn = nullptr;
    QPushButton* m_cPasteBtn   = nullptr;   // 「对方连接码」的粘贴
    QPushButton* m_cPwToggleBtn = nullptr;  // 「对方密码」的显示/隐藏
    bool         m_cPwVisible  = false;

    // ---- 画质与性能（按联系人）
    //
    // 这些值存在 %APPDATA%\RustDesk\config\peers\<对端ID>.toml，**不是** RustDesk2.toml。
    // 详见 PeerPrefs.h 的类注释（全局配置写画质完全无效，实测过两次 A/B）。
    QComboBox*   m_qualBox        = nullptr;
    QLabel*      m_qualCustomLb   = nullptr;   // "自定义值" 标签
    QSpinBox*    m_qualCustomSpin = nullptr;
    QLabel*      m_qualFpsLb      = nullptr;
    QSpinBox*    m_qualFpsSpin    = nullptr;
    QLabel*      m_qualCodecLb    = nullptr;
    QComboBox*   m_qualCodecBox   = nullptr;
    QLabel*      m_qualHint       = nullptr;
    QPushButton* m_qualApplyBtn   = nullptr;

    // ---- 网络路径（M4）----
    QComboBox*   m_netModeBox = nullptr;
    QLabel*      m_serverLabel = nullptr;   // cloudflare / custom 档才显示
    QLineEdit*   m_serverEdit  = nullptr;
    QLabel*      m_relayLabel  = nullptr;
    QLineEdit*   m_relayEdit   = nullptr;
    QLabel*      m_apiLabel    = nullptr;
    QLineEdit*   m_apiEdit     = nullptr;
    QLabel*      m_keyLabel    = nullptr;
    QLineEdit*   m_keyEdit     = nullptr;
    QLabel*      m_selfHostLabel = nullptr; // tsnet 档才显示
    QLineEdit*   m_selfHostEdit  = nullptr;
    QLabel*      m_authKeyLabel  = nullptr;
    QLineEdit*   m_authKeyEdit   = nullptr;
    QLabel*      m_netHint     = nullptr;
    QLabel*      m_netResult   = nullptr;   // 体检结果（富文本）
    QPushButton* m_netApplyBtn  = nullptr;
    QPushButton* m_netVerifyBtn = nullptr;
    QPushButton* m_netCheckBtn  = nullptr;
    QPushButton* m_netGuideBtn  = nullptr;

    // ---- 准备情况（现在只剩主界面顶部一行横幅）----
    //
    // 原来这里是 7 行清单 + 5 个「一键修」按钮，其中 3 个只是把光标挪过去，
    // 名副其实的只有 2 个。现在收成一行：缺什么写什么，给**一个**按钮去补。
    // 真源仍是 SelfCheck 那两个纯函数（--booktest 的 D 段护着它们的契约）。
    QWidget*     m_banner     = nullptr;   // 整条横幅（含边框），一项不缺时 hide()
    QLabel*      m_bannerText = nullptr;
    QPushButton* m_bannerBtn  = nullptr;
    QString      m_bannerAction;           // 按钮该干什么（SelfCheck 的 key），空 = 只能看教程
    bool         m_bannerIsDanger = false; // 横幅现在是"现在没人能连进来"那条（红）还是普通缺项（琥珀）
    bool         m_rawOverrideSet = false; // 见 setRawPasswordOverrideForTest
    QString      m_rawOverride;
    // ---- 主界面宽度（见 applyContentWidth）----
    int  m_widthNeed = 0;    // 已按内容撑到多大（只涨不缩，所以记着上次的）
    bool m_widthBusy = false;// 正在 resize —— 挡住它再引出来的 LayoutRequest
    // 见 setPolicyModeForTest：记住测试**之前**的档位，好原样放回去。
    bool         m_modeOverrideSet = false;
    QString      m_modeBeforeTest;

    // 只给**显示判断**用的"toml 里那份密码"：测试伪造过就用伪造值，否则真读。
    QString      effectiveRawPassword() const;

    // ---- 设置 / 教程对话框（modeless，MainWindow 持有）----
    //
    // 控件成员**全部留在 MainWindow**，对话框只是个容器：约 10 个 update*Ui() 和
    // 一大把 slot 都直接读写这些成员，搬进新类等于改遍每个 update 函数，
    // 而没有任何无头 UI 测试能兜住这种回归。
    QDialog*     m_settingsDlg = nullptr;
    // 设置框分三页（线路与画质 / 别人连我 / 界面）。三个布局**必须先于 buildUi() 存在** ——
    // 理由跟原来 m_settingsLay 那条一样：buildUi() 末尾的 updatePolicyUi() 等会直接
    // 解引用挂在上面的控件。分页另有一个理由：五组一字排开有 1061px 高，比屏幕还高。
    QTabWidget*  m_settingsTabs  = nullptr;
    QVBoxLayout* m_tabConnectLay = nullptr;   // 线路 + 画质
    QVBoxLayout* m_tabMineLay    = nullptr;   // 密码策略 + 有人连进来时
    QVBoxLayout* m_tabUiLay      = nullptr;   // 界面语言
    QDialog*     m_guideDlg    = nullptr;
    // 教程的目录。**原来是 QComboBox**，但四个主题的标题都很长（"自建服务器（自己的
    // VPS / frp / 端口映射）"），一个下拉里看不全，而且默认落地页本身又是一张
    // "该选哪档"的卡片列表 —— 等于同一件事让人选两遍。
    QListWidget* m_guideTopicList = nullptr;
    QTextBrowser* m_guideText  = nullptr;
    QString      m_guideTopic;   // 当前看的是哪一篇（Guide::topics() 的 key）

    // ---- 主界面那行「线路」（当前档位 + 直连/中继，以及从哪儿改）----
    //
    // 网络档位（官方 / 自建 / tailnet）原来只躺在设置里、还挂着「进阶，一般不用改」，
    // 很多人根本不知道有这个选择 —— 官方服务器慢的时候就会直接判"这软件不行"。
    // 现在把**当前走哪条**摆到主界面最上面，旁边就是入口。
    QLabel*      m_routeLabel  = nullptr;
    QPushButton* m_routeBtn    = nullptr;
    bool         m_pathKnown   = false;   // 探到过路径没有（没探到 = "还没连过"）
    bool         m_pathDirect  = false;
    QString      m_pathSummary;           // NetworkProbe 给的整句，放 tooltip

    QLabel*      m_cHostLabel = nullptr;   // 「tailnet 主机名」那行，仅 tsnet 档可见
    QWidget*     m_cHostSpacer = nullptr;  // 那一行尾部的等宽占位，跟标签/输入框一起显隐

    // ---- 界面语言（设置框最后一行）----
    //
    // 语言**不即时生效**：界面文案是 buildUi/buildSettingsDialog 里一次性写死的，
    // 真要热切换得让每个控件都接一遍 retranslate —— 那是 10 个 update*Ui() 之外
    // 又一整套并行的刷新路径，而且**一条现成的无头断言都护不住它**（现有测试全是
    // grep 中文句子，中文那句在 zh 档下永远是活的）。
    // 所以选择落盘后提示「重启后生效」，旁边给一个「立即重启」。
    QComboBox*   m_langBox     = nullptr;
    QLabel*      m_langHint    = nullptr;
    QPushButton* m_restartBtn  = nullptr;

    // ---- 开机自启（设置框「界面」页第一组）----
    // 勾选状态读的是**注册表**（Autostart::isEnabled()），不是 AppConfig 那个字段：
    // 那两处会分家（绿色包整个目录拷到另一台机器时，配置说开着而新机器注册表里没有），
    // 而"开机到底会不会起来"只由注册表说了算。
    QCheckBox*   m_autostartBox  = nullptr;
    QLabel*      m_autostartHint = nullptr;

    bool m_loadingContact = false;   // 往编辑器里灌值时挡住回调（免得把"填值"当"用户编辑"）
};

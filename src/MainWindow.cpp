#include "MainWindow.h"

#include "AddressBook.h"
#include "AppConfig.h"
#include "Autostart.h"
#include "ConnectPanel.h"
#include "ConnectSession.h"
#include "Guide.h"
#include "I18n.h"
#include "NetworkProfile.h"
#include "NotifyPolicy.h"
#include "PeerPrefs.h"
#include "PasswordPolicy.h"
#include "RustDeskController.h"
#include "TsnetBridge.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QVBoxLayout>

#include <climits>

#include <functional>

namespace {
// 把"还剩多少秒"说成人话。**必须向上取整**：整整 5 小时的码，程序启动花掉两秒之后
// 就是 17998 秒，`mins / 60` 一截断会显示成"4 小时" —— 看着像算错了，实际是被截了。
QString humanDuration(qint64 secs)
{
    if (secs <= 0)
        return TR("不到 1 分钟");

    const qint64 mins = (secs + 59) / 60;
    if (mins < 60)
        return TR("%1 分钟").arg(mins);

    const qint64 hours = mins / 60;
    const qint64 rem   = mins % 60;

    if (hours >= 24) {
        const qint64 days = hours / 24;
        const qint64 h    = hours % 24;
        return h ? TR("%1 天 %2 小时").arg(days).arg(h)
                 : TR("%1 天").arg(days);
    }
    return rem ? TR("%1 小时 %2 分").arg(hours).arg(rem)
               : TR("%1 小时").arg(hours);
}

// 「没起名字」在地址簿里是**一个中文哨兵串**（AddressBook.cpp 的 kUnnamed = "未命名"，
// 存在 JSON 里，`--booktest` 的 A8/A15 把这条契约钉死了，不能改成空串）。它既不是界面
// 文案也不是用户输入，所以不能进 TR 的翻译表 —— 显示时走 tDynamic 单独过一道。
const char* const kUnnamedZh = "未命名";

// 表单左侧那一列标签的宽度。
//
// 原来每个地方都写死一个数（60 / 76 / 84），中文下够用，英文一上来就露馅：
// "Password" 在 60px 里被截成 "Passw / ord"，"Codec" 变成 "Cod ec" —— 英文截图里
// 真截了，而且这是那种眼睛不盯着看就划过去的错（截图非空、断言全绿）。
//
// 所以改成**按当前语言量一遍这一组里最长的那个词**。同一组共用同一个返回值，
// 列还是齐的；中英各自算各自的，谁也不会被截。
int labelColumnWidth(const QStringList& texts, int minimum)
{
    const QFontMetrics fm(QApplication::font());
    int w = minimum;
    for (const QString& t : texts)
        w = qMax(w, fm.horizontalAdvance(t));
    return w + 10;   // 标签和它右边控件之间的缝
}
} // namespace

MainWindow::MainWindow(AppConfig* config, RustDeskController* rd, PasswordPolicy* policy,
                       NotifyPolicy* notify, AddressBook* book, ConnectSession* session,
                       NetworkProbe* probe, QWidget* parent)
    : QMainWindow(parent)
    , m_config(config)
    , m_rd(rd)
    , m_policy(policy)
    , m_notify(notify)
    , m_book(book)
    , m_session(session)
    , m_probe(probe)
{
    // ⚠ 顺序是硬约束：buildUi() 末尾会调 updatePolicyUi()/updateNotifyUi()/updatePathLabel()，
    // 而这三个函数直接解引用 m_modeBox / m_notifyBox / m_pathLabel 等控件 —— 它们是
    // buildSettingsDialog() 里建的。先 buildUi() 就是三个空指针解引用。
    buildSettingsDialog();
    buildGuideDialog();
    buildUi();

    connect(m_rd, &RustDeskController::idChanged, this, &MainWindow::onIdChanged);
    connect(m_rd, &RustDeskController::idRefreshFinished, this, &MainWindow::onIdRefreshFinished);
    connect(m_rd, &RustDeskController::statusMessage, this, &MainWindow::onStatus);
    if (m_policy) {
        connect(m_policy, &PasswordPolicy::passwordChanged, this,
                &MainWindow::onPolicyPasswordChanged);
        connect(m_policy, &PasswordPolicy::nextRotationChanged, this,
                &MainWindow::onNextRotationChanged);
        connect(m_policy, &PasswordPolicy::statusMessage, this, &MainWindow::onStatus);
    }
    if (m_notify) {
        connect(m_notify, &NotifyPolicy::statusMessage, this, &MainWindow::onStatus);
        connect(m_notify, &NotifyPolicy::remoteConnected, this,
                &MainWindow::onRemoteConnected);
    }
    if (m_book) {
        connect(m_book, &AddressBook::changed, this, &MainWindow::onBookChanged);
        rebuildContactList(-1);
    }
    if (m_session) {
        connect(m_session, &ConnectSession::phaseChanged, this, &MainWindow::onSessionPhase);
        connect(m_session, &ConnectSession::failed, this, &MainWindow::onSessionFailed);
        connect(m_session, &ConnectSession::sessionFinished, this,
                [this] { statusBar()->showMessage(TR("会话已结束"), 4000); });
    }

    if (m_probe) {
        connect(m_probe, &NetworkProbe::finished, this, &MainWindow::onProbeFinished);
        connect(m_probe, &NetworkProbe::pathChanged, this, &MainWindow::onPathChanged);
        // 已经探测过的结论（比如窗口是后开的）也要显示出来，不然切个窗口就丢了
        if (!m_probe->pathSummary().isEmpty())
            onPathChanged(m_probe->pathSummary(), m_probe->pathIsDirect());
    }

    updateNetUi();
    updateConnectUi();
    updateQualityUi();
    updateBanner();
}

// 设置对话框：只做一个壳（对话框 + 标签页），里面五组内容由 buildUi() 挂上来。
//
// 为什么不让 buildUi() 一并把壳也建了？因为壳必须**先于** buildUi() 存在 ——
// buildUi() 末尾的 updatePolicyUi()/updateNotifyUi() 会直接解引用
// m_modeBox / m_notifyBox 这些控件，它们现在就挂在这三个页面布局上。
//
// 为什么是标签页（原来是单页五组一字排开）：实测那一版 580×**1061**，而 1080p 屏
// 去掉任务栏只剩约 1040 —— 底部的「关闭」和整组「界面语言」落在屏幕外，且全程没有
// 滚动条。分页之后每页只有一两组，框高固定在 ~500，超屏这件事从根上没了。
void MainWindow::buildSettingsDialog()
{
    m_settingsDlg = new QDialog(this);
    m_settingsDlg->setWindowTitle(TR("PureRemote — 设置"));
    m_settingsDlg->resize(580, 520);

    auto* outer = new QVBoxLayout(m_settingsDlg);
    outer->setContentsMargins(14, 14, 14, 12);
    outer->setSpacing(10);

    m_settingsTabs = new QTabWidget(m_settingsDlg);
    outer->addWidget(m_settingsTabs, 1);

    // 三个页面先建好（空着），buildUi() 再按归属往里挂组。
    // ⚠ 顺序硬约束：本函数必须先于 buildUi() 跑完，否则那三个布局是空指针。
    const auto mkPage = [&](const QString& title) {
        auto* page = new QWidget(m_settingsTabs);
        auto* lay  = new QVBoxLayout(page);
        lay->setContentsMargins(12, 12, 12, 12);
        lay->setSpacing(12);
        m_settingsTabs->addTab(page, title);
        return lay;
    };
    // 顺序 = 打开设置第一眼看到的顺序。「连接」放第一页，因为**线路档位**必须显眼
    // （见 m_routeLabel 那段注释）—— 藏在第二页就等于没改。
    // ⚠ 这一页原来叫「连接」，跟主界面那组「连接别的电脑」撞名，用户看着像"重了一份"。
    //   它其实是**线路 + 画质**两组，跟"连谁"无关。改名即可消除误会 —— 删页不行：
    //   画质也挂在这一页上，删了它得另找地方，那是结构改动，不值当。
    m_tabConnectLay = mkPage(TR("线路与画质"));
    m_tabMineLay    = mkPage(TR("别人连我"));
    m_tabUiLay      = mkPage(TR("界面"));

    // 底部关闭按钮用**普通 QPushButton**，不用 QDialogButtonBox —— DarkTheme 的 QSS
    // 没覆盖它，用上了会是系统默认的浅色样式，跟整块深色板子对不上。
    // 它在标签页**外面**：每页各挂一个"关闭"既重复又容易只接到一个上。
    auto* stCloseRow = new QHBoxLayout();
    stCloseRow->addStretch();
    auto* stCloseBtn = new QPushButton(TR("关闭"), m_settingsDlg);
    stCloseRow->addWidget(stCloseBtn);
    stCloseRow->addStretch();
    outer->addLayout(stCloseRow);
    connect(stCloseBtn, &QPushButton::clicked, m_settingsDlg, &QDialog::hide);
}

// 图文教程对话框。正文全部来自 Guide::html()（纯函数模块，可无头断言），
// 这里只搭框、填内容、在换主题时重填。
//
// 主题选择器从**下拉框改成了左侧目录列表**。理由：四个主题全跟网络档位有关，
// 而下拉框里只看得到当前这一条 —— 用户不知道另外三条存在，也就不会去点
// （这正是"有些人不懂那个选择教程的"）。目录列表把四条同时摆出来，跟帮助手册一样。
void MainWindow::buildGuideDialog()
{
    m_guideDlg = new QDialog(this);
    m_guideDlg->setWindowTitle(TR("PureRemote — 图文教程"));
    // 比原来宽：左边多出一列目录。正文本身有表格和示意图，挤窄了会碎。
    m_guideDlg->resize(800, 720);

    auto* lay = new QVBoxLayout(m_guideDlg);
    lay->setContentsMargins(16, 16, 16, 12);
    lay->setSpacing(10);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);

    // 目录列宽按**当前语言**量最长标题（英文的 "Custom server (your own VPS / frp /
    // port forwarding)" 比中文长得多），封顶 240 —— 再宽就吃正文的地方了。
    QStringList titles;
    for (const QString& k : Guide::topics())
        titles << Guide::topicTitle(k);
    const int wToc = qMin(labelColumnWidth(titles, 150), 240);

    m_guideTopicList = new QListWidget(m_guideDlg);
    m_guideTopicList->setObjectName(QStringLiteral("guideToc"));
    m_guideTopicList->setFixedWidth(wToc);
    m_guideTopicList->setWordWrap(true);   // 标题长，换行比截断强（截断了就又看不懂了）
    m_guideTopicList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (const QString& k : Guide::topics()) {
        auto* it = new QListWidgetItem(Guide::topicTitle(k), m_guideTopicList);
        it->setData(Qt::UserRole, k);
    }
    body->addWidget(m_guideTopicList);

    m_guideText = new QTextBrowser(m_guideDlg);
    // 正文里的链接点开就交给系统浏览器。**必须开**，否则点了只是在框里跳，
    // 而且用户想看的"官方页面原文"根本出不去。
    m_guideText->setOpenExternalLinks(true);
    m_guideText->setOpenLinks(true);
    body->addWidget(m_guideText, 1);
    lay->addLayout(body, 1);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    auto* close = new QPushButton(TR("关闭"), m_guideDlg);
    btnRow->addWidget(close);
    lay->addLayout(btnRow);

    const auto showTopic = [this](int row) {
        if (!m_guideTopicList || row < 0 || row >= m_guideTopicList->count())
            return;
        const QListWidgetItem* it = m_guideTopicList->item(row);
        if (!it)
            return;
        m_guideTopic = it->data(Qt::UserRole).toString();
        m_guideText->setHtml(Guide::html(m_guideTopic));
        m_guideText->verticalScrollBar()->setValue(0);
    };
    connect(m_guideTopicList, &QListWidget::currentRowChanged, this, showTopic);
    connect(close, &QPushButton::clicked, m_guideDlg, &QDialog::hide);
    if (m_guideTopicList->count() > 0)
        m_guideTopicList->setCurrentRow(0);   // 会顺带触发一次 showTopic
}

void MainWindow::openSettingsDialog()
{
    m_settingsDlg->show();
    m_settingsDlg->raise();
    m_settingsDlg->activateWindow();
}

void MainWindow::openGuideDialog(const QString& topic)
{
    // 切到指定主题（换网络档位时从「图文教程」进来，直接落到相关那一段）
    if (!topic.isEmpty() && m_guideTopicList) {
        for (int i = 0; i < m_guideTopicList->count(); ++i) {
            const QListWidgetItem* it = m_guideTopicList->item(i);
            if (it && it->data(Qt::UserRole).toString() == topic) {
                m_guideTopicList->setCurrentRow(i);
                break;
            }
        }
    }
    m_guideDlg->show();
    m_guideDlg->raise();
    m_guideDlg->activateWindow();
}

// --screenshot-dialog 用：设置框比 1080p 屏还高，抓窗口那点可见区只会拍到前两组，
// 所以要抓的是"整份内容"控件。教程框抓 QTextBrowser 也是同理（正文比框高）。
QWidget* MainWindow::settingsShotTarget() const { return m_settingsDlg; }
QWidget* MainWindow::guideShotTarget() const    { return m_guideDlg; }

void MainWindow::buildUi()
{
    setWindowTitle(TR("PureRemote — 远程桌面"));
    // 主界面只有两组了（原来 8 组、940px 高），但要给横幅留出出现/消失的余量，
    // 不然补上缺项后窗口会跳一下。
    // ⚠ 这里的宽度只是个**起点**：buildUi() 末尾会按内容的最小宽再算一次并兜底
    //   （见那里"窗口宽度按内容算"那段）—— 写死的宽度会跟文案一起漂。
    resize(560, 660);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(18, 14, 18, 12);
    root->setSpacing(12);

    // 各组表单左侧标签列宽 —— 按当前语言量出来，别写死（理由见 labelColumnWidth）。
    const int wMyPc  = labelColumnWidth({ TR("连接码"), TR("密码") }, 60);
    const int wBook  = labelColumnWidth({ TR("名字"), TR("对方连接码"),
                                          TR("tailnet 主机名"), TR("对方密码") }, 84);
    const int wQual  = labelColumnWidth({ TR("画质"), TR("自定义值"),
                                          TR("帧率"), TR("编码器") }, 76);
    const int wNet   = labelColumnWidth({ TR("档位"), TR("信令服务器"), TR("中继服务器"),
                                          TR("API 服务器"), TR("服务器公钥"),
                                          TR("本机主机名"), TR("授权密钥") }, 76);

    // ---- 顶栏：两个入口按钮 ----
    // 设置和教程都收进对话框，主界面只剩"我的码"和"连别人"。
    auto* topRow = new QHBoxLayout();
    topRow->addStretch();
    auto* guideTopBtn = new QPushButton(TR("图文教程"), central);
    guideTopBtn->setToolTip(TR("手把手教你换网络路径，带链接和示意图"));
    auto* settingsBtn = new QPushButton(TR("⚙ 设置"), central);
    settingsBtn->setToolTip(TR("线路 / 画质 / 密码 / 连接提示 / 界面语言"));
    topRow->addWidget(guideTopBtn);
    topRow->addWidget(settingsBtn);
    root->addLayout(topRow);

    connect(guideTopBtn, &QPushButton::clicked, this,
            [this] { openGuideDialog(); });
    connect(settingsBtn, &QPushButton::clicked, this, &MainWindow::openSettingsDialog);

    // ---- 一行横幅（替代原来 7 行的「准备情况」清单）----
    // 一项不缺时整条 hide()。⚠ 横幅是 QFrame 不是 QGroupBox：它有标题栏占位，
    // 一行字用不上，还会把边距撑得很难看。
    m_banner = new QFrame(central);
    m_banner->setObjectName(QStringLiteral("banner"));
    m_banner->setStyleSheet(QStringLiteral(
        "#banner { background:#332a14; border:1px solid #6b5a2a; border-radius:4px; }"));
    auto* bnLay = new QHBoxLayout(m_banner);
    bnLay->setContentsMargins(12, 8, 12, 8);
    bnLay->setSpacing(10);

    m_bannerText = new QLabel(m_banner);
    m_bannerText->setWordWrap(true);
    bnLay->addWidget(m_bannerText, 1);

    m_bannerBtn = new QPushButton(TR("去补上"), m_banner);
    bnLay->addWidget(m_bannerBtn);
    root->addWidget(m_banner);

    connect(m_bannerBtn, &QPushButton::clicked, this, [this] {
        // 文案和动作都由 updateBanner() 定：这里只按它算出来的那个 key 派发。
        if (m_bannerAction == QLatin1String("has_id")) {
            refresh();
        } else if (m_bannerAction.isEmpty()) {
            openGuideDialog();          // 剩下的都是"补不了"的（缺内核 / 缺桥接程序）
        } else {
            // ⚠ 必须先 show() 再 setFocus()：隐藏控件上的 setFocus 是**静默**失败的。
            openSettingsDialog();
            if (m_bannerAction == QLatin1String("fixed_password"))
                m_modeBox->setFocus();
            else if (m_bannerAction == QLatin1String("tsnet_authkey"))
                m_authKeyEdit->setFocus();
            else if (m_bannerAction == QLatin1String("tsnet_hostname"))
                m_selfHostEdit->setFocus();
        }
    });

    // ---- 一行「线路」：现在走哪条，以及从哪儿改 ----
    //
    // 摆在这个位置的理由：网络档位（官方 / 自建 / tailnet）原来只躺在设置框里，
    // 标题上还写着「进阶，一般不用改」—— 很多人根本不知道有这个选择。而官方服务器
    // 慢/连不通的时候，用户只会判"这软件不行"，压根想不到是线路的事。
    // 所以把**当前走哪条**摆到主界面最上面，旁边就是入口。
    // 「直连还是中继」也一并从设置里搬了过来：那本来就是一眼该看见的状态。
    auto* routeRow = new QHBoxLayout();
    routeRow->setSpacing(10);
    m_routeLabel = new QLabel(central);
    m_routeLabel->setTextFormat(Qt::RichText);
    routeRow->addWidget(m_routeLabel);
    m_routeBtn = new QPushButton(TR("换线路…"), central);
    m_routeBtn->setToolTip(TR("官方服务器 / 自建 / tailnet —— 官方那条慢就换一条"));
    routeRow->addWidget(m_routeBtn);
    routeRow->addStretch();
    root->addLayout(routeRow);

    connect(m_routeBtn, &QPushButton::clicked, this, [this] {
        // 落到标签页第一页 —— 线路档位就在那页的最上面（详见 buildSettingsDialog）。
        openSettingsDialog();
        m_settingsTabs->setCurrentIndex(0);
        m_netModeBox->setFocus();
    });

    // ---- 本机（连接码 + 密码合并成一组）----
    // 合成一组的理由很实际：这两样是**同一个动作**（"把码给对方"）要用的两半，
    // 分开两个框反而要读两遍标题才知道该给什么。
    auto* meBox = new QGroupBox(TR("本机"), central);
    auto* meLay = new QVBoxLayout(meBox);
    meLay->setContentsMargins(16, 20, 16, 16);
    meLay->setSpacing(6);

    auto* idRow = new QHBoxLayout();
    auto* idTag = new QLabel(TR("连接码"), meBox);
    idTag->setFixedWidth(wMyPc);
    idRow->addWidget(idTag);
    // ⚠ 占位符**不带空格**。它跟真 ID 一样是 22pt + 字距 2，原来写成
    //   `— — — — — — — — —`（9 个破折号 + 8 个空格 = 17 个字符）→ 这一行光占位符就要
    //   400 多 px，"本机"整组的最小宽被顶到 657px，于是主界面出横向滚动条、
    //   最右边那列按钮被切在屏幕外（用户实机报的）。9 个破折号挨着排就够表意了。
    m_idLabel = new QLabel(QStringLiteral("—————————"), meBox);
    QFont idFont = m_idLabel->font();
    idFont.setPointSize(22);
    idFont.setBold(true);
    idFont.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
    m_idLabel->setFont(idFont);
    m_idLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    idRow->addWidget(m_idLabel, 1);
    m_copyBtn = new QPushButton(TR("复制"), meBox);
    m_refreshBtn = new QPushButton(TR("重新读取"), meBox);
    idRow->addWidget(m_copyBtn);
    idRow->addWidget(m_refreshBtn);
    meLay->addLayout(idRow);

    m_idHint = new QLabel(TR("把这个码告诉对方，他就能连过来"), meBox);
    m_idHint->setWordWrap(true);
    m_idHint->setContentsMargins(60, 0, 0, 0);   // 跟上面的行首标签对齐
    m_idHint->setStyleSheet(QStringLiteral("color:#999999; font-size:12px;"));
    meLay->addWidget(m_idHint);

    auto* meLine = new QFrame(meBox);
    meLine->setFrameShape(QFrame::HLine);
    meLine->setStyleSheet(QStringLiteral("color:#3a3a3a;"));
    meLay->addWidget(meLine);

    auto* pwRow = new QHBoxLayout();
    auto* pwTag = new QLabel(TR("密码"), meBox);
    pwTag->setFixedWidth(wMyPc);
    pwRow->addWidget(pwTag);
    m_pwLabel = new QLabel(meBox);
    QFont pwFont = m_pwLabel->font();
    pwFont.setPointSize(16);
    pwFont.setLetterSpacing(QFont::AbsoluteSpacing, 3.0);
    m_pwLabel->setFont(pwFont);
    m_pwLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pwRow->addWidget(m_pwLabel, 1);
    m_pwToggleBtn = new QPushButton(TR("显示"), meBox);
    pwRow->addWidget(m_pwToggleBtn);
    m_pwCopyBtn = new QPushButton(TR("复制"), meBox);
    m_pwCopyBtn->setToolTip(TR("把密码复制到剪贴板，直接发给对方；不用先点「显示」"));
    pwRow->addWidget(m_pwCopyBtn);
    // ⚠ 这一行原来还有一个「换成外壳能记住的新密码」的按钮（密文状态下的出口）。
    //   2026-10-10 用户实机反馈后**删掉了**：那个名字大部分人看不懂，而且它出现在
    //   **第一次打开**的时候 —— 用户只是想用软件，却被要求先理解并做一个关于凭据的
    //   选择。现在首次运行由外壳**自动接管**（见 PasswordPolicy::shouldBootstrapPassword），
    //   开箱就是"外壳管着的密码、直接能看能复制"，那个按钮再也没有出现的理由。
    meLay->addLayout(pwRow);

    m_pwHint = new QLabel(meBox);
    m_pwHint->setWordWrap(true);
    m_pwHint->setContentsMargins(60, 0, 0, 0);
    m_pwHint->setStyleSheet(QStringLiteral("color:#999999; font-size:12px;"));
    meLay->addWidget(m_pwHint);

    root->addWidget(meBox);

    connect(m_copyBtn, &QPushButton::clicked, this, &MainWindow::onCopyId);
    connect(m_refreshBtn, &QPushButton::clicked, this, &MainWindow::refresh);
    connect(m_pwToggleBtn, &QPushButton::clicked, this, &MainWindow::onTogglePassword);
    connect(m_pwCopyBtn,   &QPushButton::clicked, this, &MainWindow::onCopyPassword);

    // ================= 以下四组挂在「设置」对话框上 =================
    // 控件仍在这里建（所有控件创建集中一处便于对照），只是父窗口从 central 换成
    // m_settingsDlg、加进 m_settingsLay。⚠ 这里的顺序无所谓，但 buildSettingsDialog()
    // 必须先跑过，否则 m_settingsLay 是空的。

    // ---- 密码策略 ----
    auto* polBox = new QGroupBox(TR("密码策略"), m_settingsDlg);
    auto* polLay = new QVBoxLayout(polBox);
    polLay->setContentsMargins(16, 20, 16, 16);
    polLay->setSpacing(8);

    auto* modeRow = new QHBoxLayout();
    modeRow->addWidget(new QLabel(TR("模式"), polBox));
    m_modeBox = new QComboBox(polBox);
    // ⚠ 第 0 项以前叫「关闭 —— 只用一次性密码」。那个名字在**暗示**"只是不用固定密码，
    //   一次性密码照样能用" —— 而 RustDesk 的一次性密码只显示在它自己的窗口里，外壳
    //   读不到（IPC 对第三方关死），所以这一档的真实含义是**谁也连不进来**。
    //   用户实机就是照这个名字理解、又找不到那个"一次性密码"到底是多少，才一路查到这里。
    m_modeBox->addItem(TR("不用固定密码 —— 别人连不进来"));      // 0 = Off
    m_modeBox->addItem(TR("固定密码 —— 一直用同一个"));          // 1 = Fixed
    m_modeBox->addItem(TR("定时轮换 —— 到点换新密码"));           // 2 = Rotate
    m_modeBox->addItem(TR("限时有效 —— 到点作废"));               // 3 = Timed
    // ⚠ 第 4 项的说法要卡死在"**断开之后**才换"：用户脑子里那个"一次性密码连一次
    //   就作废"的直觉里，换的时机是"连上那一刻"，而那会把**刚连上的人掐断**
    //   （换密码必须重启 RustDesk）。写成"连过一次就换"会被读成后者。
    m_modeBox->addItem(TR("用完即换 —— 连过的人断开后换新密码")); // 4 = Once
    modeRow->addWidget(m_modeBox, 1);
    modeRow->addSpacing(12);
    m_hoursLabel = new QLabel(TR("每"), polBox);
    modeRow->addWidget(m_hoursLabel);
    m_hoursBox = new QSpinBox(polBox);
    m_hoursBox->setRange(1, 72);
    m_hoursBox->setSuffix(TR(" 小时"));
    modeRow->addWidget(m_hoursBox);
    polLay->addLayout(modeRow);

    m_nextLabel = new QLabel(polBox);
    m_nextLabel->setStyleSheet(QStringLiteral("color:#999999; font-size:12px;"));
    m_nextLabel->setWordWrap(true);
    polLay->addWidget(m_nextLabel);

    auto* applyRow = new QHBoxLayout();
    // 「未应用」：下拉里选的这一档跟 RustDesk 里**实际生效的**对不上时亮起来。
    // 为什么需要它：setMode() 只写外壳配置，真正落地要按「立即应用」（会停一次
    // RustDesk）。所以"档位显示「不用固定密码」、机器上那个固定密码却还在生效"是
    // 真会出现的状态 —— 没有这个标记，界面就是在替用户猜。
    m_notAppliedLabel = new QLabel(polBox);
    m_notAppliedLabel->setStyleSheet(
        QStringLiteral("color:#ff7b72; font-size:12px;"));
    m_notAppliedLabel->setText(TR("未应用"));
    m_notAppliedLabel->setToolTip(TR(
        "选中的这一档还没真正生效 —— 点右边的按钮才会落到 RustDesk 上。"));
    m_notAppliedLabel->setVisible(false);
    applyRow->addWidget(m_notAppliedLabel);
    applyRow->addStretch();
    m_applyBtn = new QPushButton(TR("立即应用"), polBox);
    applyRow->addWidget(m_applyBtn);
    m_regenBtn = new QPushButton(TR("换一个码"), polBox);
    m_regenBtn->setToolTip(TR(
        "立刻生成一个新密码并重新计时 —— 之前把码给出去、现在想作废就用它"));
    applyRow->addWidget(m_regenBtn);
    applyRow->addStretch();
    polLay->addLayout(applyRow);

    // 「不用固定密码」这一档的唯一逃生口：RustDesk 的一次性密码只显示在它自己的窗口里
    // （外壳读不到 —— 它的 IPC 要求对端 exe 路径完全等于 rustdesk.exe）。所以想在这一档
    // 下拿到一个能用的码，只能把 RustDesk 自己叫出来看。
    m_openRdBtn = new QPushButton(TR("打开 RustDesk 看一次性密码"), polBox);
    m_openRdBtn->setToolTip(TR(
        "RustDesk 的一次性密码只显示在它自己的窗口里，外壳读不到。"));
    m_openRdBtn->setVisible(false);
    polLay->addWidget(m_openRdBtn);
    m_tabMineLay->addWidget(polBox);          // 「别人连我」页

    connect(m_modeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onModeChanged);
    connect(m_hoursBox, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &MainWindow::onRotateHoursChanged);
    connect(m_applyBtn, &QPushButton::clicked, this, &MainWindow::onApplyClicked);
    connect(m_regenBtn, &QPushButton::clicked, this, &MainWindow::onRegenerateClicked);
    connect(m_openRdBtn, &QPushButton::clicked, this, &MainWindow::onOpenRustDeskClicked);

    // 限时模式要显示"还剩多久"，用 30 秒一跳的慢心跳刷新文案（不重算任何东西）
    m_uiTick = new QTimer(this);
    m_uiTick->setInterval(30 * 1000);
    connect(m_uiTick, &QTimer::timeout, this,
            [this] { onNextRotationChanged(m_policy ? m_policy->nextRotationAt() : QDateTime()); });
    m_uiTick->start();

    // ---- 连接提示 ----
    auto* ntBox = new QGroupBox(TR("有人连进来时"), m_settingsDlg);
    auto* ntLay = new QVBoxLayout(ntBox);
    ntLay->setContentsMargins(16, 20, 16, 16);
    ntLay->setSpacing(8);

    auto* ntRow = new QHBoxLayout();
    ntRow->addWidget(new QLabel(TR("方式"), ntBox));
    m_notifyBox = new QComboBox(ntBox);
    m_notifyBox->addItem(TR("不提示 —— 静默接受"));       // 0 = Off
    m_notifyBox->addItem(TR("托盘提示 —— 只告诉一声"));   // 1 = Toast
    m_notifyBox->addItem(TR("需要我确认 —— 不点不接受")); // 2 = Confirm
    ntRow->addWidget(m_notifyBox, 1);
    ntLay->addLayout(ntRow);

    m_notifyHint = new QLabel(ntBox);
    m_notifyHint->setStyleSheet(QStringLiteral("color:#999999; font-size:12px;"));
    m_notifyHint->setWordWrap(true);
    ntLay->addWidget(m_notifyHint);

    auto* ntApplyRow = new QHBoxLayout();
    ntApplyRow->addStretch();
    m_notifyApplyBtn = new QPushButton(TR("立即应用"), ntBox);
    ntApplyRow->addWidget(m_notifyApplyBtn);
    ntApplyRow->addStretch();
    ntLay->addLayout(ntApplyRow);
    m_tabMineLay->addWidget(ntBox);           // 「别人连我」页

    connect(m_notifyBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onNotifyModeChanged);
    connect(m_notifyApplyBtn, &QPushButton::clicked, this,
            &MainWindow::onNotifyApplyClicked);

    // ---- 连接别的电脑（地址簿）----
    //
    // 就地内联编辑，**不做 QDialog**：全项目只有 MainWindow 一个 QWidget 子类、没有对话框
    // 类先例，而弹窗要多管一份 exec/模态/生命周期，无头还更难验。就地编辑跟别处的
    // "全是内联分组"风格也一致。
    auto* cxBox = new QGroupBox(TR("连接别的电脑"), central);
    auto* cxLay = new QVBoxLayout(cxBox);
    cxLay->setContentsMargins(16, 20, 16, 16);
    cxLay->setSpacing(8);

    auto* cxIntro = new QLabel(TR(
        "对方把<b>连接码</b>念给你，填在下面的框里，点「连接」就直接连 —— 不用先存。"
        "常连的可以点「存到列表」留下来，下次从上面的列表里挑。"), cxBox);
    cxIntro->setWordWrap(true);
    cxIntro->setStyleSheet(QStringLiteral("color:#999999; font-size:12px;"));
    cxLay->addWidget(cxIntro);

    // 网络档位 + 服务器地址**不在这里**了 —— 它们搬到设置里的「网络路径」组。
    // 地址簿是"连谁"，网络路径是"从哪条路过去"，两件事混在一个组里，档位切换的
    // 影响范围就看不清了。
    m_list = new QListWidget(cxBox);
    m_list->setMinimumHeight(80);
    m_list->setMaximumHeight(132);
    cxLay->addWidget(m_list);

    // 每行尾部留一个**等宽**槽位：该放按钮的放按钮（粘贴 / 显示），不放按钮的放空占位。
    // 不等宽的话四个输入框右边缘会参差不齐，看着像没对齐的表格。
    // 宽度按当前语言量 —— 英文 "Paste"/"Show"/"Hide" 比中文两字宽，写死就会截断（M8 踩过）。
    const int wTail = labelColumnWidth({ TR("粘贴"), TR("显示"), TR("隐藏") }, 56);

    const auto tailBtn = [&](const QString& text) {
        auto* b = new QPushButton(text, cxBox);
        b->setFixedWidth(wTail);
        return b;
    };
    const auto tailPad = [&]() {
        auto* w = new QWidget(cxBox);
        w->setFixedWidth(wTail);
        return w;
    };

    // 四个字段：名字 / 连接码 / tailnet 主机名 / 对方密码。前三个里连接码和主机名
    // **两个都存**，切档位不用重填。
    // 返回标签指针是为了能整行隐藏 —— tailnet 主机名只有 tsnet 档下才有意义，
    // 其余三档露着它只会让人不知道该不该填。
    const auto addField = [&](const QString& label, QLineEdit*& out,
                              const QString& placeholder, bool password,
                              QWidget* tail) -> QLabel* {
        auto* row = new QHBoxLayout();
        auto* lb  = new QLabel(label, cxBox);
        lb->setFixedWidth(wBook);
        row->addWidget(lb);
        out = new QLineEdit(cxBox);
        out->setPlaceholderText(placeholder);
        if (password)
            out->setEchoMode(QLineEdit::Password);
        row->addWidget(out, 1);
        row->addWidget(tail);
        cxLay->addLayout(row);
        return lb;
    };
    addField(TR("名字"), m_cNameEdit,
             TR("随便起，自己能认出来就行"), false, tailPad());
    // 「粘贴」放在连接码这一行：对方的码多半是从微信/QQ 里发过来的，
    // 手抄十有八九错一位 —— 跟「本机」那组的「复制」正好对称。
    m_cPasteBtn = tailBtn(TR("粘贴"));
    addField(TR("对方连接码"), m_cIdEdit,
             TR("对方念给你的那串数字（官方档用）"), false, m_cPasteBtn);
    m_cHostSpacer = tailPad();
    m_cHostLabel = addField(TR("tailnet 主机名"), m_cHostEdit,
                            TR("对方在 tailnet 上的名字（tailnet 档用）"), false,
                            m_cHostSpacer);
    // 密码默认藏着（QLineEdit 的 Password 模式），但给一个「显示」——
    // 不给人对一下的机会，就只能靠连不上来发现"刚才打错了一位"。
    m_cPwToggleBtn = tailBtn(TR("显示"));
    addField(TR("对方密码"), m_cPwEdit,
             TR("可留空 —— 留空就让 RustDesk 自己弹窗问"), true, m_cPwToggleBtn);

    m_cHint = new QLabel(cxBox);
    m_cHint->setStyleSheet(QStringLiteral("color:#999999; font-size:12px;"));
    m_cHint->setWordWrap(true);
    cxLay->addWidget(m_cHint);

    auto* cxBtnRow = new QHBoxLayout();
    cxBtnRow->addStretch();
    m_cNewBtn  = new QPushButton(TR("新建"), cxBox);
    m_cSaveBtn = new QPushButton(TR("保存修改"), cxBox);
    m_cDelBtn  = new QPushButton(TR("删除"), cxBox);
    cxBtnRow->addWidget(m_cNewBtn);
    cxBtnRow->addWidget(m_cSaveBtn);
    cxBtnRow->addWidget(m_cDelBtn);
    cxBtnRow->addSpacing(18);
    m_cConnectBtn = new QPushButton(TR("连接"), cxBox);
    m_cDisconnBtn = new QPushButton(TR("断开"), cxBox);
    cxBtnRow->addWidget(m_cConnectBtn);
    cxBtnRow->addWidget(m_cDisconnBtn);
    cxBtnRow->addStretch();
    cxLay->addLayout(cxBtnRow);
    root->addWidget(cxBox);

    // ---- 画质与性能（按联系人）----
    //
    // ⚠ **不写 RustDesk2.toml。** 实测（2026-10-09，两次独立的自连 A/B）往全局 [options]
    //   里写 image_quality / custom_image_quality / custom-fps / codec-preference，会话
    //   日志里的参数逐字节不变，等于没写。真正生效的是**每个对端一份**的
    //   %APPDATA%\RustDesk\config\peers\<对端ID>.toml。详见 PeerPrefs.h 的类注释。
    auto* qBox = new QGroupBox(TR("画质与性能"), m_settingsDlg);
    auto* qLay = new QVBoxLayout(qBox);
    qLay->setContentsMargins(16, 20, 16, 16);
    qLay->setSpacing(8);

    auto* qIntro = new QLabel(
        TR("这些值是<b>按联系人</b>存的 —— 改的是上面选中的那一个对端，"
                       "下次连他就用这个。"), qBox);
    qIntro->setStyleSheet(QStringLiteral("color:#999999; font-size:12px;"));
    qIntro->setWordWrap(true);
    qLay->addWidget(qIntro);

    // 标签统一宽度（wQual，按语言量出来的）。返回标签是为了能整行隐藏（自定义那一行）。
    const auto addQRow = [&](const QString& text, QWidget* w) -> QLabel* {
        auto* row = new QHBoxLayout();
        auto* lb  = new QLabel(text, qBox);
        lb->setFixedWidth(wQual);
        row->addWidget(lb);
        row->addWidget(w, 1);
        qLay->addLayout(row);
        return lb;
    };

    m_qualBox = new QComboBox(qBox);
    for (const QString& v : PeerPrefs::imageQualities())
        m_qualBox->addItem(PeerPrefs::imageQualityTitle(v), v);
    addQRow(TR("画质"), m_qualBox);

    m_qualCustomSpin = new QSpinBox(qBox);
    m_qualCustomSpin->setRange(0, 100);
    m_qualCustomSpin->setSuffix(QStringLiteral(" / 100"));
    m_qualCustomLb = addQRow(TR("自定义值"), m_qualCustomSpin);

    m_qualFpsSpin = new QSpinBox(qBox);
    m_qualFpsSpin->setRange(1, 240);
    m_qualFpsSpin->setSuffix(TR(" 帧/秒"));
    m_qualFpsLb = addQRow(TR("帧率"), m_qualFpsSpin);

    m_qualCodecBox = new QComboBox(qBox);
    for (const QString& v : PeerPrefs::codecs())
        m_qualCodecBox->addItem(PeerPrefs::codecTitle(v), v);
    m_qualCodecLb = addQRow(TR("编码器"), m_qualCodecBox);

    m_qualHint = new QLabel(qBox);
    m_qualHint->setStyleSheet(QStringLiteral("color:#999999; font-size:12px;"));
    m_qualHint->setWordWrap(true);
    qLay->addWidget(m_qualHint);

    auto* qBtnRow = new QHBoxLayout();
    qBtnRow->addStretch();
    m_qualApplyBtn = new QPushButton(TR("应用到选中联系人"), qBox);
    qBtnRow->addWidget(m_qualApplyBtn);
    qBtnRow->addStretch();
    qLay->addLayout(qBtnRow);
    m_tabConnectLay->addWidget(qBox);         // 「连接」页（在线路后面）

    // 只接 activated（用户真的选了一项），**不接 currentIndexChanged** ——
    // updateQualityUi() 会 setCurrentIndex 灌值，接后者会自触发。同 updatePolicyUi 的套路。
    connect(m_qualBox, &QComboBox::activated, this, &MainWindow::onQualityEdited);
    connect(m_qualCodecBox, &QComboBox::activated, this, &MainWindow::onQualityEdited);
    connect(m_qualCustomSpin, &QSpinBox::editingFinished, this, &MainWindow::onQualityEdited);
    connect(m_qualFpsSpin, &QSpinBox::editingFinished, this, &MainWindow::onQualityEdited);
    connect(m_qualApplyBtn, &QPushButton::clicked, this, &MainWindow::onQualityApplyClicked);


    connect(m_list, &QListWidget::currentRowChanged, this, &MainWindow::onContactRowChanged);
    // 只接 editingFinished，**不接 textChanged**：那样每敲一个字母都会来回设按钮态，
    // 跟 updatePolicyUi 的规矩一样，避免信号回环。
    connect(m_cNameEdit, &QLineEdit::editingFinished, this, &MainWindow::onContactEdited);
    connect(m_cIdEdit,   &QLineEdit::editingFinished, this, &MainWindow::onContactEdited);
    connect(m_cHostEdit, &QLineEdit::editingFinished, this, &MainWindow::onContactEdited);
    connect(m_cPwEdit,   &QLineEdit::editingFinished, this, &MainWindow::onContactEdited);
    connect(m_cPasteBtn,    &QPushButton::clicked, this, &MainWindow::onPastePeerId);
    connect(m_cPwToggleBtn, &QPushButton::clicked, this, &MainWindow::onTogglePeerPassword);
    connect(m_cNewBtn,     &QPushButton::clicked, this, &MainWindow::onContactNew);
    connect(m_cSaveBtn,    &QPushButton::clicked, this, &MainWindow::onContactSave);
    connect(m_cDelBtn,     &QPushButton::clicked, this, &MainWindow::onContactDelete);
    connect(m_cConnectBtn, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(m_cDisconnBtn, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);

    // ---- 线路（M4）----
    //
    // 「走哪条路过去」是这台机器的属性，跟「连谁」（地址簿）正交，所以独立成组。
    // 四个档位**一个 combo 管到底**：服务器地址和 tailnet 身份都是它的附属输入，
    // 按档位显隐。不这么做的话就会出现"档位说 tsnet、表格里却填着自建地址"
    // 这种自相矛盾的状态 —— 而这两件事本来就必须一致。
    //
    // ⚠ 标题原来叫「网络路径（进阶，一般不用改）」。那个「进阶，一般不用改」是**劝退**：
    //    官方服务器慢/连不通的用户最需要看到的就是它，结果一看"一般不用改"就走了。
    //    改成「线路」，跟主界面那行「线路：…」用同一个词，两处能对上。
    auto* npBox = new QGroupBox(TR("线路"), m_settingsDlg);
    auto* npLay = new QVBoxLayout(npBox);
    npLay->setContentsMargins(16, 20, 16, 16);
    npLay->setSpacing(8);

    auto* nmRow = new QHBoxLayout();
    auto* nmLabel = new QLabel(TR("档位"), npBox);
    nmLabel->setFixedWidth(wNet);
    nmRow->addWidget(nmLabel);
    m_netModeBox = new QComboBox(npBox);
    // 顺序 = AppConfig::netModes()，下标就是对它的索引，别在别处再假设一遍顺序
    for (const QString& m : AppConfig::netModes())
        m_netModeBox->addItem(AppConfig::netModeTitle(m));
    nmRow->addWidget(m_netModeBox, 1);
    npLay->addLayout(nmRow);

    // 六个可选输入行，按档位显隐：tsnet 露两行、cloudflare/custom 露四行、官方一行不露。
    const auto addNetRow = [&](const QString& label, QLineEdit*& out, QLabel*& lbl,
                               const QString& placeholder, bool secret) {
        auto* row = new QHBoxLayout();
        lbl = new QLabel(label, npBox);
        lbl->setFixedWidth(wNet);
        row->addWidget(lbl);
        out = new QLineEdit(npBox);
        out->setPlaceholderText(placeholder);
        if (secret)
            out->setEchoMode(QLineEdit::Password);
        row->addWidget(out, 1);
        npLay->addLayout(row);
    };
    addNetRow(TR("信令服务器"), m_serverEdit, m_serverLabel,
              TR("hbbs 地址，host 或 host:port"), false);
    addNetRow(TR("中继服务器"), m_relayEdit, m_relayLabel,
              TR("hbbr 地址，跟信令同一台就填一样的"), false);
    addNetRow(TR("API 服务器"), m_apiEdit, m_apiLabel,
              TR("可留空（只有要网页控制台时才需要）"), false);
    addNetRow(TR("服务器公钥"), m_keyEdit, m_keyLabel,
              TR("hbbs 首次启动时打印的那串"), false);
    addNetRow(TR("本机主机名"), m_selfHostEdit, m_selfHostLabel,
              TR("这台机器在 tailnet 上的名字"), false);
    addNetRow(TR("授权密钥"), m_authKeyEdit, m_authKeyLabel,
              TR("Tailscale authkey（要 reusable、不过期）"), true);

    m_netHint = new QLabel(npBox);
    m_netHint->setStyleSheet(QStringLiteral("color:#999999; font-size:12px;"));
    m_netHint->setWordWrap(true);
    npLay->addWidget(m_netHint);

    auto* npBtnRow = new QHBoxLayout();
    npBtnRow->addStretch();
    m_netApplyBtn  = new QPushButton(TR("立即应用"), npBox);
    m_netVerifyBtn = new QPushButton(TR("自检"), npBox);
    m_netCheckBtn  = new QPushButton(TR("一键体检"), npBox);
    m_netGuideBtn  = new QPushButton(TR("图文教程"), npBox);
    npBtnRow->addWidget(m_netApplyBtn);
    npBtnRow->addWidget(m_netVerifyBtn);
    npBtnRow->addWidget(m_netCheckBtn);
    npBtnRow->addWidget(m_netGuideBtn);
    npBtnRow->addStretch();
    npLay->addLayout(npBtnRow);

    // 体检结果。用富文本(Qt::RichText)只为了给每行上色 —— 纯文本没法逐行着色。
    // ⚠ 「连接路径」原来也在这组里，已经搬到主界面 top 那行「线路」上了 —— 那是
    //    一眼就该看见的状态，压在设置框里等于没人看（而且设置框本来就比屏幕高）。
    m_netResult = new QLabel(npBox);
    m_netResult->setWordWrap(true);
    m_netResult->setTextFormat(Qt::RichText);
    m_netResult->setVisible(false);
    npLay->addWidget(m_netResult);

    // insertWidget(0)：线路档位排在**同一页的「画质」前头**。qBox 是先建的、先挂的，
    // 但先选走哪条路、再调路上的画质，这个先后顺序对用户才是对的。
    m_tabConnectLay->insertWidget(0, npBox);

    connect(m_netModeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onNetModeChanged);
    connect(m_selfHostEdit, &QLineEdit::editingFinished, this, &MainWindow::onTsnetConfigEdited);
    connect(m_authKeyEdit,  &QLineEdit::editingFinished, this, &MainWindow::onTsnetConfigEdited);
    for (QLineEdit* e : { m_serverEdit, m_relayEdit, m_apiEdit, m_keyEdit })
        connect(e, &QLineEdit::editingFinished, this, &MainWindow::onServerConfigEdited);
    connect(m_netApplyBtn,  &QPushButton::clicked, this, &MainWindow::onNetApplyClicked);
    connect(m_netVerifyBtn, &QPushButton::clicked, this, &MainWindow::onNetVerifyClicked);
    connect(m_netCheckBtn,  &QPushButton::clicked, this, &MainWindow::onNetCheckClicked);
    // 「图文教程」直接落到**当前档位**那一段（官方服务器档落到通用那篇），
    // 而不是从头翻。原来这里弹的是纯文字 QMessageBox，没有链接也没有图，名不副实。
    connect(m_netGuideBtn, &QPushButton::clicked, this, [this] {
        openGuideDialog(Guide::topicForNetMode(m_config ? m_config->netMode() : QString()));
    });

    // ---- 开机与后台 ----
    //
    // 用户点名要的：「万一人不在了，远程重启了」。绿色包没有服务，机器重启后没人
    // 去双击就是彻底连不上 —— 这一项就是那个洞的补丁。
    auto* bootBox = new QGroupBox(TR("开机与后台"), m_settingsDlg);
    auto* bootLay = new QVBoxLayout(bootBox);
    bootLay->setSpacing(8);

    m_autostartBox = new QCheckBox(TR("开机自动运行，起来只待在托盘里"), bootBox);
    bootLay->addWidget(m_autostartBox);

    m_autostartHint = new QLabel(bootBox);
    m_autostartHint->setWordWrap(true);
    m_autostartHint->setStyleSheet(QStringLiteral("color:#999999; font-size:12px;"));
    bootLay->addWidget(m_autostartHint);

    m_tabUiLay->addWidget(bootBox);           // 「界面」页
    connect(m_autostartBox, &QCheckBox::toggled, this, &MainWindow::onAutostartToggled);

    // 初值**读注册表**，不读 AppConfig 那个字段：用户可能在任务管理器的"启动"页里
    // 把它禁掉，那时候配置里还写着 true —— 界面显示"开着"就是在骗人。
    // ⚠ 这里 setChecked **必须屏蔽信号**，否则会立刻触发 onAutostartToggled 去写一遍
    //   注册表：用户只是打开设置看一眼，启动项就被重写了。
    refreshAutostartUi();

    // ---- 界面语言（放最后：它不是每次都要调的东西）----
    auto* langBox = new QGroupBox(TR("界面语言"), m_settingsDlg);
    auto* langLay = new QVBoxLayout(langBox);
    langLay->setSpacing(8);

    auto* langRow = new QHBoxLayout();
    m_langBox = new QComboBox(langBox);
    // 两个语言名**故意不翻译**：语言选择器里显示的永远是"用那种语言写的名字"，
    // 这是通行做法 —— 一个只看得懂英文的人在纯中文界面里，靠 "English" 这三个
    // 拉丁字母才找得到出口；要是翻成「英文」，他就被困住了。
    m_langBox->addItem(QStringLiteral("中文"), QStringLiteral("zh"));
    m_langBox->addItem(QStringLiteral("English"), QStringLiteral("en"));
    m_langBox->setFixedWidth(170);
    langRow->addWidget(m_langBox);
    m_restartBtn = new QPushButton(TR("立即重启"), langBox);
    m_restartBtn->setVisible(false);   // 只有真的改了、且跟正在跑的不一样才露出来
    langRow->addWidget(m_restartBtn);
    langRow->addStretch();
    langLay->addLayout(langRow);

    m_langHint = new QLabel(langBox);
    m_langHint->setWordWrap(true);
    m_langHint->setStyleSheet(QStringLiteral("color:#999999; font-size:12px;"));
    m_langHint->setText(TR("改完要重启才生效"));
    langLay->addWidget(m_langHint);

    m_tabUiLay->addWidget(langBox);           // 「界面」页
    connect(m_langBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onLangChanged);
    connect(m_restartBtn, &QPushButton::clicked, this, &MainWindow::onRestartClicked);

    // 初值从配置来。**必须屏蔽信号**：否则 setCurrentIndex 会立刻触发 onLangChanged，
    // 白白写一次盘，而且那条"改了但没重启"的提示会从一开始就挂在那儿 ——
    // 用户打开设置什么都没干，就看到「已保存，重启后生效」。
    {
        const QSignalBlocker block(m_langBox);
        const int i = m_langBox->findData(m_config ? m_config->lang() : QString());
        m_langBox->setCurrentIndex(i >= 0 ? i : 0);
    }

    // ---- 设置对话框收尾 ----
    // 每页末尾各留一个伸缩：页面比别的页矮时不至于把组拉变形，也让内容贴着顶部。
    // ⚠ 必须**在这里**加，不能在建页的时候就加 —— 那会让后面 addWidget 的组挂到
    //    伸缩后面去。关闭按钮那一行在 buildSettingsDialog() 里，不在页内。
    m_tabConnectLay->addStretch();
    m_tabMineLay->addStretch();
    m_tabUiLay->addStretch();

    root->addStretch();

    // 主界面现在只剩两组，本来用不着滚动区了；**但必须留着** —— main.cpp 的
    // --screenshot 就是靠 `qobject_cast<QScrollArea*>(win.centralWidget())` 找到内容控件再抓图的，
    // 换成直接 setCentralWidget(central) 会让那条分支静默走到别的分支上去。
    // 顺带也兜住"以后再加一组"的余量。
    auto* scroll = new QScrollArea(this);
    scroll->setWidget(central);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 视口默认铺 QPalette::Base（比窗口底色浅），会跟外面看着不是一块板 ——
    // 关掉自动填充让窗口色透上来
    scroll->viewport()->setAutoFillBackground(false);
    setCentralWidget(scroll);

    statusBar()->showMessage(TR("启动中…"));

    updatePasswordDisplay();
    updatePolicyUi();
    updateNotifyUi();
    updatePathLabel();

    applyContentWidth();
}

// 主界面宽度**按内容算，不写死**。
//
// ⚠ 横向滚动条是 ScrollBarAlwaysOff —— 内容比窗口宽**不是出滚动条，是被直接切掉**
//   （用户实机报的"最右边的部分没显示全"就是这个）。而写死一个数（原来是 560）必然
//   跟着文案漂：后来加了「换成外壳能记住的新密码」这种长按钮，英文档又普遍比中文长。
//
// ⚠ 为什么挂在 LayoutRequest 上、而不是每次改完文案手动调一遍：这个宽度取决于
//   **文案**和**某个控件亮没亮**（隐藏的控件不计入布局最小宽），而改这两样的地方
//   散在 updatePasswordDisplay / updatePolicyUi / updateConnectUi / onIdChanged …
//   好几个函数里，每个内部还有好几处 return —— 逐个塞调用点，漏掉的那个就是下次
//   "右边又被切了"。LayoutRequest 是 Qt 自己在布局失效时发的，改哪都一样会走到。
void MainWindow::applyContentWidth()
{
    auto* sa = qobject_cast<QScrollArea*>(centralWidget());
    if (!sa || !sa->widget() || m_widthBusy)
        return;
    QWidget* c = sa->widget();

    // ⚠ 量之前先把样式表套上去：DarkTheme 的 QSS 是在 polish 阶段落到控件上的，
    //   没 polish 之前量到的是**系统默认字体**下的尺寸 —— 中文差得不多看不出来，
    //   英文直接差一大截（实测同一段文案 polish 前 588、真渲染要 765）。
    c->ensurePolished();
    // 用 minimumSizeHint（要多大才能**不切**），不是 sizeHint（多宽**好看**）。
    // +20 是给右侧竖滚动条和边框留的 —— 它们会从视图口宽度里再吃掉一份。
    const int needW = c->minimumSizeHint().width() + 20;

    // 只涨不缩：文案收缩时（英文切回中文、接管后长按钮收掉）把窗口缩回去，会让用户
    // 正看着的窗口自己动一下，而且缩的那次没有任何"必须" —— 涨的那次才是（不涨就切）。
    if (needW <= m_widthNeed)
        return;
    m_widthNeed = needW;

    m_widthBusy = true;                 // resize 会再引发一次 LayoutRequest，别绕回来
    setMinimumWidth(needW);
    if (width() < needW)
        resize(needW, height());
    m_widthBusy = false;
}

bool MainWindow::event(QEvent* e)
{
    if (e->type() == QEvent::LayoutRequest)
        applyContentWidth();
    return QMainWindow::event(e);
}

void MainWindow::refresh()
{
    m_refreshBtn->setEnabled(false);
    m_idLabel->setText(TR("读取中…"));
    m_rd->refreshId();
}

void MainWindow::onIdRefreshFinished()
{
    // 收尾一律**按控制器当前的值重画**，而不是等"值变了"。
    // 点「重新读取」而 ID 没变时 idChanged 是不会发的（按名字它就只表示"值变了"），
    // 之前这里没有收尾路径 → 按钮永远灰着、大字永远是「读取中…」，只有重启才好。
    // 直接复用 onIdChanged 的渲染：它按空/非空两分支写标签和提示，正好是全部情况。
    onIdChanged(m_rd->id());
}

void MainWindow::onIdChanged(const QString& id)
{
    m_refreshBtn->setEnabled(true);

    if (id.isEmpty()) {
        m_idLabel->setText(QStringLiteral("— — — — — — — — —"));
        m_idHint->setText(TR("没读到 ID —— 检查 bin/rustdesk.exe 是否在，或点“重新读取”"));
        m_copyBtn->setEnabled(false);
        updateBanner();
        return;
    }

    m_idLabel->setText(id);
    m_idHint->setText(TR("把这个码告诉对方，他就能连过来"));
    m_copyBtn->setEnabled(true);
    updateBanner();
}

void MainWindow::onStatus(const QString& msg)
{
    statusBar()->showMessage(msg);
}

void MainWindow::onCopyId()
{
    const QString id = m_rd->id();
    if (id.isEmpty())
        return;
    QApplication::clipboard()->setText(id);
    statusBar()->showMessage(TR("已复制 ID：%1").arg(id), 3000);
}

void MainWindow::onCopyPassword()
{
    // 只复制**外壳记着的明文**。RustDesk 跑过之后可能把 toml 里的 password 加密成
    // "00"+base64，那种读不回明文 —— 要复制的是**能真正拿来连**的那串，
    // 不是文件里那串密文（密文发过去对方根本连不上）。
    const QString plain = m_config->password();
    if (plain.isEmpty()) {
        statusBar()->showMessage(TR("还没设固定密码，没有可复制的密码"), 4000);
        return;
    }
    QApplication::clipboard()->setText(plain);
    // ⚠ 状态栏**不回显密码本身**。回显等于把它摊在屏幕上，跟旁边那个「隐藏」自相矛盾。
    statusBar()->showMessage(TR("已复制密码"), 3000);
}

void MainWindow::onTogglePassword()
{
    m_pwVisible = !m_pwVisible;
    updatePasswordDisplay();
}

void MainWindow::updatePasswordDisplay()
{
    // 外壳自己记的明文优先；没有就退回"RustDesk 里到底设没设过"的判断。
    // 注意读不回明文——RustDesk 跑过之后可能把 password 字段加密了，这是设计如此。
    const QString plain = m_config->password();
    const QString raw   = effectiveRawPassword();

    // 三条分支里只有"真的没有密码"那条是警告，其余是常态。样式跟着一起切 ——
    // 留在上一条分支设的红色不放，会让人以为"已设置"也是出事了。
    const auto setHintStyle = [this](bool danger) {
        m_pwHint->setStyleSheet(danger
            ? QStringLiteral("color:#ff7b72;")
            : QString());
    };

    if (!plain.isEmpty()) {
        setHintStyle(false);
        // '•' 是 U+2022，超出 Latin-1，必须用 QChar 不能用 QLatin1Char
        m_pwLabel->setText(m_pwVisible ? plain
                                       : QString(plain.size(), QChar(0x2022)));
        m_pwHint->setText(m_policy && m_policy->mode() == PasswordPolicy::Mode::Rotate
                              ? TR("定时轮换中，到点会自动换新密码")
                          : m_policy && m_policy->mode() == PasswordPolicy::Mode::Timed
                              ? TR("限时有效中 —— 到点这个码就作废，"
                                               "作废后只能用屏幕上的临时密码")
                          : m_policy && m_policy->mode() == PasswordPolicy::Mode::Once
                              // 「用完即换」的语义要说全：换的**时机**是"人走了之后"，
                              // 不是"连上来的时候" —— 后者会把刚连上的人掐断。
                              ? TR("用完即换 —— 有人连过一次、断开之后就自动换成新的")
                              : TR("固定密码"));
        m_pwToggleBtn->setText(m_pwVisible ? TR("隐藏")
                                           : TR("显示"));
        m_pwToggleBtn->setEnabled(true);
        m_pwCopyBtn->setEnabled(true);
        return;
    }

    if (!raw.isEmpty()) {
        setHintStyle(false);
        m_pwLabel->setText(TR("已设置（外壳看不到）"));
        const auto mode = m_policy ? m_policy->mode() : PasswordPolicy::Mode::Off;
        // ⚠ 这条提示必须**跟着模式变**：那句「立即应用」只在非「关闭」模式下才存在 ——
        //   「关闭」模式下同一个按钮叫「关闭固定密码」，干的是**把密码删掉**。
        //   提示让用户去找一个当时并不存在的按钮名，他照着这一行去设置框里找，最可能
        //   点中的就是旁边那个"关掉密码"—— 正好踩上真正要防的那次事故。
        //   （用户实机那次删掉密码，根因就在这条路径上。文案指向错按钮比不写还危险。）
        //
        // ⚠ 这一行原来还挂着一个「换成外壳能记住的新密码」按钮，2026-10-10 删了：
        //   那个名字用户看不懂（"外壳能记住"是什么？），而且它出现在第一次打开的时候。
        //   现在首次运行由外壳自动接管，走到这里的是"跑过外壳之后才漂移出来"的少数情况
        //   （比如用户又拿 RustDesk 自己的窗口设了个密码）—— 出口写成**指路到「立即应用」**，
        //   那是设置里本来就有的按钮，代价（替换、原来那个作废）在这一行里直接说明白。
        const bool canTakeOver =
            m_policy && PasswordPolicy::canTakeOverPassword(mode, plain, raw);
        if (!canTakeOver) {
            m_pwHint->setText(TR("RustDesk 里已有固定密码（外壳没记明文），别人现在能连进来。"
                                 "别点「关闭固定密码」—— 那会把它删掉。"));
        } else {
            m_pwHint->setText(TR(
                // ⚠ 强调只能用 <b>。QLabel 不认 markdown —— 写 `**换成新的**` 会把星号
                //   原样画出来（本项目已踩过：assertion 全绿 + 截图非空都拦不住，只有眼睛能看见）。
                "RustDesk 里有一个固定密码，但它是加密存的，外壳看不到内容（也没法看到）。"
                "想让这里能看能复制，去「设置 → 别人连我」点<b>「立即应用」</b> —— "
                "会用外壳生成的新密码替换它，原来那个立刻失效。"));
        }
        m_pwToggleBtn->setEnabled(false);
        // 有密码，但外壳读不回明文（toml 里是加密串）→ 没有可复制的东西。
        // 这里**不能顺手复制 raw** —— 那是密文，发过去对方拿着也连不上。
        m_pwCopyBtn->setEnabled(false);
        return;
    }

    setHintStyle(true);
    m_pwLabel->setText(TR("未设置"));
    m_pwHint->setText(TR(
        "现在还连不进来 —— 没有固定密码。RustDesk 的一次性密码只显示在它自己的窗口里，"
        "外壳读不到；去「别人连我」设一个固定密码，或者在那里点「打开 RustDesk 看一次性密码」。"));
    m_pwToggleBtn->setEnabled(false);
    m_pwCopyBtn->setEnabled(false);
}

void MainWindow::updatePolicyUi()
{
    if (!m_policy)
        return;
    const auto m = m_policy->mode();
    const int  idx = (m == PasswordPolicy::Mode::Fixed)   ? 1
                     : (m == PasswordPolicy::Mode::Rotate) ? 2
                     : (m == PasswordPolicy::Mode::Timed)  ? 3
                     : (m == PasswordPolicy::Mode::Once)   ? 4
                                                           : 0;
    if (m_modeBox->currentIndex() != idx)
        m_modeBox->setCurrentIndex(idx);
    if (m_hoursBox->value() != m_policy->rotateHours())
        m_hoursBox->setValue(m_policy->rotateHours());

    const bool usesHours = (m == PasswordPolicy::Mode::Rotate || m == PasswordPolicy::Mode::Timed);
    m_hoursBox->setEnabled(usesHours);
    m_hoursLabel->setEnabled(usesHours);
    // 同一根输入框，两种模式下的读法不一样：轮换是"每 N 小时换一次"，
    // 限时是"这次设的码有效 N 小时"。
    m_hoursLabel->setText(m == PasswordPolicy::Mode::Timed ? TR("有效")
                                                           : TR("每"));
    // Rotate / Timed 有"换一个码"的意义；「用完即换」也有（这个码给错人了，想现在就换）。
    // Fixed 换掉会把已经给出去的码作废、Off 根本没有码，都不给。
    m_regenBtn->setEnabled(usesHours || m == PasswordPolicy::Mode::Once);
    m_regenBtn->setToolTip(m == PasswordPolicy::Mode::Once
        ? TR("立刻换一个新码（会断开当前的远程会话 —— RustDesk 要重启才认新密码）")
        : TR("立刻生成一个新密码并重新计时 —— 之前把码给出去、现在想作废就用它"));

    // 「未应用」：选的这一档跟 RustDesk.toml 里**实际生效的**对不上。
    // 这个标记存在的唯一理由就是：setMode() 不落地，界面不能替用户假装已经生效了。
    const bool stale = PasswordPolicy::needsApply(m, m_config->password(),
                                                  effectiveRawPassword());
    m_notAppliedLabel->setVisible(stale);

    // 「打开 RustDesk 看一次性密码」只属于「不用固定密码」这一档 —— 别的档有自己的
    // 固定密码可用，用不着去翻那个只在屏幕上飘着的临时码。
    m_openRdBtn->setVisible(m == PasswordPolicy::Mode::Off);

    // 按钮自己把后果说出来 —— 「关闭」模式下它干的是**删密码**，不是"应用设置"。
    // 这跟「存到列表 / 保存修改」是同一套写法：一个按钮在不同态下做不同的事，就别让它
    // 用同一个名字。真正危险的那一步（已经问过的那次删除）仍然会再走一遍确认框。
    m_applyBtn->setText(m == PasswordPolicy::Mode::Off ? TR("关闭固定密码")
                                                      : TR("立即应用"));

    onNextRotationChanged(m_policy->nextRotationAt());
}

void MainWindow::onModeChanged(int index)
{
    if (!m_policy)
        return;
    m_policy->setMode(index == 1 ? PasswordPolicy::Mode::Fixed
                                 : index == 2 ? PasswordPolicy::Mode::Rotate
                                 : index == 3 ? PasswordPolicy::Mode::Timed
                                 : index == 4 ? PasswordPolicy::Mode::Once
                                              : PasswordPolicy::Mode::Off);
    updatePolicyUi();
}

void MainWindow::onRotateHoursChanged(int hours)
{
    if (m_policy)
        m_policy->setRotateHours(hours);
    updatePolicyUi();
}

void MainWindow::onApplyClicked()
{
    if (!m_policy)
        return;

    // ⚠ 「关闭」模式下这个按钮做的事是**删掉本机现有的固定密码**，光看"立即应用"猜不到。
    //   这是真踩过的坑：重装/清过配置后默认模式就是「关闭」（新装的默认值），用户点一下，
    //   RustDesk.toml 里原来那个**原生加密的**固定密码就被静默删了 —— 而它在外壳眼里只是
    //   "已设置（密文）"，看着完全不像能删的东西。后果是这台机器没人看着时再也连不进来，
    //   用户那边看到的现象是"连接别的电脑 → 密码错误"（根因明明在被控端）。
    //   所以：真要删就先问一句。删是合法操作，**静默删不是**。
    if (m_policy->mode() == PasswordPolicy::Mode::Off
        && PasswordPolicy::offModeWipesExisting(m_config->password(),
                                                m_rd->readPasswordRaw())) {
        const auto ans = QMessageBox::warning(
            m_settingsDlg ? static_cast<QWidget*>(m_settingsDlg) : static_cast<QWidget*>(this),
            TR("要删掉本机的固定密码吗"),
            TR("这台机器现在是有固定密码的，「关闭」并应用会把它删掉。\n"
               "删掉之后没人看着这台机器时就没人能连进来了 —— 只剩运行时的临时密码，"
               "每次连接都得现场问一遍。\n"
               "确定要删掉吗？"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (ans != QMessageBox::Yes) {
            statusBar()->showMessage(TR("已取消，固定密码一个字节都没动"), 5000);
            return;
        }
    }

    // 非「关闭」档 + 文件里有个**外壳读不出明文**的固定密码 → 「立即应用」会把它换掉。
    // 这一条以前漏了：确认框只在「关闭」档弹，于是"点一下就把 RustDesk 原生设的固定
    // 密码悄悄换掉"是**静默**发生的 —— 上一轮那次真事故走的就是这条路径。
    // （当时界面上那一行只写着"点「立即应用」让外壳重新接管"，"接管 = 把原来那个换掉"
    //   这层意思一个字都没提。凭据被替换，不能让用户从委婉话里去猜。）
    if (m_policy->mode() != PasswordPolicy::Mode::Off
        && PasswordPolicy::canTakeOverPassword(m_policy->mode(), m_config->password(),
                                               m_rd->readPasswordRaw())
        && !confirmReplaceUnknownPassword(TR("「立即应用」"))) {
        statusBar()->showMessage(TR("已取消，RustDesk 里那个固定密码一个字节都没动"), 5000);
        return;
    }

    const bool timed = m_policy->mode() == PasswordPolicy::Mode::Timed;

    // 轮换 / 作废都要停掉 RustDesk 才能写配置，进行中的连接会断 —— 必须让用户知道
    if (m_policy->mode() != PasswordPolicy::Mode::Off && m_rd->isRunning()) {
        statusBar()->showMessage(TR("正在重启 RustDesk 以应用密码…"));
    } else {
        statusBar()->showMessage(TR("正在应用密码…"));
    }
    m_applyBtn->setEnabled(false);

    QString err;
    const bool ok = m_policy->applyNow(&err);
    m_applyBtn->setEnabled(true);

    if (ok) {
        updatePasswordDisplay();
        updatePolicyUi();
        statusBar()->showMessage(
            timed ? TR("限时密码已生效 —— 到点自动作废")
                  : TR("密码已应用"),
            4000);
    } else {
        statusBar()->showMessage(err.isEmpty() ? TR("应用失败") : err, 8000);
    }
}

void MainWindow::onRegenerateClicked()
{
    if (!m_policy)
        return;

    // ⚠ 这里以前写的是「已经连着的会话不会断（认证只在连接时做一次）」—— **那是错的**。
    //   换密码必须"停 RustDesk → 写文件 → 再拉起来"（它只在启动时读配置，而且第三方
    //   走不了它的 IPC），`stop()` 就是 taskkill /F。所以正在连的会话**一定会断**，
    //   而且断开之后对方要拿**新**密码才连得回来。话得说明白。
    if (QMessageBox::question(
            this, TR("换一个连接码"),
            TR("会立刻生成一个新密码。<br><br>"
               "<b>正在连着的远程会话会断</b> —— 换密码必须重启 RustDesk（大约几秒连不上），"
               "重新连需要用新密码。<br>"
               "旧密码从此作废，之前把码发出去的人再连就连不上了。<br><br>要继续吗？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;

    statusBar()->showMessage(TR("正在重启 RustDesk 以换新密码…"));
    m_regenBtn->setEnabled(false);

    QString err;
    const bool ok = m_policy->regenerate(&err);

    m_regenBtn->setEnabled(true);

    if (ok) {
        updatePasswordDisplay();
        updatePolicyUi();
        statusBar()->showMessage(TR("已换成新密码"), 4000);
    } else {
        statusBar()->showMessage(err.isEmpty() ? TR("换码失败") : err, 8000);
    }
}

// 「要把 RustDesk 里那个读不出明文的固定密码**换掉**」时的确认框。
// 现在只有一个入口：设置里的「立即应用」（本机那行的「换成外壳能记住的新密码」已删）。
// 返回 true = 用户可以继续。这个函数**自己不碰任何东西**。
bool MainWindow::confirmReplaceUnknownPassword(const QString& action)
{
    const auto ans = QMessageBox::warning(
        m_settingsDlg ? static_cast<QWidget*>(m_settingsDlg) : static_cast<QWidget*>(this),
        TR("要替换掉本机现在的固定密码吗"),
        // 这里是**富文本**（串里有 <b>，QMessageBox 会按 rich text 渲染）——
        // 于是 \n 不再换行，必须写成 <br>。混用 `\n` 和 `<b>` 会把整段挤成一行。
        // ⚠ 也别用 `&nbsp;` / `&bull;` 这类实体：裸 `&` 是 check_i18n.py 的致命项
        //   （Qt 把 `&x` 当助记符吃掉一个字符）。缩进用字面量空格/·就够。
        TR("RustDesk 里现在有一个固定密码。它是<b>加密存的</b>，外壳读不出明文（也没法读）。<br><br>"
           "%1会用外壳生成的新密码替换它：<br>"
           "· 新密码会显示在本机那一行，可以看、可以复制<br>"
           "· 原来那个立刻失效，手里拿着旧密码的人再也连不上<br><br>"
           "确定要继续吗？")
            .arg(action),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    return ans == QMessageBox::Yes;
}

// 「不用固定密码」那一档的唯一逃生口。
// RustDesk 的一次性密码只显示在它自己窗口里：它的值根本不落盘，只在内核进程内存里，
// 而且 IPC 通道对第三方是关死的（ipc/auth.rs 要求对端 exe 路径完全等于 rustdesk.exe）。
// 所以"想知道现在这个临时码是多少"，只有把它自己的窗口叫出来这一条路。
void MainWindow::onOpenRustDeskClicked()
{
    if (!m_rd)
        return;
    const QString exe = m_rd->cliExePath();
    if (exe.isEmpty() || !QFileInfo::exists(exe)) {
        statusBar()->showMessage(TR("找不到 RustDesk（还没解包 / 路径不对），打不开它的窗口"), 8000);
        return;
    }
    // 不带参数 = 开它自己的界面窗口（`--server` 才是无窗口的被控端）。
    if (QProcess::startDetached(exe, QStringList()))
        statusBar()->showMessage(TR("已经打开 RustDesk 窗口 —— 一次性密码显示在那里"), 6000);
    else
        statusBar()->showMessage(TR("打不开 RustDesk 窗口"), 8000);
}

void MainWindow::onPolicyPasswordChanged(const QString& plain)
{
    Q_UNUSED(plain);
    updatePasswordDisplay();
    updatePolicyUi();
    updateBanner();
}

void MainWindow::onNextRotationChanged(const QDateTime& when)
{
    if (!m_nextLabel)
        return;

    const auto m = m_policy ? m_policy->mode() : PasswordPolicy::Mode::Off;

    if (!when.isValid()) {
        switch (m) {
        case PasswordPolicy::Mode::Timed:
            m_nextLabel->setText(TR("这个码没有作废时间 —— 点「立即应用」重新计时。"));
            break;
        case PasswordPolicy::Mode::Once:
            // 这一档没有"到点"，触发点是"人走了"。把代价一并说清 —— 换密码要重启
            // RustDesk，那几秒这台机器是连不上的。
            m_nextLabel->setText(TR(
                "用完即换：有人连过一次、对方断开之后自动换新密码。"
                "换的瞬间会重启 RustDesk（大约几秒连不上），所以<b>不会</b>掐断正在连的人。"));
            break;
        case PasswordPolicy::Mode::Fixed:
            m_nextLabel->setText(TR("一直用这一个，不会自动变。"));
            break;
        case PasswordPolicy::Mode::Off:
            m_nextLabel->setText(TR("还没设固定密码 —— 别人现在连不进来。"));
            break;
        default:
            m_nextLabel->setText(TR("当前不轮换。"));
            break;
        }
        return;
    }

    const qint64 secs = QDateTime::currentDateTime().secsTo(when);

    if (m == PasswordPolicy::Mode::Timed) {
        if (secs <= 0) {
            m_nextLabel->setText(TR("已到期，正在作废…"));
        } else {
            m_nextLabel->setText(TR("这个码 %1 作废（还剩 %2）")
                                     .arg(when.toString(QStringLiteral("MM-dd HH:mm")),
                                          humanDuration(secs)));
        }
        return;
    }

    if (secs <= 0) {
        m_nextLabel->setText(TR("下次轮换：即将进行"));
    } else {
        m_nextLabel->setText(TR("下次轮换：%1（%2 后）")
                                 .arg(when.toString(QStringLiteral("MM-dd HH:mm")),
                                      humanDuration(secs)));
    }
}

void MainWindow::updateNotifyUi()
{
    if (!m_notify)
        return;
    const auto m = m_notify->mode();
    const int  idx = (m == NotifyPolicy::Mode::Toast)     ? 1
                     : (m == NotifyPolicy::Mode::Confirm) ? 2
                                                          : 0;
    if (m_notifyBox->currentIndex() != idx)
        m_notifyBox->setCurrentIndex(idx);

    switch (m) {
    case NotifyPolicy::Mode::Confirm:
        m_notifyHint->setText(TR(
            "有人连进来会弹出 RustDesk 的连接窗口，你不点“接受”就连不上。"
            "注意：这一条和“无人值守”是冲突的 —— 人不在电脑前就没人点接受。"));
        break;
    case NotifyPolicy::Mode::Toast:
        m_notifyHint->setText(TR(
            "有人连进来会在右下角弹一个气泡告诉你一声，但<b>不拦连接</b>，对方照样能进。"
            "说明：被控端的日志里没有对方的 ID，所以提示只能说“有人连进来了”，说不出是谁。"));
        break;
    case NotifyPolicy::Mode::Off:
        m_notifyHint->setText(TR(
            "对方有密码就直接连上，不弹任何东西。"));
        break;
    }
}

void MainWindow::onNotifyModeChanged(int index)
{
    if (!m_notify)
        return;
    m_notify->setMode(index == 1 ? NotifyPolicy::Mode::Toast
                                 : index == 2 ? NotifyPolicy::Mode::Confirm
                                              : NotifyPolicy::Mode::Off);
    updateNotifyUi();
}

void MainWindow::onNotifyApplyClicked()
{
    if (!m_notify)
        return;

    // 改 approve-mode 要停 RustDesk 才能写，在连的会话会断
    const bool willRestart = m_rd->isRunning();
    statusBar()->showMessage(willRestart
                                 ? TR("正在重启 RustDesk 以应用连接提示…")
                                 : TR("正在应用连接提示…"));
    m_notifyApplyBtn->setEnabled(false);

    QString err;
    const bool ok = m_notify->applyNow(&err);
    m_notifyApplyBtn->setEnabled(true);

    if (ok) {
        updateNotifyUi();
        statusBar()->showMessage(TR("连接提示已应用"), 4000);
    } else {
        statusBar()->showMessage(err.isEmpty() ? TR("应用失败") : err, 8000);
    }
}

void MainWindow::onRemoteConnected()
{
    // 界面这边只更新状态栏；气泡由 main.cpp 接到 TrayController 上弹，
    // 因为窗口可能正缩在托盘里，根本没人看状态栏。
    statusBar()->showMessage(TR("有人连进来了"), 8000);
}

// ── 远程连接 ────────────────────────────────────────────────────────────────

void MainWindow::focusAddressBook()
{
    showNormal();
    raise();
    activateWindow();
    if (m_list->currentRow() < 0 && m_list->count() > 0)
        m_list->setCurrentRow(0);
    m_list->setFocus();
}

void MainWindow::rebuildContactList(int selectRow)
{
    if (!m_book)
        return;

    const int prev = m_list->currentRow();
    m_list->blockSignals(true);
    m_list->clear();
    for (const Contact& c : m_book->contacts()) {
        QStringList bits;
        if (!c.rustdeskId.isEmpty())
            bits << c.rustdeskId;
        if (!c.tsnetHost.isEmpty())
            bits << QStringLiteral("tailnet:") + c.tsnetHost;
        // 名字走 tDynamic：英文档下「未命名」得显示成 Unnamed，否则列表里冒出四个汉字。
        // （哨兵串的解释见文件顶部 kUnnamedZh。）
        const QString shown = I18n::tDynamic(c.name);
        m_list->addItem(bits.isEmpty() ? shown
                                       : QStringLiteral("%1   %2").arg(
                                             shown, bits.join(QStringLiteral(" · "))));
    }
    // 选了哪一行尽量保持不变：地址簿一改（比如"保存修改"）就把选中跳走很难用
    const int want = (selectRow >= 0) ? selectRow : prev;
    if (want >= 0 && want < m_list->count())
        m_list->setCurrentRow(want);
    m_list->blockSignals(false);

    onContactRowChanged(m_list->currentRow());
}

void MainWindow::onBookChanged()
{
    rebuildContactList(-1);
    updateBanner();
    updateQualityUi();
}

void MainWindow::onContactRowChanged(int row)
{
    loadContactIntoEditors(row);
    updateConnectUi();
    updateQualityUi();
}

void MainWindow::loadContactIntoEditors(int row)
{
    if (!m_book || row < 0 || row >= m_book->count()) {
        m_loadingContact = true;
        m_cNameEdit->clear();
        m_cIdEdit->clear();
        m_cHostEdit->clear();
        m_cPwEdit->clear();
        m_loadingContact = false;
        return;
    }
    const Contact c = m_book->at(row);
    // 灌值前把回调挡住：设文本会触发 editingFinished 之外的东西吗？不会（我们只接了
    // editingFinished），但"保存修改"之后重灌时也走这条路，挡一下更稳。
    m_loadingContact = true;
    // 哨兵名在编辑框里显示成**空串**，不显示译文。这里放的是"待保存的值"：
    // 要是显示成 "Unnamed"，用户只是切了个语言、没改任何东西，一点「保存修改」
    // 就把 "Unnamed" 真写进地址簿了 —— 换个语言顺手改了用户数据。
    // 空串存回去会被 AddressBook 归一成哨兵，来回一趟无损。
    const QString shownName = (c.name == QLatin1String(kUnnamedZh)) ? QString() : c.name;
    if (m_cNameEdit->text() != shownName)
        m_cNameEdit->setText(shownName);
    if (m_cIdEdit->text() != c.rustdeskId)
        m_cIdEdit->setText(c.rustdeskId);
    if (m_cHostEdit->text() != c.tsnetHost)
        m_cHostEdit->setText(c.tsnetHost);
    if (m_cPwEdit->text() != c.peerPassword)
        m_cPwEdit->setText(c.peerPassword);
    m_loadingContact = false;
}

Contact MainWindow::contactFromEditors() const
{
    Contact c;
    c.name         = m_cNameEdit->text().trimmed();
    c.rustdeskId   = m_cIdEdit->text().trimmed();
    c.tsnetHost    = m_cHostEdit->text().trimmed();
    c.peerPassword = m_cPwEdit->text();      // 密码不 trim：前后空格可能是真的
    return c;
}

void MainWindow::onContactEdited()
{
    if (m_loadingContact)
        return;
    updateConnectUi();
    // ID 一改，对应的 peers 文件名就换了 —— 画质那组的可用性得跟着重算
    updateQualityUi();
}

// ── 画质与性能（按联系人）──────────────────────────────────────────────────
//
// 值是存在 %APPDATA%\RustDesk\config\peers\<对端ID>.toml 里的，**不是** RustDesk2.toml。
// 为什么（以及为什么这里要自己校验值）见 PeerPrefs.h 的类注释。

QString MainWindow::selectedPeerId() const
{
    const int row = m_list ? m_list->currentRow() : -1;
    if (!m_book || row < 0 || row >= m_book->count())
        return QString();
    const Contact c = m_book->at(row);
    // 官方档按 RustDesk ID 存；只在没有 ID 时才回落到 tailnet 主机名。
    return !c.rustdeskId.isEmpty() ? c.rustdeskId : c.tsnetHost;
}

void MainWindow::updateQualityUi()
{
    if (!m_qualBox)
        return;

    const QString peer = selectedPeerId();
    const bool    hasFile = !peer.isEmpty() && PeerPrefs::exists(peer);
    // 文件不在 = 还没连过这个对端。RustDesk 只在**第一次会话时**才建这个文件，
    // 而自己造的极简文件会被它在会话开始那一刻整个重置（实测）。所以这里只能禁用。
    const bool enable = hasFile;

    m_qualBox->setEnabled(enable);
    m_qualCustomSpin->setEnabled(enable);
    m_qualFpsSpin->setEnabled(enable);
    m_qualCodecBox->setEnabled(enable);
    m_qualApplyBtn->setEnabled(enable);

    // 没东西可读时也把控件灌成 RustDesk 自己的默认值（balanced / 50 / 30 / auto）。
    // 不灌的话禁用状态下会显示 combo 的第 0 项和 spin 的最小值 —— 也就是「省流 / 1 帧/秒」，
    // 看着像是"当前就是这么设的"，其实只是没初始化（--screenshot 抓出来过一次）。
    const PeerPrefs::Quality shown = hasFile ? PeerPrefs::read(peer) : PeerPrefs::defaults();

    // ⚠ 文件里的值可能不在我们的档位表里 —— RustDesk 自己的界面也往这个文件写
    //   （比如它内部的 'original'）。findData 找不到时**必须**回落到默认档：
    //   不回落的后果是 combo 留着**上一个联系人**的选择，用户一按「应用」就把上一个人的
    //   设置写进这一个，而且界面上看不出任何异常（静默串档）。
    QString unknown;
    const auto pick = [](QComboBox* box, const QString& v, const QString& fallback) {
        return box->findData(v) >= 0 ? box->findData(v) : box->findData(fallback);
    };
    const int iq = pick(m_qualBox, shown.imageQuality, PeerPrefs::defaults().imageQuality);
    if (iq >= 0)
        m_qualBox->setCurrentIndex(iq);
    const int cd = pick(m_qualCodecBox, shown.codec, PeerPrefs::defaults().codec);
    if (cd >= 0)
        m_qualCodecBox->setCurrentIndex(cd);
    m_qualCustomSpin->setValue(shown.customQuality);
    m_qualFpsSpin->setValue(shown.fps);

    if (hasFile && m_qualBox->findData(shown.imageQuality) < 0)
        unknown = shown.imageQuality;
    else if (hasFile && m_qualCodecBox->findData(shown.codec) < 0)
        unknown = shown.codec;

    if (peer.isEmpty()) {
        m_qualHint->setText(TR("先在上面选一个联系人。"));
    } else if (!hasFile) {
        m_qualHint->setText(
            TR("RustDesk 还没为「%1」建过设置文件 —— 它是在<b>第一次连接</b>时才建的。<br>"
                           "先连一次这个联系人，再回来调画质。上面显示的是默认值。")
                .arg(peer));
    } else if (!unknown.isEmpty()) {
        m_qualHint->setText(
            TR("⚠ 文件里存的是「%1」，不在可选的四档里（RustDesk 自己的界面也会往"
                           "这个文件写值）。上面显示的是默认档 —— 按「应用」会把它覆盖掉。")
                .arg(unknown));
    } else {
        m_qualHint->setText(TR("当前存在 %1.toml 里。改完点右边那个按钮才写盘。")
                                .arg(peer));
    }

    // 「自定义值」只在画质选了 custom 时才有意义
    const bool custom = m_qualBox->currentData().toString() == QLatin1String("custom");
    m_qualCustomLb->setVisible(custom);
    m_qualCustomSpin->setVisible(custom);
}

void MainWindow::onQualityEdited()
{
    if (m_loadingContact)
        return;
    const bool custom = m_qualBox->currentData().toString() == QLatin1String("custom");
    m_qualCustomLb->setVisible(custom);
    m_qualCustomSpin->setVisible(custom);
}

void MainWindow::onQualityApplyClicked()
{
    const QString peer = selectedPeerId();
    if (peer.isEmpty()) {
        statusBar()->showMessage(TR("先选一个联系人"), 4000);
        return;
    }

    PeerPrefs::Quality q;
    q.imageQuality  = m_qualBox->currentData().toString();
    q.customQuality = m_qualCustomSpin->value();
    q.fps           = m_qualFpsSpin->value();
    q.codec         = m_qualCodecBox->currentData().toString();

    QString err;
    if (!PeerPrefs::write(peer, q, &err)) {
        statusBar()->showMessage(err, 8000);
        return;
    }
    statusBar()->showMessage(
        TR("「%1」的画质已保存（下次连接生效）").arg(peer), 4000);
}

void MainWindow::onContactNew()
{
    if (!m_book)
        return;
    m_list->setCurrentRow(-1);
    m_loadingContact = true;
    m_cNameEdit->setText(TR("新联系人"));
    m_cIdEdit->clear();
    m_cHostEdit->clear();
    m_cPwEdit->clear();
    m_loadingContact = false;
    // 密码的"显示"状态不能漏到下一条上 —— 上一条点过「显示」的话，
    // 新的一条一上来就是明文摊在那儿的。
    m_cPwVisible = false;
    m_cPwEdit->setEchoMode(QLineEdit::Password);
    m_cPwToggleBtn->setText(TR("显示"));

    // ⚠ **必须重算一次按钮态**。这一段原来是漏的，而且是"点了新建没反应"的另一半原因：
    //   地址簿本来就空的时候，currentRow 已经是 -1，`setCurrentRow(-1)` 值没变 →
    //   QListWidget 不发 currentRowChanged → 没有任何人重算按钮，保存/连接就一直保持
    //   上一次画出来的灰。
    updateConnectUi();

    m_cNameEdit->setFocus();
    m_cNameEdit->selectAll();
    statusBar()->showMessage(TR("填上对方的连接码，点「连接」直接连；想留着就点「存到列表」"),
                             6000);
}

void MainWindow::onContactSave()
{
    if (!m_book)
        return;
    const Contact c = contactFromEditors();
    if (!c.isValid()) {
        statusBar()->showMessage(
            TR("「对方连接码」和「tailnet 主机名」至少得填一个"), 8000);
        return;
    }

    const int row = m_list->currentRow();
    bool ok = false;
    if (row >= 0 && row < m_book->count()) {
        Contact keep = m_book->at(row);
        keep.name         = c.name;
        keep.rustdeskId   = c.rustdeskId;
        keep.tsnetHost    = c.tsnetHost;
        keep.peerPassword = c.peerPassword;
        ok = m_book->update(row, keep);          // lastConnectedAt 保留
    } else {
        ok = (m_book->add(c) >= 0);
    }

    if (ok) {
        rebuildContactList(row >= 0 ? row : m_book->count() - 1);
        statusBar()->showMessage(TR("已保存"), 3000);
    } else {
        statusBar()->showMessage(TR("保存失败"), 8000);
    }
}

void MainWindow::onContactDelete()
{
    if (!m_book)
        return;
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_book->count())
        return;

    const Contact c = m_book->at(row);
    if (QMessageBox::question(
            this, TR("删除联系人"),
            TR("确定删掉「%1」吗？").arg(I18n::tDynamic(c.name).toHtmlEscaped()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;

    if (m_book->removeAt(row)) {
        rebuildContactList(row - 1 >= 0 ? row - 1 : 0);
        statusBar()->showMessage(TR("已删除"), 3000);
    }
}

void MainWindow::onConnectClicked()
{
    if (!m_session || !m_book)
        return;

    // 连的是**输入框里显示的东西**，不是"列表里选中的那条"。地址簿是"存过的常用
    // 设备"，不是连人的前置条件 —— 向日葵/ToDesk 都是填了码就能连。
    const Contact c      = contactFromEditors();
    const int     row    = m_list->currentRow();
    const bool    hasSel = (row >= 0 && row < m_book->count());

    bool matches = false;
    if (hasSel) {
        const Contact& sel = m_book->at(row);
        matches = (sel.rustdeskId   == c.rustdeskId
                   && sel.tsnetHost == c.tsnetHost
                   && sel.peerPassword == c.peerPassword);
    }

    switch (ConnectPanel::sourceFor(!c.targetFor(m_config->netMode()).isEmpty(),
                                    hasSel, matches)) {
    case ConnectPanel::Source::None:
        statusBar()->showMessage(
            m_config->netMode() == QLatin1String("tsnet")
                ? TR("先填对方的 tailnet 主机名")
                : TR("先填对方的连接码"),
            6000);
        return;
    case ConnectPanel::Source::Saved:
        // 跟选中那条一字不差 → 走地址簿的路，这样才会记「上次连接」。
        m_session->connectTo(row);
        break;
    case ConnectPanel::Source::Adhoc:
        // 改过了 / 压根没存过 → 直接连这份临时目标，不往地址簿里塞。
        m_session->connectToAdhoc(c);
        break;
    }

    statusBar()->showMessage(m_session->phaseText());
    updateConnectUi();
}

// 「对方连接码」的粘贴。对方的码通常是微信/QQ 里发过来的，手抄容易错一位。
void MainWindow::onPastePeerId()
{
    const QString text = QApplication::clipboard()->text().trimmed();
    if (text.isEmpty()) {
        statusBar()->showMessage(TR("剪贴板里没有文字"), 4000);
        return;
    }
    m_cIdEdit->setText(text);
    // setText 不发 editingFinished，按钮态得手动重算一次（真实敲键盘走 onContactEdited）
    onContactEdited();
    statusBar()->showMessage(TR("已粘贴连接码"), 3000);
}

// 「对方密码」的显示 / 隐藏。不给人对一下的机会，就只能靠"连不上"来发现打错了一位。
void MainWindow::onTogglePeerPassword()
{
    m_cPwVisible = !m_cPwVisible;
    m_cPwEdit->setEchoMode(m_cPwVisible ? QLineEdit::Normal : QLineEdit::Password);
    m_cPwToggleBtn->setText(m_cPwVisible ? TR("隐藏") : TR("显示"));
}

void MainWindow::onDisconnectClicked()
{
    if (m_session)
        m_session->disconnectNow();
    updateConnectUi();
}

void MainWindow::onSessionPhase(int phase, const QString& text)
{
    Q_UNUSED(phase)
    if (!text.isEmpty())
        statusBar()->showMessage(text);
    updateConnectUi();
}

void MainWindow::onSessionFailed(const QString& msg)
{
    statusBar()->showMessage(msg, 8000);
    updateConnectUi();
}

void MainWindow::onNetModeChanged(int index)
{
    // 下标就是对 netModes() 的索引。**不要再写 `index == 1 ? "tsnet" : "id"` 这种**——
    // 四档之后这么写会把 cloudflare/custom 静默当成官方档。
    const QStringList modes = AppConfig::netModes();
    if (index < 0 || index >= modes.size())
        return;
    if (m_config && m_config->netMode() != modes.at(index)) {
        m_config->setNetMode(modes.at(index));
        m_config->save();
    }
    updateNetUi();
    updateConnectUi();
    updateQualityUi();
    updateBanner();
    // 档位从 tsnet 换走 / 换到 tsnet，常驻桥接的生死跟着变（main 那边处理）
    emit networkSettingsChanged();
}

// 自建服务器四个框：改完落盘（跟 tsnet 那两个一样，是**本机**的属性不是联系人的）
void MainWindow::onServerConfigEdited()
{
    if (!m_config)
        return;
    bool dirty = false;
    auto sync = [&dirty](const QString& want, const QString& cur,
                         std::function<void(const QString&)> set) {
        const QString v = want.trimmed();
        if (cur != v) {
            set(v);
            dirty = true;
        }
    };
    sync(m_serverEdit->text(), m_config->customRendezvous(),
         [this](const QString& v) { m_config->setCustomRendezvous(v); });
    sync(m_relayEdit->text(), m_config->relayServer(),
         [this](const QString& v) { m_config->setRelayServer(v); });
    sync(m_apiEdit->text(), m_config->apiServer(),
         [this](const QString& v) { m_config->setApiServer(v); });
    sync(m_keyEdit->text(), m_config->publicKey(),
         [this](const QString& v) { m_config->setPublicKey(v); });
    if (dirty) {
        m_config->save();
        updateNetUi();
    }
}

void MainWindow::onNetApplyClicked()
{
    if (!m_config || !m_rd)
        return;
    // 界面上刚敲完还没失焦的值也要算数，否则"填完直接点应用"会丢掉最后一次输入
    onServerConfigEdited();
    onTsnetConfigEdited();

    QString err;
    if (NetworkProfile::apply(m_config->netMode(), *m_config, m_rd, &err)) {
        statusBar()->showMessage(
            TR("已切换到「%1」，RustDesk 配置写好了")
                .arg(AppConfig::netModeTitle(m_config->netMode())), 4000);
        updateNetUi();
        // 主机名 / 授权密钥也在这条路上落盘 —— 常驻桥接要按新值重启（main 那边处理）
        emit networkSettingsChanged();
    } else {
        statusBar()->showMessage(TR("没写成：%1").arg(err), 8000);
    }
}

void MainWindow::onNetVerifyClicked()
{
    if (!m_config || !m_rd)
        return;
    const QStringList bad = NetworkProfile::mismatches(m_config->netMode(), *m_config, m_rd);
    if (bad.isEmpty()) {
        statusBar()->showMessage(
            TR("自检通过 —— RustDesk 配置跟界面上的档位一致"), 4000);
    } else {
        statusBar()->showMessage(
            TR("有 %1 处对不上（点“立即应用”重写一遍）：%2")
                .arg(bad.size()).arg(bad.first()), 8000);
    }
}

void MainWindow::onNetCheckClicked()
{
    if (!m_config || !m_probe)
        return;
    if (m_probe->isRunning()) {
        statusBar()->showMessage(TR("还在体检中…"), 3000);
        return;
    }
    m_netResult->setVisible(true);
    m_netResult->setText(TR("<span style='color:#999999;'>正在体检"
                                        "（DNS / 延迟 / UDP / NAT / IPv6，最长约 1 分钟）…</span>"));
    m_netCheckBtn->setEnabled(false);
    statusBar()->showMessage(TR("开始网络体检…"), 4000);
    m_probe->run(*m_config);
}

void MainWindow::onProbeFinished()
{
    m_netCheckBtn->setEnabled(true);
    if (!m_probe)
        return;

    QStringList html;
    int         hard = 0;
    for (const NetworkProbe::Item& it : m_probe->items()) {
        // 「注意」是软项：环境事实（IPv6 不可用 / 官方中继端口不通），不是故障。
        // 全标红会让人以为网络坏了，其实产品照样能用。
        const char* color = it.ok ? "#4caf50" : (it.hard ? "#ff5555" : "#e0a030");
        const char* mark  = it.ok ? "✔" : (it.hard ? "✘" : "•");
        if (it.hard && !it.ok)
            ++hard;
        html << QStringLiteral("<div style='color:%1;'><b>%2 %3</b></div>")
                    .arg(QLatin1String(color), QLatin1String(mark), it.title.toHtmlEscaped());
        for (const QString& ln : it.detail.split(QLatin1Char('\n'))) {
            if (!ln.isEmpty())
                // ⚠ 这串**不翻**：里面唯一的非 ASCII 是全角空格（缩进），不是文案。
                //   它被自动包装工具扫进来过 —— 判断"这串要不要翻"看的是有没有中文，
                //   而全角空格也算汉字区。别再加 TR。
                html << QStringLiteral("<div style='color:#999999; font-size:11px;'>　%1</div>")
                            .arg(ln.toHtmlEscaped());
        }
    }
    m_netResult->setText(html.join(QString()));
    m_netResult->setVisible(true);

    statusBar()->showMessage(
        hard == 0 ? TR("体检完成 —— 没有硬性问题，可以用")
                  : TR("体检完成 —— 有 %1 项会影响远程，看下面红色的").arg(hard),
        hard == 0 ? 4000 : 8000);
}

void MainWindow::onPathChanged(const QString& summary, bool direct)
{
    // ⚠ 颜色和"直连/中继"两个字只认传进来的 direct，**不要改成"在 summary 里找
    //   「直连」两个字"**。英文档下 summary 是英文的（NetworkProbe 给的文案会过 t()），
    //   那么找会把中继错判成直连，而且**没有任何断言会发现**：界面照常渲染、
    //   只是颜色和文案反了。判定由 NetworkProbe 那边给，这里只负责画。
    m_pathKnown   = true;
    m_pathDirect  = direct;
    m_pathSummary = summary;
    updateRouteLabel();
}

// 按当前档位决定哪些输入行露出来。**从配置读真值**，别再自己存一份界面状态
// （跟 updatePolicyUi 一个套路：模型是唯一的真，界面只是投影）。
void MainWindow::updateNetUi()
{
    if (!m_config || !m_netModeBox)
        return;

    const QString mode    = m_config->netMode();
    const bool    tsnet   = (mode == QLatin1String("tsnet"));
    const bool    selfHst = (mode == QLatin1String("cloudflare")
                             || mode == QLatin1String("custom"));

    auto rowVisible = [](QLabel* lbl, QLineEdit* edit, bool on) {
        lbl->setVisible(on);
        edit->setVisible(on);
    };
    rowVisible(m_serverLabel, m_serverEdit, selfHst);
    rowVisible(m_relayLabel,  m_relayEdit,  selfHst);
    rowVisible(m_apiLabel,    m_apiEdit,    selfHst);
    rowVisible(m_keyLabel,    m_keyEdit,    selfHst);
    rowVisible(m_selfHostLabel, m_selfHostEdit, tsnet);
    rowVisible(m_authKeyLabel,  m_authKeyEdit,  tsnet);

    // combo 只在真的不一样时才设，免得来回触发 currentIndexChanged
    const int wantIdx = AppConfig::netModes().indexOf(mode);
    if (wantIdx >= 0 && m_netModeBox->currentIndex() != wantIdx)
        m_netModeBox->setCurrentIndex(wantIdx);

    // 输入框显示配置真值（不显示的话每次切档都得重敲）
    auto syncText = [](QLineEdit* e, const QString& v) {
        if (e->text() != v)
            e->setText(v);
    };
    syncText(m_serverEdit,  m_config->customRendezvous());
    syncText(m_relayEdit,   m_config->relayServer());
    syncText(m_apiEdit,     m_config->apiServer());
    syncText(m_keyEdit,     m_config->publicKey());
    syncText(m_selfHostEdit, m_config->tsnetHostname());
    syncText(m_authKeyEdit,  m_config->tsnetAuthKey());

    // 一行说明 —— 长版引导在「图文教程」里，这里只回答"我现在该做什么"
    QString hint;
    if (tsnet)
        hint = TR("两台机器组成一张私有网，RustDesk 的流量从里面走。"
                              "本机主机名两台必须不一样。");
    else if (mode == QLatin1String("cloudflare"))
        hint = TR("自己搭的 hbbs/hbbr，走 Cloudflare Workers。会自动打开 websocket。");
    else if (mode == QLatin1String("custom"))
        hint = TR("自己搭的 hbbs/hbbr，有真 TCP 端口（VPS / frp / 端口映射）。");
    else
        hint = TR("用 RustDesk 官方服务器，什么都不用配。"
                              "先点「一键体检」看看信令通不通。");
    m_netHint->setText(hint);

    m_netApplyBtn->setEnabled(m_rd != nullptr);
    m_netVerifyBtn->setEnabled(m_rd != nullptr);

    // 档位一改，主界面那行「线路」要跟着变 —— 它显示的正是这里读出来的档位。
    updateRouteLabel();
}

// 主界面那行「线路：<档位> · <直连/中继>」。**唯一的绘制点** ——
// 档位变（updateNetUi）和路径变（onPathChanged）都从这里走，别各写各的。
void MainWindow::updateRouteLabel()
{
    if (!m_routeLabel || !m_config)
        return;

    const QString mode = AppConfig::netModeTitle(m_config->netMode()).toHtmlEscaped();

    // ⚠ 颜色那层 HTML **不进 TR()** —— TR 只包住给人看的那几个字。
    //   把 <span> 写进翻译键的话，一是 check_i18n.py 会把它当成一个"表里没有的键"
    //   直接判死，二是翻译的人一不小心改坏标签，整行就渲染成源码。
    const auto paint = [](const QString& color, const QString& text) {
        return QStringLiteral("<span style='color:%1;'>%2</span>")
            .arg(color, text.toHtmlEscaped());
    };
    const QString tail =
        m_pathKnown ? paint(QLatin1String(m_pathDirect ? "#4caf50" : "#e0a030"),
                            m_pathDirect ? TR("直连") : TR("中继"))
                    : paint(QLatin1String("#888888"), TR("还没连过"));

    m_routeLabel->setText(TR("线路：%1").arg(mode) + QStringLiteral(" · ") + tail);
    // 整句原文（NetworkProbe 给的）挂 tooltip：主界面只要一个能一眼扫到的状态，
    // 想追细节的人悬停就能看到，不必为它多占一行。
    m_routeLabel->setToolTip(m_pathKnown ? m_pathSummary
                                         : TR("还没连过 —— 连上之后这里会显示走的是 P2P 直连还是中继。"));
}

// 兼容旧名。原来它只管设置框里那行灰字；现在那个位置搬到主界面了。
void MainWindow::updatePathLabel()
{
    m_pathKnown = false;
    m_pathSummary.clear();
    updateRouteLabel();
}


void MainWindow::onTsnetConfigEdited()
{
    if (!m_config)
        return;
    // tsnet 档那两个输入框是**本机身份**，不是每条联系人的字段，所以改完直接落盘
    const QString host = m_selfHostEdit->text().trimmed();
    const QString key  = m_authKeyEdit->text();
    bool dirty = false;
    if (m_config->tsnetHostname() != host) { m_config->setTsnetHostname(host); dirty = true; }
    if (m_config->tsnetAuthKey()  != key)  { m_config->setTsnetAuthKey(key);   dirty = true; }
    if (dirty) {
        m_config->save();
        updateBanner();
    }
}

void MainWindow::updateConnectUi()
{
    if (!m_config || !m_book || !m_list)
        return;

    const bool tsnet = m_config->netMode() == QLatin1String("tsnet");

    // 地址簿空着就把列表藏掉：一个空的黑框摆在那儿只会让人以为界面没加载出来，
    // 而且它会占掉一百多像素，把下面真正要填的东西挤出屏幕。
    m_list->setVisible(m_book->count() > 0);

    // tailnet 主机名那一行只有 tsnet 档才有意义，其余三档藏着 ——
    // 露着它会让第一次用的人停下来想"这格到底要不要填"。
    // ⚠ 藏的是**标签、输入框、尾占位三个**：少藏一个，那一行就会留下一个没头没尾的
    //   空白块，行高还在，看着像排版坏了。
    if (m_cHostLabel && m_cHostEdit) {
        m_cHostLabel->setVisible(tsnet);
        m_cHostEdit->setVisible(tsnet);
        if (m_cHostSpacer)
            m_cHostSpacer->setVisible(tsnet);
    }

    // 判断依据是**输入框里现在是什么**，不是"列表里选中的那条"。
    // 语义是"所见即所连"：用户既然动手改了码，能连的就该是他改完的那个。
    const Contact draft = contactFromEditors();

    ConnectPanel::State st;
    st.bookCount     = m_book->count();
    st.selectedRow   = m_list->currentRow();
    st.editorValid   = draft.isValid();
    st.targetFilled  = !draft.targetFor(m_config->netMode()).isEmpty();
    st.sessionActive = m_session && m_session->isActive();

    const ConnectPanel::Buttons b     = ConnectPanel::buttons(st);
    const bool                 hasSel = st.selectedRow >= 0 && st.selectedRow < m_book->count();

    // 文案跟着态走：新建态说「存到列表」，选中一条时说「保存修改」。
    // 两者**使能条件完全一样**（都只看输入框里有没有东西可存），差的只是用户此刻在
    // 干什么：新加一条 vs 改一条已有的。老版在新建态下摆着一个写着「保存修改」的灰
    // 按钮，用户心里想的是"我什么都没选，改什么？"——压根想不到那才是存的入口。
    m_cSaveBtn->setText(ConnectPanel::isNewMode(st) ? TR("存到列表") : TR("保存修改"));

    m_cSaveBtn->setEnabled(b.save);
    m_cDelBtn->setEnabled(b.del);
    m_cConnectBtn->setEnabled(b.connect);
    m_cDisconnBtn->setEnabled(b.disconnect);

    if (!hasSel) {
        m_cHint->setText(st.targetFilled
                             ? TR("点「连接」直接连。密码留空的话，RustDesk 会自己弹窗问。")
                             : TR("填上对方的连接码就能连 —— 想留着下次用，再点「存到列表」。"));
        return;
    }

    const Contact c      = m_book->at(st.selectedRow);
    const QString target = c.targetFor(m_config->netMode());

    QString extra;
    if (!c.lastConnectedAt.isValid()) {
        extra = TR("没连过。");
    } else {
        extra = TR("上次连接：%1。")
                    .arg(c.lastConnectedAt.toString(QStringLiteral("MM-dd HH:mm")));
    }

    if (target.isEmpty()) {
        m_cHint->setText(TR("这条在「%1」档下没有地址 —— %2")
                             .arg(tsnet ? QStringLiteral("tailnet") : TR("官方 ID"),
                                  tsnet ? TR("填一下它的 tailnet 主机名")
                                        : TR("填一下它的对方连接码")));
        return;
    }

    m_cHint->setText(TR("会连到 %1。%2%3")
                         .arg(target, extra,
                              c.peerPassword.isEmpty()
                                  ? TR(" 没填对方密码，RustDesk 会自己弹窗问。")
                                  : QString()));
}

// 顶部那一行横幅。原来这里是 7 行清单 + 5 个「一键修」按钮；现在缺什么写什么，
// 只在**第一项该补的**上面给一个按钮。真源依旧是 SelfCheck 的两个纯函数
// （--booktest 的 D 段护着它们），这里只负责把结论画出来。
void MainWindow::updateBanner()
{
    if (!m_banner)
        return;   // 还没建界面时（理论上不会）别炸

    SelfCheck::Inputs in;
    if (m_rd) {
        in.rustdeskPresent  = !m_rd->cliExePath().isEmpty();
        in.hasId            = !m_rd->id().isEmpty();
        in.fixedPasswordSet = !effectiveRawPassword().isEmpty();
    }
    in.relayPresent = !TsnetBridge::findRelayExe().isEmpty();
    if (m_config) {
        in.tsnetAuthKeySet  = !m_config->tsnetAuthKey().isEmpty();
        in.tsnetHostnameSet = !m_config->tsnetHostname().isEmpty();
    }
    // 地址簿**不参与**准备情况 —— 连接框现在是"填码直接连"，一条联系人都没有
    // 照样能连上任何一台机器。详见 SelfCheck.h 里那段说明。

    const QVector<SelfCheck::Item> items = SelfCheck::evaluate(in);
    const bool tsnet = m_config && m_config->netMode() == QLatin1String("tsnet");

    // 「没有固定密码」跟别的缺项**不是一类**：别的只是"某个功能暂时用不了"，
    // 这一项直接等于"这台机器现在谁都连不进来"。混在"还差 N 项：…"里根本读不出后果
    // —— 用户实机就是这么被绕进去的：连不上，回头去看密码那栏才发现是空的。
    //
    // 判据跟密码显示那栏**同一套**（外壳明文 + toml 里那份，两边都空才算没有），
    // 不复用 SelfCheck 的 fixed_password 项 —— 那个只看 toml 里那份，外壳刚设完
    // 还没落地时会误报。
    const bool noPassword = m_config && m_rd
                            && m_config->password().isEmpty()
                            && effectiveRawPassword().isEmpty();

    QString text;
    if (noPassword) {
        // 别的缺项照旧列出来，但**把「已设固定密码」那一条摘掉** —— 它已经单独喊过一遍了，
        // 再在"还差 N 项"里数一次，同一件事就在一行里出现两遍。
        QVector<SelfCheck::Item> rest = items;
        rest.removeIf([](const SelfCheck::Item& i) {
            return i.key == QLatin1String("fixed_password");
        });
        const QString restText = SelfCheck::bannerText(rest, tsnet);
        text = TR("现在没人能连进来：本机还没有固定密码 —— RustDesk 的一次性密码"
                  "在这个界面上看不到。点右边的「去设置」设一个（会自动替你生成）。");
        if (!restText.isEmpty())
            text += QLatin1Char(' ') + restText;
    } else {
        text = SelfCheck::bannerText(items, tsnet);
    }

    m_banner->setVisible(!text.isEmpty());
    if (text.isEmpty()) {
        m_bannerAction.clear();
        m_bannerIsDanger = false;
        return;
    }

    // 颜色跟着严重程度走：琥珀色 = "还差几项，补上更好用"；红色 = "现在真的连不进来"。
    m_banner->setStyleSheet(noPassword
        ? QStringLiteral("#banner { background:#3a1a1a; border:1px solid #8a3a3a;"
                         " border-radius:4px; }")
        : QStringLiteral("#banner { background:#332a14; border:1px solid #6b5a2a;"
                         " border-radius:4px; }"));
    m_bannerIsDanger = noPassword;

    m_bannerText->setText(QStringLiteral("⚠ ") + text);

    // 按钮跟着"第一项该补的"变文案。补不了的（比如内核文件根本不在）就指向教程 ——
    // 那类问题看一眼文档比在界面上乱点有用。
    m_bannerAction = SelfCheck::firstFixableAction(items, tsnet);

    // ⚠ 没有固定密码时**必须**把按钮钉到密码那一项上：上面那句红字明说了"点右边的
    //   「去设置」"，而 firstFixableAction 按清单顺序走，会先返回更靠前的 has_id，
    //   按钮就变成「重新读取」—— 文案指着右边、右边却是另一件事，比不写还糟。
    if (noPassword)
        m_bannerAction = QStringLiteral("fixed_password");

    if (m_bannerAction == QLatin1String("has_id"))
        m_bannerBtn->setText(TR("重新读取"));
    else if (m_bannerAction.isEmpty())
        m_bannerBtn->setText(TR("看教程"));
    else
        m_bannerBtn->setText(TR("去设置"));
}

// ---- 界面语言 ----
//
// 语言**不热切换**：所有文案都是在 buildUi()/buildSettingsDialog() 里 setText 死的，
// 真要在原地换语言，得给每个控件再写一遍赋值 —— 那是第二条并行的刷新路径，而现有
// 无头断言**一条都护不住它**（它们全是 grep 中文句子，中文在 zh 档下永远还在）。
// 所以这里只负责"存下来 + 说清楚要重启"，重启由 main.cpp 执行。
// ---- 开机自启 ----

void MainWindow::refreshAutostartUi()
{
    if (!m_autostartBox)
        return;

    const bool on = Autostart::isEnabled();
    {
        // ⚠ 必须屏蔽信号。不屏蔽的话 setChecked 会立刻走一遍 onAutostartToggled，
        //   于是"打开设置看一眼"就等于把启动项重写一次（甚至把用户在任务管理器里
        //   禁掉的那条又给装回来）。
        const QSignalBlocker block(m_autostartBox);
        m_autostartBox->setChecked(on);
    }

    // 文案必须说实话：写的是 HKCU 的 Run 键，它在**登录之后**才跑 —— 机器上设了
    // 登录密码的话，没人登录之前照样连不进来。这句不说清，用户会以为"开机就能连"。
    m_autostartHint->setText(on
        ? TR("已经开着：系统<b>登录之后</b>会自动起来，只留一个托盘图标，窗口不会弹出来。"
             "（如果开机要输密码才能进桌面，在你登录之前仍然是连不上的。）"
             "关掉这一项会把注册表里那条删掉。")
        : TR("关着。打开之后：系统<b>登录之后</b>会自动起来，只留一个托盘图标，"
             "窗口不会弹出来。（如果开机要输密码才能进桌面，在你登录之前仍然是连不上的。）"));
}

void MainWindow::onAutostartToggled(bool on)
{
    const QString exe = QCoreApplication::applicationFilePath();

    QString err;
    const bool ok = on ? Autostart::enable(exe, &err) : Autostart::disable(&err);

    if (!ok) {
        // 失败必须把勾**推回去**。"控件显示开着、注册表里没有"这种状态最坏 ——
        // 用户以为设好了，真到机器重启那天才发现什么都没发生，而且没人能问他。
        refreshAutostartUi();
        m_autostartHint->setText(TR("没设置成功：%1").arg(err));
        statusBar()->showMessage(TR("设置开机自启失败"), 8000);
        return;
    }

    m_config->setAutostart(on);
    m_config->save();

    refreshAutostartUi();
    statusBar()->showMessage(on ? TR("已设为开机自动运行（下次登录后生效）")
                                : TR("已取消开机自动运行"),
                             5000);
}

void MainWindow::onLangChanged(int index)
{
    if (!m_config || !m_langBox || index < 0)
        return;

    const QString code = m_langBox->itemData(index).toString();
    if (code.isEmpty() || code == m_config->lang())
        return;

    m_config->setLang(code);
    m_config->save();

    // 判"要不要重启"比的是**正在跑的那个语言**（I18n::code()），不是配置里存的那个：
    // 用户 en→zh→en 来回切一圈最后停回当前语言，就不该还在喊「重启后生效」。
    const bool pending = (code != I18n::code());
    m_restartBtn->setVisible(pending);
    // 提示要**指明换成哪种语言**：只说"重启后生效"，用户重启完发现还是原来那套
    // （比如以为点一下按钮就换了，没注意还有个「立即重启」）就不知道哪步漏了。
    if (!pending)
        m_langHint->setText(TR("改完要重启才生效"));
    else if (code == QLatin1String("en"))
        m_langHint->setText(TR("重启后才能换成英文。点「立即重启」，软件会自己重开。"));
    else
        m_langHint->setText(TR("重启后才能换回中文。点「立即重启」，软件会自己重开。"));
}

void MainWindow::onRestartClicked()
{
    // 真正要干的事（放掉单例口、起新进程、退出）在 main.cpp —— 那边才拿得到 QLocalServer。
    emit restartRequested();
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    // 被控端要一直挂着，关窗口默认只收进托盘
    if (m_config->closeToTray() && QSystemTrayIcon::isSystemTrayAvailable()) {
        e->ignore();
        // ⚠ 对话框是**独立的顶层窗口**，hide() 主窗口不会带着它们一起走 ——
        // 不收起来的话，托盘模式下屏幕上会孤零零飘着设置/教程框，主窗口却没了。
        if (m_settingsDlg) m_settingsDlg->hide();
        if (m_guideDlg)    m_guideDlg->hide();
        hide();
        statusBar()->showMessage(TR("已最小化到托盘，仍在后台待连"));
        return;
    }
    e->accept();
    QApplication::quit();
}

// ── --paneltest 的无头钩子 ──────────────────────────────────────────────────
//
// 这几个函数**只在 --paneltest 里用**，存在的理由见 MainWindow.h 里那段注释：
// 按钮使能这类回归，纯函数测试看不见 —— 规则可以全对，错的是喂进规则的输入。
//
// 刻意薄到没有逻辑：钩子自己一旦有逻辑，就变成"测试测的是钩子"而不是"测的是界面"。

void MainWindow::newContactForTest()
{
    onContactNew();
}

void MainWindow::setContactFieldsForTest(const QString& id, const QString& host,
                                         const QString& pw)
{
    m_cIdEdit->setText(id);
    m_cHostEdit->setText(host);
    m_cPwEdit->setText(pw);
    // ⚠ QLineEdit::setText 发的是 textChanged，**不是** editingFinished ——
    //   真实用户敲完键盘走的是 onContactEdited()，这里必须手动补一次，
    //   否则测的是"没人重算过按钮态"的那个状态，什么都证明不了。
    onContactEdited();
}

void MainWindow::setNetModeForTest(const QString& mode)
{
    // **只改内存，不 save()** —— 写盘是这一段测试唯一可能污染用户配置的路径。
    // 也不走 updateNetUi()：那个函数是从下拉框反推的，这里要的只是"档位变了之后
    // 按钮态跟着变"，而 updateConnectUi() 直接读 m_config->netMode()。
    m_config->setNetMode(mode);
    updateConnectUi();
    updateRouteLabel();   // 纯重绘，不写盘；让 "档位变了主界面那行跟着变" 可断言
}

bool MainWindow::saveButtonEnabledForTest() const    { return m_cSaveBtn->isEnabled(); }
bool MainWindow::connectButtonEnabledForTest() const { return m_cConnectBtn->isEnabled(); }
bool MainWindow::deleteButtonEnabledForTest() const  { return m_cDelBtn->isEnabled(); }

bool MainWindow::copyPasswordEnabledForTest() const  { return m_pwCopyBtn->isEnabled(); }
void MainWindow::copyPasswordForTest()               { onCopyPassword(); }

void MainWindow::setLocalPasswordForTest(const QString& pw)
{
    // 只动内存里那份 AppConfig —— **绝不 save()**。这一个测试唯一的写盘风险就是把
    // 测试密码灌进用户的 config/puremote.json，那正是项目里踩过的那个坑。
    m_config->setPassword(pw);
    updatePasswordDisplay();
    updateBanner();          // 密码变了，顶部那条的严重程度也跟着变 —— 一起重算
}

QString MainWindow::effectiveRawPassword() const
{
    return m_rawOverrideSet ? m_rawOverride : m_rd->readPasswordRaw();
}

void MainWindow::setRawPasswordOverrideForTest(const QString& raw)
{
    m_rawOverrideSet = true;
    m_rawOverride    = raw;
    updatePasswordDisplay();
    updateBanner();
}

void MainWindow::clearRawPasswordOverrideForTest()
{
    m_rawOverrideSet = false;
    m_rawOverride.clear();
    updatePasswordDisplay();
    updateBanner();
}

QString MainWindow::bannerTextForTest() const
{
    return m_bannerText ? m_bannerText->text() : QString();
}

bool MainWindow::passwordHintIsWarningForTest() const
{
    return m_pwHint && m_pwHint->styleSheet().contains(QLatin1String("#ff7b72"));
}

QString MainWindow::passwordHintForTest() const
{
    return m_pwHint ? m_pwHint->text() : QString();
}

bool MainWindow::policyIsOffModeForTest() const
{
    return m_policy && m_policy->mode() == PasswordPolicy::Mode::Off;
}

// ---- 密码策略那一格的控件态（--paneltest）----

void MainWindow::setPolicyModeForTest(const QString& mode)
{
    // ⚠ 直接写 AppConfig 的内存值，**不走 PasswordPolicy::setMode** —— 后者会
    //   persist() 落盘，在验收里等于把用户真实的档位改掉（还顺带 syncWatcher 动定时器）。
    //   这里要的只是"让界面以为档位是它"，然后看控件怎么反应。
    if (!m_modeOverrideSet) {           // 只在头一次记原值，后面的调用不许把它覆盖掉
        m_modeBeforeTest = m_config->passwordMode();
        m_modeOverrideSet = true;
    }
    m_config->setPasswordMode(mode);
    updatePolicyUi();
    updatePasswordDisplay();
    updateBanner();
}

void MainWindow::restorePolicyModeForTest()
{
    if (!m_modeOverrideSet)
        return;
    m_config->setPasswordMode(m_modeBeforeTest);
    m_modeOverrideSet = false;
    updatePolicyUi();
    updatePasswordDisplay();
    updateBanner();
}

int MainWindow::policyModeCountForTest() const
{
    return m_modeBox ? m_modeBox->count() : 0;
}

bool MainWindow::notAppliedVisibleForTest() const
{
    return m_notAppliedLabel && m_notAppliedLabel->isVisibleTo(
        const_cast<QWidget*>(static_cast<const QWidget*>(m_notAppliedLabel->parentWidget())));
}

bool MainWindow::openRdVisibleForTest() const
{
    return m_openRdBtn && m_openRdBtn->isVisibleTo(
        const_cast<QWidget*>(static_cast<const QWidget*>(m_openRdBtn->parentWidget())));
}

QString MainWindow::nextPolicyTextForTest() const
{
    return m_nextLabel ? m_nextLabel->text() : QString();
}

bool MainWindow::autostartCheckedForTest() const
{
    return m_autostartBox && m_autostartBox->isChecked();
}

QString MainWindow::autostartHintForTest() const
{
    return m_autostartHint ? m_autostartHint->text() : QString();
}

// ---- 设置框分页 / 高度 ----

int MainWindow::settingsTabCountForTest() const
{
    return m_settingsTabs ? m_settingsTabs->count() : 0;
}

void MainWindow::selectSettingsTabForTest(int idx)
{
    // 分页之后，抓图只拍得到当前那一页 —— 「开机与后台」在「界面」页，
    // 不给它一个切页钩子就等于没验（M8 那个 `Password` 被截成
    // `Passw / ord` 的坑，就是"断言全绿 + 截图非空"都拦不住的那类）。
    if (m_settingsTabs && idx >= 0 && idx < m_settingsTabs->count())
        m_settingsTabs->setCurrentIndex(idx);
}

int MainWindow::settingsTabMinGroupCountForTest() const
{
    if (!m_settingsTabs || m_settingsTabs->count() == 0)
        return 0;
    int minGroups = INT_MAX;
    for (int i = 0; i < m_settingsTabs->count(); ++i) {
        QWidget* page = m_settingsTabs->widget(i);
        // 挂了几**组**：组是 QGroupBox，末尾那个 addStretch 是 QSpacerItem，不算。
        int n = 0;
        if (page) {
            if (auto* lay = qobject_cast<QVBoxLayout*>(page->layout())) {
                for (int j = 0; j < lay->count(); ++j)
                    if (qobject_cast<QGroupBox*>(lay->itemAt(j)->widget()))
                        ++n;
            }
        }
        minGroups = qMin(minGroups, n);
    }
    return minGroups == INT_MAX ? 0 : minGroups;
}

int MainWindow::settingsHeightForTest() const
{
    // 用 sizeHint 而不是 height()：对话框还没 show 过，height() 是 resize() 给的那个数，
    // 量不出"内容到底多高"。超屏这个毛病正是内容高撑出来的，所以必须量 sizeHint。
    return m_settingsDlg ? m_settingsDlg->sizeHint().height() : 0;
}

QString MainWindow::routeTextForTest() const
{
    return m_routeLabel ? m_routeLabel->text() : QString();
}

// ---- 主界面宽度够不够 ----
//
// 跟 settingsHeightForTest 是同一类量法，方向换个轴：主界面套在 QScrollArea 里，
// **内容需要的宽度 > 窗口宽度**时会出横向滚动条 —— 最右边那列按钮（粘贴 / 显示 /
// 换一个码…）就被切在屏幕外。⚠ 这个毛病 `--screenshot` **抓不出来**：那张图抓的是
// ScrollArea 的 widget（整份内容），宽度是内容自己的宽度，怎么看都是齐的。
// 只有把"内容最小宽"和"窗口宽"两个数摆在一起才看得见。
int MainWindow::mainContentMinWidthForTest() const
{
    auto* sa = qobject_cast<QScrollArea*>(centralWidget());
    if (!sa || !sa->widget())
        return 0;
    // minimumSizeHint：布局**至少**要这么宽（各控件的最小宽 + 间距 + 边距）。
    return sa->widget()->minimumSizeHint().width();
}

int MainWindow::windowWidthForTest() const
{
    return width();
}

QString MainWindow::mainWidthBreakdownForTest() const
{
    // 693 这种数一出来，光知道"超了"没用 —— 得知道是**哪一组**超的。
    // 组名用 title()（已经是 TR 过的当前语言），宽度取该组的最小宽。
    auto* sa = qobject_cast<QScrollArea*>(centralWidget());
    if (!sa || !sa->widget())
        return QString();
    QStringList out;
    const auto boxes = sa->widget()->findChildren<QGroupBox*>(QString(), Qt::FindDirectChildrenOnly);
    for (const auto* b : boxes)
        out << QStringLiteral("%1=%2").arg(b->title()).arg(b->minimumSizeHint().width());
    out.sort();
    return out.join(QStringLiteral("  "));
}

// ---- 教程目录 ----

int MainWindow::guideTopicCountForTest() const
{
    return m_guideTopicList ? m_guideTopicList->count() : 0;
}

QString MainWindow::guideTopicKeyForTest(int row) const
{
    if (!m_guideTopicList || row < 0 || row >= m_guideTopicList->count())
        return QString();
    const QListWidgetItem* it = m_guideTopicList->item(row);
    return it ? it->data(Qt::UserRole).toString() : QString();
}

QString MainWindow::guideCurrentTopicForTest() const
{
    return m_guideTopic;
}

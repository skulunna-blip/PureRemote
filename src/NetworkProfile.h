#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include "RustDeskController.h"   // OptionEdit

class AppConfig;

// 四档网络路径 → RustDesk2.toml 的 [options] 改动清单（M4.2）。
//
// **为什么单独一个文件而不是塞进 MainWindow**：这份映射是"档位"这个词的全部含义，
// 界面上三个按钮（切换 / 一键体检 / 自检）都要用它，而且它必须能被无头断言
// （--proftest）。放进 MainWindow 就得靠抓控件来测，那是 GUI 测试的活，不是这里的。
//
// 全部是**静态纯函数**，不持有状态 —— 输入 (mode, config)，输出一行行 OptionEdit。
class NetworkProfile
{
public:
    // 这条档位该往 RustDesk2.toml 写什么。**value 为空 = 删掉这个键**。
    // 顺序固定（写盘时会按这个顺序改），好让测试逐条比对。
    static QVector<RustDeskController::OptionEdit> editsFor(const QString& mode,
                                                            const AppConfig& cfg);

    // 这条档位自不自洽（cloudflare / custom 必须有服务器地址）。不自洽就别写盘——
    // 写进去就是一串空服务器地址，RustDesk 会当成"连不上"而不是"用默认值"。
    static bool validate(const QString& mode, const AppConfig& cfg, QString* err);

    // validate → setServerProfile。**不碰 RustDesk 进程**（写 toml 要求它没在跑，这是调用方的事）。
    static bool apply(const QString& mode, const AppConfig& cfg, RustDeskController* rd,
                      QString* err);

    // 自检：把 editsFor 的期望值和 toml 里的实际值逐条比。返回不一致的描述（空 = 全对）。
    static QStringList mismatches(const QString& mode, const AppConfig& cfg,
                                  const RustDeskController* rd);

    // 图文引导正文：这条档位是干什么的、要填什么、怎么自检。
    static QString guide(const QString& mode);

    // 会被这套机制写到的全部键（自检时用它说明"清干净了没有"）。
    static QStringList managedKeys();
};

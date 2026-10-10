#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""翻译表体检：把源码里所有 TR("…") 的键跟 I18n.cpp 的表对一遍。

为什么需要它 —— t() 找不到键时**静默退回中文**。这是刻意设计（宁可显示中文也不要
显示空串或崩溃），代价就是"漏翻一条"在界面上看不出来：中文用户永远看不见，
英文用户看到一句中文，而我们自己那个英文界面截图恰好没截到那一屏，就一直没人发现。

拦的四类：
  · 用到了但表里没有的键   → 英文档下会冒中文（**致命**，退出码非 0）
  · 表里的重复键           → 后面的覆盖前面的，改了上面那条不生效（致命）
  · 空键 / 空译文          → 空译文等于把界面变没了（致命）
  · 英文值里还有中日韩字    → 复制粘贴时忘了翻（致命）
外加一类只报告不判死的：
  · 表里有、源码里没人用    → 可能是死条目，也可能是给 I18n::tDynamic 用的
                              （「未命名」就是这种），所以不敢自动判死。

键的比对是**按源码字面量**：两边都是人手写的相邻字面量拼接，`\\n` 两边写法一致，
所以不做反转义也不会误判。**不要把这条改成"解析后的值"** —— 那样两边都得实现一遍
C++ 转义规则，实现错了就成了"检查器自己判卷"，比不检查更坏。

用法：python check_i18n.py [--root <项目根>]
"""
import re
import sys
from pathlib import Path

# 一个 QStringLiteral(...)，括号内允许字符串字面量与空白，不允许裸括号
QLIT = r'QStringLiteral\(((?:[^()"\\]|"(?:\\.|[^"\\])*"\s*)*)\)'
# 表里的一条：{ QStringLiteral("zh"), QStringLiteral("en") }
ENTRY = re.compile(r'\{\s*' + QLIT + r'\s*,\s*' + QLIT + r'\s*,?\s*\}')
# 源码里的一处 TR("…")
TRCALL = re.compile(r'TR\(\s*((?:[^()"\\]|"(?:\\.|[^"\\])*"\s*)*)\)')
# 一个字符串字面量
STR = re.compile(r'"((?:\\.|[^"\\])*)"')
# .arg() 的占位符
MARK = re.compile(r'%[0-9]')
# 「裸 &」：前后都不是 &。写 && 是 Qt 里显示一个真 & 的转义写法，不算错。
AMP = re.compile(r'(?<!&)&(?!&)')
# markdown 的 `**加粗**`。Qt 的 QLabel / QMessageBox **不认 markdown**，
# 星号会连着那两个字一起原样画在界面上。想加粗只能写 <b></b>。
MD = re.compile(r'\*\*')

CJK_RANGES = (
    (0x3000, 0x303F),   # 全角标点
    (0x4E00, 0x9FFF),   # 汉字
    (0xFF00, 0xFFEF),   # 全角字母数字
)


def literals(group: str) -> str:
    """把一组相邻字面量拼成一个源码级字符串（保留 \\n 这类转义原样）。"""
    return ''.join(m.group(1) for m in STR.finditer(group))


def has_cjk(s: str) -> bool:
    return any(lo <= ord(c) <= hi for c in s for lo, hi in CJK_RANGES)


def strip_comments(text: str) -> str:
    """把注释换成等长空格（保住偏移，行号才不会错位）。

    ⚠ **不能简单地正则在 `//` 上切**。Guide.cpp 的正文是 `R"(<a href='https://…'>)"`
    这种原始字符串 —— 里面的 `//` 是 URL 的一部分，一刀切下去会把整个文件从那里
    往后全看成注释，TR 调用一个都扫不到，报告却是"全绿"。这正是这个脚本要防的那种
    "检查器自己坏了但看不出来"，所以这里老老实实按词法走一遍。

    只处理会出现在本项目的四种形态：`//` / `/* */` / `"…"` / `'…'` / `R"delim(…)delim"`。
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        # 原始字符串 R"delim( ... )delim"
        if c == 'R' and i + 1 < n and text[i + 1] == '"':
            j = i + 2
            k = j
            while k < n and text[k] != '(':
                k += 1
            if k >= n:
                out.append(c); i += 1; continue
            delim = text[j:k]
            close = ')' + delim + '"'
            end = text.find(close, k + 1)
            end = n if end < 0 else end + len(close)
            out.append(text[i:end])
            i = end
            continue
        # 行注释
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            k = text.find('\n', i)
            k = n if k < 0 else k
            out.append(' ' * (k - i))
            i = k
            continue
        # 块注释（**不嵌套**，跟 C++ 一致）
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            k = text.find('*/', i + 2)
            k = n if k < 0 else k + 2
            out.append(''.join('\n' if ch == '\n' else ' ' for ch in text[i:k]))
            i = k
            continue
        # 普通字符串
        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == '\\':
                    j += 2
                    continue
                if text[j] == '"':
                    j += 1
                    break
                j += 1
            out.append(text[i:j])
            i = j
            continue
        # 字符字面量
        if c == "'":
            j = i + 1
            while j < n:
                if text[j] == '\\':
                    j += 2
                    continue
                if text[j] == "'":
                    j += 1
                    break
                if text[j] == '\n':      # 不是字符字面量（多半是别的什么），别吞掉整行
                    break
                j += 1
            out.append(text[i:j])
            i = j
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass

    argv = sys.argv[1:]
    root = Path(argv[argv.index('--root') + 1]) if '--root' in argv else Path(__file__).resolve().parent.parent
    src = root / 'src'

    i18n_cpp = src / 'I18n.cpp'
    if not i18n_cpp.exists():
        print('找不到 %s' % i18n_cpp)
        return 1

    # ---- 读表 ----
    pairs = []
    for m in ENTRY.finditer(strip_comments(i18n_cpp.read_text(encoding='utf-8'))):
        pairs.append((literals(m.group(1)), literals(m.group(2))))

    if not pairs:
        print('I18n.cpp 里一条表项都没解析出来 —— 解析器跟文件格式对不上了，先修脚本。')
        return 1

    # ---- 扫源码 ----
    used = {}          # key -> 第一个用到它的 文件:行
    scanned = 0
    for f in sorted(src.glob('*.cpp')) + sorted(src.glob('*.h')):
        if f.name == 'I18n.cpp':
            continue
        text = strip_comments(f.read_text(encoding='utf-8'))
        for m in TRCALL.finditer(text):
            key = literals(m.group(1))
            if not key:
                continue
            scanned += 1
            if key not in used:
                used[key] = '%s:%d' % (f.name, text.count('\n', 0, m.start()) + 1)

    table = {}
    dups = []
    for zh, en in pairs:
        if zh in table:
            dups.append(zh)
        table[zh] = en

    missing = [(k, used[k]) for k in used if k not in table]
    empty_key = [zh for zh, _ in pairs if not zh]
    empty_en = [zh for zh, en in pairs if not en]
    cjk_en = [(zh, en) for zh, en in pairs if has_cjk(en)]
    # 占位符个数必须一致。QString::arg(a,b,…) 的多参重载是按标记**出现顺序**发实参的，
    # 所以编号本身无所谓，但**个数**少了就是实打实的错：译文里少写一个 %2，
    # 那一路的实参就没人接，界面上显示成光秃秃的一句话（或者残留一个 %2）。
    # 这类错在中文档下永远看不见 —— 只有切到英文才露头。
    arg_mismatch = [(zh, en) for zh, en in pairs
                    if len(MARK.findall(zh)) != len(MARK.findall(en))]
    # 裸 & 会被 Qt 当成**助记符**：控件文字里写 "Route & quality"，实际渲染出来是
    # "Route _quality" —— 吃掉一个字符，下划线跑到 q 下面。它不改长度、不报错、
    # 断言也照绿，**只有人眼看图才发现**（这条是真踩过的：英文标签页写成
    # "Route & quality"，截图里就是 "Route _quality"）。
    # 想真显示一个 & 在 Qt 里得写 &&；这个项目压根不用助记符，所以一律当错。
    amp_in_text = [(zh, en) for zh, en in pairs
                   if AMP.findall(zh) or AMP.findall(en)]
    # markdown 的 `**加粗**`。跟裸 & 同一类失败方式 —— 改的是**渲染结果**而不是字符串
    # 本身：长度没变、不报错、断言照绿，只有截图人眼看才发现。这条也是真踩过的：
    # 自启提示里写的 `**"登录之后"**`，界面上就是字面的四个星号。
    # 想加粗写 <b></b>；用了 <b> 的串是**富文本**，\n 就不再换行，得写成 <br>。
    md_in_text = [(zh, en) for zh, en in pairs
                  if MD.findall(zh) or MD.findall(en)]
    # 富文本和 `\n` **不能混用**：串里一旦有标签，QLabel/QMessageBox 就按 rich text 渲染，
    # `\n` 会被当普通空白吃掉 —— 整段挤成一行。要么全 plain（用 \n），要么全 rich（用 <br>）。
    # 这也属于"改完一半比不改更坏"：看着像修好了，实际换行全没了。
    # 判据用的是**字面量原文**（本脚本按未反转义的源码比对），所以这里找的是反斜杠 n。
    rt_nl = [(zh, en) for zh, en in pairs
             if ('<b>' in zh or '<b>' in en or '<br>' in zh or '<br>' in en)
             and ('\\n' in zh or '\\n' in en)]
    unused = sorted(set(table) - set(used))

    # ---- 报告 ----
    print('翻译表：%d 条；源码里 TR()：%d 处、%d 个不同的键' % (len(pairs), scanned, len(used)))
    print()

    fatal = 0

    def section(title, items, fmt, is_fatal):
        nonlocal fatal
        if is_fatal and items:
            fatal += len(items)
        mark = '!!' if (is_fatal and items) else '  '
        print('%s %s：%d' % (mark, title, len(items)))
        for it in items:
            print('     ' + fmt(it))
        print()

    section('用到了但表里没有（英文档下会冒中文）', missing,
            lambda it: '%s\n       ← %s' % (it[0], it[1]), True)
    section('表里的重复键（后面的覆盖前面的）', dups, lambda s: s, True)
    section('空键', empty_key, lambda s: repr(s), True)
    section('空译文', empty_en, lambda s: s, True)
    section('英文值里还有中日韩字符（多半是忘了翻）', cjk_en,
            lambda it: '%s\n       → %s' % (it[0], it[1]), True)
    section('中英 %N 占位符个数不一致（实参接不上）', arg_mismatch,
            lambda it: '%s\n       → %s' % (it[0], it[1]), True)
    section('文案里有裸 &（Qt 当助记符，会吃掉一个字符）', amp_in_text,
            lambda it: '%s\n       → %s   ← 要显示 & 得写 &&，不用助记符就别写 &'
                       % (it[0], it[1]), True)
    section('文案里有 markdown 星号（Qt 不认，会原样画出 ** ）', md_in_text,
            lambda it: '%s\n       → %s   ← 要加粗得写 <b></b>，且串里 \\n 得改 <br>'
                       % (it[0], it[1]), True)
    section('同一句里既有富文本标签又有 \\n（rich text 不换行，得写 <br>）', rt_nl,
            lambda it: '%s\n       → %s   ← 要么全用 \\n，要么把 \\n 全换成 <br>'
                       % (it[0], it[1]), True)
    section('表里有、源码里没人用（可能正常：给 tDynamic 用的）', unused, lambda s: s, False)

    if fatal:
        print('结果：%d 项致命问题' % fatal)
        return 1
    print('结果：全绿')
    return 0


if __name__ == '__main__':
    sys.exit(main())

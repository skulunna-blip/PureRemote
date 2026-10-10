"""M3 密码机制探针：读写 RustDesk.toml 的 **顶层** password 字段。

用法：
    python scripts/pwprobe.py get              # 打印顶层 password（TOML 解析，不是正则）
    python scripts/pwprobe.py set <明文>        # 写入顶层 password
    python scripts/pwprobe.py del              # 删除顶层 password
    python scripts/pwprobe.py keys             # 列出顶层键
    python scripts/pwprobe.py dump             # 打印全文（排查用）

**血泪教训 1（2026-10-08）**：早期版本用正则把 `password = 'xxx'` 追加到文件末尾。
RustDesk.toml 末尾是 `[keys_confirmed]` 表，于是 password 落进那个表里
（`keys_confirmed.password`），顶层根本没有 password。RustDesk 读顶层得到空串 →
启动 store() 回写空 → 表现成"密码被清空"，把我引去查了半天 IPC / portable service。
**根因是探针写错了作用域。**

**血泪教训 2（2026-10-08，同日）**：改成"插到第一个表头之前"后仍然错。判据用了
`line.lstrip().startswith("[")` 认表头，但 `key_pair = [[...]]` 这个**多行数组**的内层
`    [` 也以 `[` 开头，于是从第 5 行被切开，password 被插进数组内部 → TOML 非法 →
RustDesk 加载失败 → 回写默认值。表现**同样**像"密码被清空"，又一次误导。
**表头判据必须是括号深度为 0 且整行是 `[...]`，不是行首字符。**

正确做法：用括号深度扫描定位"顶层标量区"的结束位置，插在它前面。
"""
import io
import os
import re
import sys

CFG = os.path.join(os.environ["APPDATA"], "RustDesk", "config", "RustDesk.toml")

TOP_PW = re.compile(r"^[ \t]*password[ \t]*=")
KEY_OPEN_ARRAY = re.compile(r"^[A-Za-z0-9_\-]+[ \t]*=[ \t]*\[")


def read():
    with io.open(CFG, encoding="utf-8") as f:
        return f.read()


def write(s):
    with io.open(CFG, "w", encoding="utf-8", newline="") as f:
        f.write(s)


def scan(lines):
    """返回 (顶层 password 行下标 或 None, 顶层标量区结束下标)。

    顶层标量区 = 文件开头到「第一个开启数组的行」或「第一个表头行」之前。
    深度扫描：单引号串内不计数；`key_pair = [` 开数组、`]` 收数组，内部行深度 > 0。
    """
    depth = 0
    in_str = False
    pw_idx = None
    for i, ln in enumerate(lines):
        s = ln.strip()
        if depth == 0:
            # 表头：整行就是 [xxx]（不是赋值）
            if s.startswith("[") and s.endswith("]") and "=" not in s:
                return pw_idx, i
            # 开启多行数组
            if KEY_OPEN_ARRAY.match(ln):
                return pw_idx, i
            if TOP_PW.match(ln) and pw_idx is None:
                pw_idx = i
        for ch in ln:
            if ch == "'":
                in_str = not in_str
            elif not in_str:
                if ch == "[":
                    depth += 1
                elif ch == "]":
                    depth -= 1
    return pw_idx, len(lines)


def load_toml():
    import tomllib
    with open(CFG, "rb") as f:
        return tomllib.load(f)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "get"
    if not os.path.exists(CFG):
        print("(配置文件不存在)", file=sys.stderr)
        return 2

    if cmd == "dump":
        print(read())
        return 0

    if cmd == "keys":
        try:
            print(list(load_toml().keys()))
        except Exception as e:
            print("(解析失败: %s)" % e, file=sys.stderr)
            return 2
        return 0

    if cmd == "get":
        try:
            v = load_toml().get("password")
        except Exception as e:
            print("(解析失败: %s)" % e, file=sys.stderr)
            return 2
        print(v if v is not None else "(无顶层 password 字段)")
        return 0

    lines = read().split("\n")
    pw_idx, region_end = scan(lines)
    top = lines[:region_end]
    rest = lines[region_end:]
    if pw_idx is not None:
        top = [ln for i, ln in enumerate(top) if i != pw_idx]

    if cmd == "del":
        out = "\n".join(top + rest)
        if not out.endswith("\n"):
            out += "\n"
        write(out)
        print("已删除顶层 password 行" if pw_idx is not None else "(顶层本来就没有 password)")
        return 0

    if cmd == "set":
        pw = sys.argv[2] if len(sys.argv) > 2 else ""
        if "'" in pw or "\\" in pw:
            print("密码不能含单引号或反斜杠（会破坏 TOML 单引号串）", file=sys.stderr)
            return 2
        while top and top[-1].strip() == "":
            top.pop()
        top.append("password = '%s'" % pw)
        top.append("")
        out = "\n".join(top + rest)
        if not out.endswith("\n"):
            out += "\n"
        write(out)
        # 立刻用真解析器验一遍，别再自欺
        try:
            got = load_toml().get("password")
        except Exception as e:
            print("!! 写入后 TOML 解析失败: %s !!" % e, file=sys.stderr)
            return 3
        if got != pw:
            print("!! 校验不一致: 读到 %r !!" % got, file=sys.stderr)
            return 3
        print("已写入顶层 password = %r" % got)
        return 0

    print("未知命令 " + cmd, file=sys.stderr)
    return 2


sys.exit(main())

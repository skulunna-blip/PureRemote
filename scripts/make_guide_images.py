"""生成 resources/guide/*.png —— 图文教程里用的**示意图**。

这些不是截图（我们没 Tailscale 账号可截，别人的截图也不该抄），是自绘的方块加箭头，
用来把"东西在哪、该勾哪个"说清楚。每张图右下角都印着「示意图」，正文里也再写一遍。

画法照抄 scripts/make_icon.py：先按 2 倍尺寸画、再 LANCZOS 降采样，
比直接按目标尺寸画干净。背景透明，贴到深色正文上不留白边。

中英双语：每张图的绘制**只写一遍**，文案从「文案字典」里取，中英各调一次（见 ZH / EN）。
布局、字号、坐标是共享的同一套代码；中文版的文案表逐字照抄旧版，所以中文三张的像素不变。
英文更长，文案写短一些。
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

SS = 2                                   # 超采样倍数
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "resources", "guide")

BG      = (0, 0, 0, 0)                   # 透明
CARD    = (35, 35, 35, 255)
CARD_LN = (58, 58, 58, 255)
REGION  = (26, 26, 26, 255)
ACCENT  = (74, 158, 255, 255)
TEXT    = (224, 224, 224, 255)
SUB     = (153, 153, 153, 255)
GREEN   = (76, 175, 80, 255)
RED     = (255, 85, 85, 255)
AMBER   = (255, 180, 84, 255)

FONT_DIR = "C:/Windows/Fonts"
_font_cache = {}


def font(size, bold=False):
    key = (size, bold)
    if key not in _font_cache:
        name = "msyhbd.ttc" if bold else "msyh.ttc"
        _font_cache[key] = ImageFont.truetype(os.path.join(FONT_DIR, name), size * SS)
    return _font_cache[key]


def canvas(w, h):
    img = Image.new("RGBA", (w * SS, h * SS), BG)
    return img, ImageDraw.Draw(img)


def finish(img, name):
    out = img.resize((img.width // SS, img.height // SS), Image.LANCZOS)
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name)
    out.save(path)
    print("  %-18s %dx%d" % (name, out.width, out.height))


def box(d, x0, y0, x1, y1, radius=6, fill=CARD, outline=CARD_LN, width=1):
    d.rounded_rectangle([x0 * SS, y0 * SS, x1 * SS, y1 * SS], radius=radius * SS,
                        fill=fill, outline=outline, width=width * SS)


def text(d, x, y, s, size=14, color=TEXT, bold=False, anchor="la"):
    d.text((x * SS, y * SS), s, font=font(size, bold), fill=color, anchor=anchor)


def center_text(d, cx, cy, s, size=14, color=TEXT, bold=False):
    text(d, cx, cy, s, size, color, bold, anchor="mm")


def check(d, x, y, size=18, color=GREEN):
    """画一个 ✓"""
    d.line([((x + size * 0.20) * SS, (y + size * 0.52) * SS),
            ((x + size * 0.42) * SS, (y + size * 0.76) * SS)],
           fill=color, width=3 * SS)
    d.line([((x + size * 0.42) * SS, (y + size * 0.76) * SS),
            ((x + size * 0.82) * SS, (y + size * 0.22) * SS)],
           fill=color, width=3 * SS)


def cross(d, x, y, size=18, color=RED):
    """画一个 ✗"""
    m = size * 0.24
    d.line([((x + m) * SS, (y + m) * SS), ((x + size - m) * SS, (y + size - m) * SS)],
           fill=color, width=3 * SS)
    d.line([((x + size - m) * SS, (y + m) * SS), ((x + m) * SS, (y + size - m) * SS)],
           fill=color, width=3 * SS)


def checkbox(d, x, y, size=18):
    d.rounded_rectangle([x * SS, y * SS, (x + size) * SS, (y + size) * SS],
                        radius=3 * SS, outline=SUB, width=2 * SS)


def arrow(d, x0, y, x1, color=ACCENT):
    d.line([(x0 * SS, y * SS), (x1 * SS, y * SS)], fill=color, width=3 * SS)
    a = 8
    d.polygon([(x1 * SS, y * SS), ((x1 - a) * SS, (y - a * 0.7) * SS),
               ((x1 - a) * SS, (y + a * 0.7) * SS)], fill=color)


def corner_note(d, w, h, note, size=11):
    """右下角的「示意图」角标 —— 每张图都必须有，免得有人当成真截图。"""
    text(d, w - 14, h - 10, note, size, SUB, anchor="rs")


# --------------------------------------------------------------- 图 1：概念
def draw_tsnet_net(c, name):
    W, H = 560, 200
    img, d = canvas(W, H)

    # 整块 = tailnet
    box(d, 14, 10, 546, 166, radius=10, fill=REGION, outline=CARD_LN)
    text(d, 30, 20, c["net_head"], 13, SUB)

    box(d, 34, 70, 224, 148, radius=8)
    text(d, 129, 88, c["net_ctrl"], 16, TEXT, bold=True, anchor="mm")
    text(d, 129, 112, c["net_ctrl_sub"], 12, SUB, anchor="mm")
    text(d, 129, 132, c["net_ctrl_hint"], 11, SUB, anchor="mm")

    box(d, 336, 70, 526, 148, radius=8)
    text(d, 431, 88, c["net_srv"], 16, TEXT, bold=True, anchor="mm")
    text(d, 431, 112, c["net_srv_sub"], 12, SUB, anchor="mm")
    text(d, 431, 132, c["net_srv_hint"], 11, SUB, anchor="mm")

    arrow(d, 232, 106, 326)
    text(d, 279, 88, c["net_flow"], 11, ACCENT, anchor="mm")

    corner_note(d, W, H, c["corner"])
    finish(img, name)


# ------------------------------------------------------- 图 2：Keys 页该勾哪三个
def draw_tsnet_keys(c, name):
    W, H = 560, 310
    img, d = canvas(W, H)

    box(d, 12, 8, 548, 292, radius=10, fill=REGION, outline=CARD_LN)
    text(d, 32, 22, c["keys_title"], 17, ACCENT, bold=True)

    # Description
    text(d, 32, 62, c["keys_desc_label"], 13, SUB)
    box(d, 132, 54, 380, 86, radius=5, fill=(20, 20, 20, 255))
    text(d, 146, 63, c["keys_desc_val"], 13, SUB)

    # Reusable
    checkbox(d, 34, 104)
    check(d, 34, 104)
    text(d, 62, 104, c["keys_reuse"], 14, TEXT)
    text(d, 348, 106, c["keys_reuse_note"], 12, GREEN)

    # Ephemeral
    checkbox(d, 34, 144)
    cross(d, 34, 144)
    text(d, 62, 144, c["keys_eph"], 14, TEXT)
    text(d, 348, 146, c["keys_eph_note"], 12, RED)

    # Expiry
    text(d, 32, 184, c["keys_exp_label"], 13, SUB)
    box(d, 132, 176, 380, 208, radius=5, fill=(20, 20, 20, 255))
    text(d, 146, 185, c["keys_exp_val"], 13, TEXT)
    text(d, 348, 187, c["keys_exp_note"], 12, GREEN)

    # 按钮
    box(d, 32, 232, 210, 272, radius=6, fill=(42, 42, 42, 255), outline=ACCENT, width=2)
    center_text(d, 121, 252, c["keys_btn"], 14, TEXT, bold=True)

    corner_note(d, W, H, c["corner"])
    finish(img, name)


# --------------------------------------------------------------- 图 3：四档对比
def draw_net_modes(c, name):
    # 高度多留 24px 给右下角的「示意图」角标 —— 贴着最后一行会跟行内小字叠在一起。
    W, H = 560, 300
    img, d = canvas(W, H)

    y = 12
    for row_name, desc, ping, col in c["modes_rows"]:
        box(d, 12, y, 548, y + 54, radius=7)
        text(d, 30, y + 18, row_name, 14, TEXT, bold=True)
        text(d, 30, y + 38, desc, 11, SUB)
        text(d, 534, y + 27, ping, 14, col, bold=True, anchor="rm")
        y += 64

    corner_note(d, W, H - 6, c["corner"])
    finish(img, name)


# --------------------------------------------------------------- 文案表（中 / 英）
# 每张图的绘制代码按固定的 坐标 / 字号 取这些字符串。中文表逐字照抄旧版，别动 ——
# 动了中文三张 PNG 的 md5 就会变。英文更宽，所以句子写短。
ZH = {
    "corner": "示意图",
    "net_head": "tailnet · 你的私有网（Tailscale 加密隧道）",
    "net_ctrl": "控制端",
    "net_ctrl_sub": "你现在这台",
    "net_ctrl_hint": "在「连接别的电脑」点连接",
    "net_srv": "被控端",
    "net_srv_sub": "别人的电脑",
    "net_srv_hint": "把连接码 + 密码给你",
    "net_flow": "RustDesk 流量",
    "keys_title": "Generate auth key",
    "keys_desc_label": "Description",
    "keys_desc_val": "my-pc",
    "keys_reuse": "Reusable",
    "keys_reuse_note": "必须勾 —— 不勾只能用一次",
    "keys_eph": "Ephemeral",
    "keys_eph_note": "千万别勾",
    "keys_exp_label": "Expiry",
    "keys_exp_val": "No expiry",
    "keys_exp_note": "选「不限制」",
    "keys_btn": "Generate key",
    "modes_rows": [
        ("官方服务器（默认）", "经过纽约 · 中继在法国", "220 ms", RED),
        ("tailnet", "Tailscale 就近中继（香港）", "43–68 ms", GREEN),
        ("Cloudflare Workers", "apac 节点 · 预期值", "43–101 ms", GREEN),
        ("自填地址", "你自己的服务器，离你最近", "看情况", ACCENT),
    ],
}

EN = {
    "corner": "Schematic only — the real UI may differ.",
    "net_head": "tailnet · your private network (Tailscale tunnel)",
    "net_ctrl": "Controller",
    "net_ctrl_sub": "this computer",
    "net_ctrl_hint": "click Connect to another PC",
    "net_srv": "Controlled",
    "net_srv_sub": "their computer",
    "net_srv_hint": "shares code + password",
    "net_flow": "RustDesk traffic",
    "keys_title": "Generate auth key",
    "keys_desc_label": "Description",
    "keys_desc_val": "my-pc",
    "keys_reuse": "Reusable",
    "keys_reuse_note": "Required (else one-time)",
    "keys_eph": "Ephemeral",
    "keys_eph_note": "Never check",
    "keys_exp_label": "Expiry",
    "keys_exp_val": "No expiry",
    "keys_exp_note": "Pick no expiry",
    "keys_btn": "Generate key",
    "modes_rows": [
        ("Official servers (default)", "via New York · relay in France", "220 ms", RED),
        ("tailnet", "Tailscale relay nearby (Hong Kong)", "43–68 ms", GREEN),
        ("Cloudflare Workers", "apac nodes · expected", "43–101 ms", GREEN),
        ("Custom address", "your own server, nearest to you", "Depends", ACCENT),
    ],
}


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    print("生成 resources/guide/：")
    for c, suffix in ((ZH, ""), (EN, "-en")):
        draw_tsnet_net(c, "tsnet-net%s.png" % suffix)
        draw_tsnet_keys(c, "tsnet-keys%s.png" % suffix)
        draw_net_modes(c, "net-modes%s.png" % suffix)
    print("完成 ->", OUT)


if __name__ == "__main__":
    main()

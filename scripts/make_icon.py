"""生成 resources/icon.ico —— 显示器 + 光标，远程控制的直白隐喻。

超采样到 1024 再逐档降采样，比直接按小尺寸画干净（同 PureViewer 的做法）。
"""
from PIL import Image, ImageDraw

S = 1024
TOP, BOT = (46, 52, 62), (22, 25, 30)
ACCENT = (74, 158, 255, 255)
WHITE = (235, 242, 255, 255)

# ---- 圆角底板：竖向渐变 + 内描边 ----
grad = Image.new("RGBA", (S, S))
gd = ImageDraw.Draw(grad)
for y in range(S):
    t = y / (S - 1)
    gd.line([(0, y), (S, y)],
            fill=tuple(int(TOP[i] + (BOT[i] - TOP[i]) * t) for i in range(3)) + (255,))

mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.22), fill=255)

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
img.paste(grad, (0, 0), mask)
d = ImageDraw.Draw(img)
d.rounded_rectangle([6, 6, S - 7, S - 7], radius=int(S * 0.21),
                    outline=(74, 158, 255, 90), width=8)

# ---- 显示器 ----
mx0, my0, mx1, my1 = int(S * 0.17), int(S * 0.25), int(S * 0.83), int(S * 0.64)
d.rounded_rectangle([mx0, my0, mx1, my1], radius=int(S * 0.05),
                    outline=ACCENT, width=int(S * 0.045))

cx = S // 2
d.rectangle([cx - int(S * 0.035), my1, cx + int(S * 0.035), int(S * 0.73)], fill=ACCENT)
d.rounded_rectangle([cx - int(S * 0.14), int(S * 0.73), cx + int(S * 0.14), int(S * 0.79)],
                    radius=int(S * 0.02), fill=ACCENT)

# ---- 屏内光标：小尺寸下仍读得出是"指针" ----
ax, ay = int(S * 0.36), int(S * 0.33)
h = int(S * 0.22)
d.polygon([(ax, ay), (ax, ay + h), (ax + int(h * 0.72), ay + int(h * 0.78))], fill=WHITE)

# ---- 导出多尺寸 ICO ----
# 直接把 1024 母图交给 PIL 并列出 sizes —— PIL 内部会逐档 LANCZOS 降采样。
# 别自己降好再 append_images：那条路只会留下第一帧的尺寸。
SIZES = [16, 24, 32, 48, 64, 128, 256]
img.save("resources/icon.ico", format="ICO", sizes=[(s, s) for s in SIZES])

# 顺手留一张 PNG 预览，方便肉眼确认
img.resize((256, 256), Image.LANCZOS).save("resources/icon_preview.png")
print("wrote resources/icon.ico")

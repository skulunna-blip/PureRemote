# bin/ —— RustDesk 运行时（不入库）

这里放的是 **RustDesk 1.5.0 解包后的运行时**，不是官方发布的那个 25MB 便携 exe。

## 为什么不发官方便携 exe

官方 `rustdesk.exe`（25MB）其实是个 **launcher**：先自解包到
`%LOCALAPPDATA%\rustdesk\`（80MB / 106 文件），再 re-exec 里面的真身。

两个问题：

1. **launcher 那层在无控制台进程里会卡死** —— 从 bash 手敲 `--get-id` 0.47s 正常返回，
   但用 `QProcess` 拉起同一条命令会卡在解包检查阶段 >10s 且永远不产出 ID。
   附带：它留下继承 stdout 句柄的常驻子进程，管道 EOF 永远等不到。
2. **它是整套方案里唯一一处系统写入** —— 跟"纯绿色免安装"的目标直接冲突。

所以改成直接把解包结果随包发。代价是包从 25MB 涨到 80MB，换来零系统写入。

## 怎么重新生成这份运行时

官方便携 exe 跑一次就会自己解包出来：

```bash
# 1. 拿官方便携 exe（1.5.0 x86_64）
#    https://github.com/rustdesk/rustdesk/releases
curl -L -o /tmp/rustdesk.exe \
  https://github.com/rustdesk/rustdesk/releases/download/1.5.0/rustdesk-1.5.0-x86_64.exe

# 2. 跑一次触发解包，然后就可以杀掉
/tmp/rustdesk.exe --get-id

# 3. 把解包结果搬进这里
cp -r "$LOCALAPPDATA/rustdesk/." bin/
```

搬完 `bin/rustdesk.exe` 应该是 **369,520 字节**（薄壳），同目录必须有
`librustdesk.dll`（约 40MB）—— 外壳就是靠这个 dll 在不在来区分真身和 launcher 的
（见 `src/RustDeskController.cpp` 的 `cliExePath()`）。

## 内容

| 项 | 说明 |
|---|---|
| `rustdesk.exe` | 真身，369KB 薄壳 |
| `librustdesk.dll` | 39MB，核心逻辑 |
| `flutter_windows.dll` + `*_plugin.dll` | Flutter UI 运行时 |
| `data/` | Flutter 资源（app.so / icudtl.dat） |
| `usbmmidd_v2/` | 虚拟显示器驱动（关屏也能控制的备选手段，装它要管理员） |
| `drivers/RustDeskPrinterDriver/` | 远程打印驱动 |
| `meta.toml` | launcher 的解包版本戳，留着无害 |

## 验收

```bash
bash scripts/verify.sh    # 第 4 项就是断言没往 %LOCALAPPDATA% 写东西
```

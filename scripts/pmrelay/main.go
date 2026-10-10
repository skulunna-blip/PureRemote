// pmrelay —— PureRemote 的 tsnet 桥接（M4.4）
//
// 它解决的问题：RustDesk 的"直连 IP 访问"只认普通 TCP 地址，而 tailnet 地址只在
// tsnet 进程内部可达（Windows 上没有对应的系统网卡）。pmrelay 就是那个"进程内部的
// 出口"——把 tailnet 上的一个端口，接到本机 RustDesk 的直连口上。
//
// 为什么不用 Tailscale 官方客户端：Windows 内核没有内置 TUN，任何能给机器发 IP 的
// 软件都必须自带 wintun.sys 内核驱动，而装内核驱动在 Windows 上永远需要管理员。
// tsnet 是 Tailscale 官方 Go 库，纯用户态，进程内起一个 fake (no-op) TUN 设备，
// 不需要驱动、不需要服务、不需要管理员。（M4.0 闸门已实测通过，3/3 稳定。）
//
// 两种模式：
//
//	expose   被控端：tailnet 上监听 :21118 → 转发到 127.0.0.1:21118（RustDesk 直连口）
//	connect  控制端：本机 127.0.0.1:21119 监听 → 经 tailnet 连 <peer>:21118
//	         外壳随后执行 rustdesk.exe --connect 127.0.0.1:21119 即可
//
// 为什么 connect 用 21119 而不是 21118：控制端自己的 RustDesk 若也开了直连
// （direct-server='Y'），21118 已经被它占了。
//
// 输出约定（外壳靠这个解析）：
//
//	stdout —— 以 "PMRELAY " 开头、空格分隔的 key=value 行，是**协议**
//	stderr —— 人类看的日志，默认安静，-verbose 才开
//
// 分开是为了外壳能干净地只解析协议行，不用在几千行 tsnet 日志里捞东西。
package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"tailscale.com/ipn/ipnstate"
	"tailscale.com/tsnet"
)

var (
	flagMode     = flag.String("mode", "", "expose | connect")
	flagDir      = flag.String("dir", "", "tsnet 状态目录（默认 <exe目录>/tsnet-state）")
	flagHost     = flag.String("hostname", "puremote", "在 tailnet 上显示的主机名")
	flagAuthKey  = flag.String("authkey", "", "Tailscale authkey（留空则读环境变量 TS_AUTHKEY）")
	flagListen   = flag.Int("listen", 21118, "expose：tailnet 上监听的端口")
	flagTarget   = flag.String("target", "127.0.0.1:21118", "expose：转发目标（RustDesk 直连口）")
	flagLocal    = flag.String("local", "127.0.0.1:21119", "connect：本机监听的地址")
	flagPeer     = flag.String("peer", "", "connect：对端 tailnet 主机名或 IP")
	flagPeerPort = flag.Int("peerport", 21118, "connect：对端端口")

	flagTimeout   = flag.Duration("timeout", 5*time.Minute, "登录 / 拨号超时")
	flagVerbose   = flag.Bool("verbose", false, "打开 tsnet 详细日志（走 stderr）")
	flagParentPID = flag.Int("parent-pid", 0, "父进程 PID；它退出时自己也退出（外壳用它防孤儿进程）")
)

// emit 写一行协议输出到 stdout。外壳只认这个前缀。
func emit(format string, a ...any) {
	fmt.Fprintf(os.Stdout, "PMRELAY "+format+"\n", a...)
}

// san 把要放进 key=value 的自由文本（错误消息、路径）里的空白换成下划线。
//
// 协议行是空格分隔的，而 Windows 的错误消息带空格（"The system cannot find..."），
// 直接塞进去会把外壳的解析撑破——它会把消息里的每个词当成新的 key。
// 不用 URL 编码是因为这些值只给人看，可读性比可逆性重要。
func san(s string) string {
	return strings.Join(strings.Fields(s), "_")
}

func authKey() string {
	if *flagAuthKey != "" {
		return *flagAuthKey
	}
	return os.Getenv("TS_AUTHKEY")
}

func stateDir() string {
	if *flagDir != "" {
		return *flagDir
	}
	exe, err := os.Executable()
	if err != nil {
		return "tsnet-state"
	}
	return filepath.Join(filepath.Dir(exe), "tsnet-state")
}

func newServer() *tsnet.Server {
	d := stateDir()
	if err := os.MkdirAll(d, 0o755); err != nil {
		fatal("mkdir-failed", fmt.Sprintf("%s: %v", d, err))
	}
	return &tsnet.Server{
		Dir:      d,
		Hostname: *flagHost,
		AuthKey:  authKey(),
		Logf: func(f string, a ...any) {
			msg := fmt.Sprintf(f, a...)
			// 没有 authkey 时 tsnet 会打一条带登录 URL 的日志，那是首次引导唯一能
			// 拿到 URL 的地方，必须转成协议行给外壳。
			if strings.Contains(msg, "login.tailscale.com/") {
				if u := extractLoginURL(msg); u != "" {
					emit("LOGIN-URL %s", u)
				}
			}
			if *flagVerbose {
				log.Print("[tsnet] " + msg)
			}
		},
	}
}

func extractLoginURL(s string) string {
	i := strings.Index(s, "https://login.tailscale.com/")
	if i < 0 {
		return ""
	}
	u := s[i:]
	if j := strings.IndexAny(u, " \t\n\r"); j >= 0 {
		u = u[:j]
	}
	return u
}

// up 登录 tailnet 并返回本节点的 tailnet IPv4。
func up(srv *tsnet.Server) netip.Addr {
	// 没有 authkey 时要等用户在浏览器里点完登录，给足时间；有 key 时几十秒足够。
	to := *flagTimeout
	if authKey() == "" {
		to = 10 * time.Minute
		log.Print("没有 authkey —— 需要浏览器登录一次（把下面的 URL 给用户）")
	}
	ctx, cancel := context.WithTimeout(context.Background(), to)
	defer cancel()

	st, err := srv.Up(ctx)
	if err != nil {
		fatal("login-failed", err.Error())
	}
	if len(st.TailscaleIPs) == 0 {
		fatal("no-tailnet-ip", "登录了但没拿到 tailnet IP")
	}
	log.Printf("登录成功: %s (%v)", st.Self.DNSName, st.Self.TailscaleIPs)
	return st.TailscaleIPs[0]
}

func fatal(what, detail string) {
	emit("FATAL what=%s detail=%s", what, san(detail))
	log.Fatalf("致命错误 %s: %s", what, detail)
}

// matchPeer 判一条 netmap 记录是不是用户说的那个对端。
// 用户可能填 "puremote-pc"、也可能是 FQDN "puremote-pc.tailXXXX.ts.net."，
// 三种形态都得认。
func matchPeer(p *ipnstate.PeerStatus, want string) bool {
	if strings.EqualFold(strings.TrimSuffix(p.DNSName, "."), want) ||
		strings.EqualFold(p.HostName, want) {
		return true
	}
	// FQDN 的第一段
	if d := strings.TrimSuffix(p.DNSName, "."); d != "" {
		if i := strings.Index(d, "."); i > 0 && strings.EqualFold(d[:i], want) {
			return true
		}
	}
	for _, a := range p.TailscaleIPs {
		if a.String() == want {
			return true
		}
	}
	return false
}

// findPeer 在 netmap 里找对端，**优先返回在线的那条**。
//
// 为什么不能取第一个匹配就返回：同名设备可能同时存在多条 —— 状态目录被删掉后重新
// 注册，Tailscale 会把新节点改名叫 "xxx-1"，而老节点还挂在 tailnet 上没消失（重装、
// 换机器、删 state 目录都会这样）。这两条的 HostName 是**一样的**，只有 DNSName 不同，
// 所以按名字模糊匹配时两条都命中。
//
// 而 Go 的 map 遍历顺序是随机的 —— 取第一个就会出现实测到的那种现象：对端状态每 2 秒
// 在 true/false 之间乱跳，连接时好时坏。必须在多个候选里收敛到一个确定答案。
func findPeer(st *ipnstate.Status, want string) *ipnstate.PeerStatus {
	want = strings.TrimSuffix(want, ".")
	var first *ipnstate.PeerStatus
	n := 0
	for _, p := range st.Peer {
		if !matchPeer(p, want) {
			continue
		}
		n++
		if first == nil {
			first = p
		}
		if p.Online {
			if n > 1 {
				warnAmbiguous(want, p.DNSName)
			}
			return p
		}
	}
	if n > 1 {
		warnAmbiguous(want, first.DNSName)
	}
	return first
}

// 同名多条只提醒一次。watchPeer 每 2 秒调一次 findPeer，提醒跟着刷屏没有意义，
// 而且这些行会经 stderr 传到外壳的日志面板里。
var (
	ambiguousMu     sync.Mutex
	ambiguousWarned = map[string]bool{}
)

func warnAmbiguous(want, chosen string) {
	k := want + "→" + chosen
	ambiguousMu.Lock()
	dup := ambiguousWarned[k]
	ambiguousWarned[k] = true
	ambiguousMu.Unlock()
	if !dup {
		log.Printf("注意：tailnet 里有多条记录匹配 %q，已固定用在线的那条 %s（其余是历史残留设备，"+
			"不影响使用；想清干净就到 Tailscale 控制台删掉同名旧设备）", want, chosen)
	}
}

// resolvePeer 决定 dial 用哪个地址。
//
// 关键：**能拿到 IP 就别用名字**。"puremote-test-a:21118" 这种名字是走 MagicDNS 解析的，
// 而同名设备恰好可能指向那条已经下线的旧记录 → dial 会一直等到 context 超时（实测
// 20 秒白等，然后 6 次重试全废）。对端的 IP 我们刚从 netmap 里读出来，就是刚才那条
// online=true 的记录，直接用它才是确定的。
func resolvePeer(srv *tsnet.Server, peer string, port int, fallback string) string {
	lc, err := srv.LocalClient()
	if err != nil {
		return fallback
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	st, err := lc.Status(ctx)
	if err != nil {
		return fallback
	}
	p := findPeer(st, peer)
	if p == nil {
		return fallback
	}
	// 优先 IPv4：本机实测 IPv6 完全不可用（只有 fe80:: 链路本地），拿 v6 去 dial 必失败。
	for _, a := range p.TailscaleIPs {
		if a.Is4() {
			return netip.AddrPortFrom(a, uint16(port)).String()
		}
	}
	if len(p.TailscaleIPs) > 0 {
		return netip.AddrPortFrom(p.TailscaleIPs[0], uint16(port)).String()
	}
	return fallback
}

// watchPeer 盯着对端上下线，**只在状态变化时**输出一行。
//
// 为什么外壳需要它：M4.0 闸门实测踩到过——对端还没在 tailnet 上"上线"就去 dial，
// 会白等一次 Windows 系统级 connect 超时（20s+）。外壳拿到 online=true 再去
// rustdesk --connect，就能避开这一下。
func watchPeer(ctx context.Context, srv *tsnet.Server, peer string) {
	lc, err := srv.LocalClient()
	if err != nil {
		log.Printf("取 LocalClient 失败，无法监视对端: %v", err)
		return
	}
	last := ""
	for {
		select {
		case <-ctx.Done():
			return
		case <-time.After(2 * time.Second):
		}
		st, err := lc.Status(ctx)
		if err != nil {
			continue
		}
		line := "online=unknown"
		if p := findPeer(st, peer); p != nil {
			line = fmt.Sprintf("online=%v relay=%s direct=%s", p.Online, p.Relay, p.CurAddr)
		}
		if line != last {
			last = line
			emit("PEER name=%s %s", peer, line)
		}
	}
}

// pipeBoth 双向搬运，并在一头结束时把另一头也收掉。
// 半关闭（CloseWrite）要往对端传，否则 RustDesk 那种"我先说完再等你回"的协议会卡住。
func pipeBoth(a, b net.Conn) {
	done := make(chan struct{}, 2)
	cp := func(dst, src net.Conn) {
		io.Copy(dst, src)
		if cw, ok := dst.(interface{ CloseWrite() error }); ok {
			cw.CloseWrite()
		}
		done <- struct{}{}
	}
	go cp(a, b)
	go cp(b, a)
	<-done
	<-done
	a.Close()
	b.Close()
}

// runExpose: 被控端。tailnet 上监听 → 转发到本机 RustDesk 直连口。
func runExpose() {
	srv := newServer()
	defer srv.Close()
	ip := up(srv)

	ln, err := srv.Listen("tcp", fmt.Sprintf(":%d", *flagListen))
	if err != nil {
		fatal("listen-failed", err.Error())
	}
	emit("READY mode=expose ip=%s hostname=%s listen=%d target=%s", ip, *flagHost, *flagListen, *flagTarget)
	log.Printf("tailnet 监听 :%d → 转发到 %s", *flagListen, *flagTarget)

	for {
		c, err := ln.Accept()
		if err != nil {
			log.Printf("Accept 结束: %v", err)
			return
		}
		emit("CONN mode=expose from=%s", c.RemoteAddr())
		go func(c net.Conn) {
			// 目标就在本机，用普通 dial；连不上说明 RustDesk 没起或没开直连。
			d, err := net.DialTimeout("tcp", *flagTarget, 10*time.Second)
			if err != nil {
				emit("ERROR what=target-dial-failed detail=%s", san(err.Error()))
				log.Printf("连本机目标 %s 失败: %v", *flagTarget, err)
				c.Close()
				return
			}
			pipeBoth(c, d)
		}(c)
	}
}

// runConnect: 控制端。本机监听 → 经 tailnet 连对端。
func runConnect() {
	if *flagPeer == "" {
		fatal("bad-args", "connect 模式必须给 -peer")
	}
	srv := newServer()
	defer srv.Close()
	up(srv)

	peerAddr := fmt.Sprintf("%s:%d", *flagPeer, *flagPeerPort)
	if a, err := netip.ParseAddr(*flagPeer); err == nil { // IPv6 要带方括号
		peerAddr = netip.AddrPortFrom(a, uint16(*flagPeerPort)).String()
	}

	ln, err := net.Listen("tcp", *flagLocal)
	if err != nil {
		fatal("local-listen-failed", err.Error())
	}
	emit("READY mode=connect local=%s peer=%s", ln.Addr(), peerAddr)
	log.Printf("本机监听 %s → 经 tailnet 连 %s", ln.Addr(), peerAddr)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go watchPeer(ctx, srv, *flagPeer)

	for {
		c, err := ln.Accept()
		if err != nil {
			log.Printf("Accept 结束: %v", err)
			return
		}
		emit("CONN mode=connect from=%s", c.RemoteAddr())
		go func(c net.Conn) {
			defer c.Close()
			// 重试：首次 dial 可能撞上 disco 还在协商（对端刚上线、DERP 刚连上）。
			// 每次重试都重新解析一遍地址 —— 对端可能刚从"同名旧设备"切到在线的那条。
			var d net.Conn
			var err error
			var used string
			for attempt := 1; attempt <= 6; attempt++ {
				used = resolvePeer(srv, *flagPeer, *flagPeerPort, peerAddr)
				dctx, dcancel := context.WithTimeout(ctx, 20*time.Second)
				d, err = srv.Dial(dctx, "tcp", used)
				dcancel()
				if err == nil {
					break
				}
				log.Printf("第 %d 次连 %s 失败: %v", attempt, used, err)
				if attempt < 6 {
					time.Sleep(3 * time.Second)
				}
			}
			if err != nil {
				emit("ERROR what=peer-dial-failed detail=%s", san(err.Error()))
				return
			}
			emit("CONNECTED peer=%s", used)
			pipeBoth(c, d)
		}(c)
	}
}

func main() {
	log.SetFlags(log.Ltime)
	log.SetOutput(os.Stderr) // stdout 留给协议行
	flag.Parse()

	if *flagParentPID > 0 {
		go watchParent(*flagParentPID)
	}

	switch *flagMode {
	case "expose":
		runExpose()
	case "connect":
		runConnect()
	case "":
		fmt.Fprintln(os.Stderr, "用法: pmrelay -mode expose|connect [选项]")
		flag.PrintDefaults()
		os.Exit(2)
	default:
		fmt.Fprintf(os.Stderr, "未知模式 %q\n", *flagMode)
		os.Exit(2)
	}
}

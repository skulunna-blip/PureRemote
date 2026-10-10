// pmrelay 探针 —— M4.0 闸门
//
// 目的：在**不装驱动、不用管理员**的前提下，验证 tsnet（Tailscale 官方 Go 库、
// 纯用户态）在本机网络（国内、双出口、国内侧 Symmetric NAT）上到底能不能跑通。
//
// 子命令：
//
//	serve  起 tailnet 节点，在 tailnet 上监听 :21118，转发到本机回显端口
//	dial   起另一个 tailnet 节点，连到 <目标>:21118，发 PING 看能不能收到回显
//	self   单节点自测（登录 + 监听 + 走自己的 tailnet IP）
//
// 为什么跨节点必须用两个独立进程：单进程自连可能被本机短路优化掉，
// 那就和 M4 调研里"RustDesk 打洞日志其实是自连"一样，是假阳性。
//
// 认证：优先读环境变量 TS_AUTHKEY；没有就打印交互式登录 URL（需要浏览器点一下）。
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
	"sort"
	"time"

	"tailscale.com/tsnet"
)

var (
	flagDir      = flag.String("dir", "", "tsnet 状态目录（默认 D:\\tsnet-state）")
	flagHostname = flag.String("hostname", "", "在 tailnet 上显示的主机名")
	flagPort     = flag.Int("port", 21118, "在 tailnet 上监听的端口")
	flagEchoPort = flag.Int("echo", 9999, "本机回显服务的端口（转发目标）")
	flagTimeout  = flag.Duration("timeout", 3*time.Minute, "登录超时")
	flagHold     = flag.Duration("hold", 0, "跑完测试后再保持运行多久（0 = 测完即退）")
)

func newServer() *tsnet.Server {
	d := *flagDir
	if d == "" {
		d = `D:\tsnet-state`
	}
	h := *flagHostname
	if h == "" {
		h = "pmrelay-probe"
	}
	if err := os.MkdirAll(d, 0o755); err != nil {
		log.Fatalf("建状态目录失败 %s: %v", d, err)
	}
	log.Printf("状态目录=%s  主机名=%s  authkey=%s", d, h, hasKey())
	return &tsnet.Server{
		Dir:      d,
		Hostname: h,
		AuthKey:  os.Getenv("TS_AUTHKEY"),
		Logf:     func(f string, a ...any) { log.Printf("[tsnet] "+f, a...) },
	}
}

func hasKey() string {
	if os.Getenv("TS_AUTHKEY") == "" {
		return "无（将打印登录 URL）"
	}
	return "有"
}

// up 登录 tailnet 并打印本节点身份。
func up(srv *tsnet.Server) netip.Addr {
	ctx, cancel := context.WithTimeout(context.Background(), *flagTimeout)
	defer cancel()

	log.Printf("正在登录 tailnet（超时 %s）...", *flagTimeout)
	// srv.Up 在没有 authkey 时会把登录 URL 经 Logf 打出来
	st, err := srv.Up(ctx)
	if err != nil {
		log.Fatalf("★ srv.Up 失败: %v", err)
	}
	log.Printf("★ 登录成功！")
	log.Printf("   DNSName     = %s", st.Self.DNSName)
	log.Printf("   HostName    = %s", st.Self.HostName)
	log.Printf("   TailscaleIPs= %v", st.Self.TailscaleIPs)
	log.Printf("   BackendState= %s", st.BackendState)
	if len(st.TailscaleIPs) == 0 {
		log.Fatalf("★ 登录了但没拿到 tailnet IP")
	}
	return st.TailscaleIPs[0]
}

// echoServer 起一个本机回显服务，作为"被转发的真实服务"。
func echoServer(port int) net.Listener {
	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		log.Fatalf("本机回显 listen 127.0.0.1:%d 失败: %v", port, err)
	}
	log.Printf("本机回显服务已起: %s", ln.Addr())
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go func(c net.Conn) {
				defer c.Close()
				c.SetDeadline(time.Now().Add(30 * time.Second))
				n, err := io.Copy(c, c) // 回显
				if err != nil {
					log.Printf("[echo] 转发结束(可能正常): %v, %d 字节", err, n)
				} else {
					log.Printf("[echo] 回显 %d 字节", n)
				}
			}(c)
		}
	}()
	return ln
}

// runServe: tailnet 上监听 → 转发到本机回显。
func runServe() {
	srv := newServer()
	defer srv.Close()
	up(srv)

	echoServer(*flagEchoPort)

	ln, err := srv.Listen("tcp", fmt.Sprintf(":%d", *flagPort))
	if err != nil {
		log.Fatalf("★ tailnet Listen :%d 失败: %v", *flagPort, err)
	}
	log.Printf("★★ tailnet 监听成功: %s → 转发到 127.0.0.1:%d", ln.Addr(), *flagEchoPort)

	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				log.Printf("tailnet Accept 结束: %v", err)
				return
			}
			log.Printf("[serve] tailnet 收到来自 %s 的连接", c.RemoteAddr())
			go func(c net.Conn) {
				defer c.Close()
				c.SetDeadline(time.Now().Add(60 * time.Second))
				dst, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", *flagEchoPort), 10*time.Second)
				if err != nil {
					log.Printf("[serve] 连本机回显失败: %v", err)
					return
				}
				defer dst.Close()
				done := make(chan struct{}, 2)
				go func() { io.Copy(dst, c); done <- struct{}{} }()
				go func() { io.Copy(c, dst); done <- struct{}{} }()
				<-done
			}(c)
		}
	}()

	log.Printf("SERVE-READY  — 现在可以用另一个节点 dial 本机的 tailnet 地址")
	hold(*flagHold)
}

// waitPeerOnline 等到目标 peer 在 netmap 里出现**且 Online**，再开始 dial。
//
// 为什么必须等：直接 dial 会在对端还没在 tailnet 上"上线"时白等一次 Windows
// 系统级 connect 超时（实测 20s+），而且重试同样慢。实测踩到过：同一套代码
// 第一次 PASS、第二次 FAIL，差别只是对端上线了没有。
// 正式版 pmrelay.exe 里"控制端先起、被控端还没上线"是常态，所以这段必须有。
func waitPeerOnline(ctx context.Context, srv *tsnet.Server, ip string) bool {
	lc, err := srv.LocalClient()
	if err != nil {
		log.Printf("   (取 LocalClient 失败，跳过等待: %v)", err)
		return false
	}
	deadline := time.Now().Add(*flagTimeout)
	for time.Now().Before(deadline) {
		if st, err := lc.Status(ctx); err == nil {
			for _, p := range st.Peer {
				for _, a := range p.TailscaleIPs {
					if a.String() == ip && p.Online {
						log.Printf("   peer %s 已上线（home relay=%s，直连地址=%q）", p.DNSName, p.Relay, p.CurAddr)
						return true
					}
				}
			}
		}
		time.Sleep(500 * time.Millisecond)
	}
	log.Printf("   ⚠ 等 peer 上线超时（%s），仍尝试 dial", *flagTimeout)
	return false
}

// runDial: 另一个独立节点，连 <目标>:port。
func runDial(target string) {
	if target == "" {
		log.Fatal("用法: probe dial <tailnet-IP-或-DNS名>")
	}
	srv := newServer()
	defer srv.Close()
	up(srv)

	addr := fmt.Sprintf("%s:%d", target, *flagPort)
	if a, perr := netip.ParseAddr(target); perr == nil { // IPv6 要带方括号
		addr = netip.AddrPortFrom(a, uint16(*flagPort)).String()
	}

	ctx, cancel := context.WithTimeout(context.Background(), *flagTimeout)
	defer cancel()

	waitPeerOnline(ctx, srv, target)

	// 重试：首次 dial 可能撞上 disco 还在协商（对端刚上线、DERP 刚连上）。
	log.Printf("★★ 正在通过 tailnet 连 %s ...", addr)
	var c net.Conn
	var err error
	t0 := time.Now()
	for attempt := 1; attempt <= 6; attempt++ {
		at := time.Now()
		c, err = srv.Dial(ctx, "tcp", addr)
		if err == nil {
			log.Printf("   连接建立，用时 %s（第 %d 次尝试，从开始共 %s）",
				time.Since(at), attempt, time.Since(t0))
			break
		}
		log.Printf("   第 %d 次 dial 失败（耗时 %s）: %v", attempt, time.Since(at), err)
		if attempt < 6 {
			time.Sleep(3 * time.Second)
		}
	}
	if err != nil {
		log.Fatalf("★ Dial 失败（已重试 6 次）: %v", err)
	}
	defer c.Close()

	msg := []byte(fmt.Sprintf("PING-through-tailnet-%d", time.Now().Unix()))
	c.SetDeadline(time.Now().Add(30 * time.Second))
	tRT := time.Now() // 只量首包往返，不含 dial/重试
	if _, err := c.Write(msg); err != nil {
		log.Fatalf("★ 写失败: %v", err)
	}
	buf := make([]byte, len(msg))
	if _, err := io.ReadFull(c, buf); err != nil {
		log.Fatalf("★ 读回显失败: %v", err)
	}
	ok := string(buf) == string(msg)
	log.Printf("★★ 收到回显: %q", buf)
	if ok {
		log.Printf("★★★ GATE-PASS  跨节点 tailnet 转发打通（首包往返 %s，含建连总耗时 %s）",
			time.Since(tRT), time.Since(t0))
	} else {
		log.Fatalf("★★★ GATE-FAIL  回显内容不匹配：期望 %q 实得 %q", msg, buf)
	}

	// 关键测量：这条连接到底走的是直连还是中继、中继落在哪个区域。
	// CurAddr 非空 = 直连（点对点）；CurAddr 空 + Relay 非空 = 走 DERP 中继。
	if lc, err := srv.LocalClient(); err == nil {
		if st, err := lc.Status(ctx); err == nil {
			log.Printf("   Self  Relay=%q  Addrs=%v", st.Self.Relay, st.Self.TailscaleIPs)
			for _, p := range st.Peer {
				path := "未知"
				switch {
				case p.CurAddr != "" && p.Relay != "":
					path = "直连(有中继备用)"
				case p.CurAddr != "":
					path = "★ 直连 P2P"
				case p.Relay != "":
					path = "★ 走中继 DERP"
				}
				log.Printf("   Peer %s  Online=%v  CurAddr=%q  Relay=%q  → %s",
					p.DNSName, p.Online, p.CurAddr, p.Relay, path)
			}
		} else {
			log.Printf("   (取 Status 失败: %v)", err)
		}
	}

	// 稳态延迟：首包要付打洞/握手/选路代价，真正决定"远程手感"的是后续每包的往返。
	// 只报首包会把一次性开销当成常态，反过来也会漏掉"连上后其实很慢"的情况。
	const nPing = 20
	var rtts []time.Duration
	for i := 0; i < nPing; i++ {
		m := []byte(fmt.Sprintf("PING-%02d-%s", i, "xxxxxxxxxxxxxxxxxxxx"))
		t := time.Now()
		if _, err := c.Write(m); err != nil {
			log.Printf("   稳态第 %d 包写失败: %v", i, err)
			break
		}
		b := make([]byte, len(m))
		if _, err := io.ReadFull(c, b); err != nil {
			log.Printf("   稳态第 %d 包读失败: %v", i, err)
			break
		}
		if string(b) != string(m) {
			log.Printf("   稳态第 %d 包内容不符", i)
			break
		}
		rtts = append(rtts, time.Since(t))
	}
	if len(rtts) > 0 {
		sort.Slice(rtts, func(i, j int) bool { return rtts[i] < rtts[j] })
		var sum time.Duration
		for _, d := range rtts {
			sum += d
		}
		log.Printf("   ★ 稳态 RTT (n=%d): min=%s  中位=%s  max=%s  均值=%s",
			len(rtts), rtts[0], rtts[len(rtts)/2], rtts[len(rtts)-1], sum/time.Duration(len(rtts)))
	}

	hold(*flagHold)
}

// runSelf: 单节点自测。注意这不是跨节点证据，只证明本节点自身通了。
func runSelf() {
	srv := newServer()
	defer srv.Close()
	ip := up(srv)
	echoServer(*flagEchoPort)

	ln, err := srv.Listen("tcp", fmt.Sprintf(":%d", *flagPort))
	if err != nil {
		log.Fatalf("★ tailnet Listen 失败: %v", err)
	}
	log.Printf("tailnet 监听: %s", ln.Addr())
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go func(c net.Conn) {
				defer c.Close()
				c.SetDeadline(time.Now().Add(30 * time.Second))
				io.Copy(c, c)
			}(c)
		}
	}()

	time.Sleep(2 * time.Second)
	addr := netip.AddrPortFrom(ip, uint16(*flagPort)).String()
	log.Printf("自测：连 %s（注意：这是同一节点自连，不算跨节点证据）", addr)
	ctx, cancel := context.WithTimeout(context.Background(), *flagTimeout)
	defer cancel()
	c, err := srv.Dial(ctx, "tcp", addr)
	if err != nil {
		log.Fatalf("★ 自测 Dial 失败: %v", err)
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(20 * time.Second))
	msg := []byte("PING-self")
	c.Write(msg)
	buf := make([]byte, len(msg))
	if _, err := io.ReadFull(c, buf); err != nil {
		log.Fatalf("★ 自测读回失败: %v", err)
	}
	log.Printf("★ 自测回显: %q", buf)
	hold(*flagHold)
}

func hold(d time.Duration) {
	if d <= 0 {
		return
	}
	log.Printf("保持运行 %s ...", d)
	time.Sleep(d)
}

func main() {
	log.SetFlags(log.Ltime)
	flag.Parse()
	if flag.NArg() < 1 {
		log.Fatal("用法: probe {serve|dial|self} [目标地址]")
	}
	switch flag.Arg(0) {
	case "serve":
		runServe()
	case "dial":
		runDial(flag.Arg(1))
	case "self":
		runSelf()
	default:
		log.Fatalf("未知子命令 %q", flag.Arg(0))
	}
}

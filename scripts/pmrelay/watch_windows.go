//go:build windows

package main

import (
	"log"
	"os"

	"golang.org/x/sys/windows"
)

// watchParent 盯着父进程（外壳）是否还活着，父进程一死就跟着退出，避免留下孤儿
// pmrelay 把 tailnet 节点一直挂着。
//
// 为什么不用"stdin 读到 EOF"那套：一开始就是那么写的，结果在 QProcess 下 stdin
// **立刻** EOF，pmrelay 起手就自杀 —— 而且退出码是 0，外壳侧连错误都看不到，只表现为
// "桥接一直没 ready"。同一个机制从真管道跑（`sleep 20 | pmrelay -watch-stdin`）
// 却能撑满 19 秒直到管道关闭，说明不可靠的是 QProcess 的 stdin，不是这套逻辑本身。
// 孤儿防护不能建在一个会误报的信号上。
//
// OpenProcess(SYNCHRONIZE) + WaitForSingleObject 是 Windows 上的标准做法：进程对象
// 在进程退出时变成有信号态，拿 0 超时等一下就知道了。不碰进程内存，只要一个同步句柄。
func watchParent(pid int) {
	if pid <= 0 {
		return
	}
	h, err := windows.OpenProcess(windows.SYNCHRONIZE, false, uint32(pid))
	if err != nil {
		log.Printf("打不开父进程 %d（%v），本次不做孤儿防护", pid, err)
		return
	}
	defer windows.CloseHandle(h)
	log.Printf("监视父进程 %d（它退出时我跟着退出）", pid)

	for {
		s, err := windows.WaitForSingleObject(h, 5000)
		if err != nil {
			log.Printf("等待父进程出错，停止监视: %v", err)
			return
		}
		if s == windows.WAIT_OBJECT_0 {
			log.Print("父进程已退出，跟着退出")
			os.Exit(0)
		}
		// WAIT_TIMEOUT = 父进程还活着，继续等
	}
}

//go:build !windows

package main

import "log"

// 非 Windows 上 pmrelay 只是开发/测试用的构建产物（外壳是 Windows 专用的），
// 不做孤儿防护 —— 那边的做法本来就该是另一套（System V 的 prctl / kqueue）。
func watchParent(pid int) {
	if pid > 0 {
		log.Print("非 Windows 平台不做父进程监视")
	}
}

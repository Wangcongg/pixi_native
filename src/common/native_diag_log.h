#pragma once
// native_diag_log.h — 排查态 C++ 日志的编译期总开关(三端共用)。
// 已定案排查遗留的高频 fprintf/ALOGI([pd-ack] / [spx-restore] / [native-fgui-transition] destroy hud / 每个文件一行的 [fetch])
// 默认不编入:测试与生产构建零成本;排障时用 -DPIXINATIVE_DIAG_LOG=1 重新打开。错误/警告类日志不受此开关影响。
#ifndef PIXINATIVE_DIAG_LOG
#define PIXINATIVE_DIAG_LOG 0
#endif

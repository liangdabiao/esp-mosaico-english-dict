// main/wd_assert.h —— 编译期断言宏（纯逻辑层与宿主测试共用）。
//
// 为什么要单独一个文件：这一层的模块各自只 include 自己需要的东西
// （manifest / content / quiz / progress / wrap / layout 互不依赖），
// 但它们都需要同一个"把预算钉死"的手段 —— 版式的像素预算、NVS 键名的长度
// 预算、字库的码点预算。原先这个宏定义在 wd_layout.h 里，于是 wd_manifest.h
// 想钉 NVS 键名上限时只能把宏复制一份，同一个东西两份定义迟早会漂。
//
// C 与 C++ 的写法不同（C11 的 _Static_assert / C++11 的 static_assert），
// 必须按语言分支：这一层同时被设备固件（idf.py build）和宿主测试
// （tools/run_host_tests.sh 用 gcc 编 .c）两条路编译，两条都要过。
#pragma once

#if defined(__cplusplus)
#define WD_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define WD_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

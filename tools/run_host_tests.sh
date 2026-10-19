#!/bin/sh
# tools/run_host_tests.sh —— 编译并运行全部纯逻辑层的宿主测试。
#
# 这一层跑起来是**秒级**的：不需要 ESP-IDF、不需要真机、不需要烧写、不需要联网。
# main/ 下那几个模块（blob 清单与条目解析、四选一、进度序列化、折行、版式）
# 本来就设计成不依赖 ESP-IDF 与 LVGL，为的就是这一点 ——
# 逻辑错误在这里暴露，而不是在几十分钟的交叉编译之后、或者烧进设备看屏幕才发现。
#
# 用法：
#     sh tools/run_host_tests.sh                    # 用默认的 blob
#     sh tools/run_host_tests.sh <blob 路径>        # 指定别的 blob
#
# 环境变量：
#     WD_HOST_CC     宿主编译器（默认 /d/esp-mo/mingw/w64devkit/bin/gcc.exe，
#                    见工作区 AGENTS.md：本机无系统 gcc，用 w64devkit 或 zig cc）
#     WD_TEST_OUT    输出目录（默认 <仓库根>/build-host/tests）
#
# 两个容易踩的地方，都在这里处理掉了：
#
#   1. **MSYS2_ARG_CONV_EXCL='*'**。不加的话 MSYS 会把 `/d/esp-mo/...` 这类参数
#      当路径做转换。但加了之后，参数会**原样**传给编译器 —— 而 gcc/clang 是
#      Windows 程序，看不懂 `/d/esp-mo/x.exe`，会报 "unable to open output directory"。
#      所以给编译器的输出路径必须是 `D:/...` 或相对形式，给本脚本自己用的
#      （mkdir / sed / 执行）必须是 `/d/...` 或相对形式。两个变量，各用各的。
#
#   2. **依赖列表里有空格**，所以不能用 `for x in $LIST` 分词。
#      改用 `while read` 逐行读，行内用 '|' 分隔。

set -u

REPO=$(cd "$(dirname "$0")/.." && pwd)

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        CC=${WD_HOST_CC:-/d/esp-mo/mingw/w64devkit/bin/gcc.exe}
        OUT_WIN=${WD_TEST_OUT:-build-host/tests}
        OUT_POSIX=$OUT_WIN
        ;;
    *)
        CC=${WD_HOST_CC:-cc}
        OUT_POSIX=${WD_TEST_OUT:-build-host/tests}
        OUT_WIN=$OUT_POSIX
        ;;
esac

BLOB=${1:-assets/content/wd_content.bin}

if ! command -v "$CC" >/dev/null 2>&1 && [ ! -x "$CC" ]; then
    echo "找不到宿主编译器：$CC（用 WD_HOST_CC 指定一个）" >&2
    exit 2
fi
if [ ! -f "$REPO/$BLOB" ] && [ ! -f "$BLOB" ]; then
    echo "找不到内容 blob：$BLOB（先跑 tools/wordbook/gen_content.py）" >&2
    exit 2
fi

mkdir -p "$OUT_POSIX" || exit 2
cd "$REPO" || exit 2
export MSYS2_ARG_CONV_EXCL='*'

# -Werror：警告当成错误。
# 这一层的警告基本都是真问题（未使用变量=写了没用的代码、隐式声明=漏了头文件），
# 而它们一旦积累起来就没人再看了。在这里挡住比事后清理便宜得多。
CFLAGS="-std=c11 -O2 -Wall -Wextra -Werror -I main -I tests"

# 格式：可执行名 | 测试源文件 | 需要一起编的 main/ 下的源文件
TESTS='
t_wd_manifest|tests/test_wd_manifest.c|main/wd_manifest.c
t_wd_content|tests/test_wd_content.c|main/wd_manifest.c main/wd_content.c
t_wd_quiz|tests/test_wd_quiz.c|main/wd_manifest.c main/wd_content.c main/wd_quiz.c
t_wd_progress|tests/test_wd_progress.c|main/wd_manifest.c main/wd_progress.c
t_wd_wrap|tests/test_wd_wrap.c|main/wd_manifest.c main/wd_content.c main/wd_wrap.c
t_wd_layout|tests/test_wd_layout.c|main/wd_manifest.c main/wd_content.c main/wd_layout.c
'

failed=0
compiled=0
passed=0
total=0

# while read 配 here-doc：**不创建子 shell**，所以下面的计数器能正常累加。
while IFS='|' read -r name src deps; do
    [ -z "$name" ] && continue
    total=$((total + 1))

    printf '=== %-16s ' "$name"

    if ! $CC $CFLAGS -o "$OUT_WIN/$name.exe" "$src" $deps 2>"$OUT_POSIX/$name.log"; then
        echo "编译失败"
        sed 's/^/    /' "$OUT_POSIX/$name.log"
        failed=$((failed + 1))
        continue
    fi
    compiled=$((compiled + 1))

    if "$OUT_POSIX/$name.exe" "$BLOB" 2>"$OUT_POSIX/$name.run.log"; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
    fi
    sed 's/^/    /' "$OUT_POSIX/$name.run.log"
done <<EOF
$TESTS
EOF

echo
echo "-------------------------------------------------------"
echo "共 $total 个测试：编译 $compiled 个，通过 $passed 个，失败 $failed 个"

if [ "$failed" -ne 0 ]; then
    echo "有测试未通过。" >&2
    exit 1
fi
echo "全部通过。"

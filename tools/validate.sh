#!/usr/bin/env bash
# tools/validate.sh —— english-dict 的两道门禁。
#
#   --static    不需要 ESP-IDF、不需要真机、不需要联网：文档规则、生成物是否
#               与源同步、宿主测试、门禁脚本自身的单元测试。
#   --firmware  冷编译 + 合并 + 校验 + 归档，产出 build/FoloToy-AI-Passport-full.bin。
#   --all       两者都跑（默认）。
#
# 与兄弟仓库（qiaopi-quiz / ancient-poets / ...）的 validate.sh 同一形制，
# 差别只在"本仓库有哪些生成物、有哪些测试"这两处列表。
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin actionlint_out actionlint_rc font_py

    # ① 文档规则：每个 .md 要有 .zh_CN.md 配对、英文默认页不许出现汉字、
    #    链接要能 resolve、根目录只允许白名单里的 Markdown。
    #    这一条不过，后面跑多久都没意义 —— 所以放第一个。
    python3 tools/check_repo.py

    # ② 生成物与源必须同步。
    #    内容侧：words.txt 改了却忘了重新生成 wd_content.bin / wd_content_blob.c，
    #    在这里就失败（--check 不重新生成，只核对）。
    PYTHONDONTWRITEBYTECODE=1 python3 tools/wordbook/gen_content.py --check
    #    字库侧：界面文案变一个字（注意：扫的是 main/ 下**全部**字符串字面量，
    #    日志与错误文案也算），字符集就变了，字库必须重算。
    #    这一步要 fontTools 才能逐个码点核对母字体覆盖。**优先用 WD_FONT_PYTHON
    #    指定的解释器**（本机是那个装了 fontTools 的 venv），否则退回 python3；
    #    两者都没有 fontTools 时明确跳过并说明，不假装通过 —— CI 镜像不装 fontTools，
    #    但本机装了却不用它，等于让这一条门禁长期空转。
    font_py="${WD_FONT_PYTHON:-python3}"
    if "${font_py}" -c "import fontTools" >/dev/null 2>&1; then
        PYTHONDONTWRITEBYTECODE=1 "${font_py}" tools/wordbook/gen_font.py --check
    else
        echo "skip: tools/wordbook/gen_font.py --check（${font_py} 无 fontTools；字库覆盖未校验）" >&2
    fi

    # ③ GitHub Actions 的 workflow 必须可用 —— 而且所有 action 都要钉到完整
    #    提交 SHA（检查在 check_repo.py 里，这里只保证 YAML 本身是合法的）。
    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        #    install-actionlint.sh 只认 Linux/Darwin：在 MINGW64（本机 Git Bash）
        #    上它打印 "Unsupported actionlint platform: ..." 并以非 0 退出。
        #    那不是 workflow 的问题，所以这一步失败只记因、不中断 —— 否则 set -e
        #    会把下面两步（宿主测试、门禁自测）一起掐掉，看起来像"测试没过"，
        #    实际是压根没跑。
        actionlint_bin="$(./tools/install-actionlint.sh 2>&1 || true)"
    fi
    if [[ ! -x "${actionlint_bin}" ]]; then
        echo "skip: actionlint 不可用，workflow YAML 未校验" >&2
        echo "      ${actionlint_bin}" >&2
    else
        actionlint_rc=0
        actionlint_out="$("${actionlint_bin}" -color .github/workflows/*.yml 2>&1)" || actionlint_rc=$?
        if [[ "${actionlint_rc}" -ne 0 ]]; then
            printf '%s\n' "${actionlint_out}" >&2
            return 1
        fi
    fi

    # ④ 纯逻辑层的宿主测试。六个模块全部与 ESP-IDF/LVGL 解耦，秒级跑完
    #    （160113 项断言）。用脚本而不是在这里逐条展开：编译命令与依赖列表
    #    只能有一份，否则加了测试就会漏改一处。
    sh tools/run_host_tests.sh

    # ⑤ 门禁脚本自己的单元测试 —— 一个「坏仓库会被判 FAIL」的检查脚本，
    #    必须先证明它对坏输入真的会 FAIL。
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    echo "Host tests: PASS"
}

# 子 shell 而不是函数体：这样 EXIT trap 只覆盖固件这一段，
# 不会在 --static 跑完之后也去删一个不存在的目录。
run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    # 固定目录而不是 mktemp：本机的 /tmp 落在 C: 盘，实测冷编译在那里会从
    # 50 单元/分掉到 19 单元/分（同一条工具链编 3 行 .c 在 C: 与 D: 都是 2.4~3.5 秒，
    # 所以不是磁盘慢，是 C: 临时目录的清理与扫描在抢资源）。
    # 目录每次先清空，避免上一次的 CMakeCache 把"冷编译"变成增量编译。
    validation_build_dir="${VALIDATE_BUILD_DIR:-D:/esp/edcold}"
    rm -rf -- "${validation_build_dir}"
    mkdir -p -- "${validation_build_dir}"

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac

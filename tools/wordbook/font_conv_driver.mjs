// lv_font_conv 驱动：从 JSON 文件读 argv 再调它的 run() API。
// 为什么存在：GB2312 一级 3755 字的 --symbols 约 11.5KB，超过 Windows 命令行的
// 可靠长度（家族前几个应用字符少没踩到）；走文件传参就没有任何长度限制。
// 用法：node font_conv_driver.mjs <args.json>   args.json = {"argv": ["--font", ...]}
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";

const argsFile = process.argv[2];
if (!argsFile) {
    console.error("用法：node font_conv_driver.mjs <args.json>");
    process.exit(2);
}
const { argv } = JSON.parse(readFileSync(argsFile, "utf8"));
const pkgDir = process.env.LV_FONT_CONV_DIR;
const require = createRequire(pkgDir + "/package.json");
// lv_font_conv.js 主文件被 require 时会无条件跑 CLI（读 process.argv），所以
// 这里直接用 lib/cli 的 run(argv) —— 它才是「参数数组 → 编译 → 写文件」的入口。
const cli = require("./lib/cli.js");
await cli.run(argv);

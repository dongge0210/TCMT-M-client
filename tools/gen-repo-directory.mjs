#!/usr/bin/env node
// 重新生成 docs/repo-directory.md。
//
// 这份文档以前是手写的，停在 144 个文件 —— 实际已经三百多了，AvaloniaUI 下面
// Core/ Localization/ Themes/ designs/ 整片整片不在里面。手写的东西没人会在加文件时
// 同步改，所以改成生成，加完目录跑一次：
//
//     node tools/gen-repo-directory.mjs
//
// ⚠ 「-- 说明」那一列是**从旧文档照抄**的（见 carryOverDescriptions），不是从代码里推出来的。
// 所以重跑这个脚本**永远不会**修正一句过时的说明，也不会给新文件补说明 —— 它只改文件树的
// 增删。改说明的唯一办法是直接改 docs/repo-directory.md 里那行文字，下次生成会把改过的
// 文字当既有描述搬过去。
//
// 2026-10-13 踩过一次：AGENTS.md 早已压成一句指向 CLAUDE.md 的指针，说明列却还写着
// "Agent behavioral rules"，而当时的交接记录说「那是生成物，重跑生成器才更新」—— 重跑
// 纹丝不动。凡是看到说明列与事实不符，直接改文档，不要指望重跑。
//
// 用 .mjs 不用 .py：仓库里已经有 tools/mcp-http-bridge.mjs，工具脚本走 Node 是现成的
// 先例，client 里不该再多个 Python。

import { execFileSync } from "node:child_process";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const DOC = join(ROOT, "docs", "repo-directory.md");

const KNOWN_EXT =
  "h|cpp|mm|c|cs|axaml|md|json|cmake|yml|txt|sln|vcxproj|filters|sh|mjs|manifest|ico|py";

/** 旧文档里能对上的「文件名 -- 说明」照抄，其余留空 —— 编出来的说明比没有更坏。 */
function carryOverDescriptions() {
  if (!existsSync(DOC)) return new Map();
  const text = readFileSync(DOC, "utf8");
  const re = new RegExp(`([A-Za-z0-9_./-]+\\.(?:${KNOWN_EXT}))\\s+--\\s+(.+)`, "g");
  const known = new Map();
  for (const m of text.matchAll(re)) {
    known.set(m[1].split("/").pop(), m[2].trim());
  }
  return known;
}

function buildTree(files) {
  const tree = new Map();
  for (const f of [...files].sort()) {
    const parts = f.split("/");
    let node = tree;
    for (const p of parts.slice(0, -1)) {
      if (!node.has(p)) node.set(p, new Map());
      node = node.get(p);
    }
    if (!node.has("__files__")) node.set("__files__", []);
    node.get("__files__").push(parts[parts.length - 1]);
  }
  return tree;
}

function render(node, known, prefix, out) {
  for (const [key, value] of node) {
    if (key === "__files__") continue;
    out.push(`${prefix}|-- ${key}/`);
    render(value, known, `${prefix}|   `, out);
  }
  for (const f of node.get("__files__") ?? []) {
    const desc = known.get(f);
    out.push(`${prefix}|-- ${f}${desc ? `    -- ${desc}` : ""}`);
  }
}

const files = execFileSync("git", ["ls-files"], { cwd: ROOT, encoding: "utf8" })
  .split("\n")
  .filter(Boolean);

const out = [
  "# TCMT Repository Directory",
  "",
  `${files.length} tracked files. 由 \`git ls-files\` 生成 —— 加完目录跑一次`,
  "`node tools/gen-repo-directory.mjs`，别手改这一份。",
  "",
  "```",
];
render(buildTree(files), carryOverDescriptions(), "", out);
out.push("```");

writeFileSync(DOC, out.join("\n") + "\n", "utf8");
const described = out.filter((l) => l.includes(" -- ")).length;
console.log(`docs/repo-directory.md: ${files.length} 个文件，${described} 条带说明`);

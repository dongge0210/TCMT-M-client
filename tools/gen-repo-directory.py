#!/usr/bin/env python3
"""重新生成 docs/repo-directory.md。

这份文档以前是手写的，停在 144 个文件 —— 实际已经 317 个了，AvaloniaUI 下面
Core/ Localization/ Themes/ designs/ 整片整片不在里面。手写的东西没人会在加文件时
同步改，所以改成生成，加完目录跑一次就行：

    python3 tools/gen-repo-directory.py

已有的说明文字会按文件名继承过来（旧文档里写成「路径 -- 说明」的那些），
新增的文件只有名字，没有说明 —— 假的说明比没有说明更坏。
"""
import collections
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
DOC = ROOT / "docs" / "repo-directory.md"


def carry_over_descriptions():
    """旧文档里能对上的「文件名 -- 说明」照抄，其余留空。"""
    if not DOC.exists():
        return {}
    text = DOC.read_text(encoding="utf-8")
    known = {}
    for m in re.finditer(
        r"([A-Za-z0-9_./\-]+\.(?:h|cpp|mm|c|cs|axaml|md|json|cmake|yml|txt|sln"
        r"|vcxproj|filters|sh|mjs|manifest|ico|py))\s+--\s+(.+)",
        text,
    ):
        known[m.group(1).split("/")[-1]] = m.group(2).strip()
    return known


def build_tree(files):
    tree = collections.OrderedDict()
    for f in sorted(files):
        parts = f.split("/")
        node = tree
        for p in parts[:-1]:
            node = node.setdefault(p, {})
        node.setdefault("__files__", []).append(parts[-1])
    return tree


def render(tree, known):
    lines = []

    def emit(node, prefix):
        for d in [k for k in node if k != "__files__"]:
            lines.append(f"{prefix}|-- {d}/")
            emit(node[d], prefix + "|   ")
        for f in node.get("__files__", []):
            desc = known.get(f)
            lines.append(f"{prefix}|-- {f}" + (f"    -- {desc}" if desc else ""))

    emit(tree, "")
    return lines


def main():
    files = subprocess.run(
        ["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.split()

    lines = [
        "# TCMT Repository Directory",
        "",
        f"{len(files)} tracked files. 由 `git ls-files` 生成 —— 加完目录跑一次",
        "`python3 tools/gen-repo-directory.py`，别手改这一份。",
        "",
        "```",
    ]
    lines += render(build_tree(files), carry_over_descriptions())
    lines.append("```")

    DOC.write_text("\n".join(lines) + "\n", encoding="utf-8")
    described = sum(1 for l in lines if " -- " in l)
    print(f"{DOC.relative_to(ROOT)}: {len(files)} 个文件，{described} 条带说明")


if __name__ == "__main__":
    main()

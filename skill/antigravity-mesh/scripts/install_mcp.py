#!/usr/bin/env python3
"""Cross-platform installer for ag-mesh Antigravity skill and MCP config."""
import json
import os
import shutil
import sys
from pathlib import Path


def main() -> int:
    home = Path.home()
    ag_dir = Path(os.environ.get("ANTIGRAVITY_HOME", home / ".gemini" / "antigravity"))
    ag_dir.mkdir(parents=True, exist_ok=True)

    skill_src = Path(__file__).resolve().parent.parent
    skill_dst = ag_dir / "skills" / "antigravity-mesh"
    skill_dst.parent.mkdir(parents=True, exist_ok=True)
    if skill_src != skill_dst:
        shutil.copytree(skill_src, skill_dst, dirs_exist_ok=True)

    binary = sys.argv[1] if len(sys.argv) > 1 else (shutil.which("ag-mesh") or "ag-mesh")
    cfg_file = sys.argv[2] if len(sys.argv) > 2 else ""

    mcp_path = ag_dir / "mcp_config.json"
    data = {"mcpServers": {}}
    if mcp_path.exists():
        try:
            loaded = json.loads(mcp_path.read_text(encoding="utf-8"))
            if isinstance(loaded, dict):
                data = loaded
                if not isinstance(data.get("mcpServers"), dict):
                    data["mcpServers"] = {}
        except Exception:
            pass

    args = ["mcp"]
    if cfg_file:
        args.extend(["--config", cfg_file])

    data["mcpServers"]["ag-mesh"] = {
        "command": binary,
        "args": args,
    }

    tmp_path = mcp_path.with_suffix(".json.tmp")
    tmp_path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    tmp_path.replace(mcp_path)

    print(f"Installed antigravity-mesh skill -> {skill_dst}")
    print(f"Registered ag-mesh MCP server -> {mcp_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

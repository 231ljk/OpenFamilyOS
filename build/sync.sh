#!/usr/bin/env bash
# SPDX-License-Identifier: MulanPSL-2.0
# build/sync.sh — 主仓 → 子仓单向同步（对应 README 第六章「同步到其他仓库」）
#
# 用法：
#   ./build/sync.sh all          # 同步所有子仓
#   ./build/sync.sh kernel       # 只同步内核子仓
#   ./build/sync.sh --list       # 只看映射表
#
# 机制：GitHub Contents API「创建/更新文件」——先 GET 取既有 blob sha，
#       再 PUT（存在即更新、不存在即创建），逐文件幂等同步。
# 凭据：环境变量 GITHUB_TOKEN（fine-grained PAT，含目标子仓 contents:write）。
#       未设置时只打印计划（dry-run），绝不发起写请求。

set -euo pipefail
TOP="$(cd "$(dirname "$0")/.." && pwd)"
API="https://api.github.com"

# 映射：子仓名|主仓源目录|子仓仓库路径
MAP="kernel|kernel|231ljk/OpenFamilyOS-kernel
system|services|231ljk/OpenFamilyOS-system
superring|services/super-ring|231ljk/OpenFamilyOS-superring
familyhome|services/home-bridge|231ljk/OpenFamilyOS-familyhome
aibus|services/ai-bus|231ljk/OpenFamilyOS-aibus
education|edu|231ljk/OpenFamilyOS-education
health|services/health|231ljk/OpenFamilyOS-health
tools|tools|231ljk/OpenFamilyOS-tools"

api() {  # api METHOD REPO PATH [BODY_FILE]
  if [ -n "${3:-}" ]; then
    curl -sf -X "$1" "$API/repos/$2" \
      -H "Authorization: Bearer $GITHUB_TOKEN" \
      -H "Accept: application/vnd.github+json" \
      -d @"$3"
  else
    curl -sf -X GET "$API/repos/$2" \
      -H "Authorization: Bearer $GITHUB_TOKEN" \
      -H "Accept: application/vnd.github+json"
  fi
}

sync_one() {  # name srcdir remote
  local name="$1" src="$2" remote="$3" files f
  echo "==> [$name] $src/ -> $remote"
  files=$(cd "$TOP" && find "$src" -type f \
          ! -path '*/build/*' ! -path '*/__pycache__/*' | LC_ALL=C sort)
  [ -z "$files" ] && { echo "    跳过：源目录为空"; return 0; }

  local n=0
  while IFS= read -r f; do
    local rel="${f#"$src"/}"
    if [ -z "${GITHUB_TOKEN:-}" ]; then
      echo "    (dry-run) $rel"
      n=$((n + 1)); continue
    fi
    # 1) 取既有 sha（404 = 新建）
    local sha
    sha=$(curl -s -o /dev/null -w "%{http_code}" \
            -H "Authorization: Bearer $GITHUB_TOKEN" \
            "$API/repos/$remote/contents/$rel?ref=main")
    local body tmp
    tmp=$(mktemp)
    python3 - "$TOP/$f" "$rel" "$name" <<'PY' > "$tmp"
import base64, json, sys
path, rel, name = sys.argv[1:4]
with open(path, "rb") as f:
    data = base64.b64encode(f.read()).decode()
print(json.dumps({"message": f"sync({name}): {rel}", "content": data}))
PY
    # 2) 如已存在则补 sha 字段再 PUT
    if [ "$sha" = "200" ]; then
      local old
      old=$(api GET "$remote/contents/$rel?ref=main" | \
            python3 -c 'import json,sys; print(json.load(sys.stdin)["sha"])')
      python3 - "$tmp" "$old" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    d = json.load(f)
d["sha"] = sys.argv[2]
with open(sys.argv[1], "w") as f:
    json.dump(d, f)
PY
    fi
    api PUT "$remote/contents/$rel" "$tmp" >/dev/null \
      && echo "    ✓ $rel" || echo "    ✗ $rel"
    rm -f "$tmp"
    n=$((n + 1))
  done <<< "$files"
  echo "    合计 $n 个文件"
  [ -z "${GITHUB_TOKEN:-}" ] && \
    echo "    未设置 GITHUB_TOKEN：以上仅为计划，未执行写操作"
}

case "${1:-all}" in
  --list) echo "$MAP" | column -t -s'|' ; exit 0 ;;
  -h|--help) sed -n '3,14p' "$0" ; exit 0 ;;
  all)
    echo "$MAP" | while IFS='|' read -r n s r; do sync_one "$n" "$s" "$r"; done ;;
  *)
    line=$(echo "$MAP" | grep "^$1|" ) || { echo "未知子仓: $1（--list 查看）"; exit 1; }
    IFS='|' read -r n s r <<< "$line"
    sync_one "$n" "$s" "$r" ;;
esac

#!/bin/bash
set -u
R=/home/hcy/hakios-arm64e-xuanyuan
cd "$R" || exit 1

# ---------- 1. .gitignore: keep multi-GB images and build trees out of git ----------
cat >> .gitignore <<'EOF'

# --- local artifacts that must never enter the repository ---
ios26/
sysroot-work/
userland/xnu/
userland/xnu-deps/build/
userland/userland/root/
userland/userland/obj/
userland/userland/root.apfs
userland/userland/*.apfs
*.tar.xz
payload/
phone-ios26-test/
*.bundle
EOF
echo "gitignore updated"

# ---------- 2. helper scripts from the Windows side, trimmed to the ones worth keeping ----------
mkdir -p tools/xuanyuan-helpers
for f in /mnt/x/ios/scripts/*.sh; do
  b=$(basename "$f")
  cp -f "$f" "tools/xuanyuan-helpers/$b" 2>/dev/null || true
done
ls tools/xuanyuan-helpers | wc -l
echo "helpers copied"

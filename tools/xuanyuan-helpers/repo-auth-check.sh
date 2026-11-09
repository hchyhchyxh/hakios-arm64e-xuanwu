#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
echo "=== tokens in env ==="
env | grep -iE "gh_token|github_token|token" | sed 's/=.*/=<set>/' | head
echo "=== gh / hub ==="
command -v gh || echo "no gh"
command -v hub || echo "no hub"
echo "=== git identity ==="
git config user.name; git config user.email
echo "=== ssh test ==="
timeout 15 ssh -o StrictHostKeyChecking=no -T git@github.com 2>&1 | head -2
echo "=== credential files ==="
ls -la ~/.git-credentials ~/.config/gh 2>/dev/null
echo "=== untracked dir sizes ==="
du -sh ios26 sysroot-work userland/xnu 2>/dev/null
echo "=== dirty mesa dir ==="
du -sh userland/tools/mesa 2>/dev/null
echo "=== docs count ==="
ls docs/xuanyuan | wc -l

#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
echo "=== git remote ==="
git remote -v
echo "=== branch/status ==="
git branch --show-current
git status --short | head -40
echo "=== untracked/changed count ==="
git status --porcelain | wc -l
echo "=== repo size ==="
du -sh .git 2>/dev/null
git count-objects -vH | grep -E "size-pack|count"
echo "=== top-level ==="
ls
echo "=== gitignore ==="
cat .gitignore 2>/dev/null | head -30
echo "=== gh cli ==="
which gh && gh auth status 2>&1 | head -5

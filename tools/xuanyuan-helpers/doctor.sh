#!/bin/bash
# Move the checkout into hcy's home and run HakiOS's own doctor
set -e
mkdir -p /home/hcy/hakios
rsync -a --delete /root/hakios/ /home/hcy/hakios/
chown -R hcy:hcy /home/hcy/hakios
echo "=== checkout ==="
ls -la /home/hcy/hakios | head -8
su - hcy -c 'cd ~/hakios && tools/doctor.sh; echo "doctor exit=$?"' 2>&1
echo "=== done ==="

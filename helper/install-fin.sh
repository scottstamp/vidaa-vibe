#!/bin/sh
# Install or update the YouTube helper as a systemd service on a Debian/Ubuntu
# host (written for "fin", the Jellyfin box). Run as root, with ythelper.py,
# ytlounge.py, ytextra.py and ytauth.py copied to /tmp first:
#   scp helper/ythelper.py helper/ytlounge.py helper/ytextra.py helper/ytauth.py root@HOST:/tmp/ && ssh root@HOST 'sh -s' < helper/install-fin.sh
# Needs python3-venv and a JS runtime (node) for yt-dlp's YouTube challenge.
#
# What it makes:
#   user vibeyt (system, no login); /opt/vibe-ythelper (script + venv);
#   /var/lib/vibe-ythelper/state.json (follows, positions);
#   vibe-ythelper.service (port 8770, restarts on failure);
#   vibe-ythelper-update.timer (daily: pip install -U yt-dlp, restart).
# Way back: systemctl disable --now vibe-ythelper vibe-ythelper-update.timer;
#   rm -rf /opt/vibe-ythelper /etc/systemd/system/vibe-ythelper*; userdel vibeyt
#   (/var/lib/vibe-ythelper keeps the state).
set -eu
APP=/opt/vibe-ythelper
DATA=/var/lib/vibe-ythelper
[ -f /tmp/ythelper.py ] && [ -f /tmp/ytlounge.py ] && [ -f /tmp/ytextra.py ] && [ -f /tmp/ytauth.py ] || { echo "copy ythelper.py, ytlounge.py, ytextra.py and ytauth.py to /tmp first" >&2; exit 1; }
command -v node >/dev/null || { echo "node missing" >&2; exit 1; }

id vibeyt >/dev/null 2>&1 || useradd --system --home-dir $DATA --shell /usr/sbin/nologin vibeyt
mkdir -p $APP $DATA
install -m 0644 /tmp/ytlounge.py $APP/ytlounge.py.new && mv $APP/ytlounge.py.new $APP/ytlounge.py
install -m 0644 /tmp/ytextra.py $APP/ytextra.py.new && mv $APP/ytextra.py.new $APP/ytextra.py
install -m 0644 /tmp/ytauth.py $APP/ytauth.py.new && mv $APP/ytauth.py.new $APP/ytauth.py
install -m 0644 /tmp/ythelper.py $APP/ythelper.py.new && mv $APP/ythelper.py.new $APP/ythelper.py
rm -f /tmp/ythelper.py /tmp/ytlounge.py /tmp/ytextra.py /tmp/ytauth.py
[ -x $APP/venv/bin/python ] || python3 -m venv $APP/venv
$APP/venv/bin/pip install -q -U pip "yt-dlp[default]"
chown -R vibeyt:vibeyt $DATA
chmod 0750 $DATA

cat > /etc/systemd/system/vibe-ythelper.service <<EOF
[Unit]
Description=vidaa-vibe YouTube helper (yt-dlp, SponsorBlock) for the TV
After=network-online.target
Wants=network-online.target

[Service]
User=vibeyt
Group=vibeyt
Environment=HOME=$DATA
ExecStart=$APP/venv/bin/python -I $APP/ythelper.py --port 8770 --state $DATA/state.json --js-runtime node
Restart=on-failure
RestartSec=5
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
ReadWritePaths=$DATA
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/vibe-ythelper-update.service <<EOF
[Unit]
Description=Update yt-dlp for the vidaa-vibe YouTube helper

[Service]
Type=oneshot
ExecStart=$APP/venv/bin/pip install -q -U "yt-dlp[default]"
ExecStartPost=/bin/systemctl try-restart vibe-ythelper.service
EOF

cat > /etc/systemd/system/vibe-ythelper-update.timer <<EOF
[Unit]
Description=Daily yt-dlp update for the vidaa-vibe YouTube helper

[Timer]
OnCalendar=*-*-* 04:30
RandomizedDelaySec=30m
Persistent=true

[Install]
WantedBy=timers.target
EOF

systemctl daemon-reload
systemctl enable --now vibe-ythelper.service vibe-ythelper-update.timer
systemctl restart vibe-ythelper.service
sleep 3
systemctl --no-pager --lines=3 status vibe-ythelper.service | head -n 12

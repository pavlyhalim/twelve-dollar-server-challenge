#!/usr/bin/env bash
# Load test on a GitHub Actions runner, shaped like the benchmark box: the server alone in a KVM guest
# with 1 vCPU and 2 GiB (Ubuntu 24.04, kernel 6.8), the load generator on the runner over a tap link
# with vhost-net (kernel networking, no userspace switch). Each run follows bench/load.js's stages:
# ramp 60 s, hold 300 s (HOLD=...), ramp down 30 s, on a fresh copy of the seed, like a scored hold.
#   bash loadtest/run.sh "P:340000,320000,300000 M:280000"   M = the merged build (#233), P = the PR (#245)
# A list after the colon is tried from the left until one level passes (top down: one pass answers it).
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUNS=${1:-"P:340000,320000,300000,280000,260000 M:280000"}
MERGED_REF=${MERGED_REF:-fff6f25} # upstream main with #233 merged (its submission == 3f5c305)
W=/tmp/lt; LOGS=$W/logs; mkdir -p $W $LOGS
VMIP=10.0.0.2
SSHO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5 -i $W/key"
vm() { ssh $SSHO ubuntu@$VMIP "$@"; }
log() { echo "[$(date +%H:%M:%S)] $*"; }

vm_start() {
  sudo qemu-system-x86_64 -enable-kvm -cpu host -smp 1 -m 2048 \
    -drive file=$W/disk.qcow2,if=virtio -drive file=$W/seed.iso,if=virtio,format=raw \
    -netdev tap,id=n0,ifname=tap0,script=no,downscript=no,vhost=on -device virtio-net-pci,netdev=n0,mac=52:54:00:12:34:56 \
    -display none -serial file:$W/serial.log -daemonize -pidfile $W/qemu.pid
  for _ in $(seq 120); do vm true 2>/dev/null && return 0; sleep 3; done
  echo "guest did not answer on ssh"; tail -30 $W/serial.log; return 1
}
vm_restart() {
  log "restarting the guest"
  sudo kill "$(cat $W/qemu.pid)" 2>/dev/null; sleep 3
  vm_start
}

log "== host: $(nproc) CPUs, $(awk '/MemTotal/ {print int($2/1048576)}' /proc/meminfo) GB, $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2)"
ls -l /dev/kvm || { echo "no /dev/kvm on this runner"; exit 1; }
sudo apt-get install -y -qq qemu-system-x86 qemu-utils cloud-image-utils >/dev/null
curl -fsSL -o $W/base.img https://cloud-images.ubuntu.com/releases/noble/release/ubuntu-24.04-server-cloudimg-amd64.img
qemu-img create -q -f qcow2 -b $W/base.img -F qcow2 $W/disk.qcow2 12G
ssh-keygen -q -t ed25519 -N '' -f $W/key
cat > $W/user-data <<EOF
#cloud-config
users:
  - name: ubuntu
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    ssh_authorized_keys: [$(cat $W/key.pub)]
package_update: true
packages: [build-essential, sqlite3, curl, unzip, jq]
EOF
cat > $W/network-config <<EOF
version: 2
ethernets:
  nic0:
    match: {macaddress: "52:54:00:12:34:56"}
    addresses: [$VMIP/24]
    routes: [{to: default, via: 10.0.0.1}]
    nameservers: {addresses: [8.8.8.8, 1.1.1.1]}
EOF
cloud-localds --network-config=$W/network-config $W/seed.iso $W/user-data

# tap link with NAT for the guest's apt; eight extra source addresses for the load generator
sudo ip tuntap add tap0 mode tap
sudo ip addr add 10.0.0.1/24 dev tap0
for i in $(seq 100 107); do sudo ip addr add 10.0.0.$i/24 dev tap0; done
sudo ip link set tap0 up
sudo sysctl -qw net.ipv4.ip_forward=1 net.ipv4.ip_local_port_range="1024 65535" net.ipv4.tcp_max_tw_buckets=2000000 net.ipv4.tcp_tw_reuse=1 # load generator side only
EXT=$(ip route show default | awk '{print $5; exit}')
sudo iptables -t nat -A POSTROUTING -s 10.0.0.0/24 -o "$EXT" -j MASQUERADE
sudo iptables -I FORWARD -i tap0 -j ACCEPT
sudo iptables -I FORWARD -o tap0 -m state --state RELATED,ESTABLISHED -j ACCEPT
# The NAT above loads connection tracking, whose table (262,144 entries on this runner) would drop
# packets for every connection past it, around 210-265k: the load generator's traffic to the guest
# is left untracked (the box has no firewall and no tracking table), and the limit raised anyway.
sudo iptables -t raw -I OUTPUT -o tap0 -d $VMIP -j NOTRACK
sudo iptables -t raw -I PREROUTING -i tap0 -s $VMIP -d 10.0.0.0/24 -j NOTRACK
sudo sysctl -qw net.netfilter.nf_conntrack_max=4000000 2>/dev/null || true

log "== booting the guest"
vm_start || exit 1
vm 'cloud-init status --wait >/dev/null; echo "guest: $(uname -r), $(nproc) vCPU, MemTotal $(awk "/MemTotal/ {print int(\$2/1024)}" /proc/meminfo) MB, $(grep -m1 "model name" /proc/cpuinfo | cut -d: -f2)"'

log "== copying the two builds and the seed"
rm -rf $W/merged && mkdir -p $W/merged
git -C "$ROOT" archive "$MERGED_REF" submissions/c-iouring-pavlyhalim | tar -x -C $W/merged
tar -C "$ROOT/submissions" --exclude=bin --exclude=pgo -cf - c-iouring-pavlyhalim | vm 'sudo mkdir -p /srv/P && sudo chown ubuntu /srv/P && tar -x -C /srv/P --strip-components=1'
tar -C $W/merged/submissions -cf - c-iouring-pavlyhalim | vm 'sudo mkdir -p /srv/M && sudo chown ubuntu /srv/M && tar -x -C /srv/M --strip-components=1'
scp $SSHO "$ROOT/seed/feed.db" "$ROOT/loadtest/vm-server.sh" "$ROOT/loadtest/calib.c" ubuntu@$VMIP:/tmp/
vm 'sudo mv /tmp/feed.db /tmp/vm-server.sh /srv/ && gcc -O2 -o /tmp/calib /tmp/calib.c && sudo mv /tmp/calib /srv/'

log "== building both (install.sh as root, build.sh with PGO as the user)"
for b in M P; do
  vm "cd /srv/$b && sudo bash install.sh >/dev/null 2>&1; bash build.sh 2>&1 | grep -E 'train:|built|rror'" | sed "s/^/$b: /"
done

gcc -O2 -o $W/loadgen "$ROOT/loadtest/loadgen.c"
SRC=$(seq -s, -f "10.0.0.%g" 100 107)
printf '%-6s %8s  %-58s %8s %s\n' build users result "req/s" server > $LOGS/summary.txt
one_run() { # build users -> 0 on PASS
  local b=$1 n=$2
  log "== $b at $n users"
  vm true 2>/dev/null || vm_restart || exit 1
  vm "sudo bash /srv/vm-server.sh start /srv/$b" | tee $LOGS/$b-$n.log
  sudo env LG_SRC=$SRC prlimit --nofile=1048576:1048576 $W/loadgen $VMIP 80 "$ROOT/seed/tokens.json" "$n" 60 "${HOLD:-300}" 30 1 2>&1 |
    grep --line-buffered -vE '^(slow|idle socket)|^\s+(GET|POST)' | tee -a $LOGS/$b-$n.log | grep --line-buffered -E '^win:|RESULT|stopping'
  if vm true 2>/dev/null; then vm "sudo bash /srv/vm-server.sh stop" >> $LOGS/$b-$n.log; else echo "guest not answering after the run (frozen)" >> $LOGS/$b-$n.log; vm_restart; fi
  local res srv rps
  res=$(grep -m1 RESULT $LOGS/$b-$n.log | sed 's/RESULT: //'); srv=$(grep -m1 '== server:' $LOGS/$b-$n.log | sed 's/== server: //')
  rps=$(grep -E '^win:' $LOGS/$b-$n.log | awk -F'req/s=' '{split($2,a," "); s+=a[1]; k++} END {if (k) printf "%.0f", s/k}')
  printf '%-6s %8s  %-58s %8s %s\n' "$b" "$n" "${res:-no result}" "${rps:-?}" "${srv:-guest frozen}" | tee -a $LOGS/summary.txt
  grep -iE 'killed process|frozen' $LOGS/$b-$n.log | head -2
  echo "   host: conntrack $(cat /proc/sys/net/netfilter/nf_conntrack_count 2>/dev/null || echo -)/$(cat /proc/sys/net/netfilter/nf_conntrack_max 2>/dev/null || echo -), $(grep -m1 RESULT -B1 $LOGS/$b-$n.log | head -1 | grep -oE 'conn_err=[0-9]+|timeout=[0-9]+|eof=[0-9]+|max_loop=[0-9]+ms' | tr '\n' ' ')"
  [[ $res == PASS* ]]
}
for spec in $RUNS; do
  b=${spec%%:*}
  for n in $(echo "${spec##*:}" | tr ',' ' '); do
    if one_run "$b" "$n"; then log "$b passes $n users"; break; fi
  done
done
echo; cat $LOGS/summary.txt

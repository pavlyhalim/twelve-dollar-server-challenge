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
MERGED_REF=${MERGED_REF:-6a1eab9} # upstream main with #245 merged (its submission == 7387a31, the scored code)
W=/tmp/lt; LOGS=$W/logs; mkdir -p $W $LOGS
VMIP=10.0.0.2
SSHO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5 -i $W/key"
vm() { ssh $SSHO ubuntu@$VMIP "$@"; }
log() { echo "[$(date +%H:%M:%S)] $*"; }

# The guest's vCPU thread alone can be slowed to the box's speed (BOX_US: the box's CPU per request,
# 25 us for #233 at 270k on the leaderboard): QEMU goes into a cgroup with two threaded children,
# the vCPU thread ("CPU 0/KVM") in one with a cpu.max quota, every other thread (vhost-net, I/O) in
# the other, uncapped, as the box's host does that work outside its vCPU. Capping the whole process
# starved the vCPU behind the network thread. The 10 ms period keeps the pauses short.
QUOTA=100
CG=/sys/fs/cgroup/lt
set_quota() {
  [ -d $CG/vcpu ] || return 0
  if [ "$QUOTA" -ge 100 ]; then echo "max 10000"; else echo "$((QUOTA * 100)) 10000"; fi | sudo tee $CG/vcpu/cpu.max >/dev/null
}
cg_setup() {
  local pid tid t
  pid=$(sudo cat $W/qemu.pid) # written by QEMU as root
  tid=$(ps -T -p "$pid" -o spid=,comm= | awk '/CPU 0\/KVM/ {print $1; exit}')
  [ -n "$tid" ] || { echo "no vCPU thread found; running unthrottled"; return 0; }
  grep -qw cpu /sys/fs/cgroup/cgroup.subtree_control || echo +cpu | sudo tee /sys/fs/cgroup/cgroup.subtree_control >/dev/null
  sudo mkdir -p $CG
  echo "$pid" | sudo tee $CG/cgroup.procs >/dev/null
  sudo mkdir -p $CG/vcpu $CG/other
  echo threaded | sudo tee $CG/vcpu/cgroup.type $CG/other/cgroup.type >/dev/null
  echo +cpu | sudo tee $CG/cgroup.subtree_control >/dev/null
  for t in $(ls /proc/"$pid"/task); do echo "$t" | sudo tee $CG/other/cgroup.threads >/dev/null 2>&1; done
  echo "$tid" | sudo tee $CG/vcpu/cgroup.threads >/dev/null
  set_quota
  echo "vCPU thread $tid capped alone: cpu.max $(cat $CG/vcpu/cpu.max)"
}
vm_start() {
  # debug-threads=on names the vCPU thread "CPU 0/KVM", which is how cg_setup finds it
  sudo qemu-system-x86_64 -name tdsc,debug-threads=on -enable-kvm -cpu host -smp 1 -m 2048 \
    -drive file=$W/disk.qcow2,if=virtio -drive file=$W/seed.iso,if=virtio,format=raw \
    -netdev tap,id=n0,ifname=tap0,script=no,downscript=no,vhost=on -device virtio-net-pci,netdev=n0,mac=52:54:00:12:34:56 \
    -display none -serial file:$W/serial.log -daemonize -pidfile $W/qemu.pid
  cg_setup || echo "cgroup setup failed; running unthrottled"
  # asked to match the box's speed but the cap isn't in place: the runs would mean nothing
  # (cgroup files report size 0, so the check reads the content)
  if [ -n "${BOX_US:-}" ] && [ -z "$(cat $CG/vcpu/cgroup.threads 2>/dev/null)" ]; then echo "VCPU CAP NOT APPLIED; stopping"; exit 4; fi
  for _ in $(seq 120); do vm true 2>/dev/null && return 0; sleep 3; done
  echo "guest did not answer on ssh"; tail -30 $W/serial.log; return 1
}
vm_restart() {
  log "restarting the guest"
  sudo kill "$(sudo cat $W/qemu.pid)" 2>/dev/null; sleep 3
  vm_start
}

log "== host: $(nproc) CPUs, $(awk '/MemTotal/ {print int($2/1048576)}' /proc/meminfo) GB, $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2)"
# EPYC 7763 hosts measured 45-68 us per request in the guest against the box's ~26: nothing run there says
# anything about the box, so stop before spending an hour on it
if grep -m1 'model name' /proc/cpuinfo | grep -q 7763; then
  mkdir -p $LOGS; echo "RUNNER SLOWER THAN THE BOX: EPYC 7763 host; rerun to get a faster host" | tee -a $LOGS/summary.txt; exit 3
fi
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
one_run() { # build users [ramp hold rampdown] -> 0 on PASS
  local b=$1 n=$2 ramp=${3:-60} hold=${4:-${HOLD:-300}} down=${5:-30}
  log "== $b at $n users"
  vm true 2>/dev/null || vm_restart || exit 1
  vm "sudo bash /srv/vm-server.sh start /srv/$b" | tee $LOGS/$b-$n.log
  sudo env LG_SRC=$SRC prlimit --nofile=1048576:1048576 $W/loadgen $VMIP 80 "$ROOT/seed/tokens.json" "$n" "$ramp" "$hold" "$down" 1 2>&1 |
    grep --line-buffered -vE '^(slow|idle socket)|^\s+(GET|POST)' | tee -a $LOGS/$b-$n.log | grep --line-buffered -E '^win:|RESULT|stopping'
  if vm true 2>/dev/null; then vm "sudo bash /srv/vm-server.sh stop" >> $LOGS/$b-$n.log; else echo "guest not answering after the run (frozen)" >> $LOGS/$b-$n.log; vm_restart; fi
  local res srv rps
  res=$(grep -m1 RESULT $LOGS/$b-$n.log | sed 's/RESULT: //'); srv=$(grep -m1 '== server:' $LOGS/$b-$n.log | sed 's/== server: //')
  rps=$(grep -E '^win:' $LOGS/$b-$n.log | awk -F'req/s=' '{split($2,a," "); s+=a[1]; k++} END {if (k) printf "%.0f", s/k}')
  printf '%-6s %8s  %-58s %8s %s quota=%s%%\n' "$b" "$n" "${res:-no result}" "${rps:-?}" "${srv:-guest frozen}" "$QUOTA" | tee -a $LOGS/summary.txt
  grep -iE 'killed process|frozen' $LOGS/$b-$n.log | head -2
  [ -f $CG/vcpu/cpu.stat ] && echo "   vCPU cgroup: $(grep -E 'usage_usec|nr_throttled|throttled_usec' $CG/vcpu/cpu.stat | tr '\n' ' ')"
  echo "   host: conntrack $(cat /proc/sys/net/netfilter/nf_conntrack_count 2>/dev/null || echo -)/$(cat /proc/sys/net/netfilter/nf_conntrack_max 2>/dev/null || echo -), $(grep -m1 RESULT -B1 $LOGS/$b-$n.log | head -1 | grep -oE 'conn_err=[0-9]+|timeout=[0-9]+|eof=[0-9]+|max_loop=[0-9]+ms' | tr '\n' ' ')"
  [[ $res == PASS* ]]
}
# Calibration (BOX_US set): the merged build at CAL_USERS with the guest unthrottled; its CPU per
# request against the box's sets the quota for every run after it.
if [ -n "${BOX_US:-}" ]; then
  one_run M "${CAL_USERS:-270000}" 60 60 0 >/dev/null 2>&1 || true
  f=$LOGS/M-${CAL_USERS:-270000}.log; mv "$f" "$LOGS/calibration.log"; f=$LOGS/calibration.log
  ms=$(grep -m1 -oE 'cpu_ms=[0-9]+' "$f" | cut -d= -f2)
  reqs=$(grep -E '^\[' "$f" | tail -1 | grep -oE 'reqs=[0-9]+' | cut -d= -f2)
  us=$(awk -v ms="${ms:-0}" -v r="${reqs:-0}" 'BEGIN {if (r > 0) printf "%.2f", ms * 1000 / r}')
  QUOTA=$(awk -v us="${us:-0}" -v box="$BOX_US" 'BEGIN {q = int(100 * us / box + 0.5); if (q < 10) q = 10; if (q > 100) q = 100; print q}')
  set_quota
  log "calibration: merged build at ${CAL_USERS:-270000} users unthrottled: ${us} us per request (box ${BOX_US}) -> vCPU quota ${QUOTA}% (cpu.max $(cat $CG/vcpu/cpu.max 2>/dev/null))"
  echo "calibration: ${us} us/request unthrottled, box ${BOX_US} -> quota ${QUOTA}%" >> $LOGS/summary.txt
  # a guest slower than the box even uncapped can't stand in for it (GitHub's EPYC 7763 hosts measured
  # 45-68 us against 10.6 on a 9V45: nested virtualization costs more there); stop and let it be rerun
  # (the merged build failing uncapped where the box passed means the same, whatever the CPU figure:
  # a server that falls behind uses CPU out of proportion to the requests it serves)
  if ! grep -q 'RESULT: PASS' "$f" || awk -v us="${us:-999}" -v box="$BOX_US" 'BEGIN {exit !(us > box)}'; then
    echo "RUNNER SLOWER THAN THE BOX: ${us} us per request uncapped against ${BOX_US}; rerun to get a faster host" | tee -a $LOGS/summary.txt
    exit 3
  fi
fi
for spec in $RUNS; do
  b=${spec%%:*}
  for n in $(echo "${spec##*:}" | tr ',' ' '); do
    if one_run "$b" "$n"; then log "$b passes $n users"; break; fi
  done
done
echo; cat $LOGS/summary.txt

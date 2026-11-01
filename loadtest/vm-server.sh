#!/usr/bin/env bash
# Inside the KVM guest, as root. Runs a submission the way the benchmark box does: a systemd service
# with MemoryMax=1750M, LimitNOFILE=65535 and CAP_NET_BIND_SERVICE, direct on 0.0.0.0:80, on a fresh
# copy of the seed; samples memory every 5 s without walking sockets.
#   vm-server.sh start <submission-dir>
#   vm-server.sh stop
#   vm-server.sh profile <seconds>   perf (all CPUs, call graphs) and TCP counters over a window, mid-run
set -euo pipefail
RUN=/srv/run
CG=/sys/fs/cgroup/system.slice/app.service
case $1 in
start)
  systemctl stop app sampler 2>/dev/null || true
  systemctl reset-failed app sampler 2>/dev/null || true
  rm -rf $RUN && mkdir -p $RUN && cp /srv/feed.db $RUN/feed.db && chown -R ubuntu:ubuntu $RUN
  sync; echo 3 > /proc/sys/vm/drop_caches
  systemd-run --unit=app -p MemoryMax=1750M -p LimitNOFILE=65535 -p AmbientCapabilities=CAP_NET_BIND_SERVICE \
    --uid=ubuntu --gid=ubuntu -E SQLITE_PATH=$RUN/feed.db -E JWT_SECRET=twelve-dollar-challenge \
    -E HOST=0.0.0.0 -E PORT=80 --working-directory="$2" "$2/start.sh" >/dev/null
  for _ in $(seq 60); do curl -sf http://127.0.0.1/health >/dev/null && break; sleep 1; done
  curl -sf http://127.0.0.1/health >/dev/null || { echo "server did not come up"; journalctl -u app --no-pager -o cat | tail -20; exit 1; }
  systemctl show -p CPUUsageNSec --value app > $RUN/cpu0
  date +%s > $RUN/since
  systemd-run --unit=sampler bash -c "
    t0=\$(date +%s)
    echo '  t  avail   free  sunrec  srec  app_MB     tcp  tcp_mem  calib_ms'
    while sleep 5; do
      cg=\$(cat $CG/memory.current 2>/dev/null || echo 0)
      read tcp tm < <(awk '/^TCP:/ {print \$3, \$11}' /proc/net/sockstat)
      read cc cw < <(/srv/calib)
      awk -v t=\$(( \$(date +%s) - t0 )) -v cg=\$cg -v tcp=\$tcp -v tm=\$tm -v cc=\$cc '
        /^MemAvailable:/ {a=\$2} /^MemFree:/ {f=\$2} /^SUnreclaim:/ {u=\$2} /^SReclaimable:/ {r=\$2}
        END {printf \"%4d %6d %6d %6d %5d %7d %7d %8d %9s\\n\", t, a/1024, f/1024, u/1024, r/1024, cg/1048576, tcp, tm*4/1024, cc}' /proc/meminfo
    done > $RUN/mem.log" >/dev/null
  echo "server up: $(uname -r), MemTotal $(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo) MB"
  ;;
stop)
  systemctl stop sampler 2>/dev/null || true
  cpu1=$(systemctl show -p CPUUsageNSec --value app 2>/dev/null || echo 0)
  echo "== server: $(systemctl show -p ActiveState -p Result app | tr '\n' ' ') cpu_ms=$(( (${cpu1:-0} - $(cat $RUN/cpu0)) / 1000000 ))"
  journalctl -k --no-pager -o cat --since=@$(cat $RUN/since) | grep -iE 'out of memory|killed process' | cut -c1-160 || true
  echo "== server stats (every 10 s)"
  journalctl -u app --no-pager -o cat --since=@$(cat $RUN/since) | grep -E '^open=' |
    sed -E 's/ batches\/s=[^ ]+ avg_batch=[^ ]+//; s/ max_sweep=[^ ]+//' || true
  echo "== memory (MB, every 5 s)"; cat $RUN/mem.log
  systemctl stop app 2>/dev/null || true
  ;;
profile)
  set +e +o pipefail # head closes pipes early; a short report beats none
  d=${2:-30}
  sysctl -qw kernel.kptr_restrict=0 kernel.perf_event_paranoid=-1
  echo "== profile: ${d} s from $(date +%T)"
  echo "-- sockstat before"; cat /proc/net/sockstat
  nstat -n
  perf record -a -g -F 499 -o /tmp/perf.data -- sleep "$d" >/dev/null 2>&1 || echo "perf record failed"
  echo "-- TCP counters over the window"; nstat | grep -vE '^#' | sort -k2 -nr | head -40
  echo "-- sockstat after"; cat /proc/net/sockstat
  echo "-- tcp_mem limits (pages)"; cat /proc/sys/net/ipv4/tcp_mem
  echo "-- CPU by process and object"; perf report -i /tmp/perf.data --no-children --sort comm,dso -g none --stdio 2>/dev/null | grep -E '^ +[0-9]' | head -12
  echo "-- CPU by function (self)"; perf report -i /tmp/perf.data --no-children --sort sym -g none --stdio 2>/dev/null | grep -E '^ +[0-9]' | head -50
  echo "-- callers of the top kernel functions"
  perf report -i /tmp/perf.data --no-children --sort sym -g caller,0.5,callee --stdio 2>/dev/null | grep -vE '^$|^#' | head -150
  ;;
esac

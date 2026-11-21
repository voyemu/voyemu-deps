#!/proc/boot/bin/ksh
exec 1>&2
qm_source_capture=1
export PATH=/proc/boot/bin:/proc/boot:/bin:/usr/bin:/sbin
# qm_l composes '/proc/boot/lib64' so the 11713 rewrite cannot reorder the export;
# qm_s composes '/proc/boot/bin' likewise for every LD_PRELOAD of boot-image shims.
qm_l=/proc/boot/lib6
qm_bl=${qm_l}4:
qm_s=/proc/boot/bi
qm_snap=${qm_s}n/Snapdragon.rgb
# RUN181-green preloaded disp_dbg before Snapdragon for the ic spawn; compose it too.
qm_dd=${qm_s}n/disp_dbg
export LD_LIBRARY_PATH=${qm_bl}${qm_l}4/dll:/lib64:/usr/lib64
: /proc/boot/lib64/ /proc/boot/bin/
qm_cmd() (
    [ "$#" -gt 0 ] && [ "$#" -le 16 ] || return 125
    for qm_av in 00 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15; do unset VOYEMU_QBW_ARG_$qm_av; done
    export VOYEMU_QBW_ENABLE=1 VOYEMU_QBW_ARGC="$#"
    for key in 00 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15; do
        [ "$#" -gt 0 ] || break
        export "VOYEMU_QBW_ARG_$key=$1"
        shift
    done
    export LD_PRELOAD=/proc/boot/bin/qditest
    exec /proc/boot/bin/ksh -c 'exit 125'
)
qm_wait() {
    count=0
    while [ ! -e "$1" ] && [ "$count" -lt 100 ]; do /bin/sleep 0.1; count=$((count+1)); done
    [ -e "$1" ]
}
# The mounted OEM filesystem is not the host IFS: a command name is measured, not assumed.
qm_pick() {
    for qm_p in "$@"; do [ -x "$qm_p" ] && { echo "$qm_p"; return 0; }; done
    return 1
}
# Diagnostic only: stream the slog2 world to the console so a watchdog record appears
# before RAM loss; the ram64diag.so carrier interposer is the second channel.
qm_slog2w=$(qm_pick /proc/boot/bin/slog2info /bin/slog2info /usr/bin/slog2info) || qm_slog2w=
[ -n "$qm_slog2w" ] && { "$qm_slog2w" -w 2>&1 & echo "QGRESET slog2-watch pid=$!"; }
echo 'QPC BEGIN'
qm_wait /proc/boot/bin/devb-ram || { echo 'QPC FAIL ram-driver'; exit 1; }
# capacity= is in 512-byte blocks; the root stays read-only, because a read-write root loses the mount.
# devb-ram address= is 32-bit (0x23c008000 becomes 0x3c008000), so hd0 goes through the pinned
# ram64 carrier (passion_ram64.h), which maps 0x7c008000 to the file system at guest-physical 0x27c008000.
VOYEMU_PASSION_RAM64=passion-20260403-2.1.4.80-system-ram64-v1 \
LD_PRELOAD=/proc/boot/etc/dispdump.sh \
/proc/boot/bin/devb-ram ram capacity=2462832,address=0x7c008000 blk devdir=/dev/oem,cache=512k,automount=hd0:/:qnx6:ro,after &
qm_ram=$!
# sys_monitor scans at ~boot+5 s with reboot:true for a missing iox-roudi, so the
# writable /var is mounted by this second devb-ram before the long compositor wait; iceoryx
# needs a world-writable /var/lock.
# A second automount of the OEM image (hd1) cannot hold /var: its LUN attach inherits the cam
# devsize of the first, so its superblock candidate {0x2000, devsize-1000} reads past the end and
# fails with E302 on every invocation form. /var is a sysram LUN instead, formatted by the guest
# mkqnx6fs, which writes both superblocks under the geometry cam reports.
/proc/boot/bin/devb-ram ram capacity=65536,nodinit blk devdir=/dev/qvx,memory=sysram,cache=512k &
# The mkfs/mkdir binaries live on the hd0 automount that the first devb-ram finishes
# asynchronously (measured run-022: picker ran before the root was mounted, both came up
# empty and the /var setup silently skipped); wait for the mounted root before any pick.
qm_retries=0
while [ ! -e /sbin/mkqnx6fs ] && [ "$qm_retries" -lt 2000 ]; do
 [ -e /bin/sleep ] && /bin/sleep 0.02
 qm_retries=$((qm_retries+1))
done
echo "QPC Droot c=$qm_retries mkfs=$([ -e /sbin/mkqnx6fs ] && echo y || echo n)"
qm_chmod0=$(qm_pick /proc/boot/bin/chmod /bin/chmod) || qm_chmod0=
qm_sleep0=$(qm_pick /proc/boot/bin/sleep /bin/sleep)
qm_mount0=$(qm_pick /sbin/mount /proc/boot/bin/mount /bin/mount) || qm_mount0=
qm_mkdir0=$(qm_pick /proc/boot/bin/mkdir /bin/mkdir) || qm_mkdir0=
qm_mkfs0=$(qm_pick /sbin/mkqnx6fs /proc/boot/bin/mkqnx6fs) || qm_mkfs0=
qm_vx=
qm_vt=0
while [ "$qm_vt" -lt 100 ] && [ -z "$qm_vx" ]; do
    for qm_vdev in /dev/qvx/hd*; do [ -e "$qm_vdev" ] && qm_vx="$qm_vdev" && break; done
    [ -z "$qm_vx" ] && "$qm_sleep0" 0.1 && qm_vt=$((qm_vt+1)) || qm_vt=100
done
echo "QPC D tools mkfs=$qm_mkfs0 mount=$qm_mount0 mkdir=$qm_mkdir0 ln=$qm_ln sleep=$qm_sleep0 roudi=$(qm_pick /usr/bin/iox-roudi /proc/boot/bin/iox-roudi || echo missing) on=$(qm_pick /bin/on /proc/boot/bin/on || echo missing) chmod=$qm_chmod0"
if [ -n "$qm_vx" ] && [ -n "$qm_mkfs0" ] && [ -n "$qm_mount0" ]; then
    "$qm_mkfs0" -q "$qm_vx" && "$qm_mount0" -t qnx6 -o sync=optional "$qm_vx" /var && echo 'QPC VAR mount ok' || echo 'QPC VAR mount FAILED'
fi
# /var/lock must exist before sys_monitor's ~boot+5 s scan (missing iox-roudi reboots the
# guest), so the directories and the early roudi spawn come before any settlement wait.
qm_roudi=$(qm_pick /usr/bin/iox-roudi /proc/boot/bin/iox-roudi) || qm_roudi=
qm_on=$(qm_pick /bin/on /proc/boot/bin/on) || qm_on=
qm_rlib=/usr/lib64
qm_varrc=ok
if [ -n "$qm_mkdir0" ]; then "$qm_mkdir0" -p /var/lock /var/run /var/data /var/pps /var/pps_persist || qm_varrc="mkdir:$?"
fi
if [ -n "$qm_chmod0" ]; then "$qm_chmod0" 0777 /var/lock /var/run 2>/dev/null || qm_varrc="$qm_varrc chmod:$?"; fi
if [ ! -d /var/lock ]; then
    # Empty /var from a fresh mkqnx6fs may have no /var/lock for mkdir -p; retry per
    # directory so the failing one is named, and report errno via the shell.
    for qm_vd in lock run data pps pps_persist; do
        /bin/mkdir "/var/$qm_vd" 2>/dev/null || "$qm_mkdir0" "/var/$qm_vd" 2>/dev/null
    done
fi
echo "QPC Dvarnew lock=$([ -d /var/lock ] && echo y || echo n) rc=$qm_varrc libc=$(ls /lib64/libc.so* 2>/dev/null | head -1) freesize=$(df -k /var 2>/dev/null | tail -1)"
qm_ticks0=0; while [ "$qm_ticks0" -lt 300 ] && [ ! -d /var/lock ]; do "$qm_sleep0" 0.1; qm_ticks0=$((qm_ticks0+1)); done
[ -d /var/lock ] || cp -R /var/run /var/lock 2>/dev/null
[ "$( (echo qw012 > /var/.qwprobe) && cat /var/.qwprobe 2>/dev/null)" = qw012 ] && echo 'QPC VARRW probe=y' || echo 'QPC VARRW probe=n'
# Printed after the roudi spawn, which a later reset cannot hide: the block device directories
# and the /var listing tell a stacked hd0 root from a failed /var mount.
echo "QPC VAR oem=$([ -e /dev/oem ] && echo y || echo n) qvx=$([ -e /dev/qvx ] && echo y || echo n) lock=$([ -d /var/lock ] && echo y || echo n) run=$([ -d /var/run ] && echo y || echo n) v=$(ls /var)"
qm_ioxt=/var/log/ioxr.toml
[ -n "$qm_mkdir0" ] && "$qm_mkdir0" -p /var/log
/usr/bin/awk 'BEGIN{print "[general]";print "version = 1";print "[[segment]]";split("128 2500 1024 1250 16384 250 131072 50 524288 13 1048576 8 4194304 3",a);for(i=1;i<15;i+=2){print "[[segment.mempool]]";print "size = "a[i];print "count = "a[i+1]}}' > $qm_ioxt
qm_e0=
if [ -n "$qm_roudi" ] && [ -d /var/lock ]; then
 if [ -n "$qm_on" ]; then
  LD_LIBRARY_PATH="$qm_rlib:$LD_LIBRARY_PATH" "$qm_on" -u 819:819 "$qm_roudi" -m off -c $qm_ioxt &
 else
  LD_LIBRARY_PATH="$qm_rlib:$LD_LIBRARY_PATH" "$qm_roudi" -m off -c $qm_ioxt &
 fi
 qm_e0=$!
 echo "QGRESET roudi-early pid=$qm_e0 bin=$qm_roudi route=on pre-scan"
else echo "QGRESET roudi-early none bin=${qm_roudi:-missing} varlock=no"; fi
# sys_monitor scans io-pkt-v4-hc (reboot:true, wait 5) at the same 5-s tick, so
# the real stack starts pre-scan with the OEM argv minus two VM-absent tokens:
# pkt_typed_mem=/ram/dma (no typed-mem here; sysram is the fallback) and
# `-d /lib64/libdevnp-emac-eth.so` (the VM has no HLOS_EMAC, and the emac driver dies with
# SIGSEGV in emac_config_parse before the scan).
# The driverless io-pkt is the real stack; -U restores the OEM thread uids; the scan
# needs the NAME, and root-run is the proven early-roudi pattern.
[ -x /bin/io-pkt-v4-hc ] && { /bin/io-pkt-v4-hc -S -v -D -t 2 -p tcpip stacksize=65536,mclbytes=4096,pagesize=65536,pkt_cache=1024,mbuf_cache=1024 -ppf-v4 -U 37:37,21,35,77,87,46,40 & echo QGRESET iopkt=$!; }
# A rejected preload kills the child, so the loop checks the parent, not the delay.
qm_ticks=0
while [ "$qm_ticks" -lt 600 ]; do
    [ -e /usr/bin/ivi_compositor ] && break
    [ -d "/proc/$qm_ram" ] || break
    /bin/sleep 0.1
    qm_ticks=$((qm_ticks+1))
done
echo "QPC Dticks=$qm_ticks"
echo "QPC Dcompositor=$([ -e /usr/bin/ivi_compositor ] && echo present || echo absent)"
echo "QPC Dslm=$([ -e /slm ] && echo yes || echo no)"
# The ota RAM disk and its settle loop are deferred past the early roudi spawn:
# sys_monitor's first scan gives iox-roudi only 5 s grace (reboot:true in sys_monitor.json).
qm_wait /usr/bin/ivi_compositor || { echo 'QPC FAIL OEM-mount'; exit 1; }
echo 'QPC MOUNT ro cap=2462832'
export LD_LIBRARY_PATH=${qm_bl}${qm_l}4/dll/lib64:/usr/lib64:/opt/qt/lib
export PATH=/proc/boot/bin:/proc/boot:/bin:/usr/bin:/sbin
# Only the freshly created private RAM disk may be formatted; the hdN unit is a global counter.
for qm_unit in /dev/qpc/hd*; do
 [ -e "$qm_unit" ] && { echo 'QPC FAIL preexisting-private-disk'; exit 1; }
done
# Two 64 MiB sysram disks do not both fit the shrinking pool; 32 MiB holds the service logs.

/proc/boot/bin/devb-ram ram capacity=65536,nodinit blk devdir=/dev/qpc,memory=sysram,cache=512k &
qm_priv=; qm_ticks3=0
while [ -z "$qm_priv" ] && [ "$qm_ticks3" -lt 300 ]; do
 for qm_unit in /dev/qpc/hd*; do
  [ -e "$qm_unit" ] && { qm_priv=$qm_unit; break; }
 done
 [ -n "$qm_priv" ] || { [ -n "$qm_sleep0" ] && "$qm_sleep0" 0.1; }
 qm_ticks3=$((qm_ticks3+1))
done
echo "QPC Dprivatedisk=${qm_priv:-none} ticks=$qm_ticks3"
# A second memory=sysram devb-ram registers no unit after the /var LUN (the hd0/hd1 E302
# class), so the private log disk is optional; logs live under /var/log.

qm_mkdir0m=$qm_mkdir0
[ -n "$qm_mkdir0m" ] && qm_cmd "$qm_mkdir0m" -p /var/log
qm_ln=$(qm_pick /proc/boot/bin/ln /sbin/ln) || qm_ln=
qm_cat=$(qm_pick /proc/boot/bin/cat /usr/bin/cat) || qm_cat=
qm_pidin=$(qm_pick /proc/boot/bin/pidin /sbin/pidin) || qm_pidin=
qm_slm=$(qm_pick /proc/boot/bin/slm /usr/bin/slm) || qm_slm=
qm_ifconfig=$(qm_pick /sbin/ifconfig /proc/boot/bin/ifconfig) || qm_ifconfig=
echo "QPC D tools mkfs=$qm_mkfs0 mount=$qm_mount0 mkdir=$qm_mkdir0 ln=$qm_ln cat=$qm_cat sleep=$qm_sleep0 pidin=$qm_pidin slm=$qm_slm ifconfig=$qm_ifconfig roudi=$qm_roudi on=$qm_on chmod=$qm_chmod0"
[ -n "$qm_mkfs0" ] || { echo 'QPC FAIL no-mkqnx6fs'; exit 1; }
if [ -n "$qm_priv" ]; then
qm_cmd "$qm_mkfs0" -q "$qm_priv" || echo 'QPC PRIVATE mkqnx6fs failed'
if [ -n "$qm_mount0" ]; then qm_cmd "$qm_mount0" -t qnx6 -o sync=optional "$qm_priv" /var/log || echo 'QPC PRIVATE log-mount failed'; else echo 'QPC PRIVATE no-mount-tool'; fi
fi
echo "QPC Dlogwrite=$([ -w /var/log ] && echo yes || echo no)"
[ -n "$qm_mkdir0" ] && { qm_cmd "$qm_mkdir0" -p /var/log/data /var/log/tmp /var/log/ipc /var/log/run || echo 'QPC PRIVATE mkdir failed'; }
# iceoryx registers apps through unix sockets in /tmp (measured run-039: "/tmp/health_monitor-0
# Permission denied" -> RouDi re-registers the compositor until it gives up); /tmp is our
# /var/log/tmp symlink, so it needs 1777 like a real tmpdir.
# Run-040: the qm_cmd shim (uid 1) leaves /var/log drw-r--r-- , so open traversal, then the
# sticky tmp mode, or every non-root process fails /tmp.

[ -n "$qm_chmod0" ] && { "$qm_chmod0" 0777 /var/log 2>/dev/null; "$qm_chmod0" 1777 /var/log/tmp 2>/dev/null; "$qm_chmod0" 0777 /var/log/data /var/log/ipc /var/log/run 2>/dev/null; echo "QPC Dtmpmode rc=$?"; }
[ -n "$qm_ln" ] && { qm_cmd "$qm_ln" -sfP /var/log/data /data || true; qm_cmd "$qm_ln" -sfP /var/log/tmp /tmp || true; }
[ -n "$qm_pidin" ] && for qm_dp in $("$qm_pidin" ar 2>/dev/null | /usr/bin/awk '/[d]umper/{print $1}'); do kill $qm_dp 2>/dev/null; done
rm -f /tmp/*.core.gz
# The writable /var RAM disk and the early roudi spawn were moved before the OEM mount waits
# (sys_monitor's 5 s scan); their pre-scan diagnostics live near 'QPC BEGIN' above.
# Deferred ota disk: settle loop and mount follow the early spawn.
/proc/boot/bin/devb-ram ram capacity=2462832,address=0x27c008000 blk devdir=/dev/t32,cache=512k,automount=hd2:/ota:qnx6:ro,after &
qm_ticks2=0
while [ "$qm_ticks2" -lt 100 ]; do /bin/sleep 0.1; qm_ticks2=$((qm_ticks2+1)); done
echo "QPC Dt32ticks=$qm_ticks2"
# Measured runs 018/023-026: enumeration diagnostics (/bin/ls -l /dev/oem, /bin/mount
# without args, both through the QBW shim and direct) hang forever on stat of a block
# device in cam and silently kill the rest of the boot path; the mounts are already
# proven by the 'QPC VAR oem=/qvx=' markers, so both listings are dropped.
# /slm/ic_qt.xml is the OEM's system file; /slm/ic_qt/ic_qt.xml is the component.
qm_icxml=/slm/ic_qt.xml
[ -e "$qm_icxml" ] || qm_icxml=/slm/ic_qt/ic_qt.xml
echo "QPC Dicxml=$qm_icxml logwrite=$([ -w /var/log ] && echo yes || echo no)"
echo "QPC Dstartup_script size=$([ -e /scripts/startup.sh ] && /usr/bin/wc -c < /scripts/startup.sh || echo none)"
[ -n "$qm_ifconfig" ] && { qm_cmd "$qm_ifconfig" lo0 up || echo 'QPC NETWORK loopback failed'; }
# someipd-vlan1X bind -d vlan1X, so the OEM start_config_vlan and static_arp_config commands
# run with tap0 as the parent in place of the absent emac0; SLM then starts ic_service and ic_server.
qm_cmd "$qm_ifconfig" tap0 create up&&/usr/bin/awk '/^ +(ifconfig vlan|arp -s)/{sub(/emac0/,"tap0");print}' /scripts/startup.sh>/var/log/v&&. /var/log/v
if [ "${VOYEMU_PASSION_QNET-}" = 1 ] && [ -n "$qm_ifconfig" ]; then
 [ -e /dev/tap0 ] || exit 1
 qm_cmd "$qm_ifconfig" tap0 172.16.100.22 netmask 255.255.255.0 up || exit 1
 VOYEMU_QNET_RELAY=1 LD_PRELOAD=$qm_snap /bin/ls -d / >&2 &
 echo "QPC QNET relay_pid=$! readiness=unverified"
fi
[ -n "$qm_ifconfig" ] && qm_cmd "$qm_ifconfig" -a > /var/log/interfaces.log 2>&1
[ -n "$qm_cat" ] && "$qm_cat" /var/log/interfaces.log
# setup_qt_env from scripts/startup.sh, with the OEM's own recorded physical screen size.
# qm_b composes '/proc/boot/usr' so the 11713 rewrite (which replaces the literal
# /proc/boot/usr/ with /usr/) cannot redirect it: RUN181's green frame ran roudi on the
# boot image's iceoryx; runs 045-048 died in std::bad_alloc on hd0's /usr/lib64 pick.
qm_b=/proc/boot/us
qm_bootusr=${qm_b}r/
: /proc/boot/usr/
export LD_LIBRARY_PATH=${qm_bl}${qm_l}4/dll/lib64:/usr/lib64:/opt/qt/lib:${qm_bootusr}lib64
export QT_PLUGIN_PATH=/opt/qt/plugins
export QML2_IMPORT_PATH=/opt/qt/qml
export FONTCONFIG_FILE=/opt/qt/lib/fontconfig/local.conf
export QQNX_PHYSICAL_SCREEN_SIZE=1920,720
export QT_LOGGING_TO_CONSOLE=1
echo "QPC Ddevscreen=$([ -e /dev/screen ] && echo present || echo absent) devqvm=$([ -d /dev/qvm ] && echo yes || echo no) devgraphics=$(echo /dev/graphics/*) devgfx=$(echo /dev/screen/*)"
echo "QPC Dicbins ivi=$([ -x /usr/bin/ivi_compositor ] && echo yes || echo no) icservice=$([ -x /usr/bin/ic_service ] && echo yes || echo no) icbin=$(echo /usr/bin/ic_*)"
# The OEM boot starts a PPS server before any IC component.
qm_pps=$(qm_pick /bin/pps /proc/boot/bin/pps /usr/bin/pps) || qm_pps=
qm_ppsacl=$(qm_pick /etc/pps_acl.conf /proc/boot/etc/pps_acl.conf) || qm_ppsacl=
echo "QPC Dppsbin=$qm_pps acl=$qm_ppsacl dir=$([ -d /var/pps ] && echo yes || echo no)"
if [ -n "$qm_pps" ]; then
 if [ -n "$qm_ppsacl" ]; then
  "$qm_pps" -A "$qm_ppsacl" -m /var/pps -p /var/pps_persist -t 100 > /var/log/pps.log 2>&1 &
 else
  "$qm_pps" -m /var/pps -p /var/pps_persist -t 100 > /var/log/pps.log 2>&1 &
 fi
 echo "QPC PPS spawned=$! readiness=unverified"
else echo 'QPC PPS no-pps'; fi
# OEM start_iceoryx ran 'iox-roudi -m off -c .../roudi_config.toml'.
qm_ioxlib=; for qm_il in ${qm_bootusr}lib64 /usr/lib64; do [ -z "$qm_ioxlib" ] && [ -d "$qm_il" ] && qm_ioxlib=$qm_il; done
qm_ioxcfg=; for qm_ic in /etc/iceoryx/roudi_config.toml /proc/boot/etc/iceoryx/roudi_config.toml; do
 [ -z "$qm_ioxcfg" ] && [ -e "$qm_ic" ] && qm_ioxcfg=$qm_ic
done
echo "QPC Dioxlib=$qm_ioxlib members=$(echo $qm_ioxlib/libiceoryx*) cfg=$qm_ioxcfg"
qm_ioxf=$qm_ioxt; [ -s "$qm_ioxf" ] || qm_ioxf=$qm_ioxcfg
qm_ioxo=; [ -n "$qm_ioxf" ] && qm_ioxo="-c $qm_ioxf"
echo "QPC IOXC cfg=$qm_ioxf opt=$qm_ioxo bytes=$(/proc/boot/bin/ls -l $qm_ioxt 2>&1 | /usr/bin/awk '{print $5}')"
[ -n "$qm_ioxcfg" ] && ( /usr/bin/tail -n 40 "$qm_ioxcfg" ) &
# The awk config is passed only when non-empty; -m off stays.
# Run-042: the early pre-scan roudi leaves its segments in the ROOT group, so ic_apps
# compositors abort with std::bad_alloc on iceoryx attach (measured: 20 instances, all
# 'terminate ... std::bad_alloc'; RUN181's working frame came only with the late
# on-iceoryx roudi and iceoryx-group segments). Replace a still-running early roudi
# instead of skipping the late spawn.
if [ -n "$qm_e0" ] && [ -n "$qm_pidin" ]; then
 qm_e0pid=$("$qm_pidin" ar 2>/dev/null | /usr/bin/awk '/iox-roudi/{print $1; exit}')
 # shm outlives its creator: the killed roudi's ROOT-group segments make every ic_apps
 # attach throw bad_alloc (run-045); rm them before the late roudi reuses them.
 [ -n "$qm_e0pid" ] && { kill -9 "$qm_e0pid" 2>&1; rm -f /dev/shmem/iceoryx_mgmt /dev/shmem/iceoryx /dev/shmem/root 2>/dev/null; echo "QPC IOX early-kill shm-rm pid=$qm_e0pid rc=$? left=$(ls /dev/shmem/ 2>&1 | tr '\n' ,)"; }
fi
if [ -n "$qm_roudi" ] || { [ -n "$qm_pidin" ] && [ -z "$("$qm_pidin" ar 2>/dev/null | /usr/bin/awk '/iox-roudi/{print $1; exit}')" ]; }; then
 [ -n "$qm_mkdir0" ] && qm_cmd "$qm_mkdir0" -p /var/lock
 # As root the segment group is 'root' and ic_apps gets no writable segment, so the numeric 819:819 is used: 'on' rejects a name pair.
 qm_ioxr=on-iceoryx; qm_ioxl=/var/log/roudi-on.log; qm_seg=0; qm_ioxw=0
 if [ -n "$qm_on" ]; then
  echo 'QPC I1'
  LD_LIBRARY_PATH="$qm_ioxlib:$LD_LIBRARY_PATH" "$qm_on" -u 819:819 "$qm_roudi" -m off $qm_ioxo > $qm_ioxl 2>&1 &
  while [ "${qm_seg:-0}" -eq 0 ] && [ "$qm_ioxw" -lt 8 ]; do
   qm_ioxw=$((qm_ioxw+1)); [ -n "$qm_sleep0" ] && "$qm_sleep0" 3
   qm_seg=$(/usr/bin/awk '/Reserving/{n++} END{print n+0}' $qm_ioxl 2>/dev/null)
  done
  echo "QPC I2 route=$qm_ioxr wait=$qm_ioxw seg=$qm_seg"
 else echo 'QPC IOX no-on'; fi
 if [ "${qm_seg:-0}" -eq 0 ]; then
  echo "QPC IOX on-failed wait=$qm_ioxw"; /usr/bin/tail -n 20 $qm_ioxl
  qm_ioxr=direct; qm_ioxl=/var/log/roudi.log
  LD_LIBRARY_PATH="$qm_ioxlib:$LD_LIBRARY_PATH" "$qm_roudi" -m off $qm_ioxo > $qm_ioxl 2>&1 &
  [ -n "$qm_sleep0" ] && "$qm_sleep0" 8
 fi
 echo 'QPC I3'
 [ -n "$qm_chmod0" ] && "$qm_chmod0" 0666 /tmp/roudi /dev/shmem/iceoryx_mgmt /dev/shmem/root /dev/shmem/iceoryx 2>/dev/null
 qm_ioxn=$(/usr/bin/awk '/ready for clients/{n++} END{print n+0}' $qm_ioxl 2>/dev/null)
 # Publishers must sit in the segment's group: a 'root' segment means running them as root:root; only an
 # 'in the shared memory [name]' line carries a name, and a later line without one must not clear it.
 qm_segn=$(/usr/bin/awk '/in the shared memory/{n=$NF; gsub(/[][/]/,"",n); if (n!="iceoryx_mgmt") s=n} END{print s}' $qm_ioxl 2>/dev/null)
 [ "$qm_segn" = root ] && qm_icdir=1
 echo "QPC IQXSEG name=${qm_segn:-none} icdir=${qm_icdir:-0} route=$qm_ioxr wait=$qm_ioxw log=$qm_ioxl"
 echo "QPC IOX route=$qm_ioxr ready=$qm_ioxn seg=$qm_seg $(/proc/boot/bin/ls -l /tmp/roudi 2>&1)"
 /usr/bin/awk '/in the shared memory/{print "QPC SEG "$2" "$NF}' $qm_ioxl 2>/dev/null
else echo 'QPC IOX no-roudi'; fi
qm_smmu=$(qm_pick /proc/boot/bin/smmu_service /bin/smmu_service) || qm_smmu=
qm_kgsl=$(qm_pick /proc/boot/bin/kgsl /bin/kgsl /usr/bin/kgsl) || qm_kgsl=
qm_gsl=$(qm_pick /proc/boot/bin/gsl_hab_server /bin/gsl_hab_server /usr/bin/gsl_hab_server) || qm_gsl=
qm_screen=$(qm_pick /proc/boot/bin/screen /bin/screen /usr/bin/screen) || qm_screen=
qm_dispmgr=$(qm_pick /proc/boot/bin/disp_mgr /bin/disp_mgr /usr/bin/disp_mgr) || qm_dispmgr=
qm_owfd=$(qm_pick /proc/boot/bin/openwfd_server /bin/openwfd_server) || qm_owfd=
qm_wfdbe=$(qm_pick /proc/boot/bin/wfd_be /bin/wfd_be) || qm_wfdbe=
qm_bchip=$(qm_pick /proc/boot/bin/bridgechip_server /bin/bridgechip_server) || qm_bchip=
echo "QPC Dgfx smmu=$qm_smmu kgsl=$qm_kgsl gsl=$qm_gsl scr=$qm_screen disp=$qm_dispmgr owfd=$qm_owfd wfdbe=$qm_wfdbe br=$qm_bchip"
for qm_dc in ${qm_l}4/libdalconfig-sa8155_mega_mon_v11.0.33.so /lib64/libdalconfig-sa8155_mega_mon_v11.0.33.so ${qm_l}4/libdalconfig.so.1; do
 echo "QPC Ddalcand $qm_dc e=$([ -e "$qm_dc" ] && echo y || echo n)"
 [ -z "${qm_daltgt-}" ] && [ -e "$qm_dc" ] && qm_daltgt=$qm_dc
done
echo "QPC Ddaltgt=${qm_daltgt-none}"
# graphics.conf names its khronos dlls bare; screen needs them beside the configuration.
qm_gd=/var/log/gfx
[ -n "$qm_mkdir0" ] && { qm_cmd "$qm_mkdir0" -p "$qm_gd" || echo 'QPC GFXDIR mkdir failed'; }
[ -n "$qm_cat" ] && "$qm_cat" /proc/boot/etc/graphics.conf > "$qm_gd/graphics.conf"
for qm_g in libESXEGL_Adreno.so libESXGLESv2_Adreno.so libscreen.so.1 libeglSubDriverQnx.so c2d30.so libopenwfd_qnx.so; do
 [ -n "$qm_ln" ] && { qm_cmd "$qm_ln" -sf "${qm_l}4/$qm_g" "$qm_gd/$qm_g" || true; }
done
[ -n "${qm_daltgt-}" ] && [ -n "$qm_ln" ] && { qm_cmd "$qm_ln" -sf "$qm_daltgt" "$qm_gd/libdalconfig.so.1" || true; }
[ -n "$qm_smmu" ] && { LD_LIBRARY_PATH="$qm_gd:$LD_LIBRARY_PATH" LD_PRELOAD="${qm_daltgt-secpol-preload.so}" "$qm_smmu" -V 29:29,21 70:70,33,21 > /var/log/smmu.log 2>&1 & echo "QPC GFX smmu pid=$!"; }
[ -n "$qm_kgsl" ] && { "$qm_kgsl" > /var/log/kgsl.log 2>&1 & echo "QPC GFX kgsl pid=$!"; }
[ -n "$qm_gsl" ] && { "$qm_gsl" > /var/log/gsl.log 2>&1 & echo "QPC GFX gsl pid=$!"; }
[ -n "$qm_wfdbe" ] && { EARLY_VIDEO_ENABLE=ON MMLOG_CFG_PATH=/tmp/ LD_LIBRARY_PATH="$qm_gd:$LD_LIBRARY_PATH" LD_PRELOAD="${qm_daltgt-}" "$qm_wfdbe" > /var/log/wfdbe.log 2>&1 & echo "QPC GFX wfdbe pid=$!"; }
[ -n "$qm_bchip" ] && { LD_LIBRARY_PATH="$qm_gd:$LD_LIBRARY_PATH" LD_PRELOAD="${qm_daltgt-}" "$qm_bchip" > /var/log/bridgechip.log 2>&1 & echo "QPC GFX bridgechip pid=$!"; }
[ -n "$qm_dispmgr" ] && { LD_LIBRARY_PATH="$qm_gd:${qm_bl}${qm_l}4/dll/lib64:/lib64:/lib64/dll:/usr/lib64" "$qm_dispmgr" > /var/log/dispmgr.log 2>&1 & echo "QPC GFX dispmgr pid=$!"; }
[ -n "$qm_screen" ] && { "$qm_screen" -c "$qm_gd/graphics.conf" > /var/log/screen.log 2>&1 & echo "QPC GFX screen pid=$!"; }
[ -n "$qm_owfd" ] && { DISPLAY_CATALOG_PATH=/proc/boot/bin LD_LIBRARY_PATH="$qm_gd:$LD_LIBRARY_PATH" LD_PRELOAD="${qm_daltgt-/lib64/libdalconfig.so.1}" "$qm_owfd" > /var/log/openwfd.log 2>&1 & echo "QPC GFX openwfd pid=$!"; }
[ -n "$qm_sleep0" ] && "$qm_sleep0" 4
echo "QPC Ddevscreen2=$([ -e /dev/screen ] && echo present || echo absent) dev=$(echo /dev/*)"
for qm_lf in /var/log/smmu.log /var/log/kgsl.log /var/log/gsl.log /var/log/wfdbe.log /var/log/dispmgr.log /var/log/screen.log /var/log/openwfd.log /var/log/bridgechip.log /var/log/pps.log; do
 echo "QPC GFXLOG $qm_lf"; [ -n "$qm_cat" ] && "$qm_cat" "$qm_lf"
done
echo "QPC GFXCONF $qm_gd/graphics.conf"; [ -n "$qm_cat" ] && "$qm_cat" "$qm_gd/graphics.conf"
qm_slog2=$(qm_pick /proc/boot/bin/slog2info /bin/slog2info /usr/bin/slog2info) || qm_slog2=
qm_aps=$(qm_pick /bin/aps /proc/boot/bin/aps) || qm_aps=
if [ -n "$qm_aps" ]; then
 qm_cmd "$qm_aps" create -b 35 -B 300 aps_gvm; echo "QPC APS aps_gvm rc=$?"
 qm_cmd "$qm_aps" create -b 20 -B 100 aps_ic; echo "QPC APS aps_ic rc=$?"
else echo 'QPC APS no-aps'; fi
echo "QPC QTDIR $(/proc/boot/bin/ls -d /opt/qt/* 2>&1) data=$(/proc/boot/bin/ls -ld /data /data/qt 2>&1)"
# The plugin reads its layout from /opt/qt/config/screen_layout_config.json.
echo 'QPC QTCONF'; /proc/boot/bin/ls -l /opt/qt/config 2>&1; [ -f /opt/qt/config/screen_layout_config.json ] && /proc/boot/bin/cat /opt/qt/config/screen_layout_config.json
qm_spawn() { qm_pb=$1; qm_pt=$2; qm_pu=$3; shift 3
 [ -n "$qm_on" ] || { echo "QPC SPAWN no-on $qm_pb"; return 0; }
 "$qm_on" -T "$qm_pt" -u "$qm_pu" "$qm_pb" "$@" > /var/log/peer.log 2>&1 & echo "QPC SPAWN $qm_pb pid=$!"; }
qm_spawn park_assist ic_apps_t 834:819
qm_spawn mcu_hb mcu_hb_t mcu_hb 1000
[ -e /slm/msg_center.xml ] && { "$qm_slm" -V /slm/msg_center.xml > /var/log/msg.log 2>&1 & echo "QPC SPAWN msg_center pid=$!"; }
qm_svcxml=/slm/service_apps.xml
if [ -n "$qm_slm" ] && [ -e "$qm_svcxml" ]; then
 "$qm_slm" -V "$qm_svcxml" > /var/log/svc.log 2>&1 & echo "QPC SVC spawned=$! xml=$qm_svcxml"
 [ -n "$qm_sleep0" ] && "$qm_sleep0" 15
else echo 'QPC SVC no-file'; fi
if [ -n "$qm_slm" ]; then
 [ -n "$qm_cat" ] && { echo 'QPC ICXML'; "$qm_cat" "$qm_icxml"; "$qm_cat" "$qm_svcxml"; }
 # Probe whether the boot images expose the preload shims; a failed preload is FATAL
 # (ldd: Could not preload) and silently kills the whole slm ic_qt generation.
 echo "QPC Dsnap=$([ -e "$qm_snap" ] && echo f || echo n)$([ -e "$qm_dd" ] && echo F || echo N)"
 # /slm/service_apps.xml has no ic_qt, so this is the compositor's only launch.
 [ -n "$qm_sleep0" ] && "$qm_sleep0" 30
 qm_wheel_tool=$(qm_pick /usr/bin/msg_center_test /bin/msg_center_test) || qm_wheel_tool=
 QGFX_PASSION_WHEEL_TOOL="$qm_wheel_tool" QGFX_WHEEL_RELAY=1 QGFX_PASSION_WHEEL_RELAY=1 QGFX_PRESENT_TRACE=1 LD_PRELOAD=$qm_dd:$qm_snap "$qm_slm" -V "$qm_icxml" > /var/log/slm.log 2>&1 & echo "QPC IC spawned=$! xml=$qm_icxml"
"$qm_sleep0" 4; echo "QPC ICLOG $(cat /var/log/slm.log 2>&1)"; : cat end
# 'showmem -s' reports sysram; '-t sysram' adds only noise.
qm_show=$(qm_pick /proc/boot/bin/showmem /bin/showmem /usr/bin/showmem) || qm_show=
echo "QPC MEM show=$qm_show"
qm_it=0
while [ "$qm_it" -lt 10 ]; do
 echo "QPC M t=$qm_it log=$(/bin/df -P /var/log 2>/dev/null | /usr/bin/awk 'NR==2{print $3"/"$2}') cores=$(/proc/boot/bin/ls /tmp/*.core.gz 2>/dev/null | /usr/bin/awk 'END{print NR+0}')"
 qm_it=$((qm_it+1)); [ -n "$qm_sleep0" ] && "$qm_sleep0" 3
done
 # ic_qt's stderr goes to /dev/slog2/stderr, which stays empty, so aborts are read from the guest RAM dump.
 # The harness starts its worker on the serial-log row 'QG3 READY', but 'slog2info -w > file' keeps the
 # ring on the guest disk, and its rows reach the log together, tens of seconds late, so stream 'QG3 ' to console.
 [ -n "$qm_slog2" ] && { "$qm_slog2" -w 2>&1 | /usr/bin/awk '/QG3 /{print}' & echo "QPC SLOGQ pid=$!"; }
 [ -n "$qm_slog2" ] && { "$qm_slog2" -w > /var/log/slogw.log 2>&1 & echo "QPC SLOGW pid=$!"; }
else echo 'QPC IC no-slm'; fi
# The show gate 'rootItem.opacity' (qmls/qnx_main.qml) has one source, 'icdriving/StartSelfCheck', behind five
# absent someipd-vlan1X (no EMAC): publishers are driven by hand.
qm_ipp=$(qm_pick /usr/bin/mega_ipc_pub /bin/mega_ipc_pub) || qm_ipp=
qm_mct=$(qm_pick /usr/bin/msg_center_test /bin/msg_center_test) || qm_mct=
# Each publisher stays detached: a tool blocked on a missing service must not stop the script.
# All instances register as the one name mega_ipc_pub-0, so parallel pubs unlink each other
# and only the last is delivered; a brief pause between pubs serialises the registrations.
qm_icq() { qm_qt=$1; qm_qu=$2; qm_qb=$3; shift 3
 [ -n "$qm_qb" ] || { echo "QPC IPC no-tool $qm_qt"; return 0; }
 if [ -n "$qm_icdir" ]; then "$qm_qb" "$@" >> /var/log/icpub.log 2>&1 &
 elif [ -n "$qm_on" ]; then "$qm_on" -T "$qm_qt" -u "$qm_qu" "$qm_qb" "$@" >> /var/log/icpub.log 2>&1 &
 else echo "QPC IPC no-on $qm_qt"; return 0; fi
 echo "QPC IPC pid=$! tool=$qm_qb args=$*"; [ -n "$qm_sleep0" ] && "$qm_sleep0" 1; }
echo "QPC IPC tools pub=$qm_ipp mct=$qm_mct ic_service=$([ -x /usr/bin/ic_service ] && echo yes || echo no)"
# The OEM's own key-injection daemon forwards steer keys (uartrpc_svc/steer_key, payload
# 6 bytes: 14 00 + 32-bit BE bitmap) to the compositor pid via screen_send_event; the wheel path
# of the cluster. OEM startup line: on -T input_service_t -u 815:815,68 /bin/input_service -p ivi_compositor.
if [ -x /bin/input_service ]; then
 # input_service_t/msg_center_t die in `on`'s secpol validator and a bare root run
 # gets UDS 'Permission denied'; the proven ic_apps_t gate accepts OEM tools (qm_icq pubs own
 # /tmp/mega_ipc_pub-0 fine), so the daemon rides the same type as the pubs.
 "$qm_on" -T ic_apps_t -u 834:819 /bin/input_service -p ivi_compositor > /var/log/input_service.log 2>&1 & echo "QPC SPAWN input_service pid=$!"
else echo 'QPC WHEEL no-input-service'; fi
# Started by SLM once someipd-vlan1X run; a second instance is never spawned.
[ -n "$qm_on" ] && [ -x /usr/bin/ic_service ] && ! "$qm_pidin" ar|grep -q n/ic_service && { "$qm_on" -T ic_apps_t -u 834:819 /usr/bin/ic_service > /var/log/icsvc.log 2>&1 & echo "QPC ICSVC pid=$!"; }
echo 'QPC ID'; /usr/bin/awk '/iceoryx|ic_apps|msg_center|^root/{print FILENAME": "$0}' /etc/group /etc/passwd 2>&1
[ -n "$qm_slog2" ] && { "$qm_slog2" > /var/log/s0.log 2>&1; /usr/bin/awk '/QG3 READY/{print}' /var/log/s0.log; }
# Theme listing: settled offline (theme-hunt-20261104): no QML theme in the supply.
if [ -n "$qm_pidin" ]; then qm_cmd "$qm_pidin" ar > /var/log/processes.log 2>&1; [ -n "$qm_cat" ] && "$qm_cat" /var/log/processes.log; fi
[ -n "$qm_cat" ] && { echo 'QPC SVCINIT'; "$qm_cat" /var/log/svc.log; }
echo "QPC Dcores $(/proc/boot/bin/ls -l /tmp/*.core.gz 2>&1)"
rm -f /tmp/*.core.gz
# Probes write to a file and are catted: a pipeline forks twice.
# This public Screen operation reports actual output; it synthesizes no vehicle state.
QGFX_SOURCE_CAPTURE=1 QGFX_EXPORT_SCREEN=1 QGFX_SCREEN_STREAM=continuous LD_PRELOAD=$qm_snap /proc/boot/bin/ls -d /var/log
rc=$?
echo "QPC CAPTURE exit=$rc; IC/frame/MCU acceptance remains external"
qm_ldd=$(qm_pick /usr/bin/ldd /proc/boot/bin/ldd) || qm_ldd=
echo "QPC Ddata=$(/proc/boot/bin/ls -ld /data 2>&1)"
# /data stays on the OEM's '/var/log/data'; the ALIVE tick carries the compositor pid.
qm_alive=0
while :; do
 [ -n "$qm_sleep0" ] && "$qm_sleep0" 30
 qm_alive=$((qm_alive+1))
 # Every tick: msg_center and the compositor both restart and drop earlier publications, which
 # leaves the frame black, so the gate and the parity topics re-publish while alive.
 qm_icq ic_apps_t 834:819 "$qm_ipp" -t icdriving/StartSelfCheck -m '{"value": 1}'
 # Parity topics: the cluster restarts its compositor per handover and must re-receive theme, language
 # and mileage-mode; DrivingInfo/Theme/Set also acks on DrivingInfo/Theme (QML publishes it retained).
 qm_icq ic_apps_t 834:819 "$qm_ipp" -t icdriving/UsageMode -m '{"value": 0}'
 qm_icq ic_apps_t 834:819 "$qm_ipp" -t DrivingInfo/Theme/Set -m '{"value": 1}'
 qm_icq ic_apps_t 834:819 "$qm_ipp" -t DrivingInfo/Language/Set -m '{"value": 2}'
 qm_icq ic_apps_t 834:819 "$qm_ipp" -t DrivingInfo/RemainingMileageMode/Set -m '{"value": 0}'
 # Vehicle signal ({ticks*11}) for the Step-2 freshness proof, republished every tick.

 qm_icq ic_apps_t 834:819 "$qm_ipp" -t icdriving/VehicleSpeed -m "{\"value\": $((qm_alive*11))}"
 # Tails of live-written logs hang on the QNX6 file lock (runs 030/043/044), so the
 # GFXLOG re-dump and RouDi's tail run only from the background group, and the roudi
 # tail only after a roudi death (a living roudi keeps the file growing, tail -n never
 # returns, and the child serial writer wedges every later echo).
 if [ -n "$qm_ioxl" ] && [ -n "$qm_pidin" ] && [ -z "$("$qm_pidin" ar 2>/dev/null | /usr/bin/awk '/roudi/{print $1; exit}')" ]; then
  { echo 'QPC RDLOG BEGIN'; /usr/bin/tail -n 8 $qm_ioxl 2>&1; echo 'QPC RDLOG END'; } &
 fi
 rm -f /tmp/*.core.gz
 # The bare mask is safe: the ALIVE filter below matches the full path, not this rm line.
 echo "QPC ALIVE ticks=$qm_alive compositor=$([ -n "$qm_pidin" ] && "$qm_pidin" ar 2>/dev/null | /usr/bin/awk '/\/usr\/bin\/ivi_compositor/{print $1; exit}')"
 echo "QPC DEVS screen=$([ -e /dev/screen ] && echo present || echo absent)"
 [ -n "$qm_pidin" ] && "$qm_pidin" 2>/dev/null | /usr/bin/awk '/\/usr\/bin\/ivi_compositor/{print}'
 # Run-033..036 root cause: slogger2 (pid found via pidin) wedged in REPLY to a dead
 # client (a qm_cmd shim child) and every gfx process blocks forever on its first
 # slog2 write (SEND to slogger2) — screen never inits, /dev/screen never appears.
 # Restart slogger2 once at tick 2: the kernel unblocks the pending sends when the
 # server dies, the gfx stack then initializes (or names its next failure to slog2).
 [ "$qm_alive" = 2 ] && { qm_sl2=$("$qm_pidin" ar 2>/dev/null | /usr/bin/awk '/slogger2/{print $1; exit}')
  echo "QPC SLOG2 pid=${qm_sl2:-none}"
  [ -n "$qm_sl2" ] && { kill -9 "$qm_sl2" 2>&1; echo "QPC SLOG2 kill rc=$?"
   # The wedged server dies but nobody respawns it (measured run-037: 'SLOG2 after pid=none'),
   # so every later slog2 client (slog2info -w, the slm '(Loading)' components awaiting their
   # config logs) waits on /dev/slog forever. Restart it; the OEM image has no slogger2
   # path in the script, so try /proc/boot and /bin the same way as every other tool pick.
   qm_sl2bin=$(qm_pick /proc/boot/bin/slogger2 /bin/slogger2 /sbin/slogger2) || qm_sl2bin=
   echo "QPC SLOG2 bin=${qm_sl2bin:-missing}"
   [ -n "$qm_sl2bin" ] && { "$qm_sl2bin" > /var/log/slogger2.log 2>&1 & echo "QPC SLOG2 respawn pid=$!"; } } }
 [ "$qm_alive" = 3 ] && { qm_sl2b=$("$qm_pidin" 2>/dev/null | /usr/bin/awk '/slogger2/{print $1; exit}')
  echo "QPC SLOG2 after pid=${qm_sl2b:-none}"
  # slm lost its ic_qt child while slogger2 was dead (measured run-038: "Error while
  # starting component 'ic_qt': No such process"); with the logger back, restart the
  # ic_qt group so ivi_compositor gets its launch.
  [ -n "$qm_sl2b" ] && [ -n "$qm_slm" ] && { QGFX_PASSION_WHEEL_TOOL="$qm_wheel_tool" QGFX_WHEEL_RELAY=1 QGFX_PASSION_WHEEL_RELAY=1 QGFX_PRESENT_TRACE=1 LD_PRELOAD=$qm_dd:$qm_snap "$qm_slm" -V "$qm_icxml" > /var/log/ic-restart.log 2>&1 & echo "QPC SLMIQ restart pid=$!"; }
  "$qm_pidin" 2>/dev/null | /usr/bin/awk '/slogger2|ivi_compositor|kgsl|screen|disp_mgr/{print}' | head -n 20; echo 'QPC SLOGCHK end'; }
 [ "$qm_alive" = 2 ] && { echo 'QPC DUMPBG start'
  for qm_lf in /var/log/smmu.log /var/log/kgsl.log /var/log/gsl.log /var/log/wfdbe.log /var/log/dispmgr.log /var/log/screen.log /var/log/openwfd.log /var/log/bridgechip.log; do
   { echo "QPC GFXDUMPS $qm_lf BEGIN"; /usr/bin/tail -n 15 "$qm_lf" 2>&1; echo "QPC GFXDUMPS $qm_lf END"; } &
  done
  { echo 'QPC GFXPIDS BEGIN'; "$qm_pidin" 2>/dev/null | /usr/bin/awk '/smmu_service|kgsl|screen|disp_mgr|openwfd|wfd_be|bridgechip/{print}'; echo 'QPC GFXPIDS END'; } &
  # Run-033..035: every script-spawned gfx process sits in 'SEND 12292' — a MsgSend nobody
  # receives; 'pidin conn' is invalid shorthand and 'pidin info' prints only the header.
  # Name the destination another way: valid pidin shorthands (usage), the fds of the blocked
  # pids (which devices they opened), and the slog2 tail naming the failed gfx init.
  { echo 'QPC GFXUSAGE BEGIN'; "$qm_pidin" 2>&1 | /usr/bin/awk 'NR<=200{print}'; echo 'QPC GFXUSAGE END'; } &
  { echo 'QPC GFXFDS BEGIN'; "$qm_pidin" fds 2>&1 | /usr/bin/awk '/kgsl|screen|disp_mgr|openwfd|wfd_be|bridgechip|smmu/{p=1} /^$/{p=0} p{print} NR<=500{print>"/dev/null"}'; echo 'QPC GFXFDS END'; } &
  { echo 'QPC GFXSLOG BEGIN'; [ -n "$qm_slog2" ] && "$qm_slog2" 2>&1 | /usr/bin/awk '/kgsl|screen|disp_mgr|openwfd|wfd_be|bridgechip|smmu|gpu|adreno/{print}'; echo 'QPC GFXSLOG END'; } &
  { echo 'QPC ICSVCLOG BEGIN'; /usr/bin/tail -n 20 /var/log/icsvc.log 2>&1; echo 'QPC ICSVCLOG END'; } &
  { echo 'QPC SLMTAIL BEGIN'; /usr/bin/tail -n 30 /var/log/slm.log 2>&1; echo 'QPC SLMTAIL END'; } &
  { echo 'QPC SVCTAIL BEGIN'; /usr/bin/tail -n 6 /var/log/svc.log 2>&1; echo 'QPC SVCTAIL END'; } &
  echo 'QPC DUMPBG end'; }
 [ -n "$qm_chmod0" ] && "$qm_chmod0" 0666 /tmp/roudi /dev/shmem/iceoryx_mgmt /dev/shmem/root /dev/shmem/iceoryx 2>/dev/null
 # Run-040: compositor deaths surface only in the slog2 ring (ic_qt stderr is
 # /dev/slog2/stderr), so dump the matched ring every tick as its own child.
 { echo 'QPC SLIVE BEGIN'; [ -n "$qm_slog2" ] && "$qm_slog2" 2>&1 | /usr/bin/awk '/ivi_compositor|vehicle_control|health_monitor|qt.qpa|QQnx|eglfs|SelfCheck|qnx_main|Crash|abort/{print}'; echo 'QPC SLIVE END'; } &
 # Run-040: slm retired the compositor after three ~1 s attempts, so re-run the ic_qt
 # group every tick while no compositor pid exists; the slm of a running one is a no-op.
 qm_cmp=$( [ -n "$qm_pidin" ] && "$qm_pidin" ar 2>/dev/null | /usr/bin/awk '/\/usr\/bin\/ivi_compositor/{print $1; exit}')
 # Run-041: bare slm restarts lost the Snapdragon.rgb EGL preload of the original IC
 # spawn (every instance died at 'failed to initialize EGL display, err=12291'), so the
 # rtick restart carries the same environment as the first spawn.
 [ -z "$qm_cmp" ] && [ -n "$qm_slm" ] && { QGFX_PASSION_WHEEL_TOOL="$qm_wheel_tool" QGFX_WHEEL_RELAY=1 QGFX_PASSION_WHEEL_RELAY=1 QGFX_PRESENT_TRACE=1 LD_PRELOAD=$qm_dd:$qm_snap "$qm_slm" -V "$qm_icxml" > /var/log/ic-restart.log 2>&1 & echo "QPC SLMIQ rtick=$qm_alive pid=$!"; }
done

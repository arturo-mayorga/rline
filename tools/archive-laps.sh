#!/bin/bash
# Rescue every completed lap CSV out of relay's reused filenames, and check the
# quality of each one as it lands.
#
# relay names each file from iRacing's lap counter, so lap-NNNN.csv is rewritten
# every time the counter returns to NNNN - on any reset to pits, not just on a
# relay restart. Three things have cost real laps here:
#
#   timing    A finished lap exists only until the driver resets, which can be
#             seconds. A version polling every 8 s with a 10 s settle had a copy
#             window the same size as the survival window and saved 1 of 8
#             flying laps on 2026-08-05.
#
#   naming    Destinations keyed on <HHMMSS>-lap-NNNN collide across stints: he
#             drives the same hour most evenings, and "[ -e ] || cp" then
#             SILENTLY DROPS the new lap. Names now carry the full date and a
#             content hash, so distinct laps can never collide and identical
#             ones are never duplicated (re-running this script is a no-op).
#
#   the last  Even at a 1 s poll and a 3 s settle, a reset within ~3 s of
#   3 seconds crossing the line truncates the file before the settle expires.
#             That lost one lap on 2026-08-05 during a run of rapid
#             reset-to-pits laps. So the settle scan is now only a safety net:
#             the primary trigger is relay's own log. relay prints
#             "lap N written" *after* fclose, so that line is a guarantee the
#             file is complete, and it can be copied that instant with no wait.
#
# Every archived lap is passed through lap-qc.py and the verdict appended to
# qc.log, so splices, partial laps and push-to-pass laps are visible DURING the
# stint rather than in the post-mortem.
POLL=1
SETTLE=3
MINSZ=4000000
SRC=/mnt/c/rline-coach/laps
DEST=/mnt/c/rline-coach/archive/live
QCLOG=/mnt/c/rline-coach/archive/qc.log
RELAYLOG=${RELAYLOG:-/mnt/c/Users/amayorga/rline-build/relay-out.txt}
QC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lap-qc.py"

cd "$SRC" || exit 1
mkdir -p "$DEST" "$(dirname "$QCLOG")"
declare -A seen

# Copy one lap file if we do not already have its exact contents.
take()
{
  local f=$1 why=$2
  [ -f "$f" ] || return
  local sz m
  sz=$(stat -c%s "$f" 2>/dev/null) || return
  m=$(stat -c%Y "$f" 2>/dev/null) || return
  [ "$sz" -gt "$MINSZ" ] || return

  local key="$f:$sz:$m"
  [ "${seen[$f]}" = "$key" ] && return

  # Hash the SOURCE, not a copy: on startup we rescan every lap still in the
  # directory, and hashing (~60 ms) is far cheaper than copying 8 MB only to
  # find we already have it. A slow rescan blocks this serial loop and can make
  # it miss a live lap.
  local hash lapno stamp dest tmp
  hash=$(md5sum "$f" 2>/dev/null | cut -c1-8)
  [ -n "$hash" ] || return
  lapno=$(echo "$f" | sed -n 's/^lap-\([0-9]*\)\.csv$/\1/p')
  stamp=$(date -d @"$m" +%Y%m%d-%H%M%S)
  dest="$DEST/${stamp}-lap${lapno}-${hash}.csv"

  if [ -e "$dest" ]; then
    seen[$f]="$key"
    return
  fi

  # NEVER mark seen before the copy succeeds. relay holds the file open while
  # it writes, so cp fails intermittently; marking it handled first means the
  # retry never happens and the lap dies when relay reuses the name.
  tmp="$DEST/.incoming-$$-$RANDOM.csv"
  if ! cp "$f" "$tmp" 2>/dev/null; then
    rm -f "$tmp"
    echo "$(date +%H:%M:%S) cp failed for $f, will retry" >&2
    return
  fi
  mv "$tmp" "$dest"
  seen[$f]="$key"
  echo "$(date +%H:%M:%S) [$why] $(python3 "$QC" "$dest" 2>&1)" | tee -a "$QCLOG"
}

# Where the relay log had got to when we started, so a restart does not replay
# the whole session's worth of announcements.
loglines=$(wc -l < "$RELAYLOG" 2>/dev/null || echo 0)

while true; do
  # Primary trigger: relay announcing a completed lap. The file is closed by
  # the time this line exists, so copy it immediately rather than waiting for a
  # settle that a fast reset would beat.
  if [ -r "$RELAYLOG" ]; then
    now=$(wc -l < "$RELAYLOG" 2>/dev/null || echo 0)
    if [ "$now" -lt "$loglines" ]; then
      loglines=0 # relay restarted and truncated its log
    fi
    if [ "$now" -gt "$loglines" ]; then
      while read -r n; do
        [ -n "$n" ] && take "$(printf 'lap-%04d.csv' "$n")" log
      done < <(tail -n +$((loglines+1)) "$RELAYLOG" 2>/dev/null |
               sed -n 's/^relay: lap \([0-9]\+\) written.*/\1/p')
      loglines=$now
    fi
  fi

  # Safety net: anything that has been sitting still long enough, in case the
  # relay log is unavailable or a lap was written before this started.
  for f in lap-*.csv; do
    [ -f "$f" ] || continue
    m=$(stat -c%Y "$f" 2>/dev/null) || continue
    now=$(date +%s)
    [ $((now-m)) -ge "$SETTLE" ] || continue
    take "$f" scan
  done

  sleep "$POLL"
done

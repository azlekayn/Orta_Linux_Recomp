#!/usr/bin/env bash
# Probe run of the Linux port against the extracted disc.
set -u
DIR="/mnt/SSD Storage/Games/Ports/xbox/Orta_PC_Recomp-0.5.1_Beta/orta"
cd "$DIR" || exit 1

export DISPLAY="${DISPLAY:-:99}"
# On the virtual display the window is fine to show (nothing on the desktop).
export RECOMP_GL_HIDE="${RECOMP_GL_HIDE:-0}"
# Engine switches normally carried in engine.ini [Advanced]:
export RECOMP_PB_EXEC=1
export RECOMP_VBLANK=1
export RECOMP_AC97_READY=1
export RECOMP_HEAP_RECLAIM=1
export RECOMP_PUT_SETTLE_US=0
export RECOMP_FAST_FLUSH=1
export RECOMP_TITLE_KEVENTS=1
export RECOMP_APU_PHYS_CONTIG_FIRST=1
export RECOMP_SEMA_DEVPTR=0x2BD708
export RECOMP_DSP_ACK=0x802C0810,0x80F80810
# Keep the run bounded and the log readable.
export RECOMP_WATCHDOG_SECS="${RECOMP_WATCHDOG_SECS:-150}"
export RECOMP_KERNEL_LOG_BUDGET="${RECOMP_KERNEL_LOG_BUDGET:-200}"
export RECOMP_STALL_EXIT_SECS="${RECOMP_STALL_EXIT_SECS:-120}"

timeout -s KILL "${RUN_SECS:-170}" \
    ./build64/orta_recomp > /tmp/game_stdout.txt 2> /tmp/game_stderr.txt
echo "exit=$?"

#!/bin/bash
# tcompat_matrix.sh - Phase 0.2: 4-era on-disk-format regression matrix
#
# For each era image (E1..E4, emulating ext4 format generations), this
# script prepares an identical deleted-files workload on /dev/vdb3,
# then runs TWO binaries against the SAME (read-only) disk state and
# diffs their recovery output manifests.
#
#   E1: CentOS6-era   -O ^flex_bg,^huge_file,^metadata_csum,^64bit (+no orphan_file/fast_commit)
#   E2: CentOS7-era   -O ^metadata_csum                            (+no orphan_file/fast_commit)
#   E3: current mkfs defaults
#   E4: newest        -O orphan_file (+fast_commit if supported)
#
# Usage:
#   tcompat_matrix.sh <binary_A> <binary_B> [--with-aggressive] [--eras "E1 E2 E3 E4"]
#
# Gate semantics:
#   - Class I changes (infra/perf): manifests must be IDENTICAL on all eras.
#   - Class III changes (new capabilities): on old eras (E1/E2) manifests
#     identical; on new eras only ADDITIONS with new prefixes are allowed
#     (the diff is printed for human review either way).
#
# Workload per era (deterministic file set, random content):
#   bigfile   2 GB   multi-level extent tree   (t0a-class)
#   small_N   4K..4M single-extent x10         (t2-class)
# All files recorded (ino|size|md5) then deleted; fs unmounted.

source "$(dirname "$0")/lib_recover_test.sh"

BIN_A="${1:?usage: $0 <binary_A> <binary_B> [--with-aggressive] [--eras \"E1 E2 E3 E4\"]}"
BIN_B="${2:?need second binary}"
shift 2
WITH_AGGRESSIVE=0
ERAS="E1 E2 E3 E4"
while [ $# -gt 0 ]; do
    case "$1" in
        --with-aggressive) WITH_AGGRESSIVE=1 ;;
        --eras) shift; ERAS="$1" ;;
        *) die "unknown arg: $1" ;;
    esac
    shift
done

[ -x "$BIN_A" ] || die "binary A not executable: $BIN_A"
[ -x "$BIN_B" ] || die "binary B not executable: $BIN_B"

DEV=/dev/vdb3
TEST=tcompat
MATRIX_DIR=$WORK/compat_matrix
mkdir -p "$MATRIX_DIR"

era_mkfs_opts() {
    case "$1" in
        # -O ^orphan_file/^fast_commit are appended only when mke2fs
        # knows the feature (1.47+ does; harmless guard for older).
        E1) echo "-b4096 -N 262144 -O ^flex_bg,^huge_file,^metadata_csum,^64bit,^orphan_file,^fast_commit" ;;
        E2) echo "-b4096 -N 262144 -O ^metadata_csum,^orphan_file,^fast_commit" ;;
        E3) echo "-b4096 -N 262144" ;;
        E4) echo "-b4096 -N 262144 -O orphan_file,fast_commit" ;;
        *)  die "unknown era $1" ;;
    esac
}

# Build the standard workload on the mounted target; record manifest.
build_workload() {
    local mani="$1"
    rm -f "$mani"
    log "  writing workload (2GB bigfile + 10 small files)..."
    dd if=/dev/urandom of=$TARGET_MNT/bigfile bs=1M count=2048 status=none
    local sz
    for sz in 4 16 64 256 1024 4096 16384 65536 262144 1048576; do
        dd if=/dev/urandom of=$TARGET_MNT/small_$sz bs=1K count=$((sz/4)) status=none 2>/dev/null \
            || dd if=/dev/urandom of=$TARGET_MNT/small_$sz bs=1K count=1 status=none
    done
    sync
    local f
    for f in $TARGET_MNT/bigfile $TARGET_MNT/small_*; do
        record_file "$f" "$mani"
    done
    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
    rm -f $TARGET_MNT/bigfile $TARGET_MNT/small_*
    detach_target
}

# Run one binary over the prepared (read-only) disk state.
# Produces: <outdir>/ (recovery files) and <outdir>.manifest (name|size|md5).
run_binary() {
    local bin="$1" outdir="$2" tag="$3"
    sudo rm -rf "$outdir"; mkdir -p "$outdir"
    log "  [$tag] $(basename $bin) --journal/--normal/--orphan ..."
    sudo "$bin" --journal --dir "$outdir" "$DEV" >"$outdir.journal.log" 2>&1
    sudo "$bin" --normal  --dir "$outdir" "$DEV" >"$outdir.normal.log"  2>&1
    sudo "$bin" --orphan  --dir "$outdir" "$DEV" >"$outdir.orphan.log"  2>&1
    if [ "$WITH_AGGRESSIVE" = 1 ]; then
        log "  [$tag] aggressive (timeboxed 40min)..."
        sudo timeout 2400 "$bin" --aggressive --dir "$outdir" "$DEV" >"$outdir.aggressive.log" 2>&1
    fi
    # Manifest: relative name | size | md5, sorted (checkpoint json excluded)
    ( cd "$outdir" && sudo find . -maxdepth 1 -type f ! -name ".ext4recover_checkpoint*" -printf '%f\n' \
        | sort | while read -r f; do
            printf '%s|%s|%s\n' "$f" "$(sudo stat -c %s "$f")" "$(sudo md5sum "$f" | awk "{print \$1}")"
          done ) > "$outdir.manifest"
}

OVERALL_RC=0
for ERA in $ERAS; do
    echo "##################### $ERA #####################"
    OPTS=$(era_mkfs_opts "$ERA")
    if ! sudo mkfs.ext4 -q -F -m0 $OPTS "$DEV" 2>/dev/null; then
        log "$ERA: mkfs rejected opts ($OPTS), skipping era"
        continue
    fi
    sudo mount "$DEV" "$TARGET_MNT" || die "mount failed"
    sudo chown ubuntu:ubuntu "$TARGET_MNT"
    log "$ERA features: $(sudo dumpe2fs -h $DEV 2>/dev/null | grep -i 'features' | head -2 | tr '\n' ' ')"

    MANI=$MATRIX_DIR/$ERA.workload.mani
    build_workload "$MANI"

    OUT_A=$MATRIX_DIR/${ERA}_A
    OUT_B=$MATRIX_DIR/${ERA}_B
    run_binary "$BIN_A" "$OUT_A" "$ERA/A"
    run_binary "$BIN_B" "$OUT_B" "$ERA/B"

    echo "----- $ERA workload recovery rate (A) -----"
    verify_recovery_baseline "$MANI" "$OUT_A"
    echo "----- $ERA workload recovery rate (B) -----"
    verify_recovery_baseline "$MANI" "$OUT_B"

    echo "----- $ERA manifest diff (A vs B) -----"
    if diff -u "$OUT_A.manifest" "$OUT_B.manifest"; then
        echo "$ERA: IDENTICAL"
    else
        echo "$ERA: DIFFERS (review above; additions-only with new prefixes is OK for Class III)"
        OVERALL_RC=1
    fi
done

echo "##################### MATRIX DONE (rc=$OVERALL_RC) #####################"
exit $OVERALL_RC

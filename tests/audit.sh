#!/bin/bash
# 修复版verify_recovery_baseline,加sudo + 直接md5(不用临时文件)
source /mnt/work/testfw/lib_recover_test.sh

verify_recovery_baseline() {
    local mani="$1" rdir="$2"
    local total=0 ok=0
    while IFS='|' read -r ino size md5 path; do
        total=$((total+1))
        local rf
        rf=$(sudo find "$rdir" -name "${ino}_file" 2>/dev/null | head -1)
        [ -z "$rf" ] && rf="$rdir/${ino}_file"
        if sudo test ! -f "$rf"; then
            echo "  MISS ino=$ino size=$size $(basename $path)"
            continue
        fi
        local rmd5
        rmd5=$(sudo head -c "$size" "$rf" 2>/dev/null | md5sum | awk '{print $1}')
        if [ "$rmd5" = "$md5" ]; then
            ok=$((ok+1))
            echo "  OK   ino=$ino size=$size $(basename $path) md5=$rmd5"
        else
            echo "  FAIL ino=$ino orig=$md5 got=$rmd5 $(basename $path)"
        fi
    done < "$mani"
    echo "RESULT: $ok/$total"
    [ "$ok" -gt 0 ] && [ "$ok" -eq "$total" ]
}

echo "##### Phase 0 Final Audit (using existing recovery products) #####"
echo
echo "=== ORIG (last run) ==="
verify_recovery_baseline /mnt/work/manifests/t0a_largefile.mani \
    /mnt/work/recover_out/t0a_largefile_orig/RECOVER
echo
echo "=== V5 --normal (latest run /tmp/v5dbg2) ==="
verify_recovery_baseline /mnt/work/manifests/t0a_largefile.mani \
    /tmp/v5dbg2

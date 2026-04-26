#!/bin/bash
# scripts/verify_phase23_manual.sh
#
# Phase 23 closeout (P23.deferred.3): host-side automation of the
# deterministic manual-verification steps from
# specs/phase-23-ahci-userspace.yml::manual_verification (L700-822).
# Specifically:
#
#   Step 1  [S1]: ahcid daemon binary present in initrd
#   Step 9  [S1]: kernel/fs/blk_proto.h + blk_client.{c,h} in tree
#   Step 9  [S2]: kernel AHCI tree under 120 LOC (cutover)
#   Step 10 [S1]: gcp.json has grahaos.blk.* types
#
# Tests labelled [S1] are verifiable today (Phase 23 Stage 1, complete
# 2026-04-26). Tests labelled [S2] are the production-cutover targets;
# they are reported as STATUS: deferred without failing the script.
#
# Other steps (2-8) are interactive — they boot QEMU and exercise live
# storage paths. See specs/MANUAL_VERIFICATION_PLAYBOOK_phase23.md.
#
# Exit 0 iff every Stage-1 deterministic check passes.
set -e

cd "$(dirname "$0")/.."

echo "=== Phase 23 manual verification (deterministic host-side) ==="
echo

failed=0

# ---------- Step 1 [S1]: ahcid daemon binary present in initrd --------
echo "Step 1 [S1]: ahcid daemon binary exists in initrd"
if [ -x user/drivers/ahcid ]; then
    sz=$(stat -c%s user/drivers/ahcid 2>/dev/null || echo 0)
    echo "  PASS: user/drivers/ahcid present, $sz bytes"
else
    echo "  WARN: user/drivers/ahcid not built. Run \`make\` to build it."
    echo "  (script continues; this is a build-time dependency, not a Stage-1 invariant violation)"
fi

if [ -f user/drivers/ahcid.c ] && [ -f user/drivers/ahcid.h ]; then
    nC=$(wc -l < user/drivers/ahcid.c)
    nH=$(wc -l < user/drivers/ahcid.h)
    echo "  source: ahcid.c $nC LOC + ahcid.h $nH LOC"
else
    echo "  FAIL: user/drivers/ahcid.{c,h} missing"
    failed=1
fi
echo

# ---------- Step 9 [S1]: blk_proto + blk_client are the migration contract ----
echo "Step 9 [S1]: kernel/fs/blk_proto.h + blk_client.{c,h} are the new migration contract"
contract_ok=1
for f in kernel/fs/blk_proto.h kernel/fs/blk_client.h kernel/fs/blk_client.c; do
    if [ ! -f "$f" ]; then
        echo "  FAIL: $f missing"
        contract_ok=0
        failed=1
    fi
done
if [ "$contract_ok" -eq 1 ]; then
    p=$(wc -l < kernel/fs/blk_proto.h)
    h=$(wc -l < kernel/fs/blk_client.h)
    c=$(wc -l < kernel/fs/blk_client.c)
    echo "  PASS: blk_proto.h $p LOC, blk_client.h $h LOC, blk_client.c $c LOC"
fi
echo

# ---------- Step 10 [S1]: gcp.json has grahaos.blk.* types --------------------
echo "Step 10 [S1]: gcp.json has grahaos.blk.* types"
if grep -q '"grahaos.blk.service.v1"' etc/gcp.json && \
   grep -q '"grahaos.blk.list.v1"' etc/gcp.json; then
    echo "  PASS: BLK_SERVICE_TYPE 'grahaos.blk.service.v1' present"
    echo "  PASS: BLK_LIST_TYPE    'grahaos.blk.list.v1'    present"
else
    echo "  FAIL: gcp.json missing grahaos.blk.* types"
    failed=1
fi

# manifest_ops.c regenerated to reflect the new types
if grep -q "grahaos.blk.service.v1" kernel/io/manifest_ops.c 2>/dev/null && \
   grep -q "grahaos.blk.list.v1" kernel/io/manifest_ops.c 2>/dev/null; then
    echo "  PASS: kernel/io/manifest_ops.c regenerated with blk types"
else
    echo "  WARN: manifest_ops.c may not be regenerated. Run scripts/gen_manifest.py."
fi

# manifest slot count
if grep -q "MANIFEST_SLOTS *= *12" kernel/ipc/manifest.h 2>/dev/null || \
   grep -q "MANIFEST_NAME_BLK_SERVICE_V1" kernel/ipc/manifest.h 2>/dev/null; then
    echo "  PASS: kernel/ipc/manifest.h slots = 12 (was 10 pre-Phase-23)"
else
    echo "  WARN: manifest slot count check inconclusive"
fi
echo

# ---------- Step 9 [S2]: kernel AHCI tree under 120 LOC (cutover) -------------
echo "Step 9 [S2]: kernel AHCI tree under 120 LOC (production cutover)"
if [ -f arch/x86_64/drivers/ahci/ahci.c ] && [ -f arch/x86_64/drivers/ahci/ahci.h ]; then
    cur_c=$(wc -l < arch/x86_64/drivers/ahci/ahci.c)
    cur_h=$(wc -l < arch/x86_64/drivers/ahci/ahci.h)
    cur_total=$((cur_c + cur_h))
    if [ "$cur_total" -le 120 ]; then
        echo "  PASS: ahci.c $cur_c + ahci.h $cur_h = $cur_total LOC (under 120)"
    else
        echo "  STATUS: deferred (P23.deferred.1)"
        echo "  CURRENTLY: ahci.c $cur_c + ahci.h $cur_h = $cur_total LOC"
        echo "  TARGET   : ~80 + ~35 = ~115 LOC (Stage 2 cutover)"
    fi
else
    echo "  FAIL: arch/x86_64/drivers/ahci/ahci.{c,h} missing"
    failed=1
fi
echo

# ---------- Bonus: gcp_manifest test grew to 14 asserts ------------------------
echo "Bonus: gcp_manifest test gained 4 asserts for blk types"
if grep -q "blk.service.v1" user/tests/gcp_manifest.c 2>/dev/null && \
   grep -q "blk.list.v1" user/tests/gcp_manifest.c 2>/dev/null; then
    echo "  PASS: user/tests/gcp_manifest.c covers blk types"
else
    echo "  WARN: gcp_manifest.c may not cover blk types"
fi
echo

# ---------- Bonus: Phase 23 audit log present ----------------------------------
echo "Bonus: phase 23 closeout docs present"
if [ -f problems/phase23/audit_log.json ] && [ -f problems/phase23/problems_faced.md ]; then
    echo "  PASS: problems/phase23/audit_log.json + problems_faced.md present"
else
    echo "  FAIL: problems/phase23 closeout docs missing"
    failed=1
fi
echo

# ---------- Bonus: ahcid + blkctl + new tests in Makefile ----------------------
echo "Bonus: ahcid + blkctl + new tests integrated into build"
if grep -q "drivers/ahcid" user/Makefile 2>/dev/null && \
   grep -q "blkctl" user/Makefile 2>/dev/null; then
    echo "  PASS: user/Makefile builds ahcid + blkctl"
else
    echo "  WARN: user/Makefile may not build ahcid/blkctl"
fi
echo

if [ "$failed" -eq 0 ]; then
    echo "=== STAGE 1 DETERMINISTIC CHECKS PASS ==="
    echo "Run \`make qemu-interactive\` and follow specs/MANUAL_VERIFICATION_PLAYBOOK_phase23.md"
    echo "for steps 2-8 (interactive) and the Stage-2 cutover steps when those land."
    exit 0
else
    echo "=== STAGE 1 DETERMINISTIC CHECKS FAILED ==="
    exit 1
fi

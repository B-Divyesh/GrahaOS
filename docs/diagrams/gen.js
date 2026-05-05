#!/usr/bin/env node
// Generate technical SVG diagrams for the GrahaOS report.
const fs = require('fs');
const path = require('path');

const OUT = __dirname;

// ----- helpers --------------------------------------------------------------
const svg = (w, h, body, title='') =>
`<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}" font-family="Calibri, Helvetica, Arial, sans-serif">
<style>
  .box   { fill:#fafafa; stroke:#222; stroke-width:1.5; }
  .accent{ fill:#dbe9ff; stroke:#244a91; stroke-width:1.5; }
  .warn  { fill:#fde8c5; stroke:#8a5a18; stroke-width:1.5; }
  .ok    { fill:#dcefd6; stroke:#2c5d2c; stroke-width:1.5; }
  .danger{ fill:#fbd9d9; stroke:#8a1f1f; stroke-width:1.5; }
  .lbl   { font-size:13px; fill:#111; }
  .lbl-sm{ font-size:11px; fill:#222; }
  .lbl-xs{ font-size:10px; fill:#333; }
  .ttl   { font-size:14px; font-weight:bold; fill:#111; }
  .mono  { font-family:"Courier New", monospace; font-size:11px; fill:#111; }
  .arrow { fill:none; stroke:#222; stroke-width:1.5; marker-end:url(#arrow); }
  .arrowd{ fill:none; stroke:#222; stroke-width:1.5; stroke-dasharray:5,3; marker-end:url(#arrow); }
  .gridln{ stroke:#bbb; stroke-width:0.5; stroke-dasharray:2,2; }
  text   { dominant-baseline:middle; }
</style>
<defs>
  <marker id="arrow" markerWidth="10" markerHeight="10" refX="9" refY="3" orient="auto" markerUnits="strokeWidth">
    <path d="M0,0 L0,6 L9,3 z" fill="#222"/>
  </marker>
</defs>
${'' /* caption rendered by docx, not embedded */}
${body}
</svg>`;

const box = (x,y,w,h,label,cls='box',sub='') => `
<rect x="${x}" y="${y}" width="${w}" height="${h}" class="${cls}" rx="4" ry="4"/>
<text x="${x+w/2}" y="${y+h/2 - (sub?7:0)}" text-anchor="middle" class="lbl">${label}</text>
${sub ? `<text x="${x+w/2}" y="${y+h/2+9}" text-anchor="middle" class="lbl-xs">${sub}</text>` : ''}`;

const ar = (x1,y1,x2,y2,cls='arrow',label='') => {
  const mx = (x1+x2)/2, my = (y1+y2)/2 - 6;
  return `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" class="${cls}"/>
${label ? `<text x="${mx}" y="${my}" text-anchor="middle" class="lbl-sm">${label}</text>` : ''}`;
};

// ----- 1. System architecture ----------------------------------------------
fs.writeFileSync(path.join(OUT, 'fig-architecture.svg'), svg(820, 540, `
${box(40, 50, 740, 60, 'Hardware (x86_64): CPU · MMU · APIC · IOAPIC · AHCI · NIC · Framebuffer','box')}
${box(40, 130, 740, 130, 'Kernel half (Ring 0)','accent')}
${box(60, 162, 160, 38, 'PMM / VMM','box','HHDM, COW')}
${box(230, 162, 160, 38, 'Scheduler / SMP','box','per-CPU runq')}
${box(400, 162, 170, 38, 'Capability core','box','tokens · objects')}
${box(580, 162, 180, 38, 'Channels · VMOs','box','typed IPC')}
${box(60, 210, 160, 38, 'Submission Streams','box','SQE / CQE rings')}
${box(230, 210, 160, 38, 'GrahaFS v2 core','box','journal · GC')}
${box(400, 210, 170, 38, 'Snapshot · Txn','box','barrier · replay')}
${box(580, 210, 180, 38, 'Audit · Pledge','box','256 B records')}

${box(40, 280, 740, 220, 'User half (Ring 3)','warn')}
${box(60, 320, 160, 50, 'init / gash / grahai','box','PID 1 · shell · agent')}
${box(230, 320, 160, 50, 'libtui · libc','box','frame compositing')}
${box(400, 320, 170, 50, 'libnet · libhttp · libtls','box','client surface')}
${box(580, 320, 180, 50, 'tests (40+)','box','TAP gate harness')}
${box(60, 380, 160, 50, 'ahcid','box','AHCI driver')}
${box(230, 380, 160, 50, 'e1000d','box','NIC driver')}
${box(400, 380, 170, 50, 'netd','box','TCP/IP stack')}
${box(580, 380, 180, 50, 'wasmd','box','Wasmtime substrate')}
${box(60, 450, 700, 38, '/etc/gcp.json — single public manifest (kernel + user share verbatim)','accent')}

<text x="20" y="80" text-anchor="start" class="lbl-sm">HW</text>
<text x="20" y="195" text-anchor="start" class="lbl-sm">K</text>
<text x="20" y="400" text-anchor="start" class="lbl-sm">U</text>
${ar(410, 280, 410, 260)}
${ar(410, 130, 410, 110)}
`, 'Figure 5.1 — High-Level System Architecture'));

// ----- 2. Boot flow ---------------------------------------------------------
fs.writeFileSync(path.join(OUT, 'fig-bootflow.svg'), svg(820, 380, `
${box(20, 60, 130, 60, 'Limine','accent','UEFI loader')}
${box(170, 60, 130, 60, 'kmain (BSP)','box','PMM · VMM init')}
${box(320, 60, 130, 60, 'GDT · IDT','box','ISRs · TSS')}
${box(470, 60, 130, 60, 'SMP bring-up','box','APs released')}
${box(620, 60, 180, 60, 'subsystems','box','channels · audit · GCP')}

${ar(150, 90, 170, 90)}
${ar(300, 90, 320, 90)}
${ar(450, 90, 470, 90)}
${ar(600, 90, 620, 90)}

${box(20, 180, 130, 60, 'snap_init','box','barrier · cow_tracker')}
${box(170, 180, 130, 60, 'fs_init','box','GrahaFS v2 mount')}
${box(320, 180, 130, 60, 'kt task','box','spawns ahcid')}
${box(470, 180, 130, 60, 'autorun','box','init or ktest')}
${box(620, 180, 180, 60, 'PID 1 (init)','ok','daemons · gsh / ktest')}

${ar(150, 210, 170, 210)}
${ar(300, 210, 320, 210)}
${ar(450, 210, 470, 210)}
${ar(600, 210, 620, 210)}
${ar(85, 120, 85, 180)}

${box(20, 300, 780, 50, 'Steady state — kernel idle on each CPU; daemons drive I/O via channels; tests drive gate via TAP','accent')}
`, 'Figure 1.2 — Boot Flow (Limine → kmain → SMP → autorun)'));

// ----- 3. Capability token + object layout ---------------------------------
fs.writeFileSync(path.join(OUT, 'fig-cap-layout.svg'), svg(820, 460, `
<text x="50" y="55" class="ttl">cap_token_t (64 bits)</text>
${box(50, 70, 760, 60, '','box')}
<line x1="50"  y1="70" x2="50"  y2="130" stroke="#222"/>
<line x1="430" y1="70" x2="430" y2="130" stroke="#222"/>
<line x1="715" y1="70" x2="715" y2="130" stroke="#222"/>
<line x1="810" y1="70" x2="810" y2="130" stroke="#222"/>
<text x="240" y="92" text-anchor="middle" class="lbl">generation : 32</text>
<text x="572" y="92" text-anchor="middle" class="lbl">index : 24</text>
<text x="762" y="92" text-anchor="middle" class="lbl">flags : 8</text>
<text x="240" y="118" text-anchor="middle" class="lbl-xs">ABA-prevention counter</text>
<text x="572" y="118" text-anchor="middle" class="lbl-xs">slab slot index</text>
<text x="762" y="118" text-anchor="middle" class="lbl-xs">fast-path bits</text>

<text x="50" y="170" class="ttl">cap_object_t (96 bytes)</text>
${box(50, 185, 250, 230, 'header','accent')}
<text x="175" y="210" text-anchor="middle" class="lbl-sm">kind : 8 (14 kinds)</text>
<text x="175" y="232" text-anchor="middle" class="lbl-sm">rights : 17 (bitmask)</text>
<text x="175" y="254" text-anchor="middle" class="lbl-sm">flags : 16</text>
<text x="175" y="276" text-anchor="middle" class="lbl-sm">refcount, gen, watchers</text>
<text x="175" y="298" text-anchor="middle" class="lbl-sm">parent_idx, root_idx</text>
<text x="175" y="320" text-anchor="middle" class="lbl-sm">audience_set[8]</text>
<text x="175" y="342" text-anchor="middle" class="lbl-sm">audit_mask</text>
<text x="175" y="364" text-anchor="middle" class="lbl-sm">created_tsc</text>
<text x="175" y="394" text-anchor="middle" class="lbl-xs">audited derivation chain</text>

${box(330, 185, 470, 230, '14 capability kinds','box')}
<text x="345" y="210" class="lbl-sm">CAN = 1     CHANNEL = 2     VMO = 3     STREAM = 4</text>
<text x="345" y="234" class="lbl-sm">SCHED = 5   AUDIT = 6       WASM_INSTANCE = 7   FS_NODE = 8</text>
<text x="345" y="258" class="lbl-sm">STATE = 9   AI_METADATA = 10   PROCESS = 11</text>
<text x="345" y="282" class="lbl-sm">SNAPSHOT = 12   TRANSACTION = 13</text>
<text x="345" y="306" class="lbl-sm">SYSTEM = 14     CONSOLE = 15</text>
<text x="345" y="340" class="lbl-sm" font-weight="bold">17 rights bits</text>
<text x="345" y="360" class="lbl-xs">READ · WRITE · INVOKE · DERIVE · REVOKE · INSPECT</text>
<text x="345" y="376" class="lbl-xs">RESTORE · DELETE · COMMIT · ABORT · ATTACH · OBSERVE</text>
<text x="345" y="392" class="lbl-xs">subset-narrowing only — no expansion ever</text>
`, 'Figure 5.3 — Capability Token & Object Layout'));

// ----- 4. CAN dependency graph ---------------------------------------------
fs.writeFileSync(path.join(OUT, 'fig-can.svg'), svg(820, 480, `
${box(340, 60, 140, 50, 'g_root','accent','root switch')}
${box(80, 160, 140, 50, 'mm','box','memory')}
${box(240, 160, 140, 50, 'sched','box','SMP runqs')}
${box(400, 160, 140, 50, 'ipc','box','channels')}
${box(560, 160, 180, 50, 'cap','box','token table')}
${box(80, 260, 140, 50, 'fs','box','GrahaFS v2')}
${box(240, 260, 140, 50, 'audit','box','256 B ring')}
${box(400, 260, 140, 50, 'snap','box','barrier')}
${box(560, 260, 180, 50, 'txn','box','speculation')}

${box(80, 360, 140, 50, 'ahcid','warn','user driver')}
${box(240, 360, 140, 50, 'e1000d','warn','user driver')}
${box(400, 360, 140, 50, 'netd','warn','TCP/IP')}
${box(560, 360, 180, 50, 'wasmd','warn','Wasmtime')}

${ar(380, 110, 150, 160)}
${ar(390, 110, 310, 160)}
${ar(420, 110, 470, 160)}
${ar(450, 110, 650, 160)}
${ar(150, 210, 150, 260)}
${ar(310, 210, 310, 260)}
${ar(470, 210, 470, 260)}
${ar(650, 210, 650, 260)}
${ar(150, 310, 150, 360)}
${ar(310, 310, 310, 360)}
${ar(470, 310, 470, 360)}
${ar(650, 310, 650, 360)}

<text x="60" y="180" text-anchor="end" class="lbl-sm">layer 0</text>
<text x="60" y="280" text-anchor="end" class="lbl-sm">layer 1</text>
<text x="60" y="380" text-anchor="end" class="lbl-sm">layer 2</text>
<text x="60" y="80"  text-anchor="end" class="lbl-sm">root</text>

<text x="40" y="445" class="lbl-sm" font-weight="bold">Activation algorithm:</text>
<text x="170" y="445" class="lbl-sm">topological order, cycle-detected, on/off cascade — instant blast-radius.</text>
`, 'Figure 5.4 — Capability Activation Network (CAN)'));

// ----- 5. Channel + VMO IPC dataflow ---------------------------------------
fs.writeFileSync(path.join(OUT, 'fig-channel-vmo.svg'), svg(820, 460, `
${box(40, 70, 200, 250, 'Process A (sender)','accent')}
${box(580, 70, 200, 250, 'Process B (receiver)','accent')}

${box(60, 110, 160, 50, 'chan_send()','box','typed msg + handles')}
${box(60, 180, 160, 50, 'inline payload','box','≤ 256 B')}
${box(60, 250, 160, 50, 'handle list','box','VMO · cap · stream')}

${box(600, 110, 160, 50, 'chan_recv()','box','blocks until ready')}
${box(600, 180, 160, 50, 'msg + new handles','box','installed in handle table')}
${box(600, 250, 160, 50, 'VMO mapped','box','via vmm_map_user')}

${box(290, 130, 240, 130, 'Channel ring','warn','typed · FNV-1a hash · bounded')}
<text x="410" y="170" text-anchor="middle" class="lbl-sm">type_hash: u64</text>
<text x="410" y="190" text-anchor="middle" class="lbl-sm">capacity: 256</text>
<text x="410" y="210" text-anchor="middle" class="lbl-sm">freezable (snap)</text>
<text x="410" y="230" text-anchor="middle" class="lbl-sm">handle xfer with audience check</text>

${ar(220, 135, 290, 165)}
${ar(530, 165, 600, 135)}

${box(140, 360, 540, 60, 'Shared VMO (zeroed · ondemand · COW · pinned · MMIO · contiguous) — handle passed through channel','ok')}
${ar(220, 280, 250, 360,'arrowd','map')}
${ar(600, 280, 570, 360,'arrowd','map')}
`, 'Figure 5.5 — Channel + VMO IPC Dataflow'));

// ----- 6. GCP dispatch ------------------------------------------------------
fs.writeFileSync(path.join(OUT, 'fig-gcp.svg'), svg(820, 440, `
${box(280, 50, 260, 60, '/etc/gcp.json — single manifest','accent','types · syscalls · ops · audit')}

${box(40, 170, 220, 60, 'kernel parser','box','syscall slot validation')}
${box(290, 170, 240, 60, 'gcp2wit.py (build-time)','box','generates etc/gcp.wit')}
${box(560, 170, 220, 60, 'user libgcp','box','wrappers + type defs')}

${ar(360, 110, 150, 170)}
${ar(410, 110, 410, 170)}
${ar(460, 110, 670, 170)}

${box(40, 290, 220, 60, 'syscall dispatch','box','manifest-gated')}
${box(290, 290, 240, 60, 'WIT bindings (Wasmtime)','box','imports = GCP ops')}
${box(560, 290, 220, 60, 'user code','box','same protocol as kernel')}

${ar(150, 230, 150, 290)}
${ar(410, 230, 410, 290)}
${ar(670, 230, 670, 290)}

${box(40, 380, 740, 40, 'AI agent + human dev + WASM module + kernel ALL read the same machine-readable contract','ok')}
`, 'Figure 5.6 — GCP Manifest as Single Public Interface'));

// ----- 7. Submission stream ring -------------------------------------------
fs.writeFileSync(path.join(OUT, 'fig-streams.svg'), svg(820, 380, `
${box(40, 60, 200, 240, 'User process','accent')}
${box(50, 100, 180, 50, 'libsubmit','box','build SQE')}
${box(50, 170, 180, 50, 'wait CQE','box','poll · block')}

${box(280, 60, 240, 240, 'Kernel','warn')}
${box(290, 100, 220, 50, 'SQE ring (submission)','box','manifest-gated dispatch')}
${box(290, 170, 220, 50, 'worker pool','box','SUBMISSION_OP table')}
${box(290, 240, 220, 50, 'CQE ring (completion)','box','result + opaque user_data')}

${box(560, 60, 220, 240, 'Backing','ok')}
${box(570, 100, 200, 50, 'fs ops','box','GrahaFS v2 batched')}
${box(570, 170, 200, 50, 'ahcid / e1000d','box','user driver via chan')}
${box(570, 240, 200, 50, 'audit / cap','box','manifest-bound only')}

${ar(230, 125, 290, 125, 'arrow', 'submit')}
${ar(510, 195, 570, 195, 'arrow', 'dispatch')}
${ar(290, 265, 230, 195, 'arrow', 'complete')}

<text x="410" y="345" text-anchor="middle" class="lbl-sm">io_uring-style; manifest-bound — every SQE op_id must exist in /etc/gcp.json::ops[]</text>
`, 'Figure 5.7 — Submission Streams (SQE / CQE)'));

// ----- 8. Scheduler / IPI doorbell -----------------------------------------
fs.writeFileSync(path.join(OUT, 'fig-sched.svg'), svg(820, 440, `
${box(40, 60, 180, 280, 'CPU 0 (BSP)','accent')}
${box(50, 100, 160, 60, 'runq[0]','box','head → tail')}
${box(50, 170, 160, 60, 'idle_task','box','hlt')}
${box(50, 240, 160, 60, 'IPI receiver','box','vector 48')}

${box(240, 60, 180, 280, 'CPU 1','accent')}
${box(250, 100, 160, 60, 'runq[1]','box','runnable=2')}
${box(250, 170, 160, 60, 'current','box','task_t *')}
${box(250, 240, 160, 60, 'IPI receiver','box','vector 48')}

${box(440, 60, 180, 280, 'CPU 2','accent')}
${box(450, 100, 160, 60, 'runq[2]','box','runnable=0')}
${box(450, 170, 160, 60, 'idle_task','box','hlt')}
${box(450, 240, 160, 60, 'IPI receiver','box','vector 48')}

${box(640, 60, 160, 280, 'CPU 3','accent')}
${box(650, 100, 140, 60, 'runq[3]','box','runnable=5')}
${box(650, 170, 140, 60, 'current','box','task_t *')}
${box(650, 240, 140, 60, 'IPI receiver','box','vector 48')}

<text x="410" y="370" text-anchor="middle" class="lbl-sm" font-weight="bold">Cross-CPU wake</text>
<text x="410" y="390" text-anchor="middle" class="lbl-sm">sender on CPU 1 → chan_send wakes blocked task on CPU 2 → IPI vector 48 → schedule()</text>
<text x="410" y="408" text-anchor="middle" class="lbl-xs">Wake-to-run drops from ~10 ms (tick) to &lt; 1 µs (IPI delivery + dispatch). Pattern from seL4 (Lyons et al. ATC '18).</text>

${ar(330, 270, 530, 270, 'arrow', 'IPI')}
${ar(330, 290, 530, 290, 'arrowd', 'work-steal')}
`, 'Figure 5.8 — Per-CPU Scheduler & IPI Doorbell'));

// ----- 9. GrahaFS v2 layout -------------------------------------------------
fs.writeFileSync(path.join(OUT, 'fig-grahafs.svg'), svg(820, 480, `
${box(40, 60, 740, 50, 'Block device (AHCI / image)','accent')}
${box(40, 130, 130, 50, 'superblock','box','magic · gen')}
${box(180, 130, 130, 50, 'inode table','box','512 B / inode')}
${box(320, 130, 130, 50, 'journal','box','64 MB · 2-barrier')}
${box(460, 130, 130, 50, 'data segments','box','immutable')}
${box(600, 130, 180, 50, 'GC / recluster ranges','box','SimHash + leader')}

<text x="40" y="220" class="ttl">Inode (512 B)</text>
${box(40, 235, 740, 110, '','box')}
<text x="60"  y="265" class="lbl-sm" font-weight="bold">core (128 B)</text>
<text x="60"  y="288" class="lbl-xs">size · mode · ctime · mtime · gen · ai_flags</text>
<text x="240" y="265" class="lbl-sm" font-weight="bold">block ptrs (256 B)</text>
<text x="240" y="288" class="lbl-xs">12 direct + indirect + double-indirect</text>
<text x="240" y="306" class="lbl-xs">→ 4 GB max file</text>
<text x="500" y="265" class="lbl-sm" font-weight="bold">AI metadata (128 B)</text>
<text x="500" y="288" class="lbl-xs">simhash u64</text>
<text x="500" y="306" class="lbl-xs">cluster_id · embedding[0..3]</text>
<text x="500" y="324" class="lbl-xs">tag · provenance</text>

<text x="40" y="380" class="ttl">Version chain (capped at 16)</text>
${box(40, 400, 100, 50, 'v1','ok')}
${box(160, 400, 100, 50, 'v2','ok')}
${box(280, 400, 100, 50, 'v3','ok')}
<text x="430" y="425" class="lbl-sm">…</text>
${box(470, 400, 100, 50, 'v15','ok')}
${box(590, 400, 100, 50, 'v16 (head)','warn')}
${ar(140, 425, 160, 425)}
${ar(260, 425, 280, 425)}
${ar(380, 425, 410, 425)}
${ar(450, 425, 470, 425)}
${ar(570, 425, 590, 425)}
<text x="710" y="425" class="lbl-sm">→ snapshot pin</text>
`, 'Figure 7.1 — GrahaFS v2 On-Disk Layout & Version Chain'));

// ----- 10. Snapshot barrier timeline ---------------------------------------
fs.writeFileSync(path.join(OUT, 'fig-snapshot.svg'), svg(820, 380, `
<line x1="40" y1="200" x2="780" y2="200" stroke="#222" stroke-width="2"/>
<text x="20" y="200" class="lbl-sm">t →</text>

<line x1="80"  y1="190" x2="80"  y2="210" stroke="#222" stroke-width="2"/>
<line x1="200" y1="190" x2="200" y2="210" stroke="#222" stroke-width="2"/>
<line x1="330" y1="190" x2="330" y2="210" stroke="#222" stroke-width="2"/>
<line x1="470" y1="190" x2="470" y2="210" stroke="#222" stroke-width="2"/>
<line x1="610" y1="190" x2="610" y2="210" stroke="#222" stroke-width="2"/>

${box(40,  60, 100, 90, 'snap_create','accent','syscall')}
${box(160, 60, 100, 90, 'IPI all CPUs','box','schedule hook')}
${box(290, 60, 100, 90, 'park non-idle','box','barrier_flag')}
${box(430, 60, 100, 90, 'capture','box','PML4 walk · regs · FDs · pin FS')}
${box(570, 60, 100, 90, 'mark RO','box','clear PTE_W')}
${box(710,  60, 80, 90, 'thaw','ok','resume')}

<text x="80"  y="240" text-anchor="middle" class="lbl-xs">CAS barrier_flag</text>
<text x="200" y="240" text-anchor="middle" class="lbl-xs">IPI fan-out</text>
<text x="330" y="240" text-anchor="middle" class="lbl-xs">poll runq.current=idle</text>
<text x="470" y="240" text-anchor="middle" class="lbl-xs">cow_tracker++</text>
<text x="610" y="240" text-anchor="middle" class="lbl-xs">install snap_pf handler</text>

${box(40, 280, 740, 50, 'After thaw — first write to a captured page → cow_fault → fresh frame, parent unaffected. 100 ms watchdog if barrier stalls.','warn')}
`, 'Figure 7.2 — Snapshot Barrier Timeline'));

// ----- 11. Transaction state machine ---------------------------------------
fs.writeFileSync(path.join(OUT, 'fig-txn.svg'), svg(820, 460, `
${box(340, 60, 140, 50, 'IDLE','box')}
${box(60, 180, 140, 50, 'ACTIVE','accent','snap captured')}
${box(280, 180, 160, 50, 'COMMITTING','warn','replay buffer')}
${box(500, 180, 160, 50, 'COMMITTING_FAILED','danger','partial-external')}
${box(60, 320, 140, 50, 'ABORTING','box','restore snap')}
${box(280, 320, 160, 50, 'COMMITTED','ok','external visible')}
${box(500, 320, 160, 50, 'ABORTED','ok','clean rollback')}

${ar(390, 110, 130, 180, 'arrow', 'txn_begin')}
${ar(200, 205, 280, 205, 'arrow', 'txn_commit')}
${ar(440, 205, 500, 205, 'arrow', 'send fault')}
${ar(130, 230, 130, 320, 'arrow', 'txn_abort')}
${ar(360, 230, 360, 320, 'arrow', 'replay drained')}
${ar(580, 230, 580, 320, 'arrowd', 'fall back to abort')}

<text x="40" y="430" class="lbl-sm" font-weight="bold">CAS-serialised transitions:</text>
<text x="240" y="430" class="lbl-sm">ACTIVE → COMMITTING (single-writer wins). Concurrent abort sleeps 100 ms watchdog.</text>
`, 'Figure 7.3 — Transactional Speculation State Machine'));

// ----- 12. Wasmtime + GCP-as-WIT pipeline ----------------------------------
fs.writeFileSync(path.join(OUT, 'fig-wasm.svg'), svg(820, 480, `
${box(40, 60, 180, 60, 'etc/gcp.json','accent','single manifest')}
${box(260, 60, 180, 60, 'gcp2wit.py','box','build-time generator')}
${box(480, 60, 300, 60, 'etc/gcp.wit (kebab-case interfaces)','accent','partitioned by pledge class')}

${ar(220, 90, 260, 90)}
${ar(440, 90, 480, 90)}

${box(40, 180, 180, 60, '.wat / .wasm module','box','authored against WIT')}
${box(260, 180, 180, 60, 'wasmd loader','box','header + section walk')}
${box(480, 180, 300, 60, 'cap_object_create(KIND_WASM_INSTANCE)','warn','60 B slab entry')}

${ar(220, 210, 260, 210)}
${ar(440, 210, 480, 210)}

${box(40, 300, 380, 60, 'pledge_narrow_exec','danger','PLEDGE_FLAG_NARROW_EXEC + cap delegation')}
${box(460, 300, 320, 60, 'wasm sandbox running','ok','only manifest-permitted ops callable')}
${ar(420, 330, 460, 330)}

${box(40, 400, 740, 50, 'Result — module imports = a strict subset of GCP that the parent process explicitly delegated.','accent')}
`, 'Figure 7.4 — Wasmtime + GCP-as-WIT Pipeline'));

// ----- 13. Userspace driver model ------------------------------------------
fs.writeFileSync(path.join(OUT, 'fig-userdrv.svg'), svg(820, 480, `
${box(40, 60, 740, 60, 'Hardware (PCI dev, MMIO BAR, IRQ line)','accent')}

${box(40, 160, 280, 60, 'Kernel: userdrv substrate','warn','drv_register · drv_irq_wait · mmio_vmo_create')}
${box(360, 160, 200, 60, 'IOAPIC routing','box','vector → IRQ chan')}
${box(600, 160, 180, 60, 'audit_attach_drv','box','death detection')}

${ar(180, 120, 180, 160, 'arrow', 'IRQ')}
${ar(180, 220, 180, 270, 'arrow', 'chan')}
${ar(460, 220, 460, 270, 'arrow', 'IRQ chan')}

${box(40, 280, 280, 60, 'ahcid (userspace)','accent','SATA driver')}
${box(360, 280, 200, 60, 'e1000d (userspace)','accent','NIC driver')}
${box(600, 280, 180, 60, 'wasmd (userspace)','accent','WASM host')}

${box(40, 380, 740, 60, 'On daemon death → kernel detects on chan close → respawn (init policy) → MMIO VMO retained → tests resume','ok')}
${ar(180, 340, 180, 380, 'arrowd')}
${ar(460, 340, 460, 380, 'arrowd')}
${ar(690, 340, 690, 380, 'arrowd')}
`, 'Figure 8.1 — Userspace Driver Model (Drift Outward)'));

// ----- 14. UML sequence — capability invoke --------------------------------
fs.writeFileSync(path.join(OUT, 'fig-uml-sequence-cap.svg'), svg(820, 460, `
${box(60,  50, 140, 36, 'caller')}
${box(220, 50, 140, 36, 'syscall layer')}
${box(380, 50, 140, 36, 'pledge gate')}
${box(540, 50, 140, 36, 'cap_handle_resolve')}
${box(700, 50, 80,  36, 'op')}
<line x1="130" y1="86" x2="130" y2="430" stroke="#222" stroke-dasharray="4,3"/>
<line x1="290" y1="86" x2="290" y2="430" stroke="#222" stroke-dasharray="4,3"/>
<line x1="450" y1="86" x2="450" y2="430" stroke="#222" stroke-dasharray="4,3"/>
<line x1="610" y1="86" x2="610" y2="430" stroke="#222" stroke-dasharray="4,3"/>
<line x1="740" y1="86" x2="740" y2="430" stroke="#222" stroke-dasharray="4,3"/>

${ar(130, 120, 290, 120, 'arrow', 'sysenter (token, args)')}
${ar(290, 160, 450, 160, 'arrow', 'pledge_check(class)')}
${ar(450, 200, 290, 200, 'arrow', 'OK / -EPERM')}
${ar(290, 240, 610, 240, 'arrow', 'lock-free resolve(token)')}
${ar(610, 280, 290, 280, 'arrow', 'cap_object* | NULL')}
${ar(290, 320, 740, 320, 'arrow', 'invoke + audit')}
${ar(740, 360, 290, 360, 'arrow', 'result')}
${ar(290, 400, 130, 400, 'arrow', 'sysret')}

<text x="780" y="445" text-anchor="end" class="lbl-xs">Hot-path: 4 mem refs, no spinlocks; AUDIT_CAP_INVOKE emitted async.</text>
`, 'Figure 9.1 — UML Sequence: Capability Invoke'));

// ----- 15. UML sequence — snapshot create + restore -------------------------
fs.writeFileSync(path.join(OUT, 'fig-uml-sequence-snap.svg'), svg(820, 480, `
${box(40,  50, 130, 36, 'caller')}
${box(190, 50, 130, 36, 'snap_create')}
${box(340, 50, 130, 36, 'barrier')}
${box(490, 50, 130, 36, 'capture')}
${box(640, 50, 140, 36, 'GrahaFS pin')}
<line x1="105" y1="86" x2="105" y2="450" stroke="#222" stroke-dasharray="4,3"/>
<line x1="255" y1="86" x2="255" y2="450" stroke="#222" stroke-dasharray="4,3"/>
<line x1="405" y1="86" x2="405" y2="450" stroke="#222" stroke-dasharray="4,3"/>
<line x1="555" y1="86" x2="555" y2="450" stroke="#222" stroke-dasharray="4,3"/>
<line x1="710" y1="86" x2="710" y2="450" stroke="#222" stroke-dasharray="4,3"/>

${ar(105, 120, 255, 120, 'arrow', 'SYS_SNAP_CREATE(scope)')}
${ar(255, 160, 405, 160, 'arrow', 'snap_begin_barrier')}
${ar(405, 200, 405, 230, 'arrow', 'IPI + park (≤100 ms)')}
${ar(405, 250, 555, 250, 'arrow', 'walk PML4 + regs + FDs')}
${ar(555, 290, 710, 290, 'arrow', 'pin file versions')}
${ar(710, 320, 255, 320, 'arrow', 'OK · 16 chains pinned')}
${ar(255, 360, 105, 360, 'arrow', 'cap handle')}

${box(40, 400, 740, 38, 'Restore: replay (virt,phys,flags) into live CR3 with PTE_W cleared → next write COWs → caller pages skipped','warn')}
`, 'Figure 9.2 — UML Sequence: Snapshot Create & Restore'));

// ----- 16. UML component diagram --------------------------------------------
fs.writeFileSync(path.join(OUT, 'fig-uml-component.svg'), svg(820, 540, `
<text x="50" y="55" class="ttl">«kernel»</text>
${box(60, 70, 720, 220, '','box')}
${box(80,  100, 150, 50, 'core::vmm','accent')}
${box(245, 100, 150, 50, 'core::sched','accent')}
${box(410, 100, 150, 50, 'core::cap','accent')}
${box(575, 100, 190, 50, 'core::ipc::channel','accent')}
${box(80,  170, 150, 50, 'core::ipc::vmo','accent')}
${box(245, 170, 150, 50, 'core::audit','accent')}
${box(410, 170, 150, 50, 'core::pledge','accent')}
${box(575, 170, 190, 50, 'core::stream','accent')}
${box(80,  240, 150, 40, 'fs::grahafs_v2','box')}
${box(245, 240, 150, 40, 'snap','box')}
${box(410, 240, 150, 40, 'txn','box')}
${box(575, 240, 190, 40, 'cap::wasm','box')}

<text x="50" y="320" class="ttl">«userspace»</text>
${box(60, 340, 720, 180, '','warn')}
${box(80,  360, 150, 50, 'init / gash','accent')}
${box(245, 360, 150, 50, 'grahai','accent')}
${box(410, 360, 150, 50, 'wasmd','accent')}
${box(575, 360, 190, 50, 'libtui · libnet','accent')}
${box(80,  430, 150, 50, 'ahcid','accent')}
${box(245, 430, 150, 50, 'e1000d','accent')}
${box(410, 430, 150, 50, 'netd','accent')}
${box(575, 430, 190, 50, 'tests (TAP)','accent')}

<text x="780" y="535" text-anchor="end" class="lbl-xs">All boundaries between components are channels + manifest entries.</text>
`, 'Figure 9.3 — UML Component Diagram'));

// ----- 17. UML deployment diagram -------------------------------------------
fs.writeFileSync(path.join(OUT, 'fig-uml-deployment.svg'), svg(820, 480, `
${box(40, 60, 740, 90, '«device» Host (Linux x86_64) — make test / make qemu-interactive','accent')}
${box(60, 90, 200, 50, 'gcc 15 / Limine 9.3.3','box')}
${box(280, 90, 200, 50, 'mkfs.gfs / build/limine.iso','box')}
${box(500, 90, 270, 50, 'QEMU 8.x (-enable-kvm -cpu host)','box')}

${box(40, 180, 740, 240, '«executionEnvironment» QEMU virtual machine','warn')}
${box(60, 220, 320, 50, 'CPU 0..3 + 512 MB RAM + AHCI ctrl','box')}
${box(400, 220, 370, 50, 'Limine boots /boot/grahaos.elf','box')}
${box(60, 290, 320, 50, 'GrahaOS kernel half (Ring 0)','accent')}
${box(400, 290, 370, 50, 'GrahaFS v2 disk image (mkfs.gfs)','accent')}
${box(60, 360, 320, 50, 'Userspace ELFs from initrd (CPIO)','accent')}
${box(400, 360, 370, 50, '/etc/gcp.json + /etc/gcp.wit shipped','accent')}

${box(40, 430, 740, 38, '«artifact» phase-28-complete tag — gate clean × 3-iter KVM + 3-iter TCG','ok')}
`, 'Figure 9.4 — UML Deployment Diagram'));

// ----- 18. UML state diagram — channel handle ------------------------------
fs.writeFileSync(path.join(OUT, 'fig-uml-state-chan.svg'), svg(820, 380, `
<circle cx="80" cy="80" r="14" fill="#111"/>
${box(160, 60, 140, 50, 'CONNECTED','ok','live ring')}
${box(360, 60, 140, 50, 'FROZEN','warn','snap captured')}
${box(560, 60, 140, 50, 'DRAINED','warn','queued msgs cached')}
${box(160, 200, 140, 50, 'ABORTED','danger','snapshot abort')}
${box(360, 200, 140, 50, 'COMMITTED','ok','txn replay drained')}
${box(560, 200, 140, 50, 'CLOSED','box','endpoint reaped')}

${ar(94, 80, 160, 80, 'arrow', 'create')}
${ar(300, 80, 360, 80, 'arrow', 'snap')}
${ar(500, 80, 560, 80, 'arrow', 'capture body')}
${ar(230, 110, 230, 200, 'arrow', 'thaw + abort')}
${ar(430, 110, 430, 200, 'arrow', 'thaw + commit')}
${ar(630, 110, 630, 200, 'arrow', 'all readers gone')}

<circle cx="630" cy="320" r="14" fill="none" stroke="#111" stroke-width="2"/>
<circle cx="630" cy="320" r="8"  fill="#111"/>
${ar(630, 250, 630, 305, 'arrow', '')}
`, 'Figure 9.5 — UML State Diagram: Channel Handle Lifecycle'));

console.log('Wrote 18 SVG files to', OUT);

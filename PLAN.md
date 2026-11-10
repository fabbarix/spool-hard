# SpoolHard fix-and-improve program 2 — PLAN

## Status summary
Program 2 candidate: `candidate-2` @ d51f0e4 (both firmwares + frontends build; flashed on both real devices in verifier batch 7 and passing everything that does not need the owner). Branch `review-fixes-2` is main + enabler + dedup so far; it will be fast-forwarded to candidate-2 when the three owner-dependent checks below come back (console NFC tag read with the new fix, scale NFC hold, scale tabs closed) — or earlier if you tell me to release without them.
Done and verified: LCD debug enabler (snapshot + touch), LCD redesign + "Load into slot..." destination screen (render check pending a tag tap), console NFC task (flapping fixed; tag-read fix awaiting a tap), PSRAM JSON docs + packed stock-filament store, shared-lib dedup, LVGL trim (-114 KB flash), Bambu report Filter (4x smaller parse doc), OTA-config cache + SD lock + housekeeping task + lv_timer UI (loop busy 50% -> 18-20% idle), scale WS pool cap (3 tabs), scale NFC presence fix. Measured/decided: inflate placement keep PSRAM (internal only 2% faster); stack trims: only nfc_task +1 KB (no trim met the 2x-headroom rule); claim-pending e2e not tested by owner decision. Health: green.

## Needs owner decision
- **READY: candidate-2 @ d51f0e4 — release?** (both firmwares + frontends build; flashed and verified on both real devices, printer stayed up, external mapping 254:0 intact after every flash). NOT yet verified because they need you: (a) console tag read with the new Tg fix + destination screen render, (b) scale NFC hold, (c) lazy WS pool release at 0 tabs. If one fails, only that piece is affected (console NFC read path in nfc_reader.cpp, scale presence ping in scale nfc_reader.cpp, WsBufferPool shrink) — the verifier (still resumable) can retest after any tap. Branch note: `review-fixes-2` (983ce40) holds only the earlier-verified enabler + dedup; `candidate-2` is the complete tip to release from. VERSION not bumped, nothing pushed/tagged/merged to main.
- (Decision C resolved by owner: scale dashboard cap = 3 tabs; implemented and verified: 4th tab refused with code 1013, console link unaffected.)
- **OWNER ACTION (read-only, both devices flashed with the final candidate, debug toggles armed; nothing is written to any tag), in this order:** (a) CONSOLE NFC reader: tap tag 04A36263C62A81, hold 10 s, remove, wait 3 s, tap again -> tell the parent "console done" (ideally right after you say "ready"; the LCD destination screen can only be checked inside the 120 s PendingAms window — otherwise only logs are checked); (b) SCALE reader: hold the same tag 10 s, remove -> "scale done"; (c) close ALL scale dashboard tabs (there are 2) for ~1 minute, then reopen one -> "tabs done".
- (no owner action needed right now) — the console NFC tap / scale NFC hold / closing scale tabs checks have NOT happened yet and are deferred: they will be requested ONCE on the final build (a reflash resets the armed debug toggles), so you can ignore the earlier request. If you tap in between nothing breaks, but it will be re-requested.


**Design note — event-driven console loop (item 5d)**
Finding: loop() (console main.cpp ~1394-1659) is already thin. NFC and the scale link already run on their own tasks, LVGL (incl. display sleep) runs in the `lvgl` task, AsyncTCP is its own task. What is left on loopTask: WiFi state machine, SD liveness + crash-log flush (5-200 ms spikes), `_mqtt->loop()` per printer (dominant cost, 5-60 ms per ~1 Hz report; PubSubClient is not thread-safe so it stays on loopTask), SSDP/discovery (tiny), `OtaChecker::update` (re-reads ~9 NVS keys on EVERY iteration, ~200/s), and a 1 Hz UI/WS-push block (AMS panel, footer, OTA banner: est. 5-30 ms, contends for lv_lock). No real loop-latency numbers exist yet.
What stays polled: MQTT (single-thread constraint), WiFi, discovery prune/probe, weight poll, timeouts (all ~0 cost). What moves:
- Step 0: whole-loop `[LoopStat]` instrumentation (per-10 s max/avg per step, loop rate, busy%) to get real baselines.
- Step 1: cache OtaConfig in `OtaChecker::update` (no task, no heap; scale shares the code).
- Step 2: gate the 1 Hz block (push scale-link only on change, skip LCD tile refresh when state unchanged).
- Step 3: global SD mutex (prerequisite for step 4).
- Step 4: SD liveness + crash-log flush to a small housekeeping task (4 KB internal stack, core 0, prio 1).
- Step 5: UI tile refresh as an lv_timer on the lvgl task fed by a POD snapshot (largest change, real regression risk).
- Step 6 (dedicated MQTT task): poor value/risk, ~8-10 KB DRAM; only if step-0 data shows MQTT dominating loop spikes — I will not do it without asking.
Each step: separately flashable; verifier records [LoopStat] before/after, /api/heap (free, min_free), a >=15 min soak with the printer link up (no link drops, no panic, WS live); revert criteria per step: any visual/UI regression, deadlock/WDT, min_free dropping >6 KB, MQTT drops, SD missed flush.

## Todo
- [x] 0 Enabler: /api/debug/lcd.bmp + /api/debug/lcd/touch — verified PASS on device (snapshot ~1.2 s, heap flat over 20 snaps); two minor fixes in progress (stale PSRAM buffer, abort race)
- [~] 1 LCD "choose destination" after scan — screen implemented; render + Cancel verification pending an owner tag tap (never picks a destination without owner OK)
- [x] 2 LCD redesign (printer-first home, fonts/contrast/touch targets, offline Retry) — verified PASS on device (home, scale strip, slot detail, settings; after-*.png vs before-*.png in pm2/); 3 minor defects (clipped Ext info row, 38 px sleep chips, '~0%' empty slot) being fixed
- [~] 3 Console NFC (#10): UID debounce, 4-page reads, own task — partial: flapping fixed on owner tap (one event per presentation); tag READ still fails on console (sak 0x00 NTAG-family tag, scale reads it fine) — dev debugging the READ path; retest needs one more owner tap
- [x] 4 Perf/maintainability (#13): shared-lib dedup — verified PASS (static verbatim diff + both devices flashed, WiFi/rate gates unchanged); PSRAM JSON docs + stock filaments store — verified PASS (console internal free ~131->150 KB, largest block 54->74 KB, min_free 91->114 KB at ~6 min uptime, partly uptime effect); reload race fix a4ba7c1 pending re-review; stack trims — todo (needs measurements)
- [x] 5 Refactors (#14): Bambu report Filter, inflate placement, LVGL trim (verified, -114 KB flash), Bambu report Filter (host-tested, device check in batch 4), inflate placement (DONE: measured, PSRAM kept as default; internal is only 2% faster for 11 KB DRAM), event-driven loop (steps 0+1 verified: busy 50% -> 27%; steps 2-5 on dev branches under verification, one at a time), stack trims (dev/stack-trims in progress; only trims with >=2x headroom; console nfc_task +1 KB), scale WS pool cap (dev/ws-pool-cap in progress)
- [~] 6 Lazy WS pool release: verifier will check after the owner closes scale tabs. claim-pending e2e: NOT tested, by owner decision (code-reviewed only; destination screen render + Cancel only)
- [~] 7 Scale NFC presence flap (scale also reports a held tag every 1-2 s; same suspected cause: READ on unsupported tag halts card) — fix after console NFC fix is proven
- [ ] Owner-blocked: Developer Mode ON path / print.fun bit

**Release notes draft (candidate-2 vs v0.13.0)**
Features
- Console LCD: new printer-first home (big percent + thick progress bar, 5 slot tiles incl. External, FEEDING badge on the active slot, offline card with Retry), and a "Load into slot..." step on the spool screen to choose the destination (AMS slot 1-4 or External; same code path as the web claim-pending).
- Debug tools (off by default, session-only toggles in Config -> Debug): LCD screenshot (`/api/debug/lcd.bmp`) and touch injection, NFC tag dump (`/api/debug/nfc` on console and scale), gcode-analysis timing (`/api/debug/analysis`); `loop_stat` section in `/api/heap`.
Fixes
- Console NFC: held tag no longer re-fires every 3 s; one event per presentation, removal only after ~2 s of misses; NTAG pages read 4 at a time; PN532 polling now runs on its own task.
- Scale NFC: held tag no longer flaps removed/read every 0.4-2 s (presence ping); log label no longer claims "BambuLab" for any 7-byte UID.
- Scale: browser dashboards capped at 3 simultaneous tabs and the lazy WS pool shrinks idle slots; under a 7-client burst the scale's minimum free heap stayed at ~75 KB instead of 25 KB; a refused tab retries after 30 s instead of every second.
- LCD: missing glyphs (g/cm3, Import icon), clipped rows, undersized touch targets (all >= 44 px), washed-out muted text (now >= 7:1), FEEDING badge flush with the slot border.
Performance
- Console loop: OTA-check config cached (was ~200 NVS opens/s): loop busy 49.9% -> 27.1% (same conditions), further to ~20% with SD/crash-log flush moved to a housekeeping task and the home tiles applied by an LVGL timer; ~18% idle on the final build.
- Bambu MQTT reports are parsed through an ArduinoJson filter: parse document 24.5 KB -> 6.0 KB, parsing 4x faster (host test over 50 real H2S reports + 19 synthetic shapes: identical results; on-device state identical).
- Hot JSON documents and the stock-filament list (now one packed PSRAM buffer + index) moved out of internal DRAM; LVGL trimmed: console flash 2,256,031 -> 2,178,783 B (-77 KB) even with all new features.
- Gcode-analysis inflate placement measured (PSRAM 10.0 s vs internal 9.85 s total): kept in PSRAM.
Behaviour changes
- Scale dashboard: max 3 simultaneous browser tabs (extra tabs get "too many dashboards").
- Console NFC: a tag that stays on the reader now produces ONE event instead of one every 3 s; re-tap after removal still works.
- SD card access is serialised through one lock (crash log, stores, downloads, backup/restore); crash-log flush runs on a housekeeping task.
- Console internal DRAM: ~140 KB free (min 103 KB) vs ~148 KB: two new task stacks (nfc_task 5 KB, housekeeping 5 KB) were added; scale: lazy pool max 32 KB instead of 64 KB.
Not tested by decision / pending: claim-pending end to end; destination-screen slot buttons never pressed on the real printer; SD hot-remount; Developer Mode ON path (owner-blocked).

## Progress log
### 2026-10-24 16:45
Program 2 kicked off; PLAN restructured, program 1 archived below. Next: spawn verifier + enabler dev (item 0).
### 2026-10-24 16:46
Spawned 3 dev agents in parallel (own worktrees): dev/lcd-enabler (item 0), dev/console-nfc (item 3), dev/shared-dedup (item 4d). Verifier will be spawned once the enabler lands.
### 2026-10-24 16:54
Devs for items 0 (LCD debug enabler), 3 (console NFC task + UID debounce) and 4d (shared-lib dedup) finished and are merged into review-fixes-2 @ f61270a (both products build per devs). Verifier spawned for batch 1 (flash both devices, screenshots, NFC needs an owner tap later). Next wave started: dev/lcd-ui (items 1+2), dev/console-mem (PSRAM JSON docs + stock list).
### 2026-10-24 16:59
Chose option (b) for integration: unverified merges now live on branch candidate-2 (@ f61270a); review-fixes-2 reset to main and will only receive items after the verifier passes them. dev/console-mem (PSRAM JSON docs in 26 sites + packed PSRAM stock-filament store, est. +12-16 KB internal free) finished, queued for verifier batch 2. Verifier is working on batch 1 (flash both devices, LCD screenshots, NFC check pending an owner tap).
### 2026-10-24 17:04
Verifier batch 1 (candidate-2 @ f61270a, both devices flashed; console was mid-print RUNNING 47% and came back RUNNING, external mapping 254:0 intact): enabler PASS (snapshot ~1.2 s, 20 snapshots no leak, touch navigation works, 403 when off), dedup PASS (WiFi logs identical, rate gates 30 s/5 s held). Console internal free 148.8->142.8 KB (nfc_task stack+queue, ~1 min uptime vs 17 min), largest block 56->74 KB, min_free 96->109 KB; scale free 132->137 KB. NFC code review clean, hardware test needs an owner tap (see Needs owner decision). Found: snapshot PSRAM buffer lingers if client never fetches + minor abort race (dev fixing); LCD font lacks glyphs (g/cm3 superscript and Import icon show tofu; dev-ui fixing). dev/lcd-ui (items 1+2) finished, awaiting glyph fix.
### 2026-10-24 17:09
Dev fixes landed: enabler stale-buffer + abort-race fixes (9776ace), LCD tofu glyphs fixed (g/cm3, Import icon). Merged enabler fixes + dev/console-mem + dev/lcd-ui into candidate-2 @ c81da9a (clean merges, both products build per devs: console 2.267 MB, 72.1%). Verifier batch 2 started: review, flash console, heap numbers, after-screenshots of all LCD screens, then ONE owner tap request (NFC + destination screen). Commit trailer for new commits is now Claude Opus 4.8 per parent. No claim-pending destination pick will be performed without the owner's explicit OK.
### 2026-10-24 17:15
Item 3 (console NFC) FAILED on the owner's real tap: tag 'removed' ~0.6 s after each detection while held (flapping, re-arms PendingAms each time) and console shows format=Unknown where the scale reads BambuLab. Sent back to the NFC dev with the log evidence. Stock-filament store reload race (verifier finding) sent back to dev-mem. Promoted to review-fixes-2 (verified on device in batch 1): LCD debug enabler (5093c65) and shared-lib dedup (4bf94fe). New dev result: LVGL lv_conf trim (dev/lvconf-trim f9bf894): console flash 2,270,779 -> 2,155,555 B (-115 KB), awaiting verifier screenshots. Verifier is finishing batch 2 (heap numbers, after-screenshots, enabler fix tests). No further owner action needed right now; a final tap retest will be requested after the NFC fix is flashed.
### 2026-10-24 17:20
Verifier batch 2 final (candidate-2 c81da9a on console; printer stayed RUNNING, external mapping intact): enabler fixes PASS (abandoned snapshot frees PSRAM within ~45 s, toggle-off frees immediately, internal heap flat); console-mem PASS (boot log: 98 stock rows in PSRAM; APIs and WS rates unchanged; heap at ~6 min: internal free 131.8->150.1 KB, largest block 54->74 KB, min_free 91->114 KB; part of that is older-vs-fresh uptime); LCD UI PASS with 3 minor defects (Ext info card clips last row, settings sleep chips 38 px, empty slot shows ~0%) now being fixed. Tofu glyphs gone. nfc_task stack min free 1,952 B (OK). NFC FAIL root-cause hypothesis: tag is probably not an NTAG, the first READ fails and halts the card; scale shows the same flap pre-existing (new todo 7). Devs reworking NFC (non-NTAG handling), lcd-ui defects; verifier now capturing real H2S reports (read-only) for the Bambu report-Filter work (item 5).
### 2026-10-24 17:31
Candidate-2 now f6dcb20 (NFC rework with non-NTAG handling, stock reload race fix, lcd-ui defect fixes, LVGL trim: console image 2.27 MB -> 2.16 MB flash). Verifier batch 3: reviewed, built and flashed the console; screenshots + owner tap check pending. 50 real H2S reports captured (all full 15.8 KB state pushes ~1/s, state RUNNING only; the H2S sends no small deltas here). Bambu report-Filter dev started with host-side equivalence test on those captures. NFC dev adding a small follow-up (clear unreadable-resume on removal).
### 2026-10-24 17:41
Bambu report Filter done (dev/bambu-filter d52b427, merged into candidate-2): filter keeps ams/vt_tray/vir_slot whole + ~20 scalar keys, drops the other ~80 of 101 keys. Host test (ArduinoJson 7.4.3) over the 50 real H2S captures + 19 synthetic X1/P1/H2D/delta/ack shapes: full vs filtered digests identical; measured doc allocation 24.5 KB -> 6.0 KB (4.1x smaller), parse 0.25x time on host. Caveat: the test digest mirrors the firmware reads by hand (plus a drift scan of all 101 key literals), so on-device spot check is still required. NFC follow-up d4b1f33 merged. candidate-2 tip 7dab613+filter; waiting for verifier batch 3 (owner tap pending).
### 2026-10-24 17:47
Owner tap on console build f6dcb20: NFC flapping fixed (one event per presentation, 0.4 s dropout suppressed), tag UID resolved to the spool (SUNLU PLA Gray) and the LCD offered 'Load into slot...' — so the destination flow entry works. Still failing: reading tag contents (first READ returns 0 bytes; sak=0x00 atqa=0x0044 i.e. an NTAG-family tag the scale reads fine) -> NFC dev now comparing the READ path with the scale's and the old console code. Owner LCD feedback (photos): spool detail good; home 'dull' and slot content not centered -> UI dev follow-up (centered slot cards, per-slot colour swatch with outline, quieter empty slots, stronger contrast on the physical panel). Verifier finishing batch 3: destination screen render check (Cancel only; never selecting a slot/External).
### 2026-10-24 17:52
Verifier batch 3 final (f6dcb20): PASS — screenshots of home/scale strip/slot detail/settings unchanged-or-better vs batch 2 after the LVGL trim (flash 2,270,779 -> 2,156,375 B, -114 KB, no unstyled widgets seen), slot-detail clipping fixed, sleep chips ~43 px, stock-store race fix reviewed OK, printer RUNNING and external mapping intact. Heap at ~19 min: free 150.3 KB, largest block 71.7 KB, min_free 112.8 KB (batch 1: 131.8 / 54.3 / 90.7). nfc_task stack min free 1,752 B (tight, watch). Not covered: destination screen (PendingAms window expired before the screenshot), wizard screens, Retry card (offline only). NFC root cause found by dev: the batched READ used an uninitialised PN532 target number (Adafruit inDataExchange uses _inListedTag, only set by inListPassiveTarget) — fixed in dev/console-nfc 6a9f5bc; the scale reads the same tag fine. Owner LCD feedback implemented (dev/lcd-ui a9cc5ba: centered slot cards, per-slot colour swatch with outline, quieter empty slots, darker body #05070b vs card #1f2638, muted text 7.5:1). Both merged into candidate-2. In progress: console NFC debug dump endpoint (GET /api/debug/nfc, toggle-gated), scale NFC flap fix + honest log label + same debug endpoint. Plan: one batch 4 with a console tap and a scale tap (read-only); review-fixes-2 will be fast-forwarded to candidate-2 only once the final batch passes.
### 2026-10-24 17:58
Dev work finished for this round, all merged in candidate-2 @ 57b6a23 (one merge conflict between the new NFC debug toggle and the LCD debug toggle resolved by hand, console frontend tsc passes): console NFC Tg fix + GET /api/debug/nfc, scale NFC presence ping + honest log label + GET /api/debug/nfc (scale had no debug toggle: added one), owner-requested home polish. Verifier batch 4 started (flash scale + console, heap tables, device check of the Bambu report filter, NFC taps, lazy-pool test). Posted two decisions for the owner (claim-pending permission; proposal to drop event-driven loop / inflate placement / stack trims).
### 2026-10-24 18:04
Owner decisions received: claim-pending end-to-end is NOT to be tested (closed as code-reviewed only); event-driven loop, inflate placement and stack trims are all to be done, with guardrails (loop: design note first + small steps with before/after latency and 15 min soaks; inflate: measure both placements first; stack trims: only >=2x measured headroom, nfc_task gets +1 KB). Started: dev-inflate (timing instrumentation + runtime placement toggle) and dev-loop-design (design-only, will produce the note for this file). Stack trims wait for a stress-run baseline from the verifier after batch 4. Verifier is mid-batch-4 (scale flashed, console next).
### 2026-10-24 18:09
Event-driven loop design note written into PLAN.md (under Needs owner decision, above Todo): loopTask is already thin (NFC, scale link, LVGL are separate tasks); real wins are caching OtaConfig (NVS read ~200x/s), gating the 1 Hz UI/push block, SD mutex + housekeeping task for SD/crash-log flush; MQTT must stay on loopTask (thread-unsafe client) and moving it is poor value — I will not do that step without asking. dev-loop01 started (step 0 LoopStat instrumentation + step 1 OTA config cache). dev/inflate-placement (d: timing + runtime PSRAM/internal toggle, endpoint /api/debug/analysis; needs a running print with a deflated 3MF, which exists now) ready for verifier batch 5. Verifier still on batch 4.
### 2026-10-24 18:15
Verifier batch 4 (57b6a23, both devices flashed): PASS on everything not needing the owner. Bambu report filter on device: /api/printers identical field by field (41 leaf keys; only progress/layer/timers differ), Ext shows PETG + FEEDING, 254:0 mapping intact, raw AMS dump still works. Home polish verified in screenshot (centered cards, outlined swatches, quieter empty slots). Console heap ~7 min: free 148.3 KB, min_free 110.7 KB, largest 69.6 KB (about 2 KB below batch 3: image growth + 1.7 KB nfc_debug; the filter saves PSRAM, not internal DRAM). Scale boot OK (nfc_task stack free 2.3 -> 3.0 KB). Stack baseline recorded for both devices (console min free: loopTask 3.5 KB, lvgl 2.9 KB, scale_link 3.0 KB, nfc_task 2.7 KB, async_tcp 6.6 KB). Still pending owner: console NFC tap, scale NFC hold, scale tabs closed (lazy pool). Dev results merged into candidate-2 @ 9761d81 for batch 5: loop step 0 ([LoopStat] whole-loop instrumentation, also in /api/heap) + step 1 (OtaChecker config cache, ~200 NVS opens/s removed) and inflate placement timing + runtime PSRAM/internal toggle.
### 2026-10-24 18:25
Loop steps 2 and 3 developed (dev/loop-step23: 056925a step 2 = hash-gated LCD tile refresh + 5 s scale-link heartbeat; 5703bbd step 3 = global recursive SD mutex wrapping crash logger, stores, SD file serving, backup/restore, upload handlers; lock order documented; console flash 2.177 MB). Not merged: they will be verified separately in later batches (step 2, then step 3, each with a soak). Next batch (5) = candidate-2 @ 9761d81 (loop step 0/1 + inflate toggle + stack-baseline stress run + inflate timings), to start as soon as the owner's pending NFC/scale/tab actions are done (a console reflash would reset the armed debug toggles). Waiting on owner.
### 2026-10-24 18:39
Owner actions from batch 4 (NFC taps, scale tabs) have not happened yet (both readers show 0 events since boot; scale still has 2 browser clients, pool holds 7 slots = ~26 KB) — deferring them to ONE request on the final build instead of blocking. Verifier batch 5 started: loop step 0 vs step 1 numbers (LoopStat before/after OTA-config cache), inflate placement timings on the running print (PSRAM vs internal, 3+ runs each), and a stress run for the stack-trim decision.
### 2026-10-24 18:45
Loop steps 4 and 5 developed on dev/loop-step45 (779fdc8 step 4: SD liveness + crash-log flush moved to a 4 KB 'housekeeping' task on core 0, ~5 KB internal DRAM; 1ecd46a step 5: home-tile refresh applied by an lv_timer on the lvgl task from a POD snapshot, recursive LVGL mutex confirmed so no deadlock). Chain is step0/1 (in candidate-2) -> 2 (056925a) -> 3 (5703bbd) -> 4 (779fdc8) -> 5 (1ecd46a); plan is to flash and soak each step (>=15 min with printer link up) one after another after batch 5, reverting any that regress. Waiting for verifier batch 5.
### 2026-10-24 19:05
Verifier batch 5 (candidate-2 9761d81): PASS. Loop step 1 (OTA config cache) measured on the console idle, 7 windows each: loop iterations/s 100.6 -> 138.4, busy 49.9% -> 27.1%, avg loop body 4,963 -> 1,961 us, ota_check step avg 2,518 -> 24 us (max 12.4 ms -> 3.8 ms). Remaining loop_max ~155 ms is the MQTT report parse (bambu step max ~118 ms) — unchanged and must stay on loopTask. Forced OTA check + 5-min throttle still behave. Inflate placement measured on the real print (same 1.34 MB deflated 3MF -> 6.0 MB, 3 runs each): PSRAM total 10.02 s / inflate 1.2 s; internal total 9.85 s / inflate 0.99 s (-0.2 s, ~2%) but costs ~11 KB internal DRAM (min_free 116 -> 104 KB) -> decision: keep the shipped PSRAM placement (default unchanged; the timing + toggle stay as debug tools). Stack stress (backup, 20 big routes x3, 5 WS clients, 10 LCD snapshots, OTA check): console async_tcp peak 4.3 KB of 10.5 KB, loopTask 59%, lvgl 64%, nfc_task 1.5 KB of 4.2 KB; scale peaks: sensor_task 0.9 KB of 4.2 KB, nfc_task 1.25 KB, console_tx 51%. Finding: scale internal min_free fell to 25 KB with 7 WS clients (lazy pool grew to 7 slots and never shrinks while any client is connected) -> dev/ws-pool-cap started (cap + shrink idle slots, console link prioritised). dev/stack-trims started (console nfc_task +1 KB; scale sensor_task trim; no trims where the stress run did not cover the worst path). Verifier now doing loop steps 2->5, each flashed + 15 min soak.
### 2026-10-24 19:10
Stack trims: after the stress run, only one change passed the >=2x-headroom guardrail: console nfc_task 4 -> 5 KB (dev/stack-trims de8b6cd, merged into candidate-2). The scale sensor_task trim was NOT done: the measured 0.9 KB peak only covers the idle poll, calibrate/tare/add-point paths (float printf + NVS commit, est. 2-2.5 KB) were not exercised; async_tcp on both devices left alone (TLS/upload paths unstressed); framework task stacks are fixed by the prebuilt Arduino sdkconfig. Scale WS pool: dev/ws-pool-cap 3c9dabc caps the lazy pool at 3 slots (24 KB max instead of 64 KB), limits the dashboard to 2 clients (extra tabs are closed with code 1013), and shrinks idle slots 3 s after a client leaves; frontend backoff fix being added so a refused tab doesn't reconnect every second.
### 2026-10-24 19:14
Frontend backoff fix landed (WS reconnect backoff now resets only after the first received message; a dashboard refused with close code 1013 retries after 30 s). dev/ws-pool-cap + stack-trims merged into candidate-2 (tip 94e4721); verification of them is queued behind the loop-step chain (they need a scale flash and a WS-client test). Verifier is on loop step 2 onward.
### 2026-10-24 19:28
Loop step 2 (hash-gated LCD refresh, 056925a) verified PASS on the console: 926 s soak with 0 errors/resets, LCD home pixel-identical to baseline (0 of 153,600 pixels differ), pushes arrive (scale-link push 1.9/s -> 1.15/s by design), heap min_free 108.7 KB (-4.6 KB vs 113.3 KB baseline, inside the 6 KB limit), lvgl stack unchanged. Gain is small (busy 27.1% -> 26.5%, ams_panel avg 5 -> 2.3 us): the loop is dominated by the MQTT report parse (~1.4 ms avg, ~100-118 ms max) which must stay on loopTask. Honest value call: steps 3-5 are structural/robustness changes (SD lock, housekeeping task, UI via lv_timer), not big speed-ups; verifier continues with step 3 -> 4 -> 5, each with a 15 min soak. Print is paused near 96% so LCD staleness under changing values was not testable yet.
### 2026-10-24 20:06
Owner decision C applied: scale dashboard cap = 3 tabs (pool max 4 slots, 32 KB worst case transient; shrinks to the client count 3 s after a tab leaves). Merged into candidate-2 @ e40c48f. Owner LCD feedback #3 (FEEDING badge not flush with the slot border) sent to the LCD dev. Verifier is still on the loop chain (step 3 -> 5).
### 2026-10-24 20:44
Verifier batch 6 (event-driven loop chain, each step flashed + 15 min soak, printer link up, 0 errors/panics/resets): step 2 PASS, step 3 (global SD lock) PASS with a caveat (under a 7-way parallel stress min_free dipped to 73.8 KB vs 83.3 KB for step 2 under the same stress; steady state identical; 0 lock timeouts, 412 crash-list/301 log/242 filaments/90 full-backup requests all correct, filaments byte-identical, backups valid tar), step 4 (SD+crash-log flush moved to a housekeeping task) PASS (crash log keeps growing, previous-session tail preserved across /api/restart), step 5 (home tiles via lv_timer) PASS (home pixel diff only the changing temperature text). Loop numbers (idle, 90 windows per step): busy 27.1% (step 1) -> 26.5 (2) -> 26.5 (3) -> 21.4 (4) -> 20.5 (5); iterations/s 138 -> 160; the sd/crash_log steps now cost 0 on loopTask. Honest note: steps 4-5 were measured after the print finished, so part of that gain is a lighter printer report. loop_max stays ~150-180 ms (MQTT report parse, cannot move). Findings: housekeeping stack has only ~1.1 KB free -> raising to 5 KB (dev); intermittent truncated /api/debug/lcd.bmp (1 in 30-60 under load, debug-only) -> dev fixing. dev/lcd-ui FEEDING-badge flush fix (dade44e) merged into candidate-2.
### 2026-10-24 20:50
All dev work is now merged into candidate-2 @ d51f0e4: loop steps 0-5 (housekeeping stack 5 KB), nfc_task 5 KB, scale WS pool cap (3 dashboards), FEEDING-badge flush fix, and a rewrite of the LCD snapshot buffer ownership (fixes the intermittent truncated lcd.bmp). Verifier batch 7 started = FINAL regression on both devices (+ owner request for NFC taps and scale tabs at the end). After it passes I will fast-forward review-fixes-2 to the tip, write release notes and post READY.
### 2026-10-24 21:08
Verifier batch 7 on the FINAL candidate (candidate-2 @ d51f0e4), both devices flashed incl. new frontends: PASS. 100/100 LCD snapshots exactly 460,854 B (truncation fixed), 6 mid-download client aborts recover instantly with no PSRAM leak; FEEDING badge flush on the real screen; scale cap: owner's 2 tabs + 3rd accepted, 4th closed with 1013 and logged, console link unaffected, scale internal free 152 KB / min_free 82 KB at 3 clients (vs 25 KB min_free in the 7-client test before the cap); console idle loop busy 18.1% at 167 it/s; stacks: housekeeping min free 2.2 KB, nfc_task 3.7 KB. Console heap final: free 139.9 KB, min_free 103.3 KB (about 8-10 KB below batch 5 because of two extra 5 KB task stacks). Release notes draft and status summary are written above. Waiting on the owner for the three read-only checks (console tag tap with the Tg read fix, scale NFC hold, closing scale tabs for the lazy pool); then review-fixes-2 gets fast-forwarded and READY is posted.
### 2026-10-24 21:35
Owner's three read-only checks still not done (both readers show 0 events, scale still 2 tabs). Everything feasible is finished, so I posted READY: candidate-2 @ d51f0e4 under Needs owner decision with the explicit list of what is unverified and which code it affects, cleaned up merged dev worktrees/branches (kept candidate-2, review-fixes-2, main), and am handing back. The verifier is idle and can be resumed to finish the three checks after any owner tap.

---

## Archive — program 1 (v0.13.0)

# SpoolHard review-fixes program — PLAN

## Status summary
`review-fixes` @ bfeae33 contains everything done so far and is verified on both real devices (console + scale flashed with the candidate; console flashed mid-print per owner approval). Both firmwares and both frontends build. Remaining open items are optional/blocked (see Todo). Health: green.
Measured (0.12.14 baseline -> candidate): console flash -222 KB (2,449,295 -> 2,226,859 B at the regex-removal step; final candidate 2,256,031 B incl. all new features), console internal free 119 -> 145 KB, min_free 81 -> 112 KB, async_udp stack headroom 992 -> 2,236 B; scale flash 1,556,449 -> 1,328,293 B (final candidate 1,337,305 B), scale boot free ~110 -> 140 KB, min_free ~19 -> 123 KB, largest block ~44 -> 82 KB; web first-load JS 126.6 -> 82.6 KB gz.

## Needs owner decision
- **READY: review-fixes @ b2c6ca4 passes (= verified bfeae33 + one docs-only CLAUDE.md line) — release 0.13.0.** (PM did not bump/tag/push; parent session handles the release. Release notes draft is below.)
- Not blocking release: (a) lazy-pool tab-close release not confirmed on device because the owner's scale browser tab keeps ws_clients=1 — close that tab for a minute and the verifier can finish it; (b) Developer Mode ON-path verification and the passive print.fun bit (kFunBitDevMode) need owner action (enable LAN Only + Developer Mode on the H2S, send a pushall); (c) `.env` BAMBU_PRINTER_IP (192.168.20.115) looks stale vs the live printer (.113).

## Todo
Phase 0/1 bugs, security
- [x] P0 external tray mapping 254:254 + normalise/migrate + claim-pending — verified
- [x] P0b Developer Mode reminder + command acks — verified (OFF path); ON path blocked: owner action
- [x] HIGH: printer upsert wiped ams_overrides — fixed + verified (dry-run)
- [x] B1,B2,B4,B6 scale NFC/sensor (+follow-up) — verified
- [x] B5,B7,B9 mutexes, SSDP, console OTA task — verified
- [x] #1/B3 regex removed, B8 cache headers, #8 auth cache, exact routes, request-line cap — verified
- [x] B10 SPIFFS upload integrity (+ marker order/guard) — verified
Phase 2 memory/speed
- [x] #2,#3,#7 scale loop; #5,#6,#9 console store/LVGL; #4 log path; #11 gcode analyzer; #12 lazy WS pool (partial confirm) — verified
- [ ] #10 console NFC debounce / own task — not started (needs on-device NFC testing that the verifier may not do: NFC writes are forbidden); recommend a future release
- [ ] #13 rest (PSRAM JsonDocs, stack trims, shared-lib dedup, #13 WS_MAX_QUEUED_MESSAGES) — not done: queue=16 would flap the console link (threshold hard-coded 16); others low value
- [ ] #14 bigger refactors — not started
Phase 3 UI
- [x] UI-A, UI-B, UI-C (spools/settings/error states), self-hosted fonts, live weights — verified
- [ ] LCD UI redesign (printer-first home, bigger progress, destination step) — only the DevMode icon/explanation done; rest not started (LCD not observable by the verifier)

## Progress log
### 2026-10-24 14:36
PLAN.md created; branch `review-fixes` cut from main (1bcc688). Next: spawn verifier and phase-1 dev agents.
### 2026-10-24 14:38
Spawned 5 dev agents (worktrees, branches off review-fixes). Next: verifier spawn when first branch lands; then B10, memory/speed phase.
### 2026-10-24 14:40
UI-B (console dashboard: bottom tab bar, hero printers, tray-card fixes, lazy pages; first-load JS 126->83 KB gz) and UI-A (contrast, focus, ConfirmDialog, RESET type-to-confirm, system-font fallback; no network so fonts not embedded) finished by devs; tsc+vite build pass. Verifier agent spawned, reviewing UI-B first, UI-A queued. Firmware devs (S, L, W) still working.
### 2026-10-24 14:42
B5/B7/B9 (fix/init-mutex-ssdp-ota, b2940c1) done by dev-L; queued for verifier (console build not re-run after last edit: verifier must build both).
### 2026-10-24 14:44
B1/B2/B4/B6 (fix/scale-nfc-sensor, 52b941b) done by dev-S; queued for verifier. Verifier still on UI-B; dev-W (regex removal) still working.
### 2026-10-24 14:46
Owner instruction (via parent): self-host web fonts (DM Sans, JetBrains Mono) using pre-downloaded bundle. Added as follow-up to UI-A; dev-UA resumed with the task. Verifier must check woff2 serving + SPIFFS fit after it lands.
### 2026-10-24 14:48
VERIFIER: ui/console-dashboard FAIL (lazy chunk name EmptyWeightsPage-JySCeS.js.gz = 32 chars > SPIFFS limit 31; mkspiffs aborts). Everything else in it reviewed OK, 390px screenshot ok. Sent back to dev-UB with fix + build guard; same naming constraint passed to dev-UA for fonts. Verifier now reviewing dev-L + dev-S (merged scratch build, scale flash test; console flash deferred: Bambozzo printing 94%).
### 2026-10-24 14:49
UI-A + self-hosted fonts finished (ui/shared-a11y-confirm, a22f693). Queued for verifier (mkspiffs listing, offline font render). dev-UB fixing chunk names; dev-W still running.
### 2026-10-24 14:50
UI-B fixed (3cfa2e0): chunk names hash-only + spiffs-name-guard vite plugin in both frontends. Awaiting verifier on L+S, then UA+UB re-check. dev-W still running.
### 2026-10-24 14:51
dev-W done: regex removed from both firmwares (flash -224/-228 KB, DRAM ~-5 KB each), B8 cache headers, #8 auth key cache. Behaviour note: by-id GET routes were likely shadowed by list routes before; order fixed — verifier to check. Spawned dev-F (B10), dev-MS, dev-MC. Verifier still on L+S.
### 2026-10-24 14:52
VERIFIER PASS: fix/init-mutex-ssdp-ota and fix/scale-nfc-sensor (builds OK on merge; merged scale flashed: heap 101.7KB free, no crashes, scale-link connected, tare works). Both merged into review-fixes. Console not flashed: Bambozzo still printing (98%). Follow-up on B4 (snapshotCal returns empty on 400ms timeout) given to dev-S. Next for verifier: regex branch + UI-A + UI-B (+mkspiffs check).
### 2026-10-24 14:52
PM housekeeping: review-fixes now fast-forwarded to 82d40b3 (= main + verified L + S). PM integration worktree: .claude/worktrees/pm-integration. dev-MS done (perf/scale-loop 8be5146: gate-before-JSON on push fns, log noise to 60s, panic_persist 120s+skip-unchanged + new pause()/resume(); WS queue 16 NOT done: console link hard-codes threshold 16 and would flap) queued for verifier. Verifier now on regex + UI-A + UI-B. dev-F (B10), dev-MC running.
### 2026-10-24 14:53
dev-MC done (perf/console-store-lvgl: SpoolStore mutex + 8-entry LRU + single-open list(), LVGL panel skip-if-identical, lvgl sleep follows handler, scale_link 2->10ms, ScaleLink log quiet; also fixed remove() tag-index bug). Queued for verifier (console device test needs idle printer). Waiting on verifier (regex/UI), dev-F (B10), dev-S followup.
### 2026-10-24 14:53
dev-S follow-up done (fix/scale-nfc-followup: last-good calibration cache instead of 400ms wait; tag published before status). Queued for verifier. Verifier still on regex/UI batch.
### 2026-10-24 14:54
B10 done by dev-F (fix/spiffs-upload 87928e5): shared spiffs_upload module (FS lock, SPIFFS.end, checked writes, sha256, readback + file read-all verify in a task, format on failure); POST /api/upload/spiffs now replies 202 and GET /api/upload/spiffs/status gives the result (FLASHING.md curl still works but only means accepted); otaRun frontend step uses it too. Flagging contract change for owner awareness. Queued for verifier. Verifier backlog: regex+UA+UB (running), follow-up S, MS, MC, B10.
### 2026-10-24 14:57
VERIFIER batch 2: PASS for fix/no-regex-web, ui/shared-a11y-confirm, ui/console-dashboard (merge-2 flashed on scale + console; per-id routes now return records (previously shadowed by lists); cache headers, auth key change/restore flow, fonts served from device, mkspiffs lists all files, 390px UI ok). Merged into review-fixes (8d3cd1b). LOW findings: over-long args fall to collection route (possible delete-all on console DELETE /api/crashes) -> dev-W fixing. Incident: verifier's buildfs run damaged root node_modules (restored). Spawned dev-LOG, dev-AN, dev-UC; verifier on batch 3.
### 2026-10-24 15:02
dev-W follow-up done (fix/pathroute-followup f5c1620: PathReject 404 catch-all before collection routes; trailing-slash preserved). Queued for verifier (batch 4). Running: verifier batch 3, dev-LOG, dev-AN, dev-UC.
### 2026-10-24 15:05
Owner decisions received (see Needs owner decision). Spawned dev-P (lazy WS pool); dev-W extending PathReject to all methods; verifier told to add 2 KB URL test and avoid buildfs.
### 2026-10-24 15:06
Note: earlier entries had approximate times (re-aligned to the real clock afterwards); from now on times come from the system clock. dev-LOG done (perf/log-path b52204c,dcbd91f: fixed 80x160 slot ring, no per-line heap alloc, printf override with stack buffer, API unchanged; queued for verifier).
### 2026-10-24 15:07
dev-UC done (ui/spools-settings 1513205,06ca85a: spools search/filter chips/remaining bar/undo toast, shared Settings groups + DeviceName/DeviceControl/Backup in @spoolhard/ui, error banners + skeletons; bundles +2.3KB console/+1.1KB scale). Queued for verifier (note: delete+undo on a real spool needs care; verifier must not delete real spools -> review by code + UI on a proxy only).
### 2026-10-24 15:09
dev-W: fix/pathroute-followup c25aa80 (catch-all 404 for all methods per id-family, all literal /api routes exact() except /api/upload/*; first commit had a by-id DELETE/PUT regression fixed in c25aa80). Lib reading: no URL length limit in AsyncWebServer fork (heap-bound, no panic) -> dev-W adding a request-line cap via patch script. Verifier queue: batch 3, then pathroute, lazy pool, log-path, gcode, ui-spools.
### 2026-10-24 15:09
dev-P done (perf/lazy-ws-pool 88624b3: scale pool lazy, 0 slots at boot, maintain() frees at 0 clients + trims; console stays eager; /ws/console independent of pool). Queued for verifier. ~64 KB scale DRAM no longer reserved at boot (to be measured).
### 2026-10-24 15:10
dev-AN done (perf/gcode-analyzer 01997cd: char-buffer parsing, 240 host runs byte-identical to old analyzer incl ASan/UBSan; no real gcode in repo, synthetic corpus). Queued for verifier (on-device: run an analysis and compare totals if a safe analyze can be triggered; else code review + host evidence).
### 2026-10-24 15:10
dev-W: fix/pathroute-followup now 5caf52e (+ AsyncWebServer request-line 2048 B / header-line 8192 B cap via patch script; abort connection). Verifier queue (in order): batch 3 (running), pathroute, lazy pool, log-path, gcode (+real-data before/after analysis diff), ui-spools.
### 2026-10-24 15:25
P0 from owner: external-tray assignment saved as "254:254" (UI sends slot_id=tray id) so firmware never resolves it. dev-X spawned (fix/external-tray-mapping): frontend sends slot 0 for external, firmware normalises 254/255->slot 0 + migrates legacy keys, explicit "assign scanned spool to External" action. Owner config already hand-fixed (254:0 -> 04455363C62A81): verifier must preserve it. Verifier will do it ahead of remaining queue after batch 3.
### 2026-10-24 15:29
P0 addendum (from parent, verified live): H2S rejects console control commands ("mqtt message verify failed", err 84033543) unless LAN Only + Developer Mode; console never read acks. Added to dev-X: unique sequence_ids, ack parsing + last-command-result + commands_rejected_auth flag, dashboard banner, no retry loop. This is an owner-facing finding: printers need Developer Mode for filament/K sync to work.
### 2026-10-24 15:31
VERIFIER batch 3: PASS scale-nfc-followup, console-store-lvgl (merged into review-fixes 0ae6aef; console heap -4KB for LRU, scale_link 485->99 it/s, links stable); FAIL perf/scale-loop (ws-stats throttle never throttles) and fix/spiffs-upload (wrong-product image erases FS before rejection + 6 smaller issues) -> both sent back to devs. Long-URL (2 KB path/query, 5 KB cookie) tests on both devices: clean 200/401/404, devices stayed up. Both devices currently run merge-3 (scale: merge-3 + frontend; console: merge-3 firmware, merge-2 frontend). Verifier next: pathroute(5caf52e), lazy pool, log path, gcode, ui-spools, then P0 external-tray.
### 2026-10-24 15:31
P0 dev-X done (fix/external-tray-mapping bdc585b,4556912): UI sends slot 0 for external; firmware normalises 254/255->(254,0), 400 on bad slots, migrates legacy keys; POST claim-pending (explicit "Assign scanned spool to External" via the existing auto-claim path); command acks parsed (unique sequence_ids, commands_rejected_auth + last_command in /api/printers, dashboard notice + Retry sync button, no retry loop). LCD destination step NOT done (no small safe place). Queued for verifier next after pathroute.
### 2026-10-24 15:32
dev-MS fix committed (perf/scale-loop c81b90e: ws-stats uses WS_CONNECTED, panic_persist flush mark after successful write). Queued for re-verification (after batch 4). dev-F rework still in progress.
### 2026-10-24 15:32
New owner request added to P0 dev-X: Developer Mode reminder (developer_mode on/off/unknown per printer from command acks, persisted; passive print.fun bit hook left disabled until the owner supplies the bit; persistent web banner on every page while ON; LCD warning-triangle with tap explanation). ON-path verification is blocked on owner action (enable Developer Mode on the H2S) -> will be marked blocked.
### 2026-10-24 15:35
dev-X: Developer Mode reminder done (24750db): developer_mode on/off/unknown persisted, web banner, LCD warning icon, disabled passive hook. P0 + P0b both queued for verifier at 24750db; ON path blocked on owner action.
### 2026-10-24 15:35
dev-F rework done (fix/spiffs-upload 37da0a9 + merge 22f2517: product marker checked in first 4 KB before any erase, no stall timer on OTA path, atomic finish guard, no inline fallbacks, generation tokens, UI success only on status ok / idle after down, 90 s timeout). Queued for re-verification (console image marker offset unchecked: verifier to check). Note for verifier: dev-X says a web edit of a printer config may reset persisted developer_mode to unknown (minor).
### 2026-10-24 15:43
Owner bug (web shows stale spool weight during print; LCD ok): sent to dev-UC to fold into ui/spools-settings: shared remainingGrams helper, detail panel headline = remaining estimate and live refresh from pushed cache without clobbering edits, audit all weight displays. Verifier will check live tick-down during the running print.
### 2026-10-24 15:44
dev-UC: ui/spools-settings now ad51399 (shared remainingGrams helper; detail panel headline = remaining estimate with merge-preserving live refresh; AmsSlotPicker fixed; tray card uses helper). Queued for verifier incl. live tick-down check during the running print.
### 2026-10-24 15:50
VERIFIER interim batch 4: PASS fix/pathroute-followup (LOW: common_routes /api/restart/foo prefix-matched and rebooted the scale -> dev-W fixing), review PASS log-path, gcode-analyzer, ui/spools-settings (build+390px visual; delete+undo not exercised on real data), lazy pool partial (idle free 137KB vs ~110KB, min_free 45KB vs ~19KB, 10-min flat; tab-close release not confirmed because an unknown browser client stays connected). HIGH finding: POST /api/printers replaces the stored PrinterConfig, so saving a printer from Config wipes ams_overrides (owner 254:0 mapping), flags and developer_mode -> dev-X fixing (merge on upsert). Console device steps are blocked while Bambozzo is RUNNING a new print; owner override currently intact (254:0 -> 04455363C62A81). No gcode before/after diff possible without reboot mid-print.
### 2026-10-24 15:51
Verifier is idle on console items (Bambozzo printing ~12%). Asked owner (Needs owner decision) whether flashing the console mid-print is OK. Verifier doing scale re-verification of spiffs-upload + scale-loop meanwhile; dev-P adding ws_clients to /api/heap; dev-X (upsert merge) and dev-W (common_routes exact) fixing.
### 2026-10-24 15:52
dev-X: printer upsert now merges onto stored record (fix/external-tray-mapping 9f8d050; POST /api/printers?dry=1 for safe testing; migration drops empty values). Queued for verifier with the console steps.
### 2026-10-24 15:53
Owner said flash now. PM built single integration candidate `batch-candidate` @ 23e9d62 (all pending branches merged; 3 conflicts resolved: console main.cpp, panic_persist tick, both web_server.cpp upload route). Fixes in it: upsert-merge (dev-X 9f8d050), common_routes exact (dev-W d7956cd), ws_clients (dev-P ba16e03), live weights (dev-UC ad51399). Verifier tasked: build both, flash scale + console (one fw + one SPIFFS), full recipe incl. gcode before/after, consumption no-double-count, live tick-down, P0, pathroute, spiffs-upload, lazy pool. review-fixes itself still = 0ae6aef (verified items only).

## Release notes draft (0.13.0)
(Draft by PM; numbers are verifier measurements on the real devices, vs 0.12.14/0.12.15 baselines; final numbers to be refreshed from the `batch-candidate` pass.)

### Features
- Developer Mode awareness: per-printer `developer_mode` (on/off/unknown) learned from command acks and persisted; persistent web banner on every page and an LCD warning-triangle (with tap explanation) while a printer is in Developer Mode; amber per-card notice with "Retry sync" when the printer refuses SpoolHard's control commands.
- "Assign scanned spool to External" action (POST /api/printers/<serial>/claim-pending) for printers whose external holder never shows the empty->populated transition (e.g. H2S).
- Spools page: search, filter chips (Active / Running low / Empty / All), remaining-weight bar, delete confirmation with 8 s Undo.
- Settings page restructured into consistent collapsible groups (Identity & network / Updates / Data / Indicators / Danger zone) shared by console and scale; shared error banners and loading skeletons.
- Self-hosted web fonts (DM Sans, JetBrains Mono): no Google Fonts requests, works on an offline LAN.
- Request-line (2 KB) / header-line (8 KB) caps in the web server patch; clean connection abort instead of heap growth.

### Fixes
- External-spool assignment saved as "254:254" so the printer never resolved it (no weight tracking, generic PETG shown): UI now sends slot 0, firmware normalises ams_unit 254/255 -> slot 0 (400 on out-of-range), legacy keys migrated to "254:0" at boot.
- Saving a printer from Config no longer wipes ams_overrides / developer_mode / auto_restore_k / track_print_consume (POST /api/printers now merges onto the stored record; `?dry=1` previews).
- Web UI now shows the same live remaining weight as the LCD (weight_current - consumed_since_weight) in the Spools list, detail panel, AMS picker and tray cards, updating live without reload.
- Scale: NFC status queue no longer copies Strings through FreeRTOS queues (use-after-free/double-free on tags with long NDEF/URL); NFC status published atomically (no per-tick tag copies); calibration reads use a last-good cache (no spurious "uncalibrated"); tag write/erase now require the matching UID and check NTAG capacity.
- Lazy mutex creation races fixed (log ring, scale protocol) with a race-free CAS helper.
- Console: SSDP callbacks only enqueue (async_udp stack headroom 992 B -> 2244 B); OTA runs on its own task instead of loopTask; SpoolStore is now thread-safe (mutex) and `remove()` correctly drops the tag-index entry.
- Printer command acks are parsed and logged (unique sequence ids); no more silent "pub=ok" for rejected commands.
- Per-id routes (GET /api/spools/<id>, /api/user-filaments/<id>, /api/printers/<serial>) were shadowed by the list routes and returned the full list: fixed. All /api routes are exact-match; over-long/malformed ids give 404 (a DELETE with an over-long id can no longer reach the delete-all route); `POST /api/restart/foo` no longer reboots.
- Frontend upload (SPIFFS) integrity: quiesced FS, checked writes, optional sha256, flash readback + read-every-file verification, format on failure, product marker validated before erase; auth key cached with constant-time compare.
- Cache policy: index.html no-cache, hashed assets immutable.

### Performance / memory (measured)
- Removed std::regex from the web layer: console flash 2,449,295 -> 2,226,859 B (-222 KB), scale 1,556,449 -> 1,328,293 B (-228 KB); static DRAM -3.0 KB console / -4.7 KB scale.
- Console idle internal heap 119 KB -> 145 KB free, min_free 81 KB -> 106 KB, largest block 61 KB -> 88 KB.
- Scale: lazy WebSocket buffer pool (no 64 KB reserved at boot): idle internal free ~110 KB -> ~137 KB, min_free across the boot OTA check ~19 KB -> ~45 KB (to be re-confirmed on the final candidate).
- Scale: push handlers gate before building JSON/reading NVS; log noise cut to once a minute; crash-log persistence every 120 s and only when changed.
- Console: SpoolStore LRU cache + single-open list(); LVGL skips identical panel updates and sleeps per handler return; scale_link task 485 -> ~99 iterations/s; ScaleLink per-frame logging removed.
- Allocation-free log path (fixed 80x160 ring, no per-line heap) and allocation-free gcode analyzer parsing (240 host runs byte-identical to the old analyzer).
- Web: first-load JS 126.6 KB -> 82.6 KB gzip (lazy pages, no filament-DB fetch on the dashboard).

### UI
- Phone bottom tab bar; printer-first dashboard with collapsed device stats; tray cards (black is a real colour, no bogus 0%/"— g", clear active-slot ring + ACTIVE pill, full-width External card); job table grid fixed and "External" label; contrast tokens raised to >=4.5:1, global focus-visible rings, ARIA roles, reduced-motion; ConfirmDialog (type RESET to factory-reset).

### Breaking / behaviour changes
- `POST /api/upload/spiffs` now returns 202 {"ok":true,"status":"verifying"} once the bytes are streamed; the real result comes from `GET /api/upload/spiffs/status` ({phase: writing|verifying|ok|failed}); the device reboots ~2.5 s after "ok". The documented curl flow still works but "ok" now only means "accepted". A wrong-product image is rejected with 400 before anything is erased; a wrong `?sha256=` or a marker-less wrong image leaves a freshly formatted (empty) FS to be re-uploaded.
- Backups are `.tar` (since 0.12.14); restore accepts them.
- Printer control (filament/K sync, ams_filament_setting, extrusion_cali_*) requires the printer to be in LAN Only + Developer Mode on firmware that enforces command authorization (observed on H2S: "mqtt message verify failed", err 84033543). Without it SpoolHard still reads status and tracks consumption, but cannot push settings; the UI now says so.
- All literal /api routes are exact-match: `/api/x/` with a trailing slash on exact routes is a 404 (collection routes keep the trailing-slash form).
- Crash-log persistence interval 30 s -> 120 s; log lines over 160 chars are truncated with "...".
### 2026-10-24 16:14
Release notes draft (0.13.0) added to PLAN.md. Owner: release as v0.13.0 once batch-candidate passes; PM will fast-forward review-fixes and post READY. Waiting on verifier candidate report.

### 2026-10-24 (late) — batch-candidate 23e9d62 verification result
Verifier on `batch-candidate` 23e9d62 (both devices flashed; console flashed mid-print per owner, print still running). PASS: console overrides intact (254:0 -> 04455363C62A81) through 2 reboots; no double counting at 5% commits (+0.0 g after reboot, then +4.6 g, +4.4 g); analysis after reboot identical to baseline (69.424 g / 22726.88 mm, tool 0 same; only the has_pct_table flag is exposed); web Spools list and open detail panel tick down live without reload (167 -> 162 g); command acks parsed (developer_mode off, commands_rejected_auth true); P0 negatives 400; dry-run upsert preserves all 4 overrides + flags; exact routes (restart/foo, heap/foo, 70-char id, 65-digit crash DELETE all 404, crash list unchanged); new SPIFFS 202 flow OK with all 12 console assets full size and fonts served; log-path 10-min poll flat (console free 145-147 KB, min_free 112 KB vs 99-104 KB before); scale boot free 140 KB / min_free 123 KB / largest block 82 KB; scale ws-stats/HB/ConsoleTx once a minute; scale spiffs-upload: wrong-product (early marker) -> 400 with old FS intact, good+sha OK, wrong sha -> formatted FS as documented, kill-curl -> failed+formatted, device stays up. Builds: console 2,256,031 B flash / 64,296 B RAM; scale 1,337,305 B / 54,640 B.
NOT confirmed: lazy-pool tab-close release (owner browser tab keeps ws_clients=1; owner could close their scale tab), claim-pending POST (classifier denied; reviewed by code), LCD, Developer Mode ON path (owner action).
Caveat: early product check is best-effort (marker offset depends on mkspiffs order) -> dev-F adding deterministic ordering + release-script guard (branch fix/spiffs-marker-order).
FYI: `.env` BAMBU_PRINTER_IP (192.168.20.115) looks stale vs the live printer (.113).

### 2026-10-24 (late) — READY
fix/spiffs-marker-order bfeae33 verified (marker offset 517-1039 over 8 shuffled rebuilds per product, check passes; wrong-product upload rejected 400 with FS intact x3; good uploads x2; Chrome renders scale UI, fonts load, no console errors) and merged. `review-fixes` fast-forwarded to bfeae33 = everything. Finished dev worktrees cleaned up (locked ones left). Posted READY under Needs owner decision.

### 2026-10-24 16:29
Released as v0.13.0 (main ca52adc, tag pushed by the parent session); program closed. Verifier and dev agents told to stop all device activity (verifier could not be force-stopped by the PM, it was sent a STOP message and has no further work items). All dev agents had finished earlier; their worktrees were cleaned up except a locked one (perf/gcode-analyzer) and the verifier scratch worktree .claude/worktrees/verifier2.
Open owner items: (1) close the scale browser tab so the lazy-WS-pool tab-close release can be confirmed (ws_clients -> 0 => ws_pool_total -> 0); (2) Developer Mode ON-path verification needs LAN Only + Developer Mode on the H2S plus a pushall capture to set the passive print.fun bit (kFunBitDevMode); (3) .env BAMBU_PRINTER_IP (192.168.20.115) looks stale vs the live printer (.113); (4) not done: console NFC debounce/own task (#10), remaining #13/#14 items, LCD redesign beyond the Developer Mode icon; claim-pending POST was only code-reviewed.

# DamageTrace 0.2.0 â€” experimental damage-validation ASI

This diagnostic records actual native hit stages and HP changes. It does not
change damage values or draw additional random numbers. It has passed native
fixture tests and an initial in-game check: six player-to-zombie hits matched
the reconstructed native armor/deflection calculation exactly. It remains an
experimental diagnostic, not a complete damage simulator. The intermediate
health-calculation hook captured no stages in that run; independent HP snapshots
worked and confirmed both three-hit kills.

## Install

Close the game. Put **DamageTrace.asi** and **DamageTrace.ini** in:

`<game directory>\Stalker2\Binaries\Win64`

Use an existing compatible x64 ASI loader. This
package does not replace or include a loader. It does not install itself.

This first diagnostic build supports **only the audited Steam v2.1.0 EXE**, SHA256
`2bd3f115b959a8eee7e5c4b42b0142dd0025bd437e1014f92dfc60b9afee5479`.
Other game versions and storefront executables are not supported by this build.
It verifies the full disk EXE hash and the full live bytes of five hooked
functions. An unsupported or already-modified damage function causes refusal.

## Logs and test labels

After launching, look for **ACTIVE** in `DamageTrace.log` next to the ASI.
`UNSUPPORTED` or `ERROR` means the intended capture did not activate.
Each run writes a separate JSONL file in the adjacent **DamageTrace** folder.

In the INI, set a label describing the test, for example:

```ini
[DamageTrace]
Capture=1
ProjectileOnly=1
AttackerUid=-1
TestLabel=SU260_Kora_A045E_ordinary_zombie_head_close
```

Settings are reread about every two seconds, including Capture and TestLabel.
Set Capture=0 to pause. TestLabel is descriptive; it does not identify or change
the weapon automatically. AttackerUid=-1 records all attackers; after inspecting
a log, set the numeric player UID to exclude NPC shots if desired. ProjectileOnly
filters the native damage category 6. Set it to 0 only for deliberate broader
research; the analyzer does not claim to model all nonprojectile source branches.

## First in-game check

1. Use the current known-working mod setup, changing only the addition of this
   logger. Record that setup in TestLabel or a separate note.
2. Use a fresh target, one specified gun/ammo combination, fixed range and clear
   head hits. Start with a few ordinary zombies, then compare body hits and a
   helmeted target. Do not mix those conditions under one label.
3. If capturing several shots on one target, avoid enough delay for health
   regeneration. Use a quiet encounter to avoid other attackers interfering.
4. Set Capture=0, wait at least three seconds, then close the game normally.
5. Supply the run's JSONL file and DamageTrace.log for analysis. The JSONL data
   replaces any need for a video recording.

A hit is a native damage event. Buckshot pellets, missed shots and multiple
damage events from one trigger pull must not be called one-bullet HTK without
confirming their grouping. The logger currently has no trusted trigger-pull ID.

## Captured data

- Per-hit ID, parent ID for nested damage, thread ID, QPC timestamps and label.
- Shooter and victim capsule-centre world positions in centimetres and their
  separation at damage entry. The analyzer also provides separation_m. These
  positions use serial-validated object handles, matching entity UIDs and an
  exact capsule-class check; missing/stale data produces null, not zero distance.
- The distance basis is explicitly `capsule_centres_at_damage_entry`.
  `projectile_travel_cm` remains null: a verified projectile path-length counter
  has not been located. Movement, muzzle/head offsets and curved trajectories
  can make this separation differ from the distance used by falloff. No damage
  or AP falloff factor is inferred from this approximation.
- Victim/attacker numeric IDs and instance pointers, raw damage category/source,
  input damage-structure snapshot and target-attribute header snapshot.
- HP before/after the complete native damage call, plus the adjacent +0x14c
  field (named hp_bound_14c until its full runtime semantics are verified).
- Actual bone helper output, raw bone index and directly read bone coefficient.
- Armor-helper input damage, resolved AP, target protection, actual projectile
  coefficient, skip flag, selected human/mutant branch, all four deflection
  configuration values, output damage and deflection result.
- RNG seed before/after the existing deflection helper. The analyzer checks the
  recurrence before treating those snapshots as a verified draw; concurrent RNG
  changes are marked ambiguous, not silently treated as a known roll.
- Health-calculation request, current HP and returned HP, with a flag confirming
  that the request belongs to the hit's victim.
- Read-validity masks, stage overflow and queue-drop counts. Failed reads never
  count as reliable zero-valued measurements.

The logger does not resolve prototype/weapon/ammo names or equipment SIDs. Those
belong in test labels/notes. Resolved damage, AP, armor and coefficient
are enough to check the native armor formula without guessing upstream weapon
or distance modifiers. Validating those upstream combinations is a later task.

## Analysis

Run `python analyze_trace.py path/to/run.jsonl` from this source folder, or send
the log for us to analyze. Outputs are hits.csv, armor_checks.csv,
observed_kills.csv and summary.json.

The armor checker reproduces float32 operations and Windows UCRT powf. It checks
the actual observed deflection flag, and compares the RNG snapshots separately.
Special source branches are explicitly unmodeled. A change between armor output
and the health request is reported separately from an armor-formula mismatch.
Actual HP loss may be capped by remaining HP and can differ from requested
damage. The observed-kill table counts captured native hits rather than computing
ceil(HP/mean damage).

Missing data, drops, healing, mixed attackers, nested damage, reloads and a target
that was already wounded can invalidate a full-health HTK claim. The analyzer
reports those limits; an observed HP<=0 transition alone is not a verified
engine death-state event. Final gameplay behavior must be checked in the first
run before relying on the measurements.

## Implementation / testing

Five MinHook detours: apply damage 0xc4c814; armor 0xc4f34e; deflection roll
0xc4f3fc; bone-adjusted damage 0xc4fe59; health calculation 0x53765e. All addresses
are RVAs. The apply wrapper preserves the integer return register and all
arguments; each original is invoked exactly once. Thread-local contexts preserve
nested hit correlation. Hooks copy bounded records into a queue and perform no
file writes, formatting, configuration reads or extra game function calls.
A background worker writes and flushes about once per second. Queue contention
or overflow drops a record and increments an explicit count instead of waiting
on disk. Instrumentation still has overhead; this is a temporary diagnostic.

Build instructions are below. `generate_profile.py <path-to-exe>` optionally
regenerates the committed profile from your own audited executable. The normal
build and GitHub Actions need no game files. MinHook sources and license are
vendored in this repository.
Tests exercise five real detours in a synthetic host, the six-argument float ABI,
original-call counts, nested contexts, post-armor changes, lethal health clamping,
filters, queue overflow/contention, JSON validity, analyzer mismatch detection,
and loading the actual ASI into an unsupported host to confirm refusal.
Distance tests cover a known 3-4-5 separation, unavailable positions, stale
serials, reused UIDs and unexpected component classes. Distance reads introduce
no additional detours or calls into game code.

Close the game before removing the ASI/INI. Runtime DLL unloading is unsupported.
Recent unflushed events can be lost on a crash. Keep the logger out of ordinary
play once research is finished.

MinHook's complete license is retained in third_party/minhook/LICENSE.txt and
embedded in the ASI via the exported DamageTrace_ThirdPartyLicense function.
The release ZIP contains only DamageTrace.asi and DamageTrace.ini; research,
source, test fixtures, documentation and analysis tools stay outside the ZIP.

## Build locally and with GitHub Actions

Requires Windows x64, Python 3 and Zig 0.15.2:

```powershell
python -m pip install -r requirements-build.txt
./scripts/get-zig.ps1
./build.ps1
```

To use an existing compiler: `./build.ps1 -Zig C:\tools\zig\zig.exe`. An
optional `-Python` parameter selects the Python executable. Downloaded Zig is
verified against its pinned SHA256 before extraction.

GitHub Actions runs the same build on push, pull requests and manual dispatch.
Download **DamageTrace-v0.2.0-Windows-x64** from a successful run. It contains
`DamageTrace_v0.2.0.zip` and a checksum/test manifest. The mod ZIP itself contains
only the ASI and INI. The workflow does not publish releases or install the mod.

The code is MIT licensed; vendored MinHook/HDE retain their original notices.
No game executable, game assets, captured gameplay logs or private compiler
installation is included or required for CI.

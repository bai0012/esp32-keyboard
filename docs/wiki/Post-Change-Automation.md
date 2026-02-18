# Post-Change Automation

## Goal
Enforce a consistent per-request delivery pipeline:
1. capture baseline
2. build
3. commit once
4. push main repo
5. sync/push wiki only when wiki docs changed

## Script
- Path: `tools/post_change_pipeline.ps1`
- Modes:
  - `begin`: capture branch/remotes/status baseline (main + wiki)
  - `finish`: detect touched files since baseline, run build gate, commit, push, optional wiki sync

## Usage
```powershell
# At start of request
.\tools\post_change_pipeline.ps1 -Mode begin

# After implementation
.\tools\post_change_pipeline.ps1 -Mode finish -Type feat -Message "add xyz support"
```

## Parameters
- `-Type`: `feat|fix|refactor|docs|chore`
- `-Message`: commit summary (required for `finish`)
- `-IdfInitScript`: default `C:\Espressif\Initialize-Idf.ps1`
- `-IdfId`: default `esp-idf-b29c58f93b4ca0f49cdfc4c3ef43b562`
- `-WikiRepoPath`: default `..\esp32-keyboard.wiki`

## Behavior Details
- Baseline captures:
  - branch/remotes/status snapshot for main + wiki
  - hash snapshot for initially dirty files
- Touched-file detection:
  - includes files that became dirty after baseline
  - includes initially dirty files only if hash changed after baseline
  - avoids auto-staging unrelated pre-existing edits
- Build gate:
  - runs IDF init + `idf.py build`
  - commit/push is blocked if build fails
- Push handling:
  - push once
  - on rejection: one `git pull --rebase`, then push retry
  - if still failing: stop and report blocker
- No-op handling:
  - if no effective changes since baseline, finish exits without commit
- Wiki sync:
  - triggers only if `docs/wiki/*.md` changed in touched files
  - mirrors markdown pages into wiki repo root with same file name
  - commits/pushes wiki once per request when sync produces changes

## Recommended Commit Message Style
- `feat: <summary>`
- `fix: <summary>`
- `refactor: <summary>`
- `docs: <summary>`
- `chore: <summary>`

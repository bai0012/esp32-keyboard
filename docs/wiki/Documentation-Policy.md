# Documentation Policy

## Policy
Every feature or behavior change must include documentation updates in the same change set.

## Required Updates
- Update `README.md` when user-facing behavior/setup changes.
- Update one or more pages in `docs/wiki/` when architecture, logic, APIs, or tuning changes.
- Follow the post-change automation pipeline in `tools/post_change_pipeline.ps1` for build/commit/push.

## Definition of Done
A change is complete only when:
- code is implemented and validated
- documentation is updated and consistent with code

## Minimum Checklist
- [ ] Build passes (`idf.py build`)
- [ ] Behavior tested
- [ ] README updated if needed
- [ ] Wiki pages updated if needed
- [ ] Main repo pushed
- [ ] Wiki repo pushed if wiki content changed

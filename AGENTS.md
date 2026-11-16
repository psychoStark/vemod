# AGENTS.md

## Repository Rules & Maintenance Guidelines

### Documentation Sync
- `documentation.md` **MUST** be updated on every architectural, major, or minor change in the codebase.
- Whenever new native bridges, syscall handlers, linker workarounds, guest hooks, or container lifecycle changes are introduced or modified, update `documentation.md` immediately to reflect the exact current implementation details, rationale, and file locations.
- Keep version code, version name, and compatibility matrices in `documentation.md` synchronized with `app/build.gradle.kts`.

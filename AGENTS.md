# Repository Guidelines

## Project Structure & Module Organization
`src/` contains the library and local harnesses: `fakedetector.c` owns lifecycle/public entry points, `fd_*.c` files split feature areas, `driver.c` is the snapshot-test harness, and `perf.c` is the performance harness. `clp/` holds the CLIPS rule files loaded at runtime. `tests/expected/driver.out` is the golden snapshot. `docs/` contains design notes. `vendor/clips/` is third-party code; treat it as vendored unless a task explicitly requires updating CLIPS itself.

## Build, Test, and Development Commands
On Linux/macOS, use `gmake`; on BSDs, use `make`.

- `gmake all`: build `build/driver`, the main development binary.
- `gmake test`: run the driver and diff stdout against `tests/expected/driver.out`.
- `gmake perf`: run the performance harness with API latency assertions.
- `gmake clean`: remove `build/`.

BSD equivalents use the same targets through `BSDmakefile`: `make all`, `make test`, `make perf`.

## Coding Style & Naming Conventions
Project code is C11 with strict warnings (`-Werror -Wmissing-prototypes -Wstrict-prototypes -Wshadow -Wpointer-arith`). Follow the existing style: tabs for indentation, opening braces on the next line for functions, and short, targeted block comments only where needed. Keep public API names in the `fd_*` namespace and place shared internals in `fd_internal.h`. New CLIPS files and facts should use lowercase, hyphenated or snake-like names consistent with the current rule set.

## Testing Guidelines
Tests are snapshot-driven. Update behavior through `src/driver.c`, then run `gmake test` and inspect diffs in `build/driver.out` before accepting a new snapshot. If output changes intentionally, replace `tests/expected/driver.out` in the same change. Run `gmake perf` for changes that affect query/assertion hot paths or telemetry.

## Commit & Pull Request Guidelines
Recent history favors short, imperative subjects with a prefix, for example `feat: ...`, `ergonomics: ...`, or `code quality: ...`. Keep commits focused and mention the subsystem when useful. Pull requests should describe the rule or API behavior change, list the commands run (`gmake test`, `gmake perf` when relevant), and call out any snapshot updates or CLIPS rule changes explicitly.

## Repository-Specific Notes
Do not mix project fixes with incidental edits under `vendor/clips/`. When changing rule loading or file layout, verify `fd_create()` still loads `clp/templates.clp`, `dossier.clp`, `cases.clp`, `claims.clp`, `social.clp`, `suspicion.clp`, and `stance.clp` successfully.

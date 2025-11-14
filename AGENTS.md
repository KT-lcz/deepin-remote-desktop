# Repository Guidelines

## Project Structure & Module Organization
deepin-remote-desktop groups screen acquisition and input helpers inside `src/capture`, `src/encoding`, `src/input`, and `src/utils` (packed as `libdrd-media.a`). Runtime glue, session state machines, RDP transport, and security hooks live in `src/core`, `src/session`, `src/transport`, and `src/security`, while `main.c` stays minimal. Config samples plus TLS fixtures live in `config/` and `certs/`; generated binaries stay inside `build/`. Architecture notes and task history are under `doc/`. Keep developer scratch pads inside `buildDir/` only—never commit them.

## Build, Test, and Development Commands
All workflows flow through Meson:
```bash
meson setup build                      # configure (add -Dbuildtype=debug for assertions)
meson compile -C build                 # build libs + deepin-remote-desktop
./build/src/deepin-remote-desktop --config ./config/default.ini  # local smoke
meson test -C build --suite unit       # run GLib-based suites
```
Use `meson compile -C build session/drd_rdp_session.c.o` when iterating on a single TU, and rerun the binary after touching config or TLS helpers.

## Coding Style & Naming Conventions
Target C17 with GLib/GObject. Indent with four spaces, wrap arguments past 120 columns, and keep headers free of unrelated declarations. Types/structs use `PascalCase` (`DrdRdpSession`), functions/variables use `snake_case`, and macros remain `DRD_*`. Logs (`g_message`, `g_warning`) must stay in English, whereas inline comments should justify tricky GLib or FreeRDP interactions. Run `clang-format -style=LLVM` on edited regions before review.

## Testing Guidelines
Add new GLib tests beside the implementation (e.g., `src/session/tests/test_rdp_session.c`) and register them via `g_test_add()`. Name cases `test_<module>_<scenario>` and cover at least the happy path plus one failure. For concurrency-heavy code, run `meson test -C build --repeat=2 --setup=ci` to expose races, then record FPS or rdpgfx stats from the smoke command in the PR description.

## Commit & Pull Request Guidelines
Commit messages follow the repo norm: concise Chinese subject with optional English tail (`session: 收敛 Rdpgfx ACK timeout`). Reference issues in the body when applicable. PRs must explain motivation, outline validation (meson compile/test + smoke run), and attach logs or packet traces when editing transport/session/TLS layers. Link to updated `doc/architecture.md` sections whenever diagrams change.

## Security & Configuration Tips
Treat `certs/` as disposable dev material; regenerate with `openssl req` instead of uploading personal keys. Keep `.ini` credentials to the provided `[auth]` placeholders and restart the daemon whenever you change TLS or NLA inputs because credentials are cached once per runtime. When sharing logs externally, redact hostnames and IPs before posting.

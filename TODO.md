# smallclue TODO

## Build & dependency infrastructure

- [x] **Fork + submodule the remaining third-party deps.** libgit2, openrsync,
      nextvi, and dvtm are done (pinned submodules of emkey1 forks, matching
      PSCAL's tree; nextvi's 4 sed patches were baked into commits on a
      dedicated `smallclue` branch of the fork rather than `master`, which
      carries an unrelated PSCAL snapshot).
  - [ ] `openssh` — the one deliberately left out. Fetched as a release
        tarball (not git) and patched by a dozen-plus sed/python edits (main
        renames, musl include guards, showprogress/interrupted aliasing). A
        fork means owning rebases onto upstream releases, including security
        updates — decide if that upkeep is worth it before starting.
- [ ] **Make the standalone (non-PSCAL-nested) build less fragile.** CMake
      configure currently fails cryptically if:
  - `third-party/openrsync/config_pscal.h` is absent (now fixed by the
    submodule, but the `../../third-party/openrsync` PSCAL-path fallback in
    CMakeLists.txt is still the first thing tried);
  - the `setup_posix_env.sh`-generated sources don't exist yet
    (`src/openssh_globals.c`, `src/runtime_stubs_extra.c`,
    `src/core/build_info.h`, `third-party/openssh/pscal_runtime_hooks.h`) —
    these are static content and CMake could just generate them itself;
  - openssh hasn't been `./configure`d (no `config.h`) — at minimum emit a
    clear "run X first" error instead of a compiler include failure.
- [ ] **Split `setup_posix_env.sh` into non-root and root halves.** It demands
      sudo up front, but stub-file generation and the openssh configure/build
      steps don't need root — only the rootfs/chroot assembly does.
- [ ] **Add CI.** A GitHub Actions job doing `./fetch_dependencies.sh` + cmake
      configure + build on macOS and Linux would have caught both the
      sftp-client.c `showprogress` link error and the standalone-openrsync
      configure failure before any user hit them.

## Known bugs / behavior gaps

- [ ] **rsync `-c`/`--checksum` can hang against a non-openrsync/smallclue
      peer** (both ends block in poll(); README documents `--timeout=N` as the
      workaround). Fix would be detecting/failing loudly like `-z` does.
- [ ] Legacy rsync engine (`PSCALI_RSYNC_LEGACY=1`): remote
      `-u/-c/--include/--exclude/--delete` are not implemented in the scp
      bridge path.

## Documentation

- [ ] **COMPARISON.md is stale on the shell:** it says the `sh` applet launches
      exsh (the PSCAL frontend), but standalone builds now use smallclue's own
      native POSIX shell (`src/shell/`); exsh is only the embedded-PSCAL
      (`WITH_EXSH`) behavior. README has it right.
- [x] `fetch_dependencies.sh` pinned nothing for nextvi/dvtm (HEAD of
      upstream) — fixed by the submodule conversion above.

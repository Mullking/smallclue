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
- [x] **Make the standalone (non-PSCAL-nested) build less fragile.** CMake now
      self-generates the 4 static stub files that `setup_posix_env.sh` used to
      (only if absent, so root-built/PSCAL trees are untouched) and fails with
      a clear, actionable `FATAL_ERROR` naming the exact `./configure` command
      if `third-party/openssh/config.h` is missing, instead of a bare compiler
      "file not found". The openrsync `config_pscal.h` gap is separately fixed
      by the submodule conversion above.
- [ ] **Split `setup_posix_env.sh` into non-root and root halves.** It demands
      sudo up front, but stub-file generation and the openssh configure/build
      steps don't need root — only the rootfs/chroot assembly does.
- [x] **Add CI.** `.github/workflows/build.yml` runs `fetch_dependencies.sh`
      + openssh `./configure` + cmake configure/build/smoke-test on macOS and
      Linux, on every push/PR to main. macOS path verified locally end-to-end
      against a fresh clone; Linux path is unverified (no Linux runner
      available locally) — watch its first run.

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

# Changelog

All notable changes to this project are documented here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.0] - 2026-10-14

### Added
- Install through the VRChat Creator Companion from `https://vpm.magmamc.dev/index.json`; releases ship the `package.json` the listing reads.

- Optimizer results are cached under `Library/UdonQuic/cache`, keyed by everything the optimizer is given, so recompiling unchanged programs skips the optimizer.
- `UdonQuic.Tests --bench <dir> [threads]` times serial against threaded optimization and checks both give the same output.

### Changed
- Programs in a batch are optimized in parallel on all cores, largest first, roughly halving a full project recompile.
- The batch log line reports how many results came from the cache and the batch's wall time.
- The package requires `com.vrchat.worlds` below 4.0.0, so VCC does not offer it against an untested SDK major.

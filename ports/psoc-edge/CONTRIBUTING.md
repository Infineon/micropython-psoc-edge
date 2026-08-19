# Contributing to the PSOC™ Edge Port

Thank you for your interest in contributing! This guide covers the branching model,
rebasing strategy, and pull-request workflow specific to the PSOC™ Edge port. Before
contributing, please also read the project-wide guidelines:

- [MicroPython Contributor Guidelines](https://github.com/micropython/micropython/wiki/ContributorGuidelines)
- [MicroPython Code Conventions](https://github.com/micropython/micropython/blob/master/CODECONVENTIONS.md)

---

## Overview

The PSOC™ Edge port lives in the Infineon fork of MicroPython:

> <https://github.com/Infineon/micropython-psoc-edge>

The fork tracks the official `micropython/micropython` **master** branch to stay
current with core bug fixes, security updates, and API improvements, while keeping
PSOC™ Edge-specific code isolated and maintainable.

---

## Repository Structure

```
micropython/micropython
└── master                  ← upstream; synced into the fork regularly

Infineon/micropython-psoc-edge
└── psoc-edge-main          ← primary development branch
    ├── <feature-branch>    ← short-lived contributor branches
    └── bugfix/<fix-name>
```

### `psoc-edge-main`

The main integration branch. It contains everything from upstream plus:

- PSOC™ Edge platform support and hardware abstraction
- Infineon-specific drivers
- Board configurations (`boards/KIT_PSE84_AI/`, …)
- Port-specific documentation and test infrastructure

All pull requests from contributors target this branch.

### Feature branches

Create a short-lived branch from `psoc-edge-main` for each piece of work:

```
psoc-edge-main
│
├── en-uart               ← top-level feature
│   ├── en-constructor    ← sub-feature
│   ├── en-transfer
│   └── en-read
│
└── bugfix/i2c-timeout    ← standalone bug fix
```

**Naming conventions**

| Type | Example |
|---|---|
| New feature | `en-uart`, `adc-support` |
| Bug fix | `bugfix-i2c-timeout`, `bugfix-build-failure` |
| Documentation | `docs-quickref-i2c` |
| Refactor | `refactor-machine-i2c` |

Sub-feature branches may be used when a larger feature is split across several PRs.
Once all sub-features are reviewed and merged into the parent feature branch, a final
PR merges the feature into `psoc-edge-main`.

---

## Rebasing Strategy

The fork uses **rebase instead of merge** to keep a linear, readable commit history.

`psoc-edge-main` is periodically rebased on top of `micropython/micropython:master`
as an automated job. To bring those upstream changes into your branch:

```bash
# 1. Update the local integration branch
git checkout psoc-edge-main
git pull --rebase

# 2. Rebase your feature branch on top of it
git checkout <feature-branch>
git rebase psoc-edge-main

# 3. If you have sub-feature branches, cascade the rebase
git checkout <sub-feature-branch>
git rebase <feature-branch>
```

Do this regularly — at least before opening a PR and after any upstream sync lands
on `psoc-edge-main`.

**Why rebase?**

- Produces a linear git history that is easier to read with `git log`.
- Isolates PSOC™ Edge-specific commits, making upstream contributions cleaner.
- Reduces noisy merge commits.
- Makes conflict resolution more granular (one commit at a time).

---

## Pull Request Workflow

```
feature branch
      │
      ▼
 Open PR → psoc-edge-main
      │
 Code Review
      │
 CI Validation
      │
    Merge
      ▼
 psoc-edge-main
```

### Before opening a PR

1. Rebase your branch on the latest `psoc-edge-main` (see above).
2. Squash or reorganise commits, if necessary, so that each commit is self-contained and has a
   clear, conventional commit message (see
   [CODECONVENTIONS.md](https://github.com/micropython/micropython/blob/master/CODECONVENTIONS.md)).
3. Verify that the build succeeds locally:

   ```bash
   cd ports/psoc-edge
   make BOARD=KIT_PSE84_AI
   ```

4. Run any relevant tests from `tests/ports/psoc-edge/`.

### PR checklist

- [ ] Build passes for the target board.
- [ ] Relevant tests pass (hardware-in-the-loop tests where applicable).
- [ ] Code follows MicroPython coding conventions.
- [ ] New public APIs are documented.
- [ ] No debug `printf` statements left in production code paths.
- [ ] Commit messages follow the format specified by MicroPython.

### CI

Automated CI runs on every PR and covers:

- Cross-compilation for `KIT_PSE84_AI`.
- Static analysis and coding-style checks.
- Where board and tests are available, hardware-in-the-loop tests.

A PR must pass all CI checks before it can be merged.

---

## Questions and Discussion

For questions about the port, open a
[GitHub Discussion](https://github.com/Infineon/micropython-psoc-edge/discussions)
or raise an issue.

## Upstream PR Flow

1. Choose the feature or issue to upstream.
2. Create a branch from `master+ci-hil` using the naming convention `psoc-edge-<feature>` in kebab-case (lowercase with hyphens). This is the preliminary branch used as the base for fork-internal PRs.
3. Create a review branch `psoc-edge-<feature>-review` from `psoc-edge-<feature>`. For larger changes, split the work into several stacked PRs and choose descriptive names for each split.
4. Bring in the contribution from `psoc-edge-main` using the most suitable method (for example, cherry-picking or diff-based strategies).
5. Clean up commit history:
     - Squash commits when they belong to the same file or closely related file groups.
     - Expand commit messages with additional context when useful.
     - Ensure all commit messages clearly describe the changes.
6. Identify existing tests relevant to the contributed module or unit. For tests that require hardware wiring, make sure they also run locally.

```bash
./run-tests.py -t a0
./run-tests.py -t a0 --via-mpy
./run-tests.py -t a0 --via-mpy --emit native
./run-tests.py -t a0 -d extmod_hardware  # when applicable
./run-natmodtests.py -t a0 extmod/*.py
```

7. Most of these tests will also run automatically when you open a PR from `psoc-edge-<feature>-review` to `psoc-edge-<feature>`.
8. Expect this workflow to take about 30 minutes. Some tests may still fail; if they pass locally in your setup, that is acceptable.
9. Some failures can be caused by hangs from previous tests.
10. Open a PR from `psoc-edge-<feature>-review` to `psoc-edge-<feature>`.
11. Address review feedback.
12. Remove CI-check-only content/commits before upstreaming. You can do this with interactive rebase by dropping commits that contain CI HIL changes that should not be pushed upstream, then force-push.
13. After internal approval and final review, push to upstream `master`.
14. Once accepted and merged, sync the changes from `master` back into `psoc-edge-main` (eventually these branches should differ only by fork-specific CI HIL additions).


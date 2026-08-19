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

# Upstream PR flow
1. Choose the feature/issue to upstream.
2. Create a new branch from master+ci-hil with the following the psoc-edge-<feature> convention in kebab-case (low case with hyphens). This will be a preliminary branch against which we will open PRs in our fork.
3. We create a new branch psoc-edge-<feature>-review from psoc-edge-<feature>. If the changes are large, it can be done in several (stack) PRs. Chose the appropriate descriptive name each division
4. Add the contribution from psoc-edge-main. Use the most convenient approach: cherry-picking, diff strategies, ...
5. Clean up the commit history:
- Commits belonging to the same file or file groups might be candidate for squashing.
- Extend the commit message with a description when applicable.
- Review commit messages and ensure they are descriptive of the changes.
6. Identify if there are existing tests which are applicable for the module or unit of the contributed code. Specially those which require hardware wiring, make sure that they run locally.  

```python
$ ./run-tests.py -t a0 
$ ./run-tests.py -t a0 --via-mpy
$ ./run-tests.py -t a0 --via-mpy --emit native
$ ./run-tests.py -t a0 -d extmod_hardware # When applicable
$ ./run-natmodtests.py -t a0 extmod/*.py
```

7. Most of these tests will anyhow run when you open a PR from psoc-edge-<feature>-review to the psoc-edge-<feature> branch.
This workflow will take quite some time (~30 min) and some tests will anyhow fail. If manually passing in the local setup, it is fine.
Some tests can fail, specially due to any hanging from a previous tests.
8. Open a PR from psoc-edge-<feature>-review to psoc-edge-<feature>
9. Review iterations.
10. Drop/Remove all ci check content/commits. You can do this with interactive rebase, dropping the commit with all the CI HIL changes that should not be pushed to the upstream and then force push.
11. Once we have approved and review everything, we will push it to the upstream master.
12. Once accepted and merged, bring the changes from the master to our psoc-edge-main. (At some point these should be the same + our fork ci-hil extras).


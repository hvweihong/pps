# Unified Build CLI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the obsolete `master|slave` build modes from the active build interface.

**Architecture:** `build.sh` becomes an option-only wrapper around `west build`; runtime role remains exclusively controlled by NVS. A host test covers help text and rejection of legacy positional modes.

**Tech Stack:** Bash, Python unittest, Zephyr west.

---

### Task 1: Remove legacy build modes

**Files:**
- Create: `tests/host/test_build_script.py`
- Modify: `build.sh`
- Modify: `README.md`

- [ ] Add host tests that require role-free help and reject `master`/`slave` with exit code 2.
- [ ] Run the focused test and confirm it fails against the current compatibility interface.
- [ ] Delete `MODE`, positional role parsing, role-specific directories and mode output; reject unknown pre-`--` arguments.
- [ ] Change README production/validation commands to `./build.sh -d ...`.
- [ ] Run the focused test, all host tests, ruff, production build and validation build.
- [ ] Commit and push the implementation without staging unrelated workspace changes.

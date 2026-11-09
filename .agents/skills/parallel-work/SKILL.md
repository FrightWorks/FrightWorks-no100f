---
name: parallel-work
description: >-
  Run several tasks at once, each in its own git worktree with its own Claude Code
  session, coordinated from the main checkout's session (named coordinator). Use when the
  human asks to start a task in parallel or in a new worktree/session, to check on
  or message the other sessions, to reopen one, or to finish a task (PR merged:
  remove the worktree, fold its notes into AGENTS.md).
---

# Parallel work: worktrees and sessions

`tools\debug\worktree.ps1` makes one worktree per task (its own branch, `Play\`, `autotest\`, session) and opens a
Windows Terminal tab running `claude -n <name>` there. What a worktree contains, the shared links and the game-run slots
are described at the top of that script and in `docs/testing.md` section 10. This skill is the coordinator's side.

## Roles

- **coordinator**: the session in the main checkout, named `coordinator` (`worktree.ps1 open -Name main`, or
  `/rename coordinator`). Not `main`: in SendMessage, "main" always means the sender's own conversation, so workers
  could not reach it (not even as `main [ref]`). It stays on `main` and edits nothing in the worktrees: it creates and
  removes them, starts sessions, checks on them, and after a merge updates `main` and folds lasting facts into
  `AGENTS.md`.
- **workers**: one session per worktree, named like the worktree. Each works only in its folder, keeps its notes in
  `TASK.md`, commits and pushes to its own branch without asking, and sends every question, decision or approval
  request (PRs, options, visual checks) and its done/blocked status (with what it committed and pushed) to
  `coordinator`, not to the human in its tab (AGENTS.md: "Questions go through the coordinator").
- The coordinator answers a worker itself when the answer follows from the rules, the task's goal or earlier
  decisions, or is a routine technical choice: "Coordinator's answer: ...". What is the human's call (opening,
  readying, merging or closing PRs, releases, scope or design, player-visible behaviour, deleting things) or needs the
  human's eyes or hands, it asks the human and relays: "Human's answer: ...". Only that counts as the human's
  approval.
- Permission prompts appear in the worker's own tab; when the human types in a worker's tab, the worker answers there.

## Start a task

1. Pick a short name (folder and session name) and a branch: `feat/...`, `fix/...`, `tools/...`. Fix-only PRs start from
   `origin/main` (the default `-From`).
2. Write the goal so a fresh session can start without this conversation: the issue number, what "done" means, decisions
   already made with the human, and limits (fix-only PR, what not to touch).
3. Run it (no `-Build`: the worker does its own first full build, so this returns in seconds):

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\debug\worktree.ps1 new -Name sliders -Branch fix/f10-sound-sliders -Task "Fix #37: ..."
   ```

   The goal goes into the worktree's `TASK.md` and the new session starts on it. `new` also links the worktree's
   `.claude/skills` to its `.agents/skills` (`tools/setup-agents.py`); `open` does the same, so a worktree that has merged `main` gets its link when it is reopened (or run the script by hand). Quote the goal; keep `;` out of it
   (Windows Terminal splits its command line there).

   Optional: `-Model sonnet|opus|<full model ID>` and `-Effort low|medium|high|xhigh|max` pick the worker's model and
   effort (`claude --model/--effort`); left out, the session uses the defaults. They apply to that session only, nothing
   is remembered.

`worktree.ps1` is a thin wrapper over `tools\debug\worktree.py` (Python 3.8+); the commands and options here are the
same either way.

## Check on, message, reopen

- `worktree.ps1 list`: every worktree with its branch, changed files and who holds a game-run slot. Game runs of the PowerShell scripts and of the Python test kit
  (`tools/testkit/`, `tools/smoke-test.py`) share the same run slots. Not built yet: the full sweep before a release (`.agents/REPO.md`, Testing).
- ListAgents: which sessions are running. SendMessage to `<name>` to ask for a status or pass on a decision from the
  human; the answer arrives as a message. Don't report a worker's state you haven't heard back.
- A closed tab: `worktree.ps1 open -Name <name>` starts a new session that picks the task up from `TASK.md` (so keep
  workers writing their Status there). `-Continue` tries `claude -c` (the folder's last conversation) instead. `-Model`/`-Effort` work here too but are
  not remembered: pass them again on every `open`, or the session starts on the defaults. `new` and `open` start the tab
  without the coordinator's Claude Code variables, so each worker is its own session (in ListAgents, conversation saved).
- The human tests a worktree's build with its own `Play\FrightWorks Game.exe`, or its launcher (the worker runs
  `tools\build-launcher.ps1` there, then `launcher\publish\FrightWorks.exe`). The human's own `Play\` is untouched.
- When one PR merges and another worktree's branch needs it: ask that worker to merge `origin/main` into its branch.

## Finish a task (its PR merged, or dropped)

1. Make sure its session is done: ListAgents, or ask it. Ask the human to close its tab (removing the folder under a
   running session breaks that session).
2. Read its `TASK.md`. Fold what every agent should know into `AGENTS.md`, or into `.agents/REPO.md` when it is a public rule or fact
   (that file changes in the PR that makes it true, not afterwards; case histories go in the skills, via a kit PR); `TASK.md` is deleted with the folder.
3. `worktree.ps1 remove -Name <name>`. It refuses while there are uncommitted or untracked changes, a program running
   from the folder (it stops leftover dotnet build servers itself) or the worktree's Claude session (`-n <name>`). Never delete a worktree folder by hand.
4. Update `main` (`git pull --ff-only`) and delete the merged local branch (`git branch -d <branch>`; GitHub already
   deleted the remote one). A dropped branch: ask the human before `git branch -D`.

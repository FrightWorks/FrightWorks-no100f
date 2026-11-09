#!/usr/bin/env python3
"""Parallel work: one git worktree per task, each with its own branch, Play\\ and Claude Code session.

Python 3 standard library only. On Windows run it through the wrapper, which finds a real Python and passes the
arguments through unchanged:

  powershell -ExecutionPolicy Bypass -File tools\\debug\\worktree.ps1 new -Name installer -Branch feat/installer [-From origin/main] [-Task "goal"] [-Build] [-NoOpen] [-Model m] [-Effort e]
  powershell -ExecutionPolicy Bypass -File tools\\debug\\worktree.ps1 list
  powershell -ExecutionPolicy Bypass -File tools\\debug\\worktree.ps1 open -Name installer [-Continue] [-Model m] [-Effort e]
  powershell -ExecutionPolicy Bypass -File tools\\debug\\worktree.ps1 open -Name main
  powershell -ExecutionPolicy Bypass -File tools\\debug\\worktree.ps1 remove -Name installer

Directly (Linux, or Windows with Python on PATH) the same options work as -Name/--name, case-insensitive:

  python3 tools/debug/worktree.py new --name installer --branch feat/installer --task "goal"

Worktrees live beside the main checkout, in <its parent>\\FrightWorks-worktrees\\<name> (-Base to change, or set it once with `git config frightworks.worktreesDir D:\\Worktrees\\FrightWorks` or `~/worktrees/frightworks`). Each gets:
 - its own copy of everything git tracks, on its own branch (new from -From, default origin/main; or an existing branch);
 - its own build output (Play\\, wiicompiled\\native-build, generated, launcher\\publish, dist) and autotest\\: tests in one
   worktree never touch another's Play\\ or the user's settings in the main checkout's Play\\;
 - links (junctions on Windows, symlinks elsewhere) to the main checkout's big read-only inputs: Disc\\, texture_pack\\,
   toolchain\\, game\\, wiicompiled\\Dependencies and the folders in tools\\debug\\local (Dolphin copies, icon packs),
   never user-setup;
 - .claude\\skills: a link to its own .agents\\skills (tools\\setup-agents.py, run by new and open; Claude Code looks there);
 - AGENTS.md: a link to the main checkout's (one set of rules); CLAUDE.md loads it and TASK.md, this task's own notes.
Builds can run side by side. Game runs share the test kit's run slots (tools/testkit/slots.py).

Sessions: new opens a Windows Terminal tab in the worktree running `claude -n <name>` (-NoOpen: don't). With -Task,
the goal goes into TASK.md and the session starts on it right away. open starts the session again later: a new
conversation that picks the task up from TASK.md (-Continue: claude -c, the folder's last conversation).
The tab starts without the Claude Code variables of the shell that ran this, so a session started from the
coordinator's tool shell is a session of its own (listed by ListAgents, its conversation saved), not its child.
`open -Name main` opens the main checkout's session, named `coordinator`: it creates and removes worktrees and can
message the others by name (session name = worktree name). Not `main`: in SendMessage, "main" always means the
sender's own conversation, so workers could not reach a session of that name.
Without Windows Terminal (wt) the command to run is printed instead.
-Model <name or full model ID> and -Effort <low|medium|high|xhigh|max> (new and open) are passed to claude as --model
and --effort, only when given; without them the session uses Claude Code's own defaults. They apply to that one
session only: nothing is remembered, so a later open without them starts on the defaults again. -Model can't contain
a semicolon (Windows Terminal splits its command line there).
-Build runs the first full build there (translator tools, translate, compile).
remove refuses while the worktree has uncommitted or untracked work, a program running from it (it first stops the
dotnet build servers a launcher or translator build leaves running there) or its Claude session. It unlinks the
links first (so nothing shared is deleted), checks none is left, then removes the folder. It never deletes recursively
through a link: the folder is only removed by git after the link scan comes back empty. The branch is kept: delete it
yourself when it is merged.
"""
import argparse
import datetime
import json
import os
import re
import shlex
import shutil
import stat
import subprocess
import sys
import time

WINDOWS = os.name == 'nt'
FILE_ATTRIBUTE_DIRECTORY = 0x10
FILE_ATTRIBUTE_REPARSE_POINT = 0x400

# Session name of the main checkout's coordinating session (open -Name main).
COORDINATOR = 'coordinator'

# The wrapper (worktree.ps1) hands the arguments over as JSON in this variable: Windows PowerShell 5.1 mangles
# double quotes and backslashes in native command arguments, a -Task text must arrive as typed.
ARGS_ENV = 'FRIGHTWORKS_WORKTREE_ARGS'

OPTIONS = {  # lower-case name -> argparse dest
    'name': 'name', 'branch': 'branch', 'from': 'from_', 'base': 'base', 'task': 'task', 'build': 'build',
    'noopen': 'noopen', 'continue': 'continue_', 'model': 'model', 'effort': 'effort',
}


class Fail(Exception):
    pass


def say(text=''):
    print(text, flush=True)


# ---------------------------------------------------------------------------------------------------------------------
# Arguments

def parse_args(argv):
    # PowerShell style (-Name x, any case) and GNU style (--name x) both work.
    fixed = []
    for a in argv:
        m = re.match(r'^--?([A-Za-z]+)$', a)
        if m and m.group(1).lower() in OPTIONS:
            a = '--' + m.group(1).lower()
        fixed.append(a)
    class Parser(argparse.ArgumentParser):
        def error(self, message):  # exit 1 like every other refusal (argparse defaults to 2)
            self.print_usage(sys.stderr)
            print('worktree: error: %s' % message, file=sys.stderr)
            sys.exit(1)
    p = Parser(prog='worktree', allow_abbrev=False, description='Parallel work: git worktrees with Claude sessions.')
    p.add_argument('command', type=str.lower, choices=['new', 'list', 'open', 'remove'])
    p.add_argument('--name')
    p.add_argument('--branch')
    p.add_argument('--from', dest='from_', default='origin/main')
    p.add_argument('--base')
    p.add_argument('--task')
    p.add_argument('--build', action='store_true')
    p.add_argument('--noopen', action='store_true')
    p.add_argument('--continue', dest='continue_', action='store_true')
    p.add_argument('--model')
    p.add_argument('--effort', type=str.lower, choices=['low', 'medium', 'high', 'xhigh', 'max'])
    return p.parse_args(fixed)


# ---------------------------------------------------------------------------------------------------------------------
# Helpers

def norm(path):
    return os.path.normcase(os.path.normpath(path))


def same_path(a, b):
    return norm(a) == norm(b)


def git(root, *args, check=True, capture=False):
    """git -C root args. Output goes to the console unless capture; failure raises when check."""
    sys.stdout.flush()
    r = subprocess.run(['git', '-C', root, *args], stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.DEVNULL if capture else None, text=capture, encoding='utf-8' if capture else None)
    if check and r.returncode != 0:
        raise Fail('git %s failed' % ' '.join(args))
    return r


def write_text(path, text):
    """UTF-8 without a BOM, exactly the given line endings (Claude Code reads CLAUDE.md's @imports per line)."""
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def game_holders():
    """Live game-run slots (tools/testkit/slots.py): dicts with root, why, kind, since. Empty if the kit can't be read."""
    try:
        tools = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        if tools not in sys.path:
            sys.path.insert(0, tools)
        from testkit import slots
        return slots.holders()
    except Exception:
        return []


def is_link(path):
    """A reparse point (junction, symlink, ...) or symlink; never follows it."""
    st = os.lstat(path)
    if WINDOWS:
        return bool(getattr(st, 'st_file_attributes', 0) & FILE_ATTRIBUTE_REPARSE_POINT)
    return stat.S_ISLNK(st.st_mode)


def find_links(top):
    """Every link under top (any depth), without descending into one. Raises if anything cannot be listed:
    remove must not go on with a directory it could not fully inspect."""
    links = []
    stack = [top]
    while stack:
        d = stack.pop()
        with os.scandir(d) as it:
            entries = list(it)
        for e in entries:
            st = e.stat(follow_symlinks=False)
            if WINDOWS:
                attrs = getattr(st, 'st_file_attributes', 0)
                if attrs & FILE_ATTRIBUTE_REPARSE_POINT:
                    links.append(e.path)
                elif attrs & FILE_ATTRIBUTE_DIRECTORY:
                    stack.append(e.path)
            else:
                if stat.S_ISLNK(st.st_mode):
                    links.append(e.path)
                elif stat.S_ISDIR(st.st_mode):
                    stack.append(e.path)
    return links


def unlink_link(path):
    """Remove the link itself, never what it points to."""
    st = os.lstat(path)
    if not is_link(path):
        raise Fail('refusing to unlink %s: not a link' % path)
    if WINDOWS and getattr(st, 'st_file_attributes', 0) & FILE_ATTRIBUTE_DIRECTORY:
        os.rmdir(path)  # RemoveDirectory on a junction / directory symlink removes the link only
    else:
        os.unlink(path)


def make_dir_link(link, target):
    if WINDOWS:
        try:
            import _winapi
            _winapi.CreateJunction(target, link)
            return
        except (ImportError, AttributeError, OSError):
            pass
        r = subprocess.run(['cmd', '/c', 'mklink', '/J', link, target], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode != 0:
            raise Fail('could not create junction %s -> %s' % (link, target))
    else:
        os.symlink(target, link, target_is_directory=os.path.isdir(target))


# ---------------------------------------------------------------------------------------------------------------------
# Processes (for remove)

def list_processes():
    """[{pid, name, exe, cmd}] of every process. Raises Fail if they cannot be listed: remove must then refuse
    rather than assume nothing is running."""
    procs = []
    if WINDOWS:
        ps = shutil.which('powershell') or shutil.which('pwsh')
        if not ps:
            raise Fail('cannot list running programs (no powershell); not removing anything')
        script = ("[Console]::OutputEncoding = [Text.Encoding]::UTF8; "
                  "$p = @(Get-CimInstance Win32_Process -ErrorAction Stop | ForEach-Object { "
                  "[pscustomobject]@{ pid = [int]$_.ProcessId; name = $_.Name; exe = $_.ExecutablePath; cmd = $_.CommandLine } }); "
                  "ConvertTo-Json -InputObject $p -Compress")
        r = subprocess.run([ps, '-NoProfile', '-NonInteractive', '-Command', script], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, encoding='utf-8', errors='replace')
        if r.returncode != 0 or not r.stdout.strip():
            raise Fail('cannot list running programs (%s); not removing anything' % (r.stderr.strip() or 'no output'))
        try:
            data = json.loads(r.stdout)
        except ValueError:
            raise Fail('cannot list running programs (unreadable answer); not removing anything')
        for d in data if isinstance(data, list) else [data]:
            procs.append({'pid': d.get('pid'), 'name': d.get('name') or '', 'exe': d.get('exe'), 'cmd': d.get('cmd')})
        if not procs:
            raise Fail('cannot list running programs (empty list); not removing anything')
    else:
        if not os.path.isdir('/proc/self'):
            raise Fail('cannot list running programs (no /proc); not removing anything')
        for entry in os.listdir('/proc'):
            if not entry.isdigit():
                continue
            base = '/proc/' + entry
            try:
                exe = os.readlink(base + '/exe')
            except OSError:
                exe = None  # kernel thread, or another user's process
            try:
                with open(base + '/cmdline', 'rb') as f:
                    cmd = f.read().replace(b'\0', b' ').decode('utf-8', 'replace').strip()
                with open(base + '/comm', encoding='utf-8', errors='replace') as f:
                    name = f.read().strip()
            except OSError:
                continue  # exited meanwhile
            procs.append({'pid': int(entry), 'name': name, 'exe': exe, 'cmd': cmd})
    return procs


def running_from(path):
    prefix = norm(path) + os.sep
    out = []
    for p in list_processes():
        exe = p['exe']
        if p['pid'] != os.getpid() and exe and norm(exe).startswith(prefix):
            out.append(p)
    return out


def is_dotnet(p):
    return p['name'].lower() in ('dotnet.exe', 'dotnet')


def claude_sessions(name):
    """claude processes started as `... -n <name>` / `--name <name>` (new/open start them that way)."""
    pat = re.compile(r'(^|\s)(-n|--name)\s+"?%s"?(\s|$)' % re.escape(name))
    out = []
    for p in list_processes():
        first = (p['cmd'] or '').split(' ', 1)[0].replace('\\', '/').rsplit('/', 1)[-1].lower()
        looks_like_claude = p['name'].lower() in ('claude.exe', 'claude') or first in ('claude', 'claude.exe')
        if looks_like_claude and p['pid'] != os.getpid() and p['cmd'] and pat.search(p['cmd']):
            out.append(p)
    return out


# ---------------------------------------------------------------------------------------------------------------------

class Worktree:
    def __init__(self, args):
        self.a = args
        here = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
        r = git(here, 'rev-parse', '--path-format=absolute', '--git-common-dir', check=False, capture=True)
        if r.returncode != 0:
            raise Fail('not inside a git checkout')
        self.common = os.path.abspath(r.stdout.strip().replace('/', os.sep))
        self.main = os.path.dirname(self.common)
        self.base = args.base or self.configured_base() or os.path.join(os.path.dirname(self.main), 'FrightWorks-worktrees')

    def configured_base(self):
        # frightworks.worktreesDir lives in the shared .git/config; relative paths are taken from the main checkout.
        r = git(self.main, 'config', '--get', 'frightworks.worktreesDir', check=False, capture=True)
        v = r.stdout.strip() if r.returncode == 0 else ''
        if not v:
            return ''
        return os.path.normpath(os.path.join(self.main, os.path.expanduser(v)))

    def git(self, *args, **kw):
        return git(self.main, *args, **kw)

    # -- worktree list
    def worktrees(self):
        r = git(self.main, 'worktree', 'list', '--porcelain', check=False, capture=True)
        out, cur = [], None
        for line in r.stdout.splitlines():
            if line.startswith('worktree '):
                if cur:
                    out.append(cur)
                cur = {'path': line[9:].replace('/', os.sep) if WINDOWS else line[9:], 'branch': '', 'head': ''}
            elif cur is None:
                continue
            elif line.startswith('HEAD '):
                cur['head'] = line[5:12]
            elif line.startswith('branch '):
                cur['branch'] = re.sub('^refs/heads/', '', line[7:])
            elif line == 'detached':
                cur['branch'] = '(detached)'
        if cur:
            out.append(cur)
        return out

    def worktree_path(self):
        a = self.a
        if not a.name:
            raise Fail('-Name is required for %s' % a.command)
        if not re.match(r'^[A-Za-z0-9._-]+$', a.name):
            raise Fail("-Name '%s': use letters, digits, . _ - only (it becomes a folder name)" % a.name)
        if a.name == 'main':
            if a.command == 'open':
                return self.main
            raise Fail("-Name main is the main checkout's session; pick another name")
        if a.name == COORDINATOR:
            raise Fail("-Name %s: the main checkout's session has that name; pick another" % COORDINATOR)
        return os.path.join(self.base, a.name)

    # -- open
    def open_session(self, path, title, prompt, cont):
        a = self.a
        claude = ['claude']
        if cont:
            claude.append('-c')
        if a.model:
            claude += ['--model', a.model]
        if a.effort:
            claude += ['--effort', a.effort]
        claude += ['-n', title]
        if prompt:
            claude.append(prompt)
        wt = shutil.which('wt')
        if not wt:
            quote = subprocess.list2cmdline if WINDOWS else shlex.join
            say('open: Windows Terminal (wt) not found; run: cd "%s"; %s' % (path, quote(claude)))
            return
        # The new tab gets this process's environment. Run from a Claude Code session's tool shell (as the coordinator
        # does), that includes Claude Code's own variables, and the new claude would start as a child of that session:
        # no ~/.claude/sessions entry (ListAgents/SendMessage can't find it), no saved transcript (open -Continue can't
        # resume it). Drop them from the environment handed to wt, except any the user set themselves (User or Machine
        # environment on Windows).
        env = dict(os.environ)
        for k in list(env):
            u = k.upper()
            if u in ('CLAUDECODE', 'CLAUDE_PID', 'AI_AGENT') or u.startswith('CLAUDE_CODE_'):
                if not user_or_machine_env(k):
                    del env[k]
        # -w 0: a new tab in the current Windows Terminal window (a new window if none).
        sys.stdout.flush()
        subprocess.run([wt, '-w', '0', 'new-tab', '--title', title, '-d', path, *claude], env=env)
        say("open: Windows Terminal tab '%s' running %s in %s" % (title, 'claude -c (continuing)' if cont else 'claude', path))

    def cmd_open(self):
        a = self.a
        path = self.worktree_path()
        if not os.path.exists(path):
            raise Fail('no worktree %s' % path)
        prompt = '' if (a.continue_ or a.name == 'main') else 'Read TASK.md and carry on with the task from its Status.'
        title = COORDINATOR if a.name == 'main' else a.name
        self.link_skills(path, 'open')
        self.open_session(path, title, prompt, a.continue_)

    def link_skills(self, path, verb):
        """Run tools/setup-agents.py in the worktree: .claude/skills -> .agents/skills (Claude Code looks there). Not fatal:
        a branch from before the move has no script, and a refusal (a real folder with files) is for a person to fix."""
        setup = os.path.join(path, 'tools', 'setup-agents.py')
        if not os.path.isfile(setup):
            say('%s: no tools/setup-agents.py on this branch (from before the skills moved to .agents/skills); skills not linked' % verb)
            return
        r = subprocess.run([sys.executable, setup], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding='utf-8', errors='replace')
        for line in r.stdout.splitlines():
            say('%s: %s' % (verb, line))
        if r.returncode != 0:
            say('%s: skills not linked; fix it, then run: python tools/setup-agents.py (in %s)' % (verb, path))

    def status(self, path):
        """(ok, [porcelain lines]) of a worktree, leaving out the links `new` made. Windows junctions to Disc and the like are
        directories, which the .gitignore entries (`/Disc/`) cover; a symlink is not a directory, so on Linux git lists
        it as untracked. Only an untracked entry that is a link into the main checkout is left out."""
        r = git(path, 'status', '--porcelain', check=False, capture=True)
        if r.returncode != 0:
            return False, []
        out = []
        for line in r.stdout.splitlines():
            if not line.strip():
                continue
            if line.startswith('?? '):
                rel = line[3:].strip().strip('"').rstrip('/')
                full = os.path.join(path, rel.replace('/', os.sep))
                try:
                    if os.path.lexists(full) and is_link(full):
                        target = os.path.realpath(full)
                        if norm(target).startswith(norm(self.main) + os.sep):
                            continue
                except OSError:
                    pass
            out.append(line)
        return True, out

    # -- list
    def cmd_list(self):
        holders = game_holders()
        for w in self.worktrees():
            dirty = len(self.status(w['path'])[1])
            is_main = same_path(w['path'], self.main)
            tag = 'main checkout' if is_main else os.path.basename(w['path'].rstrip('\\/'))
            state = '%d changed/untracked' % dirty if dirty else 'clean'
            game = ''
            for h in holders:
                if h.get('root') and norm(str(h['root'])) == norm(w['path']):
                    game = '  [game slot, %s: %s]' % (h.get('kind'), h.get('why'))
            say('{0:<18} {1:<34} {2}  {3}{4}'.format(tag, w['branch'], w['head'], state, game))
            if not is_main:
                say('                   ' + w['path'])

    # -- new
    def cmd_new(self):
        a = self.a
        path = self.worktree_path()
        if not a.branch:
            raise Fail('-Branch is required (e.g. feat/installer)')
        if os.path.exists(path):
            raise Fail('%s already exists' % path)
        os.makedirs(self.base, exist_ok=True)
        self.git('fetch', '-q', 'origin')
        r = git(self.main, 'show-ref', '--verify', '--quiet', 'refs/heads/' + a.branch, check=False)
        if r.returncode == 0:
            say('new: existing branch %s' % a.branch)
            self.git('worktree', 'add', path, a.branch)
        else:
            say('new: branch %s from %s' % (a.branch, a.from_))
            self.git('worktree', 'add', '--no-track', '-b', a.branch, path, a.from_)

        shared = ['Disc', 'texture_pack', 'toolchain', 'game', os.path.join('wiicompiled', 'Dependencies')]
        local = os.path.join(self.main, 'tools', 'debug', 'local')
        if os.path.isdir(local):
            for e in sorted(os.scandir(local), key=lambda e: e.name.lower()):
                if e.is_dir(follow_symlinks=False) and e.name != 'user-setup' and not is_link(e.path):
                    shared.append(os.path.join('tools', 'debug', 'local', e.name))
        for rel in shared:
            target = os.path.join(self.main, rel)
            if not os.path.exists(target):
                continue
            link = os.path.join(path, rel)
            if os.path.lexists(link):
                say('new: %s exists in the branch, not linked' % rel)
                continue
            os.makedirs(os.path.dirname(link), exist_ok=True)
            make_dir_link(link, target)
            say('new: linked %s' % rel)

        # The tracked skills are in .agents/skills; Claude Code looks in .claude/skills (git-ignored): link it to this
        # worktree's own. Not fatal: a branch from before the move has no script, and the link can be made later.
        self.link_skills(path, 'new')

        # Agent instructions: AGENTS.md = the main checkout's (local file, not in git); CLAUDE.md loads it + TASK.md.
        agents = os.path.join(self.main, 'AGENTS.md')
        agents_link = os.path.join(path, 'AGENTS.md')
        if os.path.exists(agents) and not os.path.lexists(agents_link):
            made = False
            if WINDOWS:
                # mklink: unprivileged with Windows Developer Mode (os.symlink needs the same, but this matches the
                # old script exactly).
                made = subprocess.run(['cmd', '/c', 'mklink', agents_link, agents], stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL).returncode == 0
            else:
                try:
                    os.symlink(agents, agents_link)
                    made = True
                except OSError:
                    pass
            if not made:
                write_text(agents_link, 'Read %s (the shared agent notes).\r\n' % agents)
        write_text(os.path.join(path, 'CLAUDE.md'), '@AGENTS.md\r\n@TASK.md\r\n')
        head = git(path, 'rev-parse', '--short', 'HEAD', capture=True).stdout.strip()
        goal = a.task.strip() + '\r\n' if a.task else ''
        today = datetime.date.today().strftime('%Y-%m-%d')
        lines = [
            '# Task: %s' % a.name,
            '',
            'Worktree of %s on branch `%s` (from %s at `%s`, %s).' % (self.main, a.branch, a.from_, head, today),
            'Notes for this task go here, not in AGENTS.md (AGENTS.md is shared by every worktree: general rules only).',
            'This worktree has its own Play\\ and autotest\\; Disc\\, texture_pack\\, toolchain\\ and tools\\debug\\local\\* are links to',
            'the main checkout\'s (read only: never delete or rewrite them from here). Game runs share the test kit\'s run slots.',
            '',
            'This session is named `%s`. The main checkout\'s session (named `%s`) creates and removes worktrees and' % (a.name, COORDINATOR),
            'may message you (SendMessage). Don\'t ask the human in this tab: send `%s` every question, decision or' % COORDINATOR,
            'approval request (PR, which option, a visual check) and your done/blocked status, with enough context to answer it',
            'cold (what, options, your recommendation); never `main` (that means your own conversation). Commits and pushes to',
            'this branch need no approval: say what you committed and pushed. Note a question under Status as "Waiting on: ..." and',
            'carry on with anything it doesn\'t block. Replies start "Human\'s answer:" (the human\'s decision) or "Coordinator\'s',
            'answer:" (never an approval of PR actions or anything else the human decides). Full rules: AGENTS.md. Keep Status',
            'below current: a session reopened later (worktree.ps1 open) starts from this file, not from this conversation.',
            '',
            '## Goal',
            goal,
            '## Status',
        ]
        write_text(os.path.join(path, 'TASK.md'), '\r\n'.join(lines))
        # TASK.md and CLAUDE.md stay out of git (info/exclude is shared by all worktrees).
        exclude = os.path.join(self.common, 'info', 'exclude')
        existing = ''
        try:
            with open(exclude, encoding='utf-8', errors='replace', newline='') as f:
                existing = f.read()
        except OSError:
            pass
        have = [l.strip().lower() for l in existing.splitlines()]
        eol = '\r\n' if WINDOWS else '\n'
        add = ''
        for pattern in ('/TASK.md', '/CLAUDE.md'):
            if pattern.lower() not in have:
                add += ('' if not existing or existing.endswith('\n') or add else eol) + pattern + eol
        if add:
            os.makedirs(os.path.dirname(exclude), exist_ok=True)
            with open(exclude, 'a', encoding='utf-8', newline='') as f:
                f.write(add)

        if a.build:
            say('new: first full build (translator tools, translate, compile)...')
            if not WINDOWS:
                raise Fail('-Build needs Windows (tools\\build-no100f.ps1)')
            sys.stdout.flush()
            r = subprocess.run(['powershell', '-ExecutionPolicy', 'Bypass', '-File', os.path.join(path, 'tools', 'build-no100f.ps1')],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding='utf-8', errors='replace')
            for line in r.stdout.splitlines()[-3:]:
                say(line)
            if r.returncode != 0:
                raise Fail('build failed (exit %d); see %s\\build-logs' % (r.returncode, path))
        else:
            say('new: not built yet; first build there: tools\\build-no100f.ps1 (no -SkipTools / -SkipTranslate)')
        say('new: %s ready' % path)
        if not a.noopen:
            prompt = 'Start on the goal in TASK.md.' if a.task else ''
            self.open_session(path, a.name, prompt, False)

    # -- remove
    def cmd_remove(self):
        a = self.a
        path = self.worktree_path()
        w = next((w for w in self.worktrees() if same_path(w['path'], path)), None)
        if not w:
            raise Fail('%s is not a worktree of %s' % (path, self.main))
        # Never touch the main checkout, a folder that contains it, or a worktree root that is itself a link.
        root_n = norm(path)
        if same_path(path, self.main) or (norm(self.main) + os.sep).startswith(root_n.rstrip('\\/') + os.sep):
            raise Fail('remove: %s is (or contains) the main checkout; refusing' % path)
        if is_link(path):
            raise Fail('remove: %s is itself a link; refusing' % path)
        status = self.status(path)[1]
        if status:
            raise Fail('remove: %s has uncommitted or untracked work:\n%s' % (path, '\n'.join(status)))
        # dotnet build servers (MSBuild node, C# compiler) outlive a launcher or translator build and run from the
        # worktree's toolchain link; they hold no work, so stop them the supported way.
        dotnet = os.path.join(path, 'toolchain', 'dotnet', 'dotnet.exe' if WINDOWS else 'dotnet')
        if any(is_dotnet(p) for p in running_from(path)) and os.path.exists(dotnet):
            say('remove: stopping the dotnet build servers running from it')
            subprocess.run([dotnet, 'build-server', 'shutdown'], stdout=subprocess.DEVNULL)
            for _ in range(20):
                if not any(is_dotnet(p) for p in running_from(path)):
                    break
                time.sleep(0.5)
        running = running_from(path)
        if running:
            raise Fail('remove: still running from it: %s' % ', '.join('%s (%s)' % (p['name'], p['pid']) for p in running))
        # Its Claude session (claude lives elsewhere; new/open start it as `claude ... -n <name>`): removing the
        # folder under it breaks that session. Close its tab first.
        session = claude_sessions(a.name)
        if session:
            raise Fail("remove: its Claude session is still running (%s %s); close the '%s' tab first" % (session[0]['name'], session[0]['pid'], a.name))
        ahead = git(self.main, 'rev-list', '--count', 'origin/main..' + w['branch'], check=False, capture=True).stdout.strip()

        # THE DANGEROUS STEP. The links point at the main checkout's real Disc\, toolchain\ ... Nothing below may
        # ever delete recursively through one:
        #  1. list every link at any depth (no following; any listing error aborts before anything is changed);
        #  2. remove each link itself (rmdir on a junction / dir symlink, unlink on a file symlink), never its target;
        #  3. list again: any link left aborts; only then git removes the folder, which now has no link in it.
        for link in find_links(path):
            unlink_link(link)
        left = find_links(path)
        if left:
            raise Fail('remove: these links are still there after the unlink pass (other links may already have been '
                       'unlinked; no files were deleted and git worktree remove was not run). Run remove again '
                       'once they can be unlinked:\n%s' % '\n'.join(left))
        self.git('worktree', 'remove', '--force', path)
        say('remove: %s removed; branch %s kept (%s commit(s) not in origin/main)' % (path, w['branch'], ahead))


def user_or_machine_env(name):
    """True if the variable is set in the user's or the machine's persistent environment (Windows only)."""
    if not WINDOWS:
        return False
    try:
        import winreg
    except ImportError:
        return False
    for hive, sub in ((winreg.HKEY_CURRENT_USER, 'Environment'),
                      (winreg.HKEY_LOCAL_MACHINE, r'SYSTEM\CurrentControlSet\Control\Session Manager\Environment')):
        try:
            with winreg.OpenKey(hive, sub) as k:
                winreg.QueryValueEx(k, name)
                return True
        except OSError:
            continue
    return False


def main(argv=None):
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(errors='replace')
        sys.stderr.reconfigure(errors='replace')
    if argv is None:
        packed = os.environ.pop(ARGS_ENV, None)
        argv = json.loads(packed) if packed is not None else sys.argv[1:]
    args = parse_args(argv)
    try:
        if args.model and re.search(r'[;\s]', args.model):
            raise Fail("-Model '%s': no semicolons or spaces (Windows Terminal splits its command line at semicolons)" % args.model)
        wt = Worktree(args)
        {'new': wt.cmd_new, 'list': wt.cmd_list, 'open': wt.cmd_open, 'remove': wt.cmd_remove}[args.command]()
    except Fail as e:
        print('worktree: %s' % e, file=sys.stderr)
        return 1
    except OSError as e:
        print('worktree: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

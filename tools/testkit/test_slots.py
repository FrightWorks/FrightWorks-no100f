"""Tests of slots.py with no game: python -m unittest discover -s tools/testkit -p "test_*.py" """
import datetime
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from testkit import slots  # noqa: E402

QUIET = lambda *a, **k: None  # noqa: E731


def old_time(minutes):
    return (datetime.datetime.now().astimezone() - datetime.timedelta(minutes=minutes)).isoformat()


class SlotsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.state = Path(self.tmp.name) / "FrightWorks-kit"
        self.state.mkdir()
        self.checkout = Path(self.tmp.name) / "checkout"
        self.checkout.mkdir()

    def pool(self, n=3):
        return slots.SlotPool(slots=n, state=self.state, poll=0.05)

    def test_n_slots_then_blocked_then_released(self):
        pool = self.pool(3)
        leases = [pool.acquire(f"job{i}", log=QUIET) for i in range(3)]
        self.assertEqual(sorted(l.index for l in leases), [0, 1, 2])
        with self.assertRaises(TimeoutError):
            pool.acquire("fourth", timeout=0.3, log=QUIET)
        leases[1].release()
        fourth = pool.acquire("fourth", timeout=2, log=QUIET)
        self.assertEqual(fourth.index, 1)
        for l in leases + [fourth]:
            l.release()
        self.assertEqual(pool.status()["live"], {})

    def test_slot_count_argument_env_and_default(self):
        self.assertEqual(slots.slot_count(None), 3 if not os.environ.get(slots.SLOTS_ENV) else int(os.environ[slots.SLOTS_ENV]))
        self.assertEqual(slots.slot_count(5), 5)
        self.assertEqual(slots.slot_count("2"), 2)
        self.assertEqual(slots.slot_count(0), 1)

    def test_abandoned_slot_is_taken_over(self):
        pool = self.pool(1)
        dead = {"pid": 2 ** 22 + 12345, "why": "crashed", "kind": "shared", "slot": 0, "since": old_time(1)}
        (self.state / "slots" / "slot-0.json").write_text(json.dumps(dead))
        lease = pool.acquire("next", timeout=2, log=QUIET)
        self.assertEqual(lease.index, 0)
        lease.release()

    def test_unrefreshed_slot_of_a_live_pid_is_taken_over(self):
        pool = self.pool(1)
        stuck = {"pid": os.getpid(), "why": "hung", "kind": "shared", "slot": 0, "since": old_time(5)}
        p = self.state / "slots" / "slot-0.json"
        p.write_text(json.dumps(stuck))
        past = time.time() - slots.STALE_SECONDS - 5
        os.utime(str(p), (past, past))
        pool.acquire("next", timeout=2, log=QUIET).release()

    def test_exclusive_waits_for_shared_and_blocks_new_ones(self):
        pool = self.pool(3)
        shared = pool.acquire("shared", log=QUIET)
        got = {}

        def want_exclusive():
            got["lease"] = pool.acquire("human check", exclusive=True, timeout=10, log=QUIET)

        t = threading.Thread(target=want_exclusive)
        t.start()
        time.sleep(0.4)
        self.assertNotIn("lease", got)  # still waiting for the shared run
        with self.assertRaises(TimeoutError):  # and while it waits nobody new gets in
            pool.acquire("late", timeout=0.3, log=QUIET)
        shared.release()
        t.join(5)
        self.assertIn("lease", got)
        with self.assertRaises(TimeoutError):
            pool.acquire("while exclusive", timeout=0.3, log=QUIET)
        got["lease"].release()
        pool.acquire("after", timeout=2, log=QUIET).release()
        self.assertFalse((self.state / "exclusive.lock").exists())

    def test_timed_out_exclusive_leaves_no_marker(self):
        pool = self.pool(3)
        shared = pool.acquire("shared", log=QUIET)
        with self.assertRaises(TimeoutError):
            pool.acquire("human", exclusive=True, timeout=0.3, log=QUIET)
        self.assertFalse((self.state / "exclusive.lock").exists())
        pool.acquire("another", timeout=1, log=QUIET).release()
        shared.release()

    def test_holder_for_finds_the_slot_by_checkout_and_holders_lists_live_ones(self):
        pool = self.pool(3)
        a = pool.acquire("a", root=self.checkout, log=QUIET)
        b = pool.acquire("b", root=str(self.checkout) + "-other", log=QUIET)
        with pool._mutex():
            found = pool.holder_for(str(self.checkout) + os.sep)  # a trailing separator is the same folder
        self.assertEqual(found[0], a.index)
        self.assertEqual(sorted(h["why"] for h in slots.holders(self.state)), ["a", "b"])
        a.release()
        b.release()
        self.assertEqual(slots.holders(self.state), [])

    def test_holders_ignores_a_dead_holders_file_and_leaves_it(self):
        dead = {"pid": 2 ** 22 + 777, "why": "crashed", "kind": "shared", "slot": 0, "since": old_time(1), "root": "x"}
        (self.state / "slots").mkdir(exist_ok=True)
        (self.state / "slots" / "slot-0.json").write_text(json.dumps(dead))
        self.assertEqual(slots.holders(self.state), [])
        self.assertTrue((self.state / "slots" / "slot-0.json").exists())  # looking never removes anything

    def test_no_game_run_lock_is_written_any_more(self):
        pool = self.pool(3)
        lease = pool.acquire("a", log=QUIET)
        self.assertFalse((self.state / "game-run.lock").exists())
        lease.release()


class HoldCommandTest(unittest.TestCase):
    """slots.py hold / release, the entry point of the PowerShell scripts: real child processes, no game."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.state = Path(self.tmp.name) / "kit"
        self.root_a = str(Path(self.tmp.name) / "checkout-a")
        self.root_b = str(Path(self.tmp.name) / "checkout-b")
        self.env = dict(os.environ, FRIGHTWORKS_KIT_DIR=str(self.state))
        self.procs = []
        self.addCleanup(self.kill_all)

    def kill_all(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            p.wait()
            for f in (p.stdin, p.stdout):
                if f:
                    f.close()

    def run_slots(self, *args, timeout=60):
        return subprocess.run([sys.executable, str(Path(slots.__file__)), *args], capture_output=True, text=True,
                              env=self.env, timeout=timeout)

    def hold(self, root, *args):
        """An attached hold: the child process keeps the slot while its stdin is open. Returns (process, first line)."""
        p = subprocess.Popen([sys.executable, str(Path(slots.__file__)), "hold", "--why", "test " + Path(root).name,
                              "--root", root, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=self.env)
        self.procs.append(p)
        return p, p.stdout.readline().strip()

    def live(self):
        return slots.holders(self.state)

    def wait_for(self, condition, seconds=10):
        end = time.time() + seconds
        while time.time() < end:
            if condition():
                return True
            time.sleep(0.05)
        return condition()

    def test_hold_keeps_the_slot_until_stdin_closes(self):
        p, line = self.hold(self.root_a)
        self.assertTrue(line.startswith("HELD"), line)
        self.assertEqual([h["kind"] for h in self.live()], ["exclusive"])
        self.assertEqual(self.live()[0]["root"], self.root_a)
        p.stdin.close()  # what happens when the script that started it ends or dies
        self.assertEqual(p.wait(30), 0)
        self.assertEqual(self.live(), [])

    def test_hold_releases_on_a_release_line(self):
        p, line = self.hold(self.root_a, "--shared")
        self.assertTrue(line.startswith("HELD"), line)
        self.assertEqual([h["kind"] for h in self.live()], ["shared"])
        p.stdin.write("release\n")
        p.stdin.flush()
        self.assertEqual(p.wait(30), 0)
        self.assertEqual(self.live(), [])

    def test_a_killed_holder_frees_the_slot_for_the_next(self):
        p, line = self.hold(self.root_a)
        self.assertTrue(line.startswith("HELD"), line)
        p.kill()  # no chance to clean up
        p.wait()
        q, line = self.hold(self.root_b, "--wait-minutes", "0.2")
        self.assertTrue(line.startswith("HELD"), line)

    def test_a_second_checkout_waits_for_an_exclusive_one_and_gives_up(self):
        p, line = self.hold(self.root_a)
        self.assertTrue(line.startswith("HELD"), line)
        q, first = self.hold(self.root_b, "--wait-minutes", "0.03")
        self.assertIn("waiting", first)
        self.assertTrue(q.stdout.readline().startswith("FAILED"))
        self.assertEqual(q.wait(30), 3)
        self.assertEqual([h["root"] for h in self.live()], [self.root_a])

    def test_nested_take_in_the_same_checkout_reuses_the_slot_and_gives_nothing_back(self):
        """iterate -Capture: the parent holds, the capture-window child asks for the same checkout again."""
        p, line = self.hold(self.root_a)
        self.assertTrue(line.startswith("HELD"), line)
        for extra in ((), ("--shared",)):  # exclusive covers both
            child, line = self.hold(self.root_a, *extra)
            self.assertEqual(line, "REUSED")
            self.assertEqual(child.wait(30), 0)
            self.assertEqual(len(self.live()), 1)  # the child ending did not give back the parent's slot
        p.stdin.close()
        p.wait(30)
        self.assertEqual(self.live(), [])

    def test_nested_exclusive_under_a_shared_slot_fails_instead_of_waiting_for_itself(self):
        p, line = self.hold(self.root_a, "--shared")
        self.assertTrue(line.startswith("HELD"), line)
        child, line = self.hold(self.root_a)
        self.assertTrue(line.startswith("FAILED"), line)
        self.assertEqual(child.wait(30), 4)
        self.assertEqual(len(self.live()), 1)
        again, line = self.hold(self.root_a, "--shared")
        self.assertEqual(line, "REUSED")  # shared under shared is fine

    def test_shared_holds_of_different_checkouts_run_side_by_side(self):
        p, line = self.hold(self.root_a, "--shared")
        q, line2 = self.hold(self.root_b, "--shared")
        self.assertTrue(line.startswith("HELD") and line2.startswith("HELD"), (line, line2))
        self.assertEqual(len(self.live()), 2)

    def test_detached_hold_survives_its_caller_and_release_gives_it_back(self):
        r = self.run_slots("hold", "--detach", "--why", "manual-session prepare", "--root", self.root_a)
        self.assertEqual((r.returncode, r.stdout.strip()), (0, "HELD 0 exclusive"), r.stderr)
        self.assertEqual(len(self.live()), 1)  # the caller is gone, the background process holds it
        again = self.run_slots("hold", "--detach", "--why", "manual-session start", "--root", self.root_a)
        self.assertEqual(again.stdout.strip(), "REUSED")  # a later prepare/start in the same checkout takes it back
        self.assertEqual(self.run_slots("release", "--root", self.root_a).returncode, 0)
        self.assertEqual(self.live(), [])
        self.assertFalse((self.state / "exclusive.lock").exists())
        self.assertEqual(self.run_slots("release", "--root", self.root_a).returncode, 0)  # nothing held: still fine

    def test_detached_hold_ends_by_itself_at_its_deadline(self):
        r = self.run_slots("hold", "--detach", "--why", "forgotten", "--root", self.root_a, "--max-minutes", "0.03")
        self.assertTrue(r.stdout.startswith("HELD"), r.stdout + r.stderr)
        self.assertTrue(self.wait_for(lambda: not self.live(), 30), "the background holder outlived its deadline")
        r = self.run_slots("hold", "--detach", "--why", "next", "--root", self.root_b, "--wait-minutes", "0.1")
        self.assertTrue(r.stdout.startswith("HELD"), r.stdout)

    def test_reuse_pushes_a_detached_deadline_back(self):
        r = self.run_slots("hold", "--detach", "--why", "prepare", "--root", self.root_a, "--max-minutes", "0.05")
        self.assertTrue(r.stdout.startswith("HELD"), r.stdout)
        time.sleep(1.0)
        again = self.run_slots("hold", "--detach", "--why", "start", "--root", self.root_a, "--max-minutes", "5")
        self.assertEqual(again.stdout.strip(), "REUSED")
        time.sleep(3.0)  # past the first deadline (3 s)
        self.assertEqual(len(self.live()), 1)
        self.run_slots("release", "--root", self.root_a)
        self.assertEqual(self.live(), [])

    def test_detached_hold_waiting_too_long_fails(self):
        p, line = self.hold(self.root_a)
        self.assertTrue(line.startswith("HELD"), line)
        r = self.run_slots("hold", "--detach", "--why", "second", "--root", self.root_b, "--wait-minutes", "0.03")
        self.assertEqual(r.returncode, 3, r.stdout)
        self.assertIn("FAILED", r.stdout)


@unittest.skipUnless(os.name == "nt", "game-run.ps1 is for Windows PowerShell")
class PowerShellWrapperTest(unittest.TestCase):
    """The real Enter-GameSlot / Exit-GameSlot of tools/debug/game-run.ps1 (Windows PowerShell 5.1), in a stand-in state folder."""

    GAME_RUN = Path(slots.__file__).resolve().parent.parent / "debug" / "game-run.ps1"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.state = Path(self.tmp.name) / "kit"
        self.checkout = Path(self.tmp.name) / "check out"  # a space: the argument quoting matters
        self.checkout.mkdir()
        self.env = dict(os.environ, FRIGHTWORKS_KIT_DIR=str(self.state))

    def ps(self, script, timeout=120):
        return subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                               f". '{self.GAME_RUN}'; $root = '{self.checkout}'; {script}"],
                              capture_output=True, text=True, env=self.env, timeout=timeout)

    def wait_for_free(self, seconds=20):
        end = time.time() + seconds
        while time.time() < end:
            if not slots.holders(self.state):
                return True
            time.sleep(0.1)
        return not slots.holders(self.state)

    def test_enter_nested_enter_and_exit(self):
        probe = "(python '%s' status | Select-String 'slots:').Line" % Path(slots.__file__)
        r = self.ps(f"Enter-GameSlot $root 'outer'; {probe}; Enter-GameSlot $root 'inner'; Exit-GameSlot $root; {probe}; "
                    f"Exit-GameSlot $root; {probe}")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout.split(), ["slots:", "1", "of", "3", "in", "use"] * 2 + ["slots:", "0", "of", "3", "in", "use"])

    def test_nested_exclusive_under_a_shared_slot_throws(self):
        r = self.ps("Enter-GameSlot $root 'outer' -Shared; Enter-GameSlot $root 'inner'")
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn("cannot be made exclusive", r.stdout + r.stderr)
        self.assertTrue(self.wait_for_free())  # and the script's end still gave the shared slot back

    def test_a_script_that_ends_without_exit_gives_the_slot_back(self):
        r = self.ps("Enter-GameSlot $root 'forgot to exit'")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(self.wait_for_free(), "the slot outlived the script")

    def test_a_killed_script_gives_the_slot_back(self):
        p = subprocess.Popen(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                              f". '{self.GAME_RUN}'; Enter-GameSlot '{self.checkout}' 'to be killed'; 'ready'; Start-Sleep 120"],
                             stdout=subprocess.PIPE, text=True, env=self.env)
        self.addCleanup(p.wait)
        self.addCleanup(p.stdout.close)
        self.assertEqual(p.stdout.readline().strip(), "ready")
        self.assertEqual(len(slots.holders(self.state)), 1)
        p.kill()
        self.assertTrue(self.wait_for_free(), "a killed script left its slot held")

    def test_persist_then_exit_from_a_later_script_run(self):
        r = self.ps("Enter-GameSlot $root 'manual-session prepare' -Persist")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual([h["why"] for h in slots.holders(self.state)], ["manual-session prepare"])
        r = self.ps("Exit-GameSlot $root -Persisted")  # manual-session stop, a new process
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(self.wait_for_free())


class ConcurrencyTest(unittest.TestCase):
    def test_processes_never_exceed_the_slot_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            state, counter = Path(tmp) / "kit", Path(tmp) / "running"
            counter.mkdir()
            child = (
                "import sys, time, os; sys.path.insert(0, sys.argv[1])\n"
                "from testkit import slots\n"
                "pool = slots.SlotPool(slots=3, state=sys.argv[2], poll=0.05)\n"
                "with pool.acquire('child %d' % os.getpid(), log=lambda *a: None):\n"
                "    open(os.path.join(sys.argv[3], str(os.getpid())), 'w').close()\n"
                "    seen = len(os.listdir(sys.argv[3]))\n"
                "    time.sleep(0.4)\n"
                "    os.remove(os.path.join(sys.argv[3], str(os.getpid())))\n"
                "print(seen)\n")
            root = str(Path(__file__).resolve().parent.parent)
            procs = [subprocess.Popen([sys.executable, "-c", child, root, str(state), str(counter)],
                                      stdout=subprocess.PIPE, text=True) for _ in range(8)]
            seen = [int(p.communicate(timeout=60)[0]) for p in procs]
            self.assertTrue(all(p.returncode == 0 for p in procs))
            self.assertLessEqual(max(seen), 3)
            self.assertEqual(max(seen), 3)  # and they did overlap


if __name__ == "__main__":
    unittest.main()

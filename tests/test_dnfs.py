"""Integration tests for DNFS.

Each test starts a real name server and storage servers in a temporary
directory, on free ports, and talks to them over TCP. Run with `make test`
(which builds first) or `python3 tests/test_dnfs.py`.
"""
import json
import os
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Overridable so the suite can run against sanitizer builds.
NM_BIN = os.environ.get("DNFS_NM_BIN", os.path.join(ROOT, "name_server", "nm"))
SS_BIN = os.environ.get("DNFS_SS_BIN", os.path.join(ROOT, "storage_server", "ss"))
CLIENT_BIN = os.environ.get("DNFS_CLIENT_BIN", os.path.join(ROOT, "client", "client"))
SS_TIMEOUT = 3


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_until(predicate, timeout=10.0, interval=0.05):
    end = time.time() + timeout
    while time.time() < end:
        if predicate():
            return True
        time.sleep(interval)
    return False


def call(port, obj, timeout=15):
    """Sends one JSON message and returns the parsed one-line reply."""
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall((json.dumps(obj) + "\n").encode())
        data = b""
        while b"\n" not in data:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    return json.loads(data.split(b"\n", 1)[0])


class SSConn:
    """A connection to a storage server that can carry several messages."""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=15)
        self.buf = b""

    def send(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode())

    def recv(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("storage server closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def req(self, obj):
        self.send(obj)
        return self.recv()

    def close(self):
        self.sock.close()


class Cluster:
    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="dnfs-test-")
        self.nm_port = free_port()
        self.env = dict(os.environ,
                        DNFS_SECRET="test-secret-0123456789abcdef",
                        DNFS_NM_PORT=str(self.nm_port),
                        DNFS_HEARTBEAT_INTERVAL="1",
                        DNFS_SS_TIMEOUT=str(SS_TIMEOUT),
                        DNFS_EXEC_TIMEOUT="2")
        self.procs = {}
        self.ss_ports = {}
        self.tokens = {}

    def _spawn(self, name, args):
        workdir = os.path.join(self.dir, name)
        os.makedirs(workdir, exist_ok=True)
        log = open(os.path.join(workdir, "stdout.log"), "ab")
        self.procs[name] = subprocess.Popen(args, cwd=workdir, env=self.env, stdout=log, stderr=subprocess.STDOUT)
        log.close()

    def log_count(self, name, text):
        try:
            with open(os.path.join(self.dir, name, "stdout.log"), errors="replace") as f:
                return f.read().count(text)
        except FileNotFoundError:
            return 0

    def start_nm(self):
        self._spawn("nm", [NM_BIN])

        def up():
            try:
                with socket.create_connection(("127.0.0.1", self.nm_port), timeout=0.2):
                    return True
            except OSError:
                return False
        assert wait_until(up), "name server did not start"
        self.tokens.clear()

    def start_ss(self, name, wait=True):
        port = self.ss_ports.setdefault(name, free_port())
        before = self.log_count(name, "Registered with Name Server")
        self._spawn(name, [SS_BIN, str(port), "127.0.0.1", "127.0.0.1"])
        if wait:
            assert wait_until(lambda: self.log_count(name, "Registered with Name Server") > before), \
                f"{name} did not register"
        return port

    def stop(self, name):
        proc = self.procs.get(name)
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    def alive(self, name):
        return self.procs[name].poll() is None

    def data_path(self, ss_name, filename):
        return os.path.join(self.dir, ss_name, "storage_server", "data", filename)

    def close(self):
        for name in list(self.procs):
            self.stop(name)
        shutil.rmtree(self.dir, ignore_errors=True)

    # ---- protocol helpers ----

    def login(self, user, password=None):
        reply = call(self.nm_port, {"cmd": "register_client", "username": user,
                                    "password": password or f"pw-{user}"})
        if reply.get("status") == "OK":
            self.tokens[user] = reply["token"]
        return reply

    def nm(self, user, cmd, **fields):
        if user not in self.tokens:
            self.login(user)
        return call(self.nm_port, dict({"cmd": cmd, "token": self.tokens[user]}, **fields))

    def route(self, user, cmd, filename):
        reply = self.nm(user, cmd, filename=filename)
        assert reply["status"] == "OK", reply
        return reply

    def open_ss(self, user, cmd, filename, **extra):
        """Gets a route for cmd and sends cmd to that storage server."""
        r = self.route(user, cmd, filename)
        conn = SSConn(r["ss_port"])
        first = conn.req(dict({"cmd": cmd, "filename": filename, "user": r["user"],
                               "expires": r["expires"], "ticket": r["ticket"]}, **extra))
        return conn, first, r

    def write(self, user, filename, sentence, updates):
        conn, first, _ = self.open_ss(user, "WRITE", filename, sentence_index=sentence)
        assert first["status"] == "OK", first
        for index, text in updates:
            reply = conn.req({"cmd": "UPDATE", "word_index": index, "content": text})
            assert reply["status"] == "OK", reply
        result = conn.req({"cmd": "ETIRW"})
        conn.close()
        return result

    def read(self, user, filename):
        conn, reply, _ = self.open_ss(user, "READ", filename)
        conn.close()
        return reply


class DNFSTest(unittest.TestCase):
    def setUp(self):
        self.c = Cluster()
        self.c.start_nm()
        self.c.start_ss("ss1")

    def tearDown(self):
        self.c.close()

    def create(self, user, filename, text=None):
        reply = self.c.nm(user, "CREATE", filename=filename)
        self.assertEqual(reply["status"], "OK", reply)
        if text is not None:
            self.assertEqual(self.c.write(user, filename, 0, [(0, text)])["status"], "OK")

    # C1: identity comes from a password login, not a field in each request.
    def test_login_and_tokens(self):
        self.assertEqual(self.c.login("alice")["status"], "OK")
        self.assertEqual(self.c.login("alice", "wrong")["reason"], "BAD_CREDENTIALS")
        self.assertEqual(self.c.login("../evil")["reason"], "INVALID_USERNAME")
        no_token = call(self.c.nm_port, {"cmd": "VIEW", "username": "alice"})
        self.assertEqual(no_token["reason"], "UNAUTHENTICATED")

        self.create("alice", "secret.txt", "Alice only.")
        self.c.login("bob")
        forged = call(self.c.nm_port, {"cmd": "DELETE", "token": self.c.tokens["bob"],
                                       "username": "alice", "filename": "secret.txt"})
        self.assertEqual(forged["reason"], "UNAUTHORIZED")

    # C2: a storage server only acts on a ticket the name server signed.
    def test_storage_server_requires_ticket(self):
        self.create("alice", "private.txt", "Alice secret plan.")
        port = self.c.ss_ports["ss1"]
        no_ticket = call(port, {"cmd": "READ", "filename": "private.txt", "username": "bob"})
        self.assertEqual(no_ticket["reason"], "UNAUTHORIZED")

        self.c.login("bob")
        self.assertEqual(self.c.nm("bob", "READ", filename="private.txt")["reason"], "UNAUTHORIZED")

        r = self.c.route("alice", "READ", "private.txt")
        base = {"user": r["user"], "expires": r["expires"], "ticket": r["ticket"]}
        self.assertEqual(call(port, dict(base, cmd="WRITE", filename="private.txt", sentence_index=0))["reason"],
                         "UNAUTHORIZED")
        self.assertEqual(call(port, dict(base, cmd="DELETE", filename="private.txt"))["reason"], "UNAUTHORIZED")
        self.assertEqual(call(port, dict(base, cmd="READ", filename="other.txt"))["reason"], "UNAUTHORIZED")
        self.assertEqual(call(port, dict(base, cmd="READ", filename="private.txt"))["content"], "Alice secret plan.")

    # C3: names that could escape the data directory are refused everywhere.
    def test_path_traversal_rejected(self):
        outside = os.path.join(self.c.dir, "outside.txt")
        with open(outside, "w") as f:
            f.write("keep me")
        self.assertEqual(self.c.nm("alice", "CREATE", filename="../../outside.txt")["reason"], "INVALID_FILENAME")
        self.assertEqual(self.c.nm("alice", "CREATE", filename=".hidden")["reason"], "INVALID_FILENAME")
        reply = call(self.c.ss_ports["ss1"], {"cmd": "DELETE", "filename": "../../outside.txt",
                                               "user": "nm", "expires": 0, "ticket": "x"})
        self.assertEqual(reply["reason"], "INVALID_FILENAME")
        self.assertTrue(os.path.exists(outside))

    # C4: commits to different sentences of one file never lose an edit.
    def test_concurrent_commits_keep_both_edits(self):
        self.create("alice", "race.txt")
        self.c.login("bob")
        self.assertEqual(self.c.nm("alice", "ADDACCESS", filename="race.txt", target="bob", mode="W")["status"], "OK")
        path = self.c.data_path("ss1", "race.txt")
        for trial in range(60):
            with open(path, "w") as f:
                f.write("One. Two.")
            a, ra, _ = self.c.open_ss("alice", "WRITE", "race.txt", sentence_index=0)
            b, rb, _ = self.c.open_ss("bob", "WRITE", "race.txt", sentence_index=1)
            self.assertEqual((ra["status"], rb["status"]), ("OK", "OK"))
            a.req({"cmd": "UPDATE", "word_index": 1, "content": "alpha"})
            b.req({"cmd": "UPDATE", "word_index": 1, "content": "beta"})
            barrier = threading.Barrier(2)
            results = {}

            def commit(key, conn):
                barrier.wait()
                results[key] = conn.req({"cmd": "ETIRW"})

            threads = [threading.Thread(target=commit, args=("a", a)), threading.Thread(target=commit, args=("b", b))]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            a.close()
            b.close()
            self.assertEqual(results["a"]["status"], "OK", results)
            self.assertEqual(results["b"]["status"], "OK", results)
            with open(path) as f:
                self.assertEqual(f.read(), "One alpha. Two beta.", f"trial {trial}")

    # H3: if the sentence changed underneath a session, commit is refused.
    def test_conflict_instead_of_wrong_sentence(self):
        self.create("alice", "shift.txt", "Apple. Banana.")
        u2, first, _ = self.c.open_ss("alice", "WRITE", "shift.txt", sentence_index=1)
        self.assertEqual(first["status"], "OK")
        self.assertEqual(self.c.write("alice", "shift.txt", 0, [(0, "Xray.")])["status"], "OK")
        self.assertEqual(self.c.write("alice", "shift.txt", 2, [(1, "three")])["status"], "OK")
        u2.req({"cmd": "UPDATE", "word_index": 1, "content": "two"})
        self.assertEqual(u2.req({"cmd": "ETIRW"})["reason"], "CONFLICT")
        u2.close()
        self.assertEqual(self.c.read("alice", "shift.txt")["content"], "Xray. Apple. Banana three.")

    # H4 and H6: large files come back whole.
    def test_large_read(self):
        words = " ".join(f"word{i:06d}" for i in range(30000))
        self.create("alice", "big.txt", words)
        self.assertGreater(os.path.getsize(self.c.data_path("ss1", "big.txt")), 300000)
        self.assertEqual(self.c.read("alice", "big.txt")["content"], words)

    # H1 and H2: file words can't end a stream, and leaving early is harmless.
    def test_stream_control_words_and_disconnect(self):
        text = "int main { return 0 } STOP then more"
        self.create("alice", "code.txt", text)
        conn, first, _ = self.c.open_ss("alice", "STREAM", "code.txt")
        words = [first["word"]]
        while True:
            message = conn.recv()
            if message["status"] == "END":
                break
            words.append(message["word"])
        conn.close()
        self.assertEqual(words, text.split())

        conn, first, _ = self.c.open_ss("alice", "STREAM", "code.txt")
        conn.close()
        time.sleep(0.6)
        self.assertTrue(self.c.alive("ss1"))
        self.assertEqual(self.c.read("alice", "code.txt")["content"], text)

    # C5 and M5: EXEC output is exact, and runaway commands are stopped.
    def test_exec_sandbox(self):
        self.create("alice", "path.sh", "printf '%s' 'C:\\new_dir'")
        reply = self.c.nm("alice", "EXEC", filename="path.sh")
        self.assertEqual(reply["output"], "C:\\new_dir")
        self.assertEqual(reply["exit_code"], 0)

        self.create("alice", "slow.sh", "sleep 30")
        started = time.time()
        reply = self.c.nm("alice", "EXEC", filename="slow.sh")
        self.assertTrue(reply["timed_out"])
        self.assertLess(time.time() - started, 8)

        self.c.login("bob")
        self.assertEqual(self.c.nm("bob", "EXEC", filename="path.sh")["reason"], "UNAUTHORIZED")

    # M3: UNDO restores the version before the last real change, once.
    def test_undo_semantics(self):
        self.create("alice", "undo.txt", "Version one.")
        self.assertEqual(self.c.write("alice", "undo.txt", 0, [(2, "two")])["status"], "OK")
        self.assertEqual(self.c.write("alice", "undo.txt", 0, [])["status"], "OK")
        conn, reply, _ = self.c.open_ss("alice", "UNDO", "undo.txt")
        conn.close()
        self.assertEqual(reply["status"], "OK")
        self.assertEqual(self.c.read("alice", "undo.txt")["content"], "Version one.")
        conn, reply, _ = self.c.open_ss("alice", "UNDO", "undo.txt")
        conn.close()
        self.assertEqual(reply["reason"], "NO_SNAPSHOT")

    # H5 and M1: commits reach the backup, and outage writes survive.
    def test_failover_keeps_writes(self):
        self.c.start_ss("ss2")
        reply = self.c.nm("alice", "CREATE", filename="fail.txt")
        primary = "ss1" if reply["ss_port"] == self.c.ss_ports["ss1"] else "ss2"
        backup = "ss2" if primary == "ss1" else "ss1"
        self.assertEqual(self.c.write("alice", "fail.txt", 0, [(0, "Original important text.")])["status"], "OK")

        def backup_has(text):
            try:
                with open(self.c.data_path(backup, "fail.txt")) as f:
                    return f.read() == text
            except FileNotFoundError:
                return False
        self.assertTrue(wait_until(lambda: backup_has("Original important text.")))

        self.c.stop(primary)
        time.sleep(SS_TIMEOUT + 1.5)
        self.assertEqual(self.c.read("alice", "fail.txt")["content"], "Original important text.")
        self.assertEqual(self.c.write("alice", "fail.txt", 1, [(0, "Outage edit.")])["status"], "OK")

        self.c.start_ss(primary)
        self.assertTrue(wait_until(lambda: self.c.read("alice", "fail.txt")["content"] ==
                                   "Original important text. Outage edit.", timeout=10))
        r = self.c.route("alice", "READ", "fail.txt")
        self.assertEqual(r["ss_port"], self.c.ss_ports[primary])

    # M1: storage servers find the name server whenever it comes up.
    def test_storage_server_reregisters(self):
        self.create("alice", "keep.txt", "Still here.")
        self.c.stop("nm")
        self.c.start_nm()
        self.assertTrue(wait_until(lambda: self.c.nm("alice", "CREATE", filename="after.txt")["status"] == "OK",
                                   timeout=10))
        self.assertEqual(self.c.read("alice", "keep.txt")["content"], "Still here.")

        self.c.stop("ss1")
        self.c.stop("nm")
        self.c.start_ss("ss1", wait=False)
        time.sleep(1.5)
        self.c.start_nm()
        self.assertTrue(wait_until(lambda: self.c.log_count("ss1", "Registered with Name Server") >= 3,
                                   timeout=10))

    # M6 and M9: long access lists survive a restart.
    def test_metadata_persists_long_access_list(self):
        self.create("alice", "shared.txt", "Hello.")
        names = [f"user{i:02d}" for i in range(40)]
        for name in names:
            self.assertEqual(self.c.login(name)["status"], "OK")
            self.assertEqual(self.c.nm("alice", "ADDACCESS", filename="shared.txt", target=name, mode="R")["status"],
                             "OK")
        self.c.stop("nm")
        self.c.start_nm()
        self.assertTrue(wait_until(lambda: self.c.nm("alice", "INFO", filename="shared.txt")["status"] == "OK"))
        users = {e["user"] for e in self.c.nm("alice", "INFO", filename="shared.txt")["access"]}
        self.assertEqual(users, set(names) | {"alice"})
        self.assertEqual(self.c.login("alice", "wrong")["reason"], "BAD_CREDENTIALS")

    # M4: delete waits for the storage server and respects open edits.
    def test_delete_rules(self):
        self.create("alice", "doc.txt", "Text.")
        conn, first, _ = self.c.open_ss("alice", "WRITE", "doc.txt", sentence_index=0)
        self.assertEqual(first["status"], "OK")
        self.assertEqual(self.c.nm("alice", "DELETE", filename="doc.txt")["reason"], "LOCKED")
        conn.close()
        time.sleep(0.2)

        self.c.stop("ss1")
        time.sleep(SS_TIMEOUT + 1.5)
        self.assertEqual(self.c.nm("alice", "DELETE", filename="doc.txt")["reason"], "SS_UNAVAILABLE")
        self.assertIn("doc.txt", self.c.nm("alice", "VIEW", flags="")["files"])

        self.c.start_ss("ss1")
        self.assertEqual(self.c.nm("alice", "DELETE", filename="doc.txt")["status"], "OK")
        self.assertFalse(os.path.exists(self.c.data_path("ss1", "doc.txt")))
        self.assertEqual(self.c.nm("alice", "CREATE", filename="doc.txt")["status"], "OK")

    # L1, M11 and access-list checks.
    def test_view_info_and_access_rules(self):
        self.create("alice", "b.txt", "Two words.")
        self.create("alice", "a.txt")
        self.assertEqual(self.c.nm("alice", "VIEW", flags="-x")["reason"], "INVALID_FLAGS")
        details = self.c.nm("alice", "VIEW", flags="-al")["files"]
        self.assertEqual([f["filename"] for f in details], ["a.txt", "b.txt"])
        self.assertEqual(details[1]["words"], 2)

        self.c.login("bob")
        self.assertEqual(self.c.nm("bob", "VIEW", flags="")["files"], [])
        self.assertEqual(self.c.nm("bob", "INFO", filename="a.txt")["reason"], "UNAUTHORIZED")
        self.assertEqual(self.c.nm("alice", "ADDACCESS", filename="a.txt", target="nobody", mode="R")["reason"],
                         "USER_NOT_FOUND")
        self.assertEqual(self.c.nm("alice", "ADDACCESS", filename="a.txt", target="bob", mode="X")["reason"],
                         "BAD_REQUEST")

    # The real client binary, end to end.
    def test_client_binary(self):
        script = "\n".join([
            "carol", "pw-carol",
            "CREATE notes.txt",
            "WRITE notes.txt 0", "0 Hello from \"carol\".", "ETIRW",
            "READ notes.txt",
            "VIEW -a -l",
            "exit",
        ]) + "\n"
        out = subprocess.run([CLIENT_BIN, "127.0.0.1"], input=script, capture_output=True, text=True,
                             env=self.c.env, timeout=30).stdout
        self.assertIn("Logged in as 'carol'", out)
        self.assertIn("Write Successful!", out)
        self.assertIn('Hello from "carol".', out)
        self.assertIn("notes.txt", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)

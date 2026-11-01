# DNFS: a guide for someone who has never seen it

This guide assumes you can read basic C and use a terminal. It doesn't assume
you know networking, threads or distributed systems; each idea is explained the
first time it comes up. Read it top to bottom once, then use it as a map.

Contents:
1. What the project is
2. Try it in five minutes
3. The three programs
4. Ideas you need first
5. What happens when you type a command
6. How editing works (the WRITE session)
7. Keeping copies: backups, heartbeats and failover
8. Security: logins, tickets and names
9. Where everything is in the code
10. Building, testing and debugging
11. Bugs that were fixed, and what they teach
12. Glossary

---

## 1. What the project is

DNFS (Distributed Network File System) lets several people store and edit text
files that live on other computers. It's a small version of how large systems
like Google's file system or HDFS work. It was built for the IIIT Hyderabad
Operating Systems and Networks course project.

"Distributed" means the work is split across several programs, which can run
on different machines:

- one **Name Server** knows *about* every file: who owns it, who may read or
  write it, and which machine holds it;
- one or more **Storage Servers** hold the actual file contents;
- any number of **Clients** let people type commands.

The key design choice: the Name Server only hands out directions. File
contents travel straight between the client and a storage server. That keeps
the Name Server from becoming a bottleneck, since it never carries the heavy
data.

What you can do with it: create files, read them, edit them one sentence at a
time (several people can edit different sentences at once), stream a file
word by word, undo the last change, share files read-only or read-write with
other users, and run a file as a shell script (`EXEC`).

---

## 2. Try it in five minutes

You need Linux, `gcc`, `make` and OpenSSL's headers (`sudo apt install
libssl-dev` on Ubuntu).

```bash
make all
```

Open three terminals in the project folder.

```bash
# Terminal 1: the Name Server
./name_server/nm

# Terminal 2: a Storage Server on port 9100
./storage_server/ss 9100 127.0.0.1

# Terminal 3: a client
./client/client 127.0.0.1
```

The client asks for a username and a password. The first time you use a
username, the password you type becomes its password. Then try:

```text
alice> CREATE notes.txt
File Created Successfully!
alice> WRITE notes.txt 0
Client: 0 Hello world.
Update acknowledged.
Client: 2 again
Update acknowledged.
Client: ETIRW
Write Successful!
alice> READ notes.txt
Hello world again.
alice> STREAM notes.txt
Hello world again.
alice> UNDO notes.txt
Undo Successful!
alice> READ notes.txt

alice> VIEW -l
alice> help
```

Start a second client in another terminal as `bob`, and try `READ notes.txt`.
It fails with `UNAUTHORIZED` until alice runs `ADDACCESS -R notes.txt bob`.

Start a second storage server (`./storage_server/ss 9101 127.0.0.1`) before
creating a file, and that file gets a backup copy on it.

---

## 3. The three programs

### The Name Server (`name_server/`, binary `nm`)

The Name Server is the "librarian". It never stores file contents. It keeps:

- **users**: each username with a hashed password;
- **sessions**: which login token belongs to which user;
- **files**: for each file, its owner, its access list (who has `R` or `W`),
  which storage server holds the main copy (the *primary*) and which holds the
  *backup*, timestamps and word counts;
- **storage servers**: each one's address and when it last said "I'm alive".

All of this is saved to `name_server/metadata_store.json` after changes, so a
restart doesn't lose it. Storage servers aren't saved; they announce themselves
again within seconds.

It listens on TCP port 9000 and handles each incoming connection in its own
thread.

### The Storage Server (`storage_server/`, binary `ss`)

The Storage Server is the "shelf". Each one stores files in its own
`storage_server/data/` folder, keeps one undo copy per file in
`storage_server/snapshots/`, and logs to `storage_server/logs/`.

It does the real work on file content: reading, streaming, and the
sentence-by-sentence editing. It only acts on requests that carry a *ticket*
from the Name Server (section 8). You can run many storage servers on
different ports or machines.

### The Client (`client/`, binary `client`)

The client is a command prompt. It logs in, then for each command it asks the
Name Server what to do, and for READ, WRITE, STREAM and UNDO it then connects
to the storage server the Name Server pointed it to.

### The shared code (`common/`)

Code that all three programs need lives once in `common/`: reading and writing
JSON, sending messages over TCP, checking file and user names, and the
cryptography for passwords and tickets.

---

## 4. Ideas you need first

**TCP socket.** A two-way pipe of bytes between two programs, possibly on
different machines. A server *listens* on a *port* (a number like 9000); a
client *connects* to an IP address and port. In C you use `socket()`,
`bind()`, `listen()`, `accept()` on the server and `connect()` on the client,
then `send()` and `recv()`.

**TCP is a stream, not messages.** If you send 5,000 bytes, the other side
may receive them as 1,448 + 1,448 + 2,104 bytes over three `recv()` calls. So a
program must know where one message ends. DNFS ends every message with a
newline (`\n`) and keeps reading until it sees one. That's called *framing*;
see `line_reader_next()` in `common/src/net.c`.

**JSON.** A text format for structured data: `{"cmd":"READ","filename":"a.txt"}`.
Every DNFS message is one JSON object on one line. Text inside quotes is
*escaped*: a newline becomes `\n`, a quote becomes `\"`. That's why a message
never contains a raw newline, which keeps the framing simple.

**Threads and mutexes.** Each server handles each connection in its own thread
(`pthread_create`), so many users are served at once. Threads share memory, so
two threads changing the same data at the same moment can corrupt it. A
*mutex* is a lock: only one thread can hold it at a time. The rule in this code
is "hold the lock while you touch shared data, and never while you wait for the
network".

**Race condition.** A bug that only appears when two things happen at nearly
the same moment, so it hides in normal testing. DNFS had several; section 11
describes them.

**Hash table and LRU cache.** The Name Server finds a file's record by hashing
its name into one of 1,024 buckets (`nm_metadata.c`). A small *LRU cache*
(Least Recently Used: 50 entries, `nm_cache.c`) remembers recent lookups. The
course spec asked for it.

**Hashing a password.** The server never stores your password. It stores the
result of a slow, one-way function (PBKDF2-SHA256, 100,000 rounds) of the
password plus random *salt*. To check a login it runs the same function on what
you typed and compares. Slowness makes guessing expensive for an attacker.

**HMAC.** A *signature* computed from a message and a secret key. Anyone with
the key can check it; nobody without the key can forge it. DNFS uses HMAC-SHA256
for tickets.

---

## 5. What happens when you type a command

### Logging in

```
client                          name server
  | register_client(alice, pw) -->|  look up alice; check or set password hash
  |<-------- OK, token=9f3c... ---|  remember token -> alice
```

The token is a random 64-character string. The client sends it with every
later request; the Name Server looks up whose token it is. A request can't
claim to be someone else, because the identity comes from the token, not from
anything the client says.

### A metadata command (VIEW, LIST, INFO, CREATE, DELETE, ADDACCESS, REMACCESS, EXEC)

The client opens a connection to the Name Server, sends one JSON line, reads
one JSON line back, and closes the connection. For example:

```
-> {"cmd":"VIEW","token":"9f3c...","flags":"-l"}
<- {"status":"OK","files":[{"filename":"notes.txt","owner":"alice","words":3,...}]}
```

Some of these also involve storage servers behind the scenes. CREATE asks one
storage server to create the file (and a second to hold the backup). DELETE
asks them to delete it. VIEW -l and INFO ask for fresh word counts.

### A content command (READ, WRITE, STREAM, UNDO)

These take two steps:

```
client                 name server                     storage server
  | READ notes.txt ------->|                                  |
  |                        | file exists? alice allowed (R)?  |
  |                        | pick a live, up-to-date copy     |
  |<-- ss 127.0.0.1:9100,  |                                  |
  |    ticket=ab12...      |                                  |
  | READ notes.txt + ticket ---------------------------------->|
  |                        |                 ticket valid? -> |
  |<---------------------------------- {"content":"Hello..."} |
```

The *ticket* is the Name Server's signed permission slip: "user alice may READ
notes.txt until 12:05". The storage server checks the signature with the
shared secret, so it doesn't need its own copy of the access lists.

### Which commands need which permission

| Command | Who may run it |
|---|---|
| VIEW, LIST | any logged-in user (VIEW without `-a` shows only files you can read) |
| CREATE | anyone; you become the owner |
| READ, STREAM, INFO, EXEC | the owner, or anyone with `R` or `W` |
| WRITE, UNDO | the owner, or anyone with `W` |
| DELETE, ADDACCESS, REMACCESS | the owner only |

---

## 6. How editing works (the WRITE session)

Files are edited one *sentence* at a time. A sentence ends at every `.`, `!`
or `?`, even in the middle of a word like `e.g.` (the course spec says so).
Sentences are numbered from 0, and so are the words inside a sentence.

```
WRITE notes.txt 0        lock sentence 0 of notes.txt
Client: 0 Hello world.   insert "Hello world." at word position 0
Client: 2 again          insert "again" at word position 2
Client: 1! there         replace word 1 with "there"
Client: ETIRW            commit ("WRITE" backwards)
```

Rules for the sentence number in `WRITE <file> <n>`:

- `n` less than the number of sentences: edit that sentence;
- `n` equal to the number of sentences, and the last sentence ends in `. ! ?`:
  start a new sentence at the end;
- empty file and `n` = 0: start the first sentence;
- anything else: `INVALID_INDEX`.

Only one person can lock a given sentence at a time; a second one gets
`SENTENCE LOCKED` immediately. Different sentences can be edited at once.

**What commit does** (`handle_commit` in `storage_server/src/ss_write_handlers.c`).
While you type, edits change only a copy of your sentence held in memory.
`ETIRW` then does this, while holding a lock on the whole file:

1. reload the file from disk (someone else may have committed meanwhile);
2. find the sentence you started from, by its text;
3. if it can't be found, because someone else changed it, refuse with
   `CONFLICT` rather than guess;
4. splice your edited sentence(s) in;
5. save the version being replaced as the undo snapshot;
6. write the new file to a temporary file, `fsync` it to disk, and `rename` it
   over the old one. `rename` replaces a file in one step, so readers always
   see a complete old or complete new file;
7. reply `WRITE DONE`, release the locks, and tell the Name Server the file
   changed (so the backup gets a copy).

**UNDO** puts back the snapshot from step 5, then deletes it, so it undoes one
change. It's refused while anyone holds a sentence lock on the file.

---

## 7. Keeping copies: backups, heartbeats and failover

When two or more storage servers are running, CREATE puts the file on one (the
*primary*, chosen round-robin) and an empty copy on the next one (the
*backup*).

**Heartbeats.** Each storage server sends the Name Server a small "I'm alive"
message every 5 seconds. If none arrives for 15 seconds, the Name Server
treats that server as down. If the Name Server itself restarts and forgets the
server, the next heartbeat gets `UNKNOWN_SS` and the server registers again. A
storage server started before the Name Server simply keeps retrying.

**Replication.** After every commit or undo, the storage server tells the Name
Server, which copies the file (and its undo snapshot) to the other replica. If
the other replica is down, the Name Server marks it *stale*.

**Failover.** When the primary is down, the Name Server sends clients to the
backup, which has every committed change. Writes made there mark the primary
stale. When the primary comes back, it re-registers with its list of files;
the Name Server copies the newer version back to it before sending clients
there again.

What it doesn't do: if both copies are down, the file is unavailable
(`SS_UNAVAILABLE`). There are at most two copies. Two people writing to
different replicas during a network split isn't handled; the copy made last
wins.

---

## 8. Security: logins, tickets and names

- **Login.** Passwords are stored as salted PBKDF2-SHA256 hashes in
  `metadata_store.json`. Every request carries a session token.
- **Tickets.** The Name Server and storage servers share a secret key. The Name
  Server creates `dnfs.secret` on first start; storage servers read it from
  their working directory or from the `DNFS_SECRET` environment variable. Every
  request to a storage server must carry a ticket: an HMAC of (command, user,
  file, expiry time) under that key, valid for 5 minutes. A storage server's
  own messages to the Name Server (register, heartbeat, commit) are signed the
  same way, so a stranger can't pretend to be a storage server.
- **Names.** File and user names may only contain letters, digits, `.`, `_`
  and `-`, and can't start with `.`. A name like `../../etc/passwd` is refused
  before it gets near the disk.
- **EXEC.** `EXEC` runs a file's contents as shell commands on the *Name
  Server* machine, as the Name Server's user. It runs in a separate process
  with a 10-second limit, CPU, memory and file-size limits, a 64 KB output cap,
  an empty temporary folder and a minimal environment. That limits accidents
  but isn't a real sandbox: a script can still read anything that user can
  read.
- **Not covered.** Traffic isn't encrypted (there's no TLS), so someone who
  can capture packets between the machines can read passwords and tokens.

---

## 9. Where everything is in the code

```
common/include/dnfs_shared.h   one header for all shared helpers
common/src/net.c               send_all, send_line, connect with timeout,
                               LineReader (framing), StrBuf (growable string)
common/src/json.c              json_escape, json_find_value, json_get_str/long/bool
common/src/names.c             valid file/user names, env settings
common/src/auth.c              shared secret, tickets (HMAC), password hashing

name_server/src/nm_main.c      startup, accept loop, thread limit
name_server/src/nm_network.c   reads one request, checks the token, dispatches
name_server/src/nm_handlers.c  every command; registration, heartbeats,
                               replication (sync_replica, choose_replica), EXEC sandbox
name_server/src/nm_metadata.c  file hash table, access checks, save/load JSON
name_server/src/nm_cache.c     LRU cache
name_server/src/nm_logging.c   log file

storage_server/src/ss_main.c          startup, accept loop, heartbeat thread start
storage_server/src/ss_network.c       ticket checks, command dispatch,
                                      register/heartbeat/commit messages to the NM
storage_server/src/ss_handlers.c      READ, STREAM, STAT, UNDO, CREATE, PUT, DELETE
storage_server/src/ss_write_handlers.c  WRITE begin, UPDATE, commit
storage_server/src/ss_session.c       splitting text into sentences and words
storage_server/src/ss_locking.c       sentence locks and per-file locks
storage_server/src/ss_file_ops.c      paths, atomic writes, file list
storage_server/src/ss_utils.c         logging, IP detection

client/src/client_main.c       login prompt and command loop
client/src/client_commands.c   one function per command
client/src/client_network.c    connections, login, nm_call (auto re-login)

tests/test_dnfs.py             integration tests
protocol/                      message formats and flows
```

A good reading order: `client_main.c` → `client_commands.c` (`handle_read`) →
`nm_network.c` → `nm_handlers.c` (`handle_file_operation`) → `ss_network.c`
(`parse_and_handle`) → `ss_handlers.c` (`handle_read`). That's one complete
request. Then read the WRITE path: `ss_write_handlers.c` and `ss_session.c`.

**Locks, in one place.** Name Server: `files_mutex` (file table),
`users_mutex` (users and sessions), `ss_mutex` (storage server list),
`save_mutex` (one save at a time), `replication_mutex` (one copy at a time).
When more than one is needed they're always taken in the order
save → files → users, which prevents deadlock. No lock is held while waiting
on the network. Storage Server: one mutex for the sentence-lock table, and 64
"striped" file mutexes; a file's name picks which one.

---

## 10. Building, testing and debugging

```bash
make all        # build everything
make test       # build, then run tests/test_dnfs.py (about 30 seconds)
make clean
```

The tests start real servers on free ports in a temporary folder, so they don't
touch your files or need port 9000. Each test checks one behaviour: login and
tokens, tickets, bad names, simultaneous commits, CONFLICT, large files,
streaming, EXEC limits, undo, failover, re-registration, saving long access
lists, delete rules, VIEW and INFO rules, and the real client binary.

Useful settings for experiments: `DNFS_NM_PORT`, `DNFS_HEARTBEAT_INTERVAL`,
`DNFS_SS_TIMEOUT`, `DNFS_EXEC_TIMEOUT` (see the README).

To check for races and memory errors, build with a sanitizer and point the
tests at those binaries:

```bash
gcc -D_GNU_SOURCE -Icommon/include -Iname_server/include -g -fsanitize=thread -pthread \
    common/src/*.c name_server/src/*.c -o /tmp/nm-tsan -lcrypto
DNFS_NM_BIN=/tmp/nm-tsan python3 tests/test_dnfs.py
```

(On recent kernels ThreadSanitizer needs address randomisation off for its
process: run the binary through `setarch "$(uname -m)" -R`.)

Logs: `name_server/logs/` and `storage_server/logs/`, one file per start.

---

## 11. Bugs that were fixed, and what they teach

An audit in November 2026 found 34 problems. The fixes were written with an
AI assistant (Claude) and checked with the test suite and with
ThreadSanitizer and AddressSanitizer. The most instructive ones:

- **Two commits at once deleted the file.** Every commit used the same temp
  file name and deleted the real file before renaming. When two people
  committed together, one could delete the file the other had just written; in
  a test, the whole file vanished in about half of 200 tries. *Lesson:* an
  operation of several steps on shared data needs one lock around all of it,
  and "atomic replace" means a unique temp file plus `rename`, with no delete
  first.
- **Anyone could be anyone.** The username was just a field in each request,
  and storage servers checked nothing, so any program could read or delete any
  file. *Lesson:* authorisation is worthless without authentication, and every
  server that holds data must check permission itself (here, via tickets).
- **`../` in a filename escaped the data folder.** *Lesson:* never build a
  path from user input without validating it.
- **A client closing early killed the storage server.** Writing to a closed
  socket raises the signal SIGPIPE, which ends the process by default.
  *Lesson:* network servers should ignore SIGPIPE and handle `EPIPE` errors.
- **Words in a file could stop a stream.** "STOP" in the text was read as the
  end marker. *Lesson:* never mix control signals and user data in the same
  channel without framing.
- **Large files read back empty.** Replies were built in a fixed 4 KB buffer.
  *Lesson:* size buffers from the data, and loop on `recv` until a message is
  complete.
- **The backup was always empty**, so failover served blank files. *Lesson:*
  a replica only helps if every change reaches it.
- **One slow storage server froze the Name Server** because network calls ran
  while holding the global lock. *Lesson:* copy what you need, release the
  lock, then talk to the network.

---

## 12. Glossary

- **Backup / primary / replica**: the two copies of a file; "replica" means either.
- **Commit**: making a WRITE session's edits permanent (`ETIRW`).
- **Failover**: switching to the backup when the primary is down.
- **Framing**: marking where messages start and end in a byte stream.
- **Heartbeat**: a periodic "I'm alive" message.
- **HMAC**: a keyed signature that proves a message came from a key holder.
- **Mutex**: a lock that only one thread can hold at a time.
- **PBKDF2**: a deliberately slow password-hashing function.
- **Race condition**: a bug that depends on the timing of two concurrent actions.
- **Session token**: random string proving you logged in.
- **Snapshot**: the saved previous version used by UNDO.
- **Stale**: a replica that missed a change.
- **Ticket**: the Name Server's signed, short-lived permission for one
  command on one file.

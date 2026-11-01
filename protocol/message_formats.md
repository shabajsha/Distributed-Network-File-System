# Message Formats (JSON over TCP)

Every message is one JSON object on one line, ending with `\n`. Strings are
JSON-escaped, so a message never contains a raw newline. A receiver reads
until `\n`, however many TCP reads that takes.

Every reply has a status:
{ "status": "OK", ... } or { "status": "ERR", "reason": "<CODE>" }

Filenames and usernames: 1–255 (files) or 1–63 (users) characters from
`A–Z a–z 0–9 . _ -`, not starting with `.`.

---

## Client → Name Server

### Log in
{ "cmd": "register_client", "username": "alice", "password": "..." }

→ { "status": "OK", "token": "<64 hex>" }

The first login for a username sets its password. Errors: `BAD_CREDENTIALS`,
`INVALID_USERNAME`, `PASSWORD_REQUIRED`.

### Every other command carries the token
The name server takes the user's identity from the token. Any `username`
field is ignored. A missing or unknown token gives `UNAUTHENTICATED` (the
client then logs in again and retries once).

{ "cmd": "VIEW", "token": "...", "flags": "-al" }
{ "cmd": "LIST", "token": "..." }
{ "cmd": "CREATE", "token": "...", "filename": "notes.txt" }
{ "cmd": "INFO", "token": "...", "filename": "notes.txt" }
{ "cmd": "DELETE", "token": "...", "filename": "notes.txt" }
{ "cmd": "EXEC", "token": "...", "filename": "script.sh" }
{ "cmd": "ADDACCESS", "token": "...", "filename": "notes.txt", "target": "bob", "mode": "W" }
{ "cmd": "REMACCESS", "token": "...", "filename": "notes.txt", "target": "bob" }

### File operations: the name server returns where to go, plus a ticket
{ "cmd": "READ" | "WRITE" | "STREAM" | "UNDO", "token": "...", "filename": "notes.txt" }

→ { "status": "OK", "ss_ip": "127.0.0.1", "ss_port": 9100,
    "user": "alice", "expires": 1793540000, "ticket": "<64 hex>" }

The ticket is HMAC-SHA256(secret, "<cmd>\n<user>\n<filename>\n<expires>") and
is valid for 5 minutes, for that one command and file.

---

## Name Server → Client replies

### VIEW (simple)
{ "status": "OK", "files": ["a.txt", "b.txt"] }

### VIEW (-l details), sorted by name
{ "status": "OK", "files": [
    { "filename": "notes.txt", "owner": "alice", "words": 30, "chars": 200, "bytes": 230,
      "last_accessed": "2026-11-01 18:40" } ] }

### LIST
{ "status": "OK", "users": ["alice", "bob"] }

### INFO (needs read access)
{ "status": "OK", "filename": "notes.txt", "owner": "alice",
  "created_at": "...", "last_modified": "...", "last_accessed": "...", "last_accessed_by": "bob",
  "words": 30, "chars": 200, "bytes": 230,
  "access": [ {"user": "alice", "mode": "RW"}, {"user": "bob", "mode": "R"} ],
  "ss_ip": "127.0.0.1", "ss_port": 9100, "backup_ss_ip": "127.0.0.1", "backup_ss_port": 9101 }

### EXEC
{ "status": "OK", "output": "...", "exit_code": 0, "timed_out": false, "truncated": false }

---

## Client → Storage Server

Every command that names a file carries the ticket fields from the name
server's reply. Without a valid ticket the reply is `UNAUTHORIZED`.

### READ
{ "cmd": "READ", "filename": "notes.txt", "user": "alice", "expires": 1793540000, "ticket": "..." }
→ { "status": "OK", "content": "full text" }

### WRITE session (one connection)
{ "cmd": "WRITE", "filename": "notes.txt", "sentence_index": 0, "user": "...", "expires": ..., "ticket": "..." }
→ { "status": "OK", "msg": "LOCKED" }

{ "cmd": "UPDATE", "word_index": 3, "content": "new wording here" }
{ "cmd": "UPDATE", "word_index": 3, "content": "replacement", "mode": "replace" }
→ { "status": "OK", "msg": "UPDATED" }

{ "cmd": "ETIRW" }
→ { "status": "OK", "msg": "WRITE DONE" } or { "status": "ERR", "reason": "CONFLICT" }

### STREAM
{ "cmd": "STREAM", "filename": "notes.txt", "user": ..., "expires": ..., "ticket": ... }
→ one message per word, about 0.1 s apart:
{ "status": "OK", "word": "Hello" }
...
{ "status": "END" }

### UNDO
{ "cmd": "UNDO", "filename": "notes.txt", "user": ..., "expires": ..., "ticket": ... }
→ { "status": "OK" }   (errors: LOCKED, NO_SNAPSHOT)

---

## Name Server → Storage Server

Signed like client tickets, with user "nm". Only "nm" may use these.

{ "cmd": "CREATE", "filename": "f", "content": "", "user": "nm", "expires": ..., "ticket": ... }
{ "cmd": "DELETE", "filename": "f", ... }               errors: LOCKED, FILE_NOT_FOUND
{ "cmd": "STAT", "filename": "f", ... }                 → { "words", "chars", "bytes" }
{ "cmd": "READ", "filename": "f", "with_snapshot": true, ... }  → also returns "snapshot"
{ "cmd": "PUT", "filename": "f", "content": "...", "snapshot": "...", ... }
  (replaces the file and its undo snapshot; used to copy between replicas)

---

## Storage Server → Name Server

Signed with the shared secret: "auth" is HMAC-SHA256(secret,
"<cmd>\nss:<client_port>\n<filename or empty>\n<expires>").

### Register (on start, and again whenever a heartbeat gets UNKNOWN_SS)
{ "cmd": "register_ss", "ip": "10.0.0.5", "client_port": 9100, "expires": ..., "auth": "...",
  "files": ["file1.txt", "file2.txt"] }

### Heartbeat (every DNFS_HEARTBEAT_INTERVAL seconds)
{ "cmd": "ss_heartbeat", "ip": "10.0.0.5", "client_port": 9100, "expires": ..., "auth": "..." }
→ { "status": "OK" } or { "status": "ERR", "reason": "UNKNOWN_SS" }

### A file changed (after a commit or undo)
{ "cmd": "ss_commit", "ip": "10.0.0.5", "client_port": 9100, "filename": "f", "expires": ..., "auth": "..." }

---

## Error codes

FILE_NOT_FOUND, UNAUTHORIZED, UNAUTHENTICATED, BAD_CREDENTIALS, INVALID_FILENAME,
INVALID_USERNAME, INVALID_FLAGS, BAD_REQUEST, ALREADY_EXISTS, ALREADY_OWNER,
USER_NOT_FOUND, ACCESS_NOT_FOUND, NO_SS_AVAILABLE, ALL_SS_FAILED, SS_UNAVAILABLE,
LOCKED, SENTENCE LOCKED, INVALID_INDEX, NO_ACTIVE_WRITE, CONFLICT, NO_SNAPSHOT,
EMPTY_FILE, EXEC_FAILED, MESSAGE_TOO_LARGE, SERVER_BUSY, UNKNOWN_COMMAND, UNKNOWN

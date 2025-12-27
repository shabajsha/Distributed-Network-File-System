# Message Formats (JSON over TCP)

Every message is a single JSON object sent per write() call.

All responses contain at least:
{ "status": "OK" } or { "status": "ERR", "reason": "<description>" }

---

## Client → Name Server

### Register Client
{
  "cmd": "register_client",
  "username": "alice"
}

---

## Storage Server → Name Server

### Register Storage Server
{
  "cmd": "register_ss",
  "ip": "127.0.0.1",
  "nm_port": 9000,
  "client_port": 9100,
  "files": ["file1.txt", "file2.txt", "dir/file3.txt"]
}

---

## Name Server → Storage Server

### Create File
{
  "cmd": "CREATE",
  "filename": "newfile.txt",
  "owner": "alice"
}

### Delete File
{
  "cmd": "DELETE",
  "filename": "oldfile.txt"
}

---

### VIEW (with flags -a, -l, -al)
{
  "cmd": "VIEW",
  "username": "alice",
  "flags": "-al"
}

### LIST USERS
{
  "cmd": "LIST",
  "username": "alice"
}

### CREATE file
{
  "cmd": "CREATE",
  "username": "alice",
  "filename": "notes.txt"
}

### INFO about a file
{
  "cmd": "INFO",
  "username": "alice",
  "filename": "notes.txt"
}

### ACCESS CONTROL
{
  "cmd": "ADDACCESS",
  "username": "alice",
  "filename": "notes.txt",
  "target": "bob",
  "mode": "W"
}

{
  "cmd": "REMACCESS",
  "username": "alice",
  "filename": "notes.txt",
  "target": "bob"
}

### File operation requests (NM returns SS address)
{
  "cmd": "READ",
  "username": "alice",
  "filename": "notes.txt"
}

{
  "cmd": "WRITE",
  "username": "alice",
  "filename": "notes.txt"
}

{
  "cmd": "STREAM",
  "username": "alice",
  "filename": "notes.txt"
}

{
  "cmd": "UNDO",
  "username": "alice",
  "filename": "notes.txt"
}

---

## Name Server → Client Responses

### VIEW (simple)
{
  "status": "OK",
  "files": ["a.txt", "b.txt", "c.txt"]
}

### VIEW (-l details)
{
  "status": "OK",
  "files": [
    {"filename": "notes.txt", "owner": "alice", "words": 30, "chars": 200}
  ]
}

### CREATE or lookup result
{
  "status": "OK",
  "ss_ip": "127.0.0.1",
  "ss_port": 9100
}

### INFO result
{
  "status": "OK",
  "filename": "notes.txt",
  "owner": "alice",
  "access": {"alice": "RW", "bob": "R"},
  "ss_ip": "127.0.0.1",
  "ss_port": 9100
}

---

## Client → Storage Server

### READ
{
  "cmd": "READ",
  "username": "alice",
  "filename": "notes.txt"
}

### WRITE Begin (sentence lock)
{
  "cmd": "WRITE",
  "username": "alice",
  "filename": "notes.txt",
  "sentence_index": 0
}

### WRITE Update
{
  "cmd": "UPDATE",
  "word_index": 3,
  "content": "new wording here"
}

### WRITE Finish
{ "cmd": "ETIRW" }

### STREAM
{ "cmd": "STREAM", "filename": "notes.txt" }

### UNDO
{ "cmd": "UNDO", "filename": "notes.txt" }

---

## Storage Server → Client Responses

### READ Response
{
  "status": "OK",
  "content": "full text file content here"
}

### WRITE Locked OK
{ "status": "OK", "msg": "LOCKED" }

### WRITE Done
{ "status": "OK", "msg": "WRITE DONE" }

### STREAM sends one word per line then:
"STOP"

### UNDO Done
{ "status": "OK" }

---

## Shared Error Responses
{ "status": "ERR", "reason": "FILE_NOT_FOUND" }
{ "status": "ERR", "reason": "UNAUTHORIZED" }
{ "status": "ERR", "reason": "LOCKED" }
{ "status": "ERR", "reason": "SENTENCE LOCKED" }
{ "status": "ERR", "reason": "INVALID_INDEX" }
{ "status": "ERR", "reason": "ALREADY_EXISTS" }
{ "status": "ERR", "reason": "SS_DOWN" }
{ "status": "ERR", "reason": "UNKNOWN" }

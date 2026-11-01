# Distributed Network File System (DNFS)

A modular distributed file system in C with three services:
- `name_server`: users and logins, metadata, access control, storage server routing and replication
- `storage_server`: file storage, streaming, sentence-level editing, undo
- `client`: interactive CLI for end users

Messages are JSON objects over TCP, one per line. New to the project? Start with
[docs/BEGINNERS_GUIDE.md](docs/BEGINNERS_GUIDE.md).

## Features

- Centralized metadata via the Name Server (`nm`), saved atomically to `name_server/metadata_store.json`
- Password login: the Name Server issues a session token, and every request carries it
- Signed, short-lived tickets: a Storage Server only serves a request the Name Server approved
- One or more Storage Servers (`ss`) that register themselves and send heartbeats
- Each file has a primary and (with 2+ storage servers) a backup copy; every change is copied to the backup, and reads and writes fail over when the primary is down
- Interactive client commands: create/read/write/delete, info/view/list, access control (`ADDACCESS`, `REMACCESS`), stream, undo, and `EXEC`
- Sentence-level write locks, with per-file serialisation of commits
- `EXEC` runs a file as shell commands on the Name Server, in a time- and resource-limited child process
- Logging for the Name Server and Storage Servers
- Integration tests (`make test`)

## Project Structure

```text
.
├── common/              shared by all three programs
│   ├── include/dnfs_shared.h
│   └── src/             auth.c json.c names.c net.c
├── client/
│   ├── include/
│   └── src/
├── name_server/
│   ├── include/
│   └── src/
├── storage_server/
│   ├── include/
│   └── src/
├── docs/
│   └── BEGINNERS_GUIDE.md
├── protocol/
│   ├── message_formats.md
│   ├── network_ports.md
│   └── workflow_diagrams.md
├── tests/
│   └── test_dnfs.py
└── Makefile
```

## Requirements

- Linux / Unix-like OS
- `gcc` and `make`
- POSIX threads (`pthread`)
- OpenSSL development headers (`libssl-dev` on Debian/Ubuntu), for password hashing and ticket signatures
- Python 3, only to run the tests

## Build

```bash
make all
```

Binaries produced:
- `name_server/nm`
- `storage_server/ss`
- `client/client`

Run the tests (builds first, takes about 30 seconds):

```bash
make test
```

Clean build artifacts:

```bash
make clean
```

## Quick Start (Single Machine)

Open **3 terminals** in the project root. Start them in this order.

1. Start the Name Server. On its first start it creates `dnfs.secret`, the key shared with storage servers:
```bash
./name_server/nm
```

2. Start one Storage Server (it reads `dnfs.secret` from the current directory):
```bash
./storage_server/ss 9100 127.0.0.1
```

3. Start the Client:
```bash
./client/client 127.0.0.1
```

Enter a username and a password. The first login for a username sets its password; later logins must use the same one.

## Multi-Storage Setup (Optional)

Run additional storage servers on different ports. The second one becomes the backup for new files:

```bash
./storage_server/ss 9101 127.0.0.1
./storage_server/ss 9102 127.0.0.1
```

For several machines, use the Name Server machine's IP in the client and storage server commands, and give every storage server the same secret: copy `dnfs.secret` next to it, or set `DNFS_SECRET` to the same value (16+ characters) on every machine.

## Client Commands

```text
VIEW [-a] [-l] [-al]     List files (all / detailed)
LIST                      List registered users
CREATE <filename>         Create file
READ <filename>           Read file content
WRITE <filename> <sent#>  Edit sentence interactively
STREAM <filename>         Stream file word-by-word
UNDO <filename>           Undo last change
DELETE <filename>         Delete file
INFO <filename>           Show metadata
EXEC <filename>           Run file content as shell commands on the Name Server
ADDACCESS -R <f> <user>   Grant read access
ADDACCESS -W <f> <user>   Grant write access
REMACCESS <f> <user>      Remove access
help                      Show help
exit / quit               Exit client
```

Filenames and usernames may use letters, digits, `.`, `_` and `-`, and may not start with `.`.

## WRITE Mode

`WRITE` enters an interactive session.

- Update format: `<word_index>[!] <text>`
- Without `!` the text is inserted at that word position; with `!` it replaces the word there
- Finish and commit with:

```text
ETIRW
```

If someone else changed the same sentence while you were editing, the commit is refused with `CONFLICT`; run `WRITE` again on the new text.

## Configuration Notes

Default ports:
- Name Server: `9000`
- First Storage Server: `9100` (or custom from CLI)

Storage server CLI:

```bash
./storage_server/ss [port] [nm_ip] [advertise_ip]
```

Client CLI:

```bash
./client/client [name_server_ip]
```

Environment variables (all optional):

| Variable | Default | Meaning |
|---|---|---|
| `DNFS_SECRET` | contents of `./dnfs.secret` | Shared key between Name Server and Storage Servers |
| `DNFS_NM_PORT` | `9000` | Name Server port, for all three programs |
| `DNFS_HEARTBEAT_INTERVAL` | `5` | Seconds between storage server heartbeats |
| `DNFS_SS_TIMEOUT` | `15` | Seconds without a heartbeat before the Name Server treats a storage server as down |
| `DNFS_EXEC_TIMEOUT` | `10` | Seconds an `EXEC` command may run |

## Protocol

Message formats and flow are documented in:
- `protocol/message_formats.md`
- `protocol/network_ports.md`
- `protocol/workflow_diagrams.md`

## Troubleshooting

- `NO_SS_AVAILABLE`: start at least one storage server, and wait a few seconds for it to register
- `SS_UNAVAILABLE`: the storage servers holding this file are down
- `UNAUTHORIZED`: access denied (owner or granted mode required)
- `BAD_CREDENTIALS`: wrong password for that username
- `CONFLICT`: the sentence changed while you were editing it; start the `WRITE` again
- `LOCKED`: someone is editing the file; `UNDO` and `DELETE` wait until they finish
- Storage server exits with "No cluster secret found": start the Name Server first from the same directory, or copy `dnfs.secret` / set `DNFS_SECRET`
- Client cannot connect to NM:
  - verify the Name Server is running on `9000`
  - verify the IP passed to the client is reachable

## Security Notes

- Passwords are stored as PBKDF2-SHA256 hashes. Traffic is not encrypted, so tokens and passwords can be read by anyone who can capture network traffic between the machines.
- `EXEC` runs commands as the user running the Name Server. It is limited (time, CPU, memory, output, empty working directory) but not isolated; run the Name Server as an unprivileged user.

## License

See [LICENSE](./LICENSE).

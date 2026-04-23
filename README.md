# Distributed Network File System (DNFS)

A modular distributed file system in C with three services:
- `name_server`: metadata, access control, storage server routing
- `storage_server`: file storage, streaming, editing, undo, execution
- `client`: interactive CLI for end users

This project uses JSON messages over TCP sockets and supports multi-user file workflows with read/write permissions.

## Features

- Centralized metadata via Name Server (`nm`)
- One or more Storage Servers (`ss`) registering dynamically
- Interactive Client CLI (`client`) with commands for:
  - create/read/write/delete
  - info/view/list
  - access control (`ADDACCESS`, `REMACCESS`)
  - stream and undo
  - execute file content as shell commands (`EXEC`)
- Basic failover support using backup storage mapping
- Persistent metadata storage (`name_server/metadata_store.json`)
- Logging for Name Server and Storage Server

## Project Structure

```text
.
├── client/
│   ├── include/
│   └── src/
├── name_server/
│   ├── include/
│   ├── src/
│   └── metadata_store.json
├── storage_server/
│   ├── include/
│   └── src/
├── protocol/
│   ├── message_formats.md
│   ├── network_ports.md
│   └── workflow_diagrams.md
└── Makefile
```

## Requirements

- Linux / Unix-like OS
- `gcc`
- `make`
- POSIX threads (`pthread`, usually available by default)

## Build

```bash
make all
```

Binaries produced:
- `name_server/nm`
- `storage_server/ss`
- `client/client`

Clean build artifacts:

```bash
make clean
```

## Quick Start (Single Machine)

Open **3 terminals** from project root.

1. Start Name Server:
```bash
./name_server/nm
```

2. Start one Storage Server:
```bash
./storage_server/ss 9100 127.0.0.1
```

3. Start Client:
```bash
./client/client 127.0.0.1
```

Enter a username when prompted.

## Multi-Storage Setup (Optional)

Run additional storage servers on different ports:

```bash
./storage_server/ss 9101 127.0.0.1
./storage_server/ss 9102 127.0.0.1
```

For multi-machine setups, use the Name Server machine IP in client and storage server startup commands.

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
EXEC <filename>           Execute file content as shell command(s)
ADDACCESS -R <f> <user>   Grant read access
ADDACCESS -W <f> <user>   Grant write access
REMACCESS <f> <user>      Remove access
help                      Show help
exit / quit               Exit client
```

## WRITE Mode

`WRITE` enters an interactive session.

- Update format: `<word_index>[!] <text>`
- Use `!` to replace at index instead of insertion behavior
- Finish and commit with:

```text
ETIRW
```

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

## Protocol

Message formats and flow are documented in:
- `protocol/message_formats.md`
- `protocol/network_ports.md`
- `protocol/workflow_diagrams.md`

## Troubleshooting

- `NO_SS_AVAILABLE`: start at least one storage server before client operations
- `ALL_SS_DOWN`: registered storage servers are unreachable
- `UNAUTHORIZED`: access denied (owner or granted mode required)
- Client cannot connect to NM:
  - verify Name Server is running on `9000`
  - verify IP passed to client is reachable
- Storage server registration issues:
  - ensure NM IP is correct
  - ensure storage server port is open and not already in use

## Development

Useful make targets:

```bash
make help
make client
make nm
make ss
```

## License

See [LICENSE](./LICENSE).

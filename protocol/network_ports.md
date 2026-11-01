Name Server:
IP: 127.0.0.1 (or the machine's LAN address)
PORT: 9000 (override with DNFS_NM_PORT, for all three programs)

Each Storage Server selects its own client-facing port:
Example:
SS1 → 9100
SS2 → 9101
SS3 → 9102

Who connects to whom:
Client → Name Server :9000      every command (one connection per command)
Client → Storage Server :91xx   READ, WRITE, STREAM, UNDO
Name Server → Storage Server    CREATE, DELETE, STAT, READ, PUT
Storage Server → Name Server    register_ss, ss_heartbeat, ss_commit

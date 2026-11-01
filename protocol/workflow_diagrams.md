LOGIN:
Client → NM: register_client (username, password)
NM checks the password hash (or sets it on first login) → returns a session token
Every later request to the NM carries the token

READ / STREAM / WRITE / UNDO:
Client → NM: (cmd, filename, token)
NM checks access, picks a live, up-to-date replica → ss_ip, ss_port, signed ticket
Client → SS: cmd + ticket
SS verifies the ticket, then serves the request

WRITE Operation:
Client → SS: WRITE begin with ticket (locks one sentence)
Client → SS: UPDATE many times (edits an in-memory copy)
Client → SS: ETIRW
SS, holding the file's lock: reload file, find the original sentence,
    refuse with CONFLICT if it changed, splice in the edit,
    save the replaced version as the undo snapshot, write file atomically
SS → Client: WRITE DONE
SS → NM: ss_commit → NM copies the file to the other replica

STREAM:
SS sends {"word": ...} messages about 0.1 s apart, then {"status":"END"}

UNDO:
SS restores the snapshot taken before the last change, deletes the snapshot,
then reports ss_commit like a write

STORAGE SERVER LIFECYCLE:
SS starts → retries register_ss until the NM answers (sends its file list)
SS → NM: ss_heartbeat every few seconds
NM treats an SS as down after DNFS_SS_TIMEOUT seconds without a heartbeat
If the NM restarted, the heartbeat gets UNKNOWN_SS and the SS registers again
On (re-)registration the NM marks missing files stale and copies them back
from the other replica

FAILOVER:
Primary down → NM routes to the backup (which has every committed change)
Writes made there mark the primary stale
Primary returns → NM copies the newer file back before routing to it again

WRITE Operation:
Client → NM: ("WRITE", filename)
NM → Client: ss_ip, ss_port
Client → SS: WRITE begin (gets lock)
Client → SS: UPDATE many times
Client → SS: ETIRW commit
SS applies changes → returns DONE

READ Operation:
Client → NM → ss_ip/ss_port
Client → SS → returns file content

STREAM:
Client → NM → SS info
Client → SS
SS sends words one at a time with delay
Ends with "STOP"

UNDO:
Client → NM → SS info
Client → SS (UNDO)
SS restores from snapshot

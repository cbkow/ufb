`netauth_parked.txt` — `sample <NetAuthSysAgent pid> 0.2 -mayDie` taken
2026-11-06 on a daemon with one thread parked in the SMB plugin's LSARPC
account lookup (`smb_mount → setNetworkAccountSID → GetNetworkAccountSID
→ GetAccountName → LsarGetUserName → DCERPC cond_wait`). The parser's
ground truth.

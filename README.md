# slowkicker for fluffer

This is Biohazard's SlowKicker v0.3, a glftpd speedkicker for slow uploads,
with the changes it needs to work against fluffer. The patched source is
`slowkicker.cpp`. The same changes are in `patches/fluffer.patch`, which
applies to the original v0.3 source.

It is an example. Use it as it is, or use the guide below to port your
own kicker.

## Build and run

```sh
make
install -m755 slowkicker /usr/local/bin/
slowkicker          # forks into the background, checks once a second
```

No glftpd sources are needed. The `ONLINE` struct is embedded.

Edit the constants at the top of `slowkicker.cpp` before building:

| Constant | Meaning |
|---|---|
| `GLFTPD_ROOT` | the fluffer chroot (`rootpath`) |
| `IPC_KEY` | must equal `ipc_key` in fluffer.conf (default `0x0000DEAD`) |
| `KICK_SIGNAL` | `SIGRTMIN` aborts the transfer and keeps the session; `SIGRTMIN+1` also closes the session |
| `DIRECTORIES` | path mask, minimum speed (kB/s), minimum duration (s), max kicks per user and file |

Run it as root or as the daemon user. Only those can signal the daemon.

## How fluffer differs from glftpd

glftpd runs one process per session. Its `ONLINE[i].procid` is that
process's PID, so a kicker could `kill(procid, SIGTERM)` and the session
was gone.

fluffer is a single multi-threaded process:

- Every transfer runs on its own thread. While a transfer is active, the
  slot's `procid` holds that thread's Linux TID, not a PID.
- Between transfers, `procid` holds the daemon's internal connection
  number. `SITE KICK` and `SITE SWHO` use it. It is a small integer and
  never a thread you can signal.
- The daemon's PID is the creator PID of the SHM segment. Read it with
  `shmctl(shmid, IPC_STAT, &ds)`; it is `ds.shm_cpid`. The pidfile holds
  the same value.
- The daemon removes the segment and creates a new one on every start.
  If you get a new shmid, or `IPC_STAT` fails on the one you cached, the
  daemon has restarted. Attach again and read `shm_cpid` again.

Kicks use two realtime signals, sent to one thread with `tgkill(2)`:

| Signal | Effect |
|---|---|
| `SIGRTMIN` | abort the current transfer; the session stays up |
| `SIGRTMIN+1` | abort the transfer, then close the session |

The handler only accepts thread-directed signals (`si_code == SI_TKILL`).
A plain `kill(pid, SIGRTMIN)` is ignored. glibc has `SIGRTMIN == 32`, so
build against glibc.

When an upload is aborted, the daemon runs `post_check` with `$4 = 1`
("aborted"). The exit code decides what happens:

| post_check exit | File | Client sees |
|---|---|---|
| 0 | kept and credited (the kick raced a completed upload) | `226` |
| 1 | kept, not credited | `452 Transfer terminated by external program.` |
| 2 or higher | deleted | `452` |
| no post_check configured | partial file kept, not credited | `452` |

A minimal post_check that deletes aborted partials:

```sh
#!/bin/sh
case "$4" in
    1|2|3) exit 2 ;;   # aborted / error / disconnect: delete
    *)     exit 0 ;;
esac
```

```
post_check /bin/post-validate.sh *
```

## Porting your own kicker: the checklist

1. **Never `kill(procid, ...)`.** `procid` is a thread of the daemon, so
   `kill()` delivers to the whole process. `SIGTERM` there shuts the
   daemon down for every user. Use
   `syscall(SYS_tgkill, shm_cpid, procid, SIGRTMIN)`.
2. **Check liveness with `tgkill(tgid, procid, 0)`**, not `kill(procid, 0)`.
   Between transfers `procid` is a small connection number, and
   `kill(5, 0)` can succeed against an unrelated process.
3. **Get the daemon PID from `shm_cpid`**, and read it again whenever the
   shmid changes (see above).
4. **Test whether a slot is really transferring.** It is only
   transferring when `status` is `STOR <file>` (or `APPE`/`RETR`) *and*
   `currentdir` ends with `/<file>`. glftpd leaves the last command in
   `status` until the client sends another one. The currentdir check
   rules that out on both daemons.
5. **Take the source IP from `host`, not `/proc/<pid>/fd`.** fluffer has
   one shared fd table, so procfs cannot tell which socket belongs to
   which transfer. While a transfer is active, `host` holds the bare
   data-peer IP (no `@`), which is the real source, FXP included. Until
   the data connection is established it still shows the login form
   `ident@ip`. Treat that as "not known yet". The bare form is
   guaranteed once `bytes_xfer != 0`.
6. **Don't treat the execute bit as "upload in progress".** glftpd
   creates uploads `0755` and chmods them to `0644` when they finish.
   fluffer creates them `0644`. A kicker that tests `access(X_OK)` never
   kicks anything on fluffer.
7. **Don't delete the file yourself.** The transfer is still unwinding
   when your signal returns. `post_check` may keep the file and credit
   it (exit 0), and the client may already be re-uploading under the
   same name. Let the post_check verdict handle deletion.
8. **No `/bin/undupe`.** The daemon records no dupe entry for an
   unfinished upload. If your zipscript records dupes early, its
   post_check is the place to undo that.
9. **Use the 904-byte glftpd 2.x `ONLINE` layout**: 32-bit timevals,
   `#pragma pack(4)`. glftpd's `glconf.h` layout matches. Assert
   `sizeof(ONLINE) == 904`.
10. **Measure speed over time, not from one read.** `tstart` is stamped
    once the data channel is fully up (TCP and TLS), so
    `bytes_xfer / (now - tstart)` excludes connection setup. Still,
    require a minimum duration or several consecutive slow samples, and
    allow a couple of seconds of grace while `bytes_xfer == 0`. Kicking
    a transfer in its first second causes client retry storms that cost
    more than the slow upload.
11. **Treat a kick as best-effort.** A TID can be reused between reading
    the slot and sending the signal, just as glftpd PIDs could.
    Re-checking right before the kick keeps that window small.
    `ESRCH` means the transfer already ended; ignore it.

## What the patch changes

- Embeds the `ONLINE` struct and drops the dependency on glftpd sources.
- Attaches to SHM once, reads the daemon PID from `shm_cpid`, and
  attaches again when the daemon recreates the segment.
- `kill()` becomes `tgkill()`, and the kick signal is configurable
  (`SIGTERM` becomes `SIGRTMIN`).
- The active-transfer test becomes status plus currentdir. The
  execute-bit test is removed.
- The source IP comes from `host` instead of `/proc/<pid>/fd` and
  `/proc/net/tcp`, so IPv6 works too.
- Removes the `unlink()` and the `/bin/undupe` call. `post_check` now
  decides what happens to the file.

## License

MIT, as the original. See the header of `slowkicker.cpp`.

# macOS process-exit hang & automatic cleanup

> Applies to: d9mt running under CrossOver / Wine on macOS (Apple Silicon),
> e.g. vanilla WoW 1.12 via WoWSilicon. Tested on macOS 15 / Apple M3.

## TL;DR

On quit the game process (`WoW.exe`) can wedge forever and has to be
`SIGKILL`ed by hand. The hang is **not** inside d9mt — it is the macOS
app-termination handshake (`-[NSApplication _shouldTerminate]`) in
`winemac.drv` interlocking with d9mt's background completion-watcher thread.

Two things help:

1. **In-source (partial):** stop the completion-watcher thread at backend
   teardown, before `wsi::quit()`. Removes d9mt's half of the interlock but
   does **not** fully fix the hang — the winemac/Metal teardown can still wedge
   on its own.
2. **External (the practical fix):** a tiny user-level launch agent that
   detects "a real render session ended but the process is still alive" and
   `SIGKILL`s the husk a few seconds after you quit.

The whole trick to (2) is **picking the right log signal** — see
[The essential signal](#the-essential-signal).

---

## Symptom

- You click Quit / press ⌘Q. The window may linger; the process never exits.
- `ps` shows `WoW.exe` (and its `/bin/sh` wrapper) alive at ~0% CPU.
- Only `kill -9` clears it.

## Diagnosis

Sample the stuck process:

```sh
sample <wow.exe pid> 3
```

The main thread is parked in the Cocoa termination handshake:

```
-[NSApplication _shouldTerminate]
  -> __CFRunLoopServiceMachPort -> mach_msg ...   (blocked)
```

Meanwhile d9mt's `d9mt-watcher` thread (the single background thread that
waits on submitted `MTLCommandBuffer`s and runs the completion callbacks) is
parked in a Wine syscall. The two interlock: the handshake waits for the Wine
threads to quiesce, the leaked watcher thread never does. The render side has
already torn down cleanly by this point — `d3d9fe.log` shows the full
`DxvkContext / Presenter / DxvkDevice / DxvkInstance: destroyed` sequence
*before* the hang.

## In-source mitigation (partial)

Stop the watcher thread during backend teardown, at the latest reachable point
before the Cocoa handshake — `DxvkInstance::~DxvkInstance`, right before
`wsi::quit()`:

```cpp
DxvkInstance::~DxvkInstance() {
  d9mt::logf("DxvkInstance: destroyed");
  d9mt::watcherStop();   // drain + make the watcher's run() return (never joined)
  wsi::quit();
}
```

`watcherStop()` must be a **no-op if the watcher was never created**, otherwise
it breaks startup — see the warning below.

> ### ⚠️ Startup-probe pitfall
> WoW creates a **throwaway `IDirect3D9` instance at startup just to enumerate
> adapters**, then releases it before creating the real device. That probe
> instance runs `DxvkInstance::~DxvkInstance` too. If teardown there *creates*
> the watcher singleton (e.g. by calling `watcherWaitIdle()`) and then stops
> it, the singleton's thread is dead by the time the real render instance
> needs it → command buffers never retire → **the game hangs at startup**.
>
> Fix: gate everything on whether the watcher actually exists
> (`g_watcher != nullptr`, i.e. real rendering happened), and never force it
> into existence from a destructor.

An earlier attempt called `watcherStop()` from `DllMain(DLL_PROCESS_DETACH)`.
That never runs once teardown is already wedged, so it does nothing — the
destructor is the right hook.

Even with the watcher stopped, the winemac/Metal `_shouldTerminate` teardown
can still hang by itself. Hence the external cleanup below.

## External cleanup daemon (the practical fix)

A user `LaunchAgent` polls for the exit-hang and kills the husk. The agent is
harmless during play and during startup — it only acts when a **real render
session has ended but the process is still alive**.

### The essential signal

The naive signal is "`d3d9fe.log` ends with `DxvkInstance: destroyed`". **It is
wrong** and will kill the game *at startup*: the startup adapter-probe instance
(above) logs exactly that, while the real instance is still coming up.

The robust signal is the **device** lifecycle, not the instance:

```
DxvkDevice: created      <- only ever logged for a real rendering session
DxvkDevice: destroyed    <- ... and only when that session is torn down
```

The probe instance creates a `DxvkInstance` but **never a `DxvkDevice` or
`Presenter`** (it only enumerates adapters). So:

| State            | last `DxvkDevice:` event | kill? |
|------------------|--------------------------|-------|
| startup probe    | *(none — never created)* | no    |
| in game          | `created`                | no    |
| real quit (hung) | `destroyed`              | **yes** |

Discriminator: **process alive** AND **the last `DxvkDevice: (created|destroyed)`
line is `destroyed`** AND **the log hasn't grown over a short window** (belt &
suspenders against device re-create / slow startup).

### The agent script

`~/.octowow-exit-cleanup.sh`:

```sh
#!/bin/zsh
LOG="/path/to/OctoWoW-Clean/d3d9fe.log"   # d9mt's log
KILLPAT='OctoWoW-Clean'                    # uniquely matches the game process tree
MLOG=/tmp/octowow-exit-cleanup.log
WINDOW=4
log(){ echo "$(date '+%m-%d %H:%M:%S') $1" >> "$MLOG" }

# real render session ended = last DxvkDevice lifecycle event is "destroyed"
session_ended() {
  local last
  last=$(grep -aE 'DxvkDevice: (created|destroyed)' "$LOG" 2>/dev/null | tail -1)
  [[ "$last" == *destroyed* ]]
}
loglines() { wc -l < "$LOG" 2>/dev/null | tr -d ' '; }

while true; do
  if pgrep -f "$KILLPAT" >/dev/null 2>&1 && session_ended; then
    n1=$(loglines); sleep "$WINDOW"; n2=$(loglines)
    if pgrep -f "$KILLPAT" >/dev/null 2>&1 && session_ended && [[ "$n1" == "$n2" ]]; then
      pkill -9 -f "$KILLPAT"
      log "==> KILLED exit-hung tree (DxvkDevice destroyed, log static at $n2)"
      sleep 5
    fi
  fi
  sleep 3
done
```

### The launch agent

`~/Library/LaunchAgents/com.octowow.exitcleanup.plist`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key>            <string>com.octowow.exitcleanup</string>
  <key>ProgramArguments</key> <array>
    <string>/bin/zsh</string>
    <string>/Users/YOU/.octowow-exit-cleanup.sh</string>
  </array>
  <key>RunAtLoad</key>        <true/>
  <key>KeepAlive</key>        <true/>
</dict></plist>
```

### Enable / disable

```sh
launchctl load   ~/Library/LaunchAgents/com.octowow.exitcleanup.plist   # enable
launchctl unload ~/Library/LaunchAgents/com.octowow.exitcleanup.plist   # disable
tail -f /tmp/octowow-exit-cleanup.log                                   # watch
```

After you quit, the husk is reaped within `WINDOW`–`WINDOW+3` s — effectively a
clean exit, no manual force-quit.

## Why not just fix it in d9mt?

The terminal hang is below d9mt: it is in `winemac.drv`'s
`-[NSApplication _shouldTerminate]` path and Apple's Metal teardown, reached
*after* d9mt has fully released its objects. d9mt can (and does) tear its own
watcher thread down early, but it cannot drive the host app-termination
handshake. Until that is addressed in winemac/Metal, the external reaper is the
reliable option, and the `DxvkDevice: destroyed` signal makes it precise.

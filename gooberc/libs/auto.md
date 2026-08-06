# goober.auto

Lightweight automation helpers for timed scripts and future loops.

## use

```gooberc
use goober.auto
```

## Operations (v0 source)

- `sleep N` — pause for approximately `N` milliseconds (stub: yields desktop frames)
- `print "..."` — console write (shared with `goober.console`)
- `exit` — terminate the app

## Notes for agents

Real scheduling, event loops, and rich automation DSL are a follow-up milestone.
Keep scripts short; long sleeps are clamped by the runtime stub.

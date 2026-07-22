# goober.console

Console I/O for text / legacy-style Goober apps.

## use

```gooberc
use goober.console
```

## Operations (v0 source)

- `print "..."` — write a string to the system console
- `exit` — terminate the app

## Notes for agents

Prefer this module for non-GUI tools and install helpers. Do not call kernel
C APIs directly; stay within GooberC forms documented here.

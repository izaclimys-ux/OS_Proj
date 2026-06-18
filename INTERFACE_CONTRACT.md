# kbmonitor — Interface Contract Between Members

This document is the single source of truth for shared variable names and
function signatures. If you rename anything listed here, update this file
and tell the group.

---

## Member 1 → provides (Character Device Architect)

| Symbol | Type | Description |
|--------|------|-------------|
| `device_open` | `int (*)(struct inode *, struct file *)` | `.open` in `file_operations` |
| `device_release` | `int (*)(struct inode *, struct file *)` | `.release` in `file_operations` |
| `fops` | `struct file_operations` | Must include `.read = device_read` |
| Device name | string `"kbmonitor"` | Used by `device_create`; creates `/dev/kbmonitor` |

---

## Member 2 → provides (Kernel-to-User Communication)

| Symbol | Type | Description |
|--------|------|-------------|
| `device_read` | `ssize_t (*)(struct file *, char __user *, size_t, loff_t *)` | `.read` in `file_operations` |

**Member 2 requires from Member 3:**

| Symbol | Type | Must be defined as |
|--------|------|--------------------|
| `keystroke_count` | `atomic_t` | `atomic_t keystroke_count = ATOMIC_INIT(0);` |
| `key_press_count` | `atomic_t` | `atomic_t key_press_count = ATOMIC_INIT(0);` |

---

## Member 3 → provides (Interrupt & Logic Specialist)

| Symbol | Type | Description |
|--------|------|-------------|
| `keystroke_count` | `atomic_t` | Total key events (presses + releases) |
| `key_press_count` | `atomic_t` | Key-down events only |
| `kb_notifier_block` | `struct notifier_block` | Registered with `register_keyboard_notifier()` |

---

## Member 4 → assembles final kbmonitor.c

Merge order in the final file:
1. All `#include` directives
2. `MODULE_*` macros
3. `#define` constants
4. Shared variable **definitions** (Member 3's `atomic_t` lines)
5. Member 3's notifier callback
6. Member 2's `device_read` function
7. Member 1's `device_open` and `device_release` functions
8. `file_operations` struct (Member 1 assembles, includes `.read = device_read`)
9. `module_init` / `module_exit` (Member 1)

---

## Output format of /dev/kbmonitor (defined by Member 2)

```
=== /dev/kbmonitor statistics ===
Total key events  : <N>
Key press events  : <N>
Key release events: <N>
=================================
```

Member 4's user_app.c can parse this with `sscanf` or display it raw with `printf`.

# Boxbridge

**Run x86 programs on an ARM device without thinking about it.**

Boxbridge quietly redirects any x86 or x86_64 program your system tries to launch through [`box64`](https://github.com/ptitSeb/box64), so everything just works. No root, no kernel changes.

---

## What it does

When a program starts another program, Boxbridge checks whether the new one is an x86 binary. If it is, Boxbridge swaps in `box64` to run it:

```
launch: ./some-x86-app --flag
runs:   box64 ./some-x86-app --flag
```

Native ARM programs are left completely alone.

## Why it exists

Some software, Steam being the classic example, ships x86 binaries and launches many helper programs of its own. Running the main program under `box64` isn't enough, because every helper it starts would also need `box64`. Boxbridge handles that automatically, at the moment each program is launched.

The usual alternative, `binfmt_misc`, needs root and kernel support. Android and containers rarely offer either. Boxbridge works entirely in userspace, using the same trick Termux-exec uses to get around Android's sandbox.

## How it works

Boxbridge is a small library loaded with `LD_PRELOAD`. It sits in front of the system's "launch a program" functions, looks at the target file, and rewrites the command when needed.

**Covered:** `execve`, `execv`, `execvp`, `execvpe`, `posix_spawn`, `posix_spawnp`, `fexecve`

**Not covered:**
- Programs that bypass the standard library and call the kernel directly (rare, but some runtimes do it).
- Apps that rewrite `/proc/self/exe` themselves.
- Programs that load an x86 library by hand instead of launching a process. That is a different mechanism and out of scope here.

## Usage

Build it:

```sh
make
```

Load it into the program that starts other programs:

```sh
export LD_PRELOAD=/path/to/libboxbridge.so
steam
```

### Example: Steam

```sh
apt install box64
cp libboxbridge.so /usr/local/lib/
LD_PRELOAD=/usr/local/lib/libboxbridge.so BOXBRIDGE_VERBOSE=1 \
    /root/steam-extract/usr/bin/steam
```

Every process Steam launches (web helper, launcher, runtime) passes through Boxbridge. x86_64 binaries run under `box64`; native ARM64 ones run directly.

## Settings

| Variable | Effect |
|---|---|
| `BOXBRIDGE_OFF=1` | Turn interception off without unloading the library. |
| `BOXBRIDGE_BOX64=PATH` | Where `box64` lives. Default: `/usr/bin/box64`. |
| `BOXBRIDGE_VERBOSE=1` | Log every x86 program that gets redirected. |
| `BOXBRIDGE_TRACE=1` | Also log launches that were left alone. |

## License

MIT

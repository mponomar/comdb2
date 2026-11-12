#!/usr/bin/env python3
"""Low-perturbation wall-clock stack sampler for comdb2 running as PID 1 in a container.

usage: gsample.py <container> <outdir> <nsamples> <interval_secs> [start_delay_secs]

Keeps one gdb session attached (symbols are read once), lets the process run,
and every <interval> interrupts it just long enough to dump all thread stacks.
Writes <outdir>/stack.N.txt (same format as `thread apply all bt`) and prints
how long the process was stopped per sample.
"""
import os, select, subprocess, sys, time

container, outdir, nsamples, interval = sys.argv[1], sys.argv[2], int(sys.argv[3]), float(sys.argv[4])
delay = float(sys.argv[5]) if len(sys.argv) > 5 else 0.0
os.makedirs(outdir, exist_ok=True)
p = subprocess.Popen(["docker", "exec", "-i", container, "gdb", "-q", "-nx", "-p", "1"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
os.set_blocking(p.stdout.fileno(), False)
buf = b""


def send(cmd):
    p.stdin.write((cmd + "\n").encode())
    p.stdin.flush()


def read_until(marker, timeout=60.0):
    """Return everything read up to and including marker (bytes)."""
    global buf
    end = time.time() + timeout
    m = marker.encode()
    while m not in buf:
        left = end - time.time()
        if left <= 0:
            raise TimeoutError("timeout waiting for %r; tail=%r" % (marker, buf[-300:]))
        r, _, _ = select.select([p.stdout], [], [], left)
        if r:
            chunk = p.stdout.read()
            if chunk:
                buf += chunk
    i = buf.index(m) + len(m)
    out, buf = buf[:i], buf[i:]
    return out


def sync(tag):
    send("echo %s\\n" % tag)
    return read_until(tag)


for c in ("set pagination off", "set confirm off", "set print thread-events off", "set width 0",
          "handle SIGPIPE nostop noprint pass", "handle SIGUSR1 nostop noprint pass",
          "handle SIGUSR2 nostop noprint pass", "handle SIGALRM nostop noprint pass",
          "handle SIGHUP nostop noprint pass", "handle SIGTERM nostop noprint pass"):
    send(c)
t0 = time.time()
sync("ATTACHED")  # symbols loaded, process currently stopped
print("attach+symbols: %.1fs (process stopped)" % (time.time() - t0), flush=True)
send("continue &")
sync("RUNNING")
print("READY", flush=True)
time.sleep(delay)
stopped = []
for i in range(1, nsamples + 1):
    time.sleep(interval)
    t1 = time.time()
    send("interrupt")
    read_until("SIGINT")
    sync("STOPPED%d" % i)
    send("thread apply all bt 24")
    out = sync("ENDSAMPLE%d" % i)
    send("continue &")
    sync("RESUMED%d" % i)
    stopped.append(time.time() - t1)
    with open("%s/stack.%d.txt" % (outdir, i), "wb") as f:
        f.write(out)
send("interrupt")
read_until("SIGINT")
send("detach")
send("quit")
try:
    p.wait(timeout=20)
except subprocess.TimeoutExpired:
    p.kill()
print("samples=%d stopped per sample: avg %.0f ms max %.0f ms" %
      (len(stopped), 1000 * sum(stopped) / len(stopped), 1000 * max(stopped)), flush=True)

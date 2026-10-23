#!/usr/bin/env python3
"""
test-templates.py — Unit tests for the cron job templates.

Usage: python3 tests/test-templates.py

Renders the Salt cron templates with Jinja, applies cron's own handling of
the command field (an unescaped % ends the command, \\% is a literal %), runs
the result through /bin/sh and checks the arguments the job script would
receive. Needs no root, Salt or openmediavault install.
"""
import os
import subprocess
import sys

import jinja2

HERE = os.path.dirname(os.path.abspath(__file__))
TPL_DIR = os.path.join(HERE, "..", "srv", "salt", "omv", "deploy",
                       "bcachesfcron", "files")

passed = failed = 0


def check(desc, expected, actual):
    global passed, failed
    if expected == actual:
        print(f"  PASS  {desc}")
        passed += 1
    else:
        print(f"  FAIL  {desc}")
        print(f"        expected: {expected!r}")
        print(f"        actual:   {actual!r}")
        failed += 1


def to_bool(value):
    if isinstance(value, str):
        return value.lower() in ("1", "true", "yes", "on", "y")
    return bool(value)


def shell_quote(value):
    # Salt's "quote" filter (shlex.quote).
    import shlex
    return shlex.quote(str(value))


env = jinja2.Environment(loader=jinja2.FileSystemLoader(TPL_DIR))
env.filters["to_bool"] = to_bool
env.filters["quote"] = shell_quote


def render(template, jobs):
    return env.get_template(template).render(jobs=jobs)


def job_lines(text):
    return [l for l in text.splitlines()
            if l and not l.startswith("#") and "=" not in l.split()[0]]


def cron_command(line):
    """Return the command a crontab line runs, the way cron reads it."""
    fields = line.split(None, 6 if not line.startswith("@") else 2)
    cmd = fields[-1]
    out, i = "", 0
    while i < len(cmd):
        if cmd[i] == "\\" and i + 1 < len(cmd) and cmd[i + 1] == "%":
            out += "%"
            i += 2
        elif cmd[i] == "%":
            break  # the rest would be fed to the command's stdin
        else:
            out += cmd[i]
            i += 1
    return out


def argv_of(command, script):
    """Run the command with the job script swapped for an argv printer."""
    assert command.startswith(script + " "), command
    command = command.replace(" >/dev/null 2>&1", "")
    printer = "_argv() { for a in \"$@\"; do printf '%s\\0' \"$a\"; done; }; _argv"
    out = subprocess.run(["/bin/sh", "-c", printer + command[len(script):]],
                         capture_output=True, check=True).stdout
    return out.decode().split("\0")[:-1]


BASE = {
    "enable": True, "execution": "daily",
    "minute": "0", "hour": "2", "dayofmonth": "*", "month": "*",
    "dayofweek": "*", "everynminute": False, "everynhour": False,
    "everyndayofmonth": False,
}

print("=== scrub cron ===")

tricky = "100% 'checked' $(touch /tmp/x) \"q\""
lines = job_lines(render("etc_cron.d_omv-bcachefs-scrub.j2", [
    dict(BASE, fsuuid="5e9e2f5c-7f0b-4a7e-9d4a-0e1b2c3d4e5f",
         sendemail=True, emailonerror=True, comment=tricky),
    dict(BASE, fsuuid="11111111-2222-3333-4444-555555555555",
         sendemail=False, emailonerror=False, comment="", enable=False),
]))
check("only enabled jobs are rendered", 1, len(lines))
check("% in the comment is escaped for cron", True, "\\%" in lines[0])
argv = argv_of(cron_command(lines[0]), "/usr/sbin/omv-bcachefs-scrub")
check("comment with %, quotes and $() reaches the script intact",
      ["5e9e2f5c-7f0b-4a7e-9d4a-0e1b2c3d4e5f", "-e", "onerror", "-c", tricky],
      argv)

lines = job_lines(render("etc_cron.d_omv-bcachefs-scrub.j2", [
    dict(BASE, execution="exactly", minute="15", everynhour=True, hour="6",
         fsuuid="5e9e2f5c-7f0b-4a7e-9d4a-0e1b2c3d4e5f",
         sendemail=False, emailonerror=False, comment="ignored"),
]))
check("exact schedule fields", "15 */6 * * *", " ".join(lines[0].split()[:5]))
check("no email options without sendemail",
      ["5e9e2f5c-7f0b-4a7e-9d4a-0e1b2c3d4e5f"],
      argv_of(cron_command(lines[0]), "/usr/sbin/omv-bcachefs-scrub"))

print("\n=== snapshot cron ===")

lines = job_lines(render("etc_cron.d_omv-bcachefs-snapshots.j2", [
    dict(BASE, subvolume="/srv/dev-disk-by-uuid-x/My Data 50%",
         prefix="daily", retention=7, retentionunit="days", readonly=True,
         sendemail=True, emailonerror=False, comment="it's 5% full"),
]))
check("one job rendered", 1, len(lines))
check("subvolume path and comment with % and ' survive cron and sh",
      ["/srv/dev-disk-by-uuid-x/My Data 50%", "daily", "7", "-u", "days",
       "-r", "-e", "always", "-c", "it's 5% full"],
      argv_of(cron_command(lines[0]), "/usr/sbin/omv-bcachefs-snapshot"))

print(f"\n{passed} passed, {failed} failed")
sys.exit(0 if failed == 0 else 1)

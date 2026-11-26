"""Test the Displays applet against the Undocked CLI contract on a private X server."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def fake_undocked(root, args):
    setup = json.loads((root / "setup").read_text())
    current = root / "pending"
    if args == ["status", "--json"]:
        current.touch(exist_ok=True)
        pending = current.read_text()
        outputs = [
            dict(internal=i == 0, enabled=i == 0 or pending == "mirror", position=[0 if pending == "mirror" else 800*i, 0],
                 rotation=0, mode=dict(width=800, height=600), modes=[dict(width=800, height=600)])
            for i in range(setup)
        ]
        print(json.dumps(dict(version=1, confirmation_seconds=15 if pending else 0, outputs=outputs)))
    elif args == ["presets", "--json"]:
        print(json.dumps(dict(version=1, presets=[
            dict(name="Unavailable", matches=False), dict(name='Office "dock"', matches=True)
        ])))
    else:
        with (root / "actions").open("a") as log:
            print(json.dumps(args), file=log)
        if args and args[0] in ("layout", "preset"):
            current.write_text(args[1])
        if args in (["keep"], ["revert"]):
            current.write_text("")


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, timeout=5)


def checked(*args):
    result = run(*args)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return result.stdout.strip()


def wait(fn):
    for _ in range(100):
        result = fn()
        if result:
            return result
        time.sleep(.06)
    raise AssertionError(f"Timed out waiting for {fn!r}")


def window(title):
    return run("xdotool", "search", "--onlyvisible", "--name", f"^{title}$").stdout.strip()


def key(*keys):
    checked("xdotool", "key", "--clearmodifiers", *keys)


def exercise(root, wm, control, tray):
    config = root / "config.ini"
    command = root / "test undocked"
    command.write_text('#!/bin/sh\nexec python3 "$MWM_DISPLAYS_TEST" --fixture "$@"\n')
    command.chmod(0o755)
    os.environ.update(MWM_DISPLAYS_TEST=str(Path(__file__).resolve()), MWM_DISPLAYS_TMP=str(root),
                      G_DEBUG="fatal-warnings")
    setup = root / "setup"
    setup.write_text("1")
    log = root / "actions"

    def configure(settings=""):
        config.write_text(f"[motion]\nreduced_motion = true\n[applets]\nundocked_command = {command}\n{settings}")

    def actions():
        return [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []

    def sent(*args):
        try:
            wait(lambda: list(args) in actions())
        except AssertionError as error:
            raise AssertionError(f"Expected {args!r} in {actions()!r}") from error

    def panel_height():
        found = window("mwm Displays")
        if not found:
            return 0
        geometry = checked("xdotool", "getwindowgeometry", "--shell", found)
        return int(next(line.removeprefix("HEIGHT=") for line in geometry.splitlines() if line.startswith("HEIGHT=")))

    def open_panel():
        def click_icon():
            if visible := window("mwm Displays"):
                return visible
            if icon := window("Displays"):
                run("xdotool", "mousemove", "--window", icon, "8", "8", "click", "1")
            return False

        wait(click_icon)
        time.sleep(.35)

    configure()
    wm_proc = subprocess.Popen([wm, "-c", config])
    tray_proc = None
    try:
        wait(lambda: run(control, "version").returncode == 0)
        tray_proc = subprocess.Popen([tray, "-c", config, "displays"])
        wait((root / "pending").exists)
        time.sleep(.3)
        assert not window("Displays")

        setup.write_text("3")
        open_panel()
        full_height = wait(lambda: (height := panel_height()) > 300 and height)
        key("i")
        sent("identify")
        time.sleep(.35)
        key("m")
        sent("layout", "mirror")
        wait(lambda: 0 < panel_height() < full_height)
        key("r")
        sent("revert")
        wait(lambda: panel_height() == full_height)
        key("Down", "Down", "Down", "Return")
        sent("layout", "y")
        wait(lambda: 0 < panel_height() < full_height)
        key("k")
        sent("keep")
        wait(lambda: panel_height() == full_height)
        key("Escape")
        wait(lambda: not window("mwm Displays"))
        open_panel()
        wait(lambda: panel_height() == full_height)
        key("Down", "Down", "Down", "Down", "Return")
        sent("preset", 'Office "dock"')
        wait(lambda: 0 < panel_height() < full_height)
        key("o")
        sent()
        wait(lambda: not window("mwm Displays"))

        (root / "pending").write_text("")
        setup.write_text("1")
        wait(lambda: not window("Displays"))
        setup.write_text("3")
        configure("display_visibility = never\n")
        checked(control, "reload")
        time.sleep(.5)
        assert not window("Displays")

        setup.write_text("1")
        configure("display_visibility = always\ndisplay_identify = false\ndisplay_saved_presets = false\n")
        checked(control, "reload")
        open_panel()
        count = len(actions())
        key("i", "Down", "Return")
        time.sleep(.3)
        assert len(actions()) == count
        key("Escape")
        wait(lambda: not window("mwm Displays"))

        setup.write_text("3")
        configure("display_quick_presets = column\ndisplay_saved_presets = false\ndisplay_mirror = false\n")
        checked(control, "reload")
        open_panel()
        key("Down", "Return")
        sent("layout", "column")
        assert tray_proc.poll() is None
    finally:
        for proc in (tray_proc, wm_proc):
            if proc and proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=5)


if sys.argv[1:2] == ["--fixture"]:
    fake_undocked(Path(os.environ["MWM_DISPLAYS_TMP"]), sys.argv[2:])
else:
    with tempfile.TemporaryDirectory(prefix="mwm-displays-test-") as directory:
        programs = sys.argv[1:] or ["./mwm", "./mwmc", "./mwm-tray"]
        exercise(Path(directory), *[Path(program).resolve() for program in programs])

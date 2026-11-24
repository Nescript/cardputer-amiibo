Import("env")
import subprocess


def get_git_version():
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            stderr=subprocess.DEVNULL,
        )
        ver = out.decode("utf-8").strip()
        if ver:
            return ver
    except Exception:
        pass
    return "v0.0.0-dev"


ver = get_git_version()
print(f"Building Cardputer-Adv Amiibo Firmware version: {ver}")
env.Append(CPPDEFINES=[("APP_VERSION", f'\\"{ver}\\"')])

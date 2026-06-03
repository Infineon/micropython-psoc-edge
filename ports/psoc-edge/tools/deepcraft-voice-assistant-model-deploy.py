#!/usr/bin/env python3
"""
deepcraft-voice-assistant-model-deploy.py  v0.1.0  --  Build and flash the DEEPCRAFT CM55 Voice-Assistant
                        project for PSOC Edge (KIT_PSE84_AI).

Subcommands
-----------
  build  MODEL_ASSETS_PATH  Install model assets, then build
  flash              Flash an already-built hex to the board
  all    MODEL_ASSETS_PATH  build and flash
  clean              Remove previous build output (and optionally model)
  help               Show detailed usage information

MODEL_ASSETS_PATH
----------
  Path to the DEEPCRAFT model directory or a .zip file.
  Timestamped export names (e.g. test_gpio_control_14-04-2026_15042026_135817.zip)
  are supported -- the timestamp is stripped automatically.

Configuration is stored in deepcraft-voice-assistant-model-deploy.ini next to this script, or ~/.deepcraft-voice-assistant-model-deploy.ini.
"""

import argparse
import configparser
import multiprocessing
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

try:
    import requests as _requests
except ImportError:
    _requests = None

# ---------------------------------------------------------------------------
VERSION = "0.1.0"
REPO_URL = "https://github.com/Infineon/mtb-example-psoc-edge-voice-assistant-deploy-mpy.git"
PROJ_CM55_REL = "proj_cm55"
VA_MODELS_REL = os.path.join(PROJ_CM55_REL, "va_models")
MAKEFILE = "Makefile"
APPNAME = "proj_cm55"
BUILD_DIR_BASE = "build"
_WIN = sys.platform in ("win32", "cygwin")
DEFAULT_MAKE_CMD = "mingw32-make" if _WIN else "make"
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_FOLDER_NAME = "mtb-example-psoc-edge-voice-assistant-deploy-mpy"
DEFAULT_REPO_DIR = os.path.join(_SCRIPT_DIR, _REPO_FOLDER_NAME)
_LOCAL_CONFIG_FILE = os.path.join(_SCRIPT_DIR, "deepcraft-voice-assistant-model-deploy.ini")
DEFAULT_CONFIG_FILE = (
    _LOCAL_CONFIG_FILE
    if os.path.isfile(_LOCAL_CONFIG_FILE)
    else os.path.join(os.path.expanduser("~"), ".deepcraft-voice-assistant-model-deploy.ini")
)
_OCD_DOWNLOAD_DIR = os.path.join(_SCRIPT_DIR, "openocd")
_OCD_VERSION_TAG = "release-v5.11.0"
_OCD_VERSION_STR = "5.11.0.4042"
_OCD_BASE_NAME = f"openocd-{_OCD_VERSION_STR}-"
_OCD_URL_BASE = f"https://github.com/Infineon/openocd/releases/download/{_OCD_VERSION_TAG}/"
_OCD_SUPPORTED_VERS = ["0.12.0+dev-5.8.0.3960", "0.12.0+dev-5.11.0.4042", "0.12.0+dev-5.12.0.4170"]


def print_f(*a, **kw):
    print(*a, **kw, flush=True)


def _fatal(msg):
    print_f(f"[dc-va] ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------------------
def load_config(path):
    cfg = configparser.ConfigParser()
    if os.path.isfile(path):
        cfg.read(path)
    return cfg


def cfg_get(cfg, section, key, fallback=None):
    try:
        return cfg.get(section, key) or fallback
    except (configparser.NoSectionError, configparser.NoOptionError):
        return fallback


def ensure_llvm(cfg, config_path, cli_override=None):
    if cli_override:
        return cli_override
    value = cfg_get(cfg, "tools", "llvm_dir") or os.environ.get("LLVM_DIR")
    if value and os.path.isdir(value):
        return value
    _fatal(
        "LLVM Embedded Toolchain for Arm not found.\n"
        "  Set the LLVM_DIR environment variable, or add\n"
        "  [tools] llvm_dir = <path>  in deepcraft-voice-assistant-model-deploy.ini."
    )


# ---------------------------------------------------------------------------
def _ocd_exe_name():
    return "openocd.exe" if _WIN else "openocd"


def _ocd_local_bin():
    return os.path.join(_OCD_DOWNLOAD_DIR, "bin", _ocd_exe_name())


def _ocd_in_path():
    exe = shutil.which(_ocd_exe_name())
    if not exe:
        return None
    try:
        r = subprocess.run([exe, "--version"], capture_output=True, timeout=10)
        out = (r.stdout + r.stderr).decode(errors="replace")
        if any(v in out for v in _OCD_SUPPORTED_VERS):
            return exe
    except Exception:
        pass
    return None


def _ocd_download_and_install():
    if _requests is None:
        _fatal(
            "The 'requests' package is required to auto-download OpenOCD.\nInstall it with:  pip install requests\nOr set [tools] openocd = <path>  in your config file."
        )
    if _WIN:
        suffix, ext = "windows", ".zip"
    elif sys.platform == "darwin":
        suffix, ext = "macos", ".zip"
    else:
        suffix, ext = "linux", ".tar.gz"
    file_name = _OCD_BASE_NAME + suffix + ext
    url = _OCD_URL_BASE + file_name
    archive = os.path.join(_SCRIPT_DIR, file_name)
    print_f(f"[dc-va] Downloading OpenOCD {_OCD_VERSION_STR} ...")
    print_f(f"         {url}")
    resp = _requests.get(url, stream=True)
    if resp.status_code != 200:
        _fatal(f"OpenOCD download failed (HTTP {resp.status_code}): {url}")
    with open(archive, "wb") as fh:
        for chunk in resp.iter_content(chunk_size=1 << 16):
            fh.write(chunk)
    print_f("[dc-va] Extracting OpenOCD ...")
    if ext == ".tar.gz":
        with tarfile.open(archive) as tf:
            tf.extractall(_SCRIPT_DIR)
    else:
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(_SCRIPT_DIR)
    os.remove(archive)
    exe = _ocd_local_bin()
    if not os.path.isfile(exe):
        _fatal(f"OpenOCD extraction succeeded but binary not found at: {exe}")
    if sys.platform == "darwin":
        os.chmod(exe, 0o755)
    bin_dir = os.path.dirname(exe)
    os.environ["PATH"] = bin_dir + os.pathsep + os.environ["PATH"]
    print_f(f"[dc-va] OpenOCD installed -> {exe}")
    return exe


def ensure_openocd(cfg, config_path, cli_override=None):
    explicit = cli_override or cfg_get(cfg, "tools", "openocd")
    if explicit and os.path.isfile(explicit):
        return explicit
    local = _ocd_local_bin()
    if os.path.isfile(local):
        bin_dir = os.path.dirname(local)
        os.environ["PATH"] = bin_dir + os.pathsep + os.environ["PATH"]
        print_f(f"[dc-va] Using local OpenOCD: {local}")
        return local
    sys_ocd = _ocd_in_path()
    if sys_ocd:
        print_f(f"[dc-va] Using system OpenOCD: {sys_ocd}")
        return sys_ocd
    return _ocd_download_and_install()


def get_make_cmd(cfg, cli_override=None):
    cmd = cli_override or cfg_get(cfg, "tools", "make_cmd", DEFAULT_MAKE_CMD)
    if not shutil.which(cmd):
        _fatal(
            f"'{cmd}' not found in PATH.\n"
            f"  Install MinGW (https://www.mingw-w64.org/) and add its bin\\ folder\n"
            f"  to your system PATH environment variable, then retry.\n"
            f"  Or override with [tools] make_cmd = <full path>  in your config file."
        )
    return cmd


# ---------------------------------------------------------------------------
def _git(*args, cwd=None):
    _run(["git"] + list(args), cwd=cwd)


def _ensure_git_longpaths():
    if not _WIN:
        return
    try:
        r = subprocess.run(
            ["git", "config", "--global", "core.longpaths"], capture_output=True, text=True
        )
        if r.stdout.strip() == "true":
            return
    except Exception:
        pass
    print_f("[dc-va] Enabling git core.longpaths = true (Windows long-path support)")
    _run(["git", "config", "--global", "core.longpaths", "true"])


def clone_or_update(repo_url, dest):
    _ensure_git_longpaths()
    if os.path.isdir(os.path.join(dest, ".git")):
        print_f(f"[dc-va] Updating repo: {dest}")
        _git("pull", "--rebase", "--autostash", cwd=dest)
    else:
        print_f(f"[dc-va] Cloning {repo_url}")
        print_f(f"         -> {dest}")
        parent = os.path.dirname(os.path.abspath(dest))
        os.makedirs(parent, exist_ok=True)
        _git("clone", "--recurse-submodules", repo_url, dest)


# ---------------------------------------------------------------------------
_TIMESTAMP_RE = re.compile(r"_\d{2}-\d{2}-\d{4}_\d{8}_\d{6}$")


def _strip_timestamp(name):
    return _TIMESTAMP_RE.sub("", name)


def _project_name(model_path):
    base = os.path.basename(model_path.rstrip("/\\"))
    return base[:-4] if base.lower().endswith(".zip") else base


def _zip_top_dir(zip_path):
    with zipfile.ZipFile(zip_path) as zf:
        tops = {n.split("/")[0] for n in zf.namelist() if n.split("/")[0]}
    return tops.pop() if len(tops) == 1 else None


def install_model(model_path, va_models_dir, force=False):
    model_path = os.path.abspath(model_path)
    if not os.path.exists(model_path):
        _fatal(f"Model path does not exist: {model_path}")
    is_zip = os.path.isfile(model_path) and zipfile.is_zipfile(model_path)
    raw_name = (
        (_zip_top_dir(model_path) or _project_name(model_path))
        if is_zip
        else _project_name(model_path)
    )
    project_name = _strip_timestamp(raw_name)
    dest = os.path.join(va_models_dir, project_name)
    print_f(f"[dc-va] Installing model '{project_name}' -> {dest}")
    if os.path.exists(dest):
        if not force:
            print_f(f"[dc-va] Model already exists (use --force to overwrite): {dest}")
            return project_name
        shutil.rmtree(dest)
    if is_zip:
        with tempfile.TemporaryDirectory() as tmp:
            with zipfile.ZipFile(model_path) as zf:
                zf.extractall(tmp)
            src = os.path.join(tmp, raw_name)
            if not os.path.isdir(src):
                src = os.path.join(tmp, project_name)
            if not os.path.isdir(src):
                src = tmp
            shutil.copytree(src, dest)
    else:
        shutil.copytree(model_path, dest)
    print_f("[dc-va] Model installed")
    return project_name


# ---------------------------------------------------------------------------
def _clean_model_objs(proj_cm55_dir, config):
    """Delete project-specific .o/.d files and link artifacts, keeping BSP/lib objects."""
    obj_dir = os.path.join(proj_cm55_dir, BUILD_DIR_BASE, config, "obj")
    build_dir = os.path.join(proj_cm55_dir, BUILD_DIR_BASE, config)
    removed = 0
    if os.path.isdir(obj_dir):
        for name in os.listdir(obj_dir):
            if name.startswith("proj_cm55_") and name.endswith((".o", ".d")):
                os.remove(os.path.join(obj_dir, name))
                removed += 1
    for artifact in (
        f"{APPNAME}.elf",
        f"{APPNAME}.hex",
        f"{APPNAME}.bin",
        f"{APPNAME}.map",
        "objects.rsp",
    ):
        p = os.path.join(build_dir, artifact)
        if os.path.isfile(p):
            os.remove(p)
    print_f(f"[dc-va] Model objects cleaned ({removed} .o/.d files removed)")


# ---------------------------------------------------------------------------
def clean_m55(
    *,
    proj_cm55_dir,
    config,
    make_cmd=DEFAULT_MAKE_CMD,
    va_models_dir=None,
    model_path=None,
    full=False,
):
    build_dir = os.path.join(proj_cm55_dir, BUILD_DIR_BASE, config)

    if full:
        print_f(f"[dc-va] Full clean: {build_dir}")
        if os.path.isdir(build_dir):
            shutil.rmtree(build_dir)
            print_f("[dc-va] Build output removed")
        else:
            print_f("[dc-va] Nothing to clean (build directory does not exist)")
    else:
        _clean_model_objs(proj_cm55_dir, config)

    if model_path and va_models_dir:
        model_path = os.path.abspath(model_path)
        is_zip = os.path.isfile(model_path) and zipfile.is_zipfile(model_path)
        raw_name = (
            (_zip_top_dir(model_path) or _project_name(model_path))
            if is_zip
            else _project_name(model_path)
        )
        project_name = _strip_timestamp(raw_name)
        model_dest = os.path.join(va_models_dir, project_name)
        print_f(f"[dc-va] Removing model assets: {model_dest}")
        if os.path.isdir(model_dest):
            shutil.rmtree(model_dest)
            print_f(f"[dc-va] Model '{project_name}' removed")
        else:
            print_f(f"[dc-va] Model '{project_name}' not found in va_models/")


# ---------------------------------------------------------------------------
def build_m55(
    *, proj_cm55_dir, project_name, config, llvm_dir, make_cmd=DEFAULT_MAKE_CMD, jobs=None
):
    hex_path = os.path.join(proj_cm55_dir, BUILD_DIR_BASE, config, f"{APPNAME}.hex")
    # Always clean model objects so the CC/LD/HEX/BIN steps always run and progress is visible.
    _clean_model_objs(proj_cm55_dir, config)
    print_f(f"[dc-va] Building '{project_name}'  [{config}]")
    cmd = [
        make_cmd,
        "-f",
        MAKEFILE,
        f"DEEPCRAFT_PROJECT={project_name}",
        f"CONFIG={config}",
        f"LLVM_DIR={llvm_dir}",
    ]
    if jobs:
        cmd += ["-j", str(jobs)]
    _run_build(cmd, cwd=proj_cm55_dir, env=_build_env(llvm_dir))
    if not os.path.isfile(hex_path):
        _fatal(f"Build finished but hex not found: {hex_path}")
    print_f(f"[dc-va] Build OK -> {hex_path}")


# ---------------------------------------------------------------------------
def flash_m55(*, proj_cm55_dir, config, openocd_exe, serial_number=None):
    hex_path = os.path.join(proj_cm55_dir, BUILD_DIR_BASE, config, f"{APPNAME}.hex")
    if not os.path.isfile(hex_path):
        _fatal(f"Hex not found: {hex_path}\nRun 'build' first.")
    resolved = shutil.which(_ocd_exe_name()) or openocd_exe
    ocd_root = os.path.dirname(os.path.dirname(os.path.abspath(resolved)))
    scripts_dir = os.path.join(ocd_root, "scripts")
    bsp_cfg = os.path.join(proj_cm55_dir, "bsp-cfg")
    flm = os.path.join(bsp_cfg, "PSE84_SMIF.FLM")
    print_f(f"[dc-va] Flashing: {hex_path}")

    def _fwd(p):
        return p.replace("\\", "/")

    serial_opt = f"adapter serial {serial_number};" if serial_number else ""
    tcl_script = (
        f"source [find interface/kitprog3.cfg];"
        f" {serial_opt}"
        f" transport select swd;"
        f" source [find target/infineon/pse84xgxs2.cfg];"
        f" init; reset init; adapter speed 12000;"
        f' flash write_image erase "{_fwd(hex_path)}";'
        f" reset run; shutdown;"
    )
    cmd = [
        resolved,
        "-s",
        _fwd(scripts_dir),
        "-s",
        _fwd(bsp_cfg),
        "-c",
        f"set QSPI_FLASHLOADER {_fwd(flm)}",
        "-c",
        tcl_script,
    ]
    _run(cmd)
    print_f("[dc-va] Flash complete")


# ---------------------------------------------------------------------------
def _build_env(llvm_dir=None):
    env = os.environ.copy()
    if llvm_dir:
        llvm_bin = os.path.join(llvm_dir, "bin")
        if os.path.isdir(llvm_bin):
            env["PATH"] = llvm_bin + os.pathsep + env["PATH"]
    return env


def _run(cmd, cwd=None, env=None):
    print_f(f"[dc-va] $ {' '.join(str(c) for c in cmd)}")
    try:
        r = subprocess.run(cmd, cwd=cwd, env=env)
    except FileNotFoundError:
        _fatal(
            f"Executable not found: '{cmd[0]}'\n"
            f"On Windows, set [tools] make_cmd = <path>  in your config file."
        )
    except KeyboardInterrupt:
        print_f("\n[dc-va] Interrupted by user", file=sys.stderr)
        sys.exit(1)
    if r.returncode != 0:
        _fatal(f"Command failed (exit {r.returncode})")


# Matches the  CC / CXX / AS  progress echo lines emitted by the Makefile rules.
_COMPILE_TAG_RE = re.compile(r"^\s+(CC|CXX|AS)\s+\S")
# Matches a compiler invocation (-o <file>.o) to count steps in the dry-run.
_COMPILE_CMD_RE = re.compile(r"\s-o\s+\S+\.o\b")


def _build_bar(done, total, term_width):
    suffix = f"  {done}/{total}" if total else f"  {done} files"
    prefix = "  Compiling  ["
    bar_width = max(10, term_width - len(prefix) - 1 - len(suffix))
    filled = min(bar_width, round(bar_width * done / total)) if total else min(done, bar_width)
    return f"{prefix}{'█' * filled}{'░' * (bar_width - filled)}]{suffix}"


def _run_build(cmd, cwd, env):
    """Run make and display a progress bar for CC/CXX/AS compile steps."""
    # Count expected compile steps via dry-run (fast, no actual compilation).
    total = 0
    try:
        dry = subprocess.run(
            cmd + ["-n", "--no-print-directory"],
            cwd=cwd,
            env=env,
            capture_output=True,
            text=True,
        )
        total = sum(1 for l in dry.stdout.splitlines() if _COMPILE_CMD_RE.search(l))
    except Exception:
        pass  # no total available; bar will still show count

    term_width = shutil.get_terminal_size((80, 24)).columns
    done = 0
    bar_active = False

    try:
        proc = subprocess.Popen(
            cmd,
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
    except FileNotFoundError:
        _fatal(
            f"Executable not found: '{cmd[0]}'\n"
            f"On Windows, set [tools] make_cmd = <path>  in your config file."
        )

    try:
        for raw in proc.stdout:
            line = raw.rstrip("\n")
            if _COMPILE_TAG_RE.match(line):
                done += 1
                bar_active = True
                sys.stdout.write(f"\r{_build_bar(done, total, term_width)}")
                sys.stdout.flush()
            elif line.strip():
                # Non-empty, non-compile line (LD / HEX / BIN / size / error).
                # End the bar first, then print the line normally.
                if bar_active:
                    sys.stdout.write("\n")
                    bar_active = False
                print(line)
            # Empty lines from make are silently discarded so they don't break the bar.
    except KeyboardInterrupt:
        proc.terminate()
        print_f("\n[dc-va] Interrupted by user", file=sys.stderr)
        sys.exit(1)
    finally:
        proc.wait()

    if bar_active:
        sys.stdout.write("\n")
    if proc.returncode != 0:
        _fatal(f"Command failed (exit {proc.returncode})")


# ---------------------------------------------------------------------------
def print_help(parser=None):
    print_f(f"""
deepcraft-voice-assistant-model-deploy.py  v{VERSION}
Build and flash the DEEPCRAFT CM55 Voice-Assistant firmware for PSOC Edge (KIT_PSE84_AI).

USAGE
-----
  python deepcraft-voice-assistant-model-deploy.py [--config-file PATH] COMMAND [options]

COMMANDS
--------
  build  MODEL_ASSETS_PATH  [options]
      Install model assets into va_models/ then compile firmware.

      Required:
        MODEL_ASSETS_PATH        Path to the DEEPCRAFT model folder or .zip file.

      Options:
        --llvm-dir  PATH         LLVM Embedded Toolchain for Arm directory
        --make-cmd  CMD          GNU make executable (default: mingw32-make)
        -j / --jobs N            Parallel make jobs (default: CPU count)
        --force                  Overwrite model assets if already installed

  flash  [options]
      Flash a previously built hex to the board via OpenOCD.

      Options:
        --openocd   PATH         OpenOCD executable (auto-downloaded if absent)
        --serial    SN           KitProg3 adapter serial (needed with multiple boards)

  all    MODEL_ASSETS_PATH  [options]
      Install model assets, build firmware, then flash to board.

      Required:
        MODEL_ASSETS_PATH        Path to the DEEPCRAFT model folder or .zip file.

      Options:
        --llvm-dir  PATH         LLVM Embedded Toolchain for Arm directory
        --make-cmd  CMD          GNU make executable (default: mingw32-make)
        -j / --jobs N            Parallel make jobs (default: CPU count)
        --force                  Overwrite model assets if already installed
        --openocd   PATH         OpenOCD executable (auto-downloaded if absent)
        --serial    SN           KitProg3 adapter serial (needed with multiple boards)

  clean  [options]
      Remove model object files and link artifacts, keeping BSP/dependency
      objects intact for faster recompilation when switching models.

      Options:
        --all                        Remove the entire build directory (full clean).
        --model  MODEL_ASSETS_PATH   Also remove that model from va_models/.

GLOBAL OPTIONS
--------------
  --config-file PATH   Config file (default: deepcraft-voice-assistant-model-deploy.ini next to script)
  --version            Print version and exit

CONFIG FILE  (deepcraft-voice-assistant-model-deploy.ini) (optional)
--------------------------------------------------
  [tools]
  llvm_dir      = /path/to/LLVM-ET-Arm-19.1.5   (overrides LLVM_DIR env var)
  make_cmd      = mingw32-make
  openocd       = (leave blank to auto-download)

  [board]
  serial_number = Connected hardware's serial numbers (if multiple boards are used, specify the one to flash here or via --serial)

ENVIRONMENT VARIABLES
---------------------
  LLVM_DIR    Path to the LLVM Embedded Toolchain for Arm directory.
              Used when [tools] llvm_dir is not set in deepcraft-voice-assistant-model-deploy.ini.
  PATH        Must include the MinGW bin\\ directory so that mingw32-make is reachable.
              Install MinGW and add <MinGW>\\bin to your system PATH, or set
              [tools] make_cmd = <full path to mingw32-make.exe> in the config file.
""")


def _parser():
    p = argparse.ArgumentParser(
        prog="deepcraft-voice-assistant-model-deploy.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "--version", action="version", version=f"deepcraft-voice-assistant-model-deploy {VERSION}"
    )
    p.add_argument(
        "--config-file",
        default=DEFAULT_CONFIG_FILE,
        metavar="PATH",
        help=f"Config file path (default: {DEFAULT_CONFIG_FILE})",
    )
    sub = p.add_subparsers(dest="command", metavar="COMMAND")
    sub.required = True

    def _build_args(sp):
        sp.add_argument(
            "--llvm-dir", metavar="PATH", help="LLVM Embedded Toolchain for Arm directory"
        )
        sp.add_argument("--make-cmd", metavar="CMD", help="GNU make executable (default: make)")
        sp.add_argument("-j", "--jobs", type=int, metavar="N", help="Parallel make jobs")

    def _flash_args(sp):
        sp.add_argument("--openocd", metavar="PATH", help="OpenOCD executable path")
        sp.add_argument("--serial", metavar="SN", help="KitProg3 adapter serial number")

    h = sub.add_parser("help", help="Show detailed usage information")
    h.set_defaults(command="help")

    cl = sub.add_parser(
        "clean", help="Remove model objects + link artifacts (or full build with --all)"
    )
    cl.add_argument(
        "--all",
        action="store_true",
        dest="clean_all",
        help="Remove the entire build directory (including BSP and dependency objects)",
    )
    cl.add_argument(
        "--model", metavar="MODEL_ASSETS_PATH", help="Also remove model assets from va_models/"
    )

    b = sub.add_parser("build", help="Install model assets and build")
    b.add_argument(
        "MODEL_ASSETS_PATH",
        metavar="MODEL_ASSETS_PATH",
        help="Path to model directory or .zip file",
    )
    b.add_argument(
        "--force",
        action="store_true",
        help="Overwrite model assets if they already exist in va_models/",
    )
    _build_args(b)

    f = sub.add_parser("flash", help="Flash an already-built hex to the board")
    _flash_args(f)

    a = sub.add_parser("all", help="Install model, build, then flash")
    a.add_argument(
        "MODEL_ASSETS_PATH",
        metavar="MODEL_ASSETS_PATH",
        help="Path to model directory or .zip file",
    )
    a.add_argument(
        "--force",
        action="store_true",
        help="Overwrite model assets if they already exist in va_models/",
    )
    _build_args(a)
    _flash_args(a)

    return p


# ---------------------------------------------------------------------------
def main():
    parser = _parser()

    if len(sys.argv) == 1:
        print_help(parser)
        sys.exit(0)

    args = parser.parse_args()

    if args.command == "help":
        print_help(parser)
        return

    cfg = load_config(args.config_file)

    repo_dir = DEFAULT_REPO_DIR
    build_config = cfg_get(cfg, "project", "build_config") or "Debug"
    serial = getattr(args, "serial", None) or cfg_get(cfg, "board", "serial_number")
    jobs = getattr(args, "jobs", None) or multiprocessing.cpu_count()
    force = getattr(args, "force", False)

    proj_cm55_dir = os.path.join(repo_dir, PROJ_CM55_REL)
    va_models_dir = os.path.join(repo_dir, VA_MODELS_REL)

    if args.command == "clean":
        clean_m55(
            proj_cm55_dir=proj_cm55_dir,
            config=build_config,
            make_cmd=get_make_cmd(cfg),
            va_models_dir=va_models_dir,
            model_path=getattr(args, "model", None),
            full=getattr(args, "clean_all", False),
        )
        return

    if args.command in ("build", "all"):
        clone_or_update(REPO_URL, repo_dir)

    llvm_dir, openocd, make_cmd = None, None, None

    if args.command in ("build", "all"):
        llvm_dir = ensure_llvm(cfg, args.config_file, getattr(args, "llvm_dir", None))
        make_cmd = get_make_cmd(cfg, getattr(args, "make_cmd", None))

    if args.command in ("flash", "all"):
        openocd = ensure_openocd(cfg, args.config_file, getattr(args, "openocd", None))

    if args.command in ("build", "all"):
        project_name = install_model(args.MODEL_ASSETS_PATH, va_models_dir, force=force)
        build_m55(
            proj_cm55_dir=proj_cm55_dir,
            project_name=project_name,
            config=build_config,
            llvm_dir=llvm_dir,
            make_cmd=make_cmd,
            jobs=jobs,
        )

    if args.command in ("flash", "all"):
        flash_m55(
            proj_cm55_dir=proj_cm55_dir,
            config=build_config,
            openocd_exe=openocd,
            serial_number=serial,
        )


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print_f("\n[dc-va] Interrupted by user", file=sys.stderr)
        sys.exit(1)

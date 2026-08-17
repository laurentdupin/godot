#!/usr/bin/env python3
"""Bootstrap a cached build environment and build Godot on desktop hosts."""

from __future__ import annotations

import argparse
import json
import os
import platform as host_platform
import shlex
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Sequence


SCRIPT_DIRECTORY = Path(__file__).resolve().parent
GODOT_ROOT = SCRIPT_DIRECTORY.parent
VIRTUAL_ENVIRONMENT = SCRIPT_DIRECTORY / ".venv"
CACHE_DIRECTORY = SCRIPT_DIRECTORY / ".cache"
PIP_CACHE_DIRECTORY = CACHE_DIRECTORY / "pip"
SCONS_CACHE_DIRECTORY = CACHE_DIRECTORY / "scons"
SETTINGS_PATH = SCRIPT_DIRECTORY / "settings.json"
SCONS_VERSION = "4.10.1"
SETTINGS_VERSION = 1
TARGETS = ("editor", "template_debug", "template_release")
ARCHITECTURES = ("x86_64", "arm64", "x86_32")


@dataclass
class BuildSettings:
    version: int = SETTINGS_VERSION
    architecture: str = "x86_64"
    target: str = "editor"
    mono: bool = True
    hcsr: bool = True
    angle: bool = False
    developer_build: bool = False
    jobs: int = 1
    cache_limit_gib: int = 20


def detect_godot_platform() -> str:
    if sys.platform == "win32":
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    if sys.platform.startswith("linux"):
        return "linuxbsd"
    raise RuntimeError("This build script supports Windows, macOS, and Linux hosts only.")


def detect_architecture() -> str:
    machine = host_platform.machine().lower()
    if machine in ("amd64", "x86_64"):
        return "x86_64"
    if machine in ("arm64", "aarch64"):
        return "arm64"
    if machine in ("x86", "i386", "i686"):
        return "x86_32"
    return "x86_64"


def default_settings() -> BuildSettings:
    return BuildSettings(
        architecture=detect_architecture(),
        jobs=max(1, min(os.cpu_count() or 1, 32)),
    )


def load_settings() -> BuildSettings:
    defaults = default_settings()
    if not SETTINGS_PATH.is_file():
        return defaults

    try:
        raw_settings = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        print(f"Ignoring invalid saved settings: {error}", file=sys.stderr)
        return defaults

    if not isinstance(raw_settings, dict) or raw_settings.get("version") != SETTINGS_VERSION:
        return defaults

    settings = BuildSettings()
    for field_name in asdict(settings):
        if field_name in raw_settings:
            setattr(settings, field_name, raw_settings[field_name])

    if settings.architecture not in ARCHITECTURES:
        settings.architecture = defaults.architecture
    if settings.target not in TARGETS:
        settings.target = defaults.target
    settings.mono = bool(settings.mono)
    settings.hcsr = bool(settings.hcsr)
    settings.angle = bool(settings.angle)
    settings.developer_build = bool(settings.developer_build)
    if not isinstance(settings.jobs, int) or isinstance(settings.jobs, bool):
        settings.jobs = defaults.jobs
    settings.jobs = max(1, min(settings.jobs, 256))
    if not isinstance(settings.cache_limit_gib, int) or isinstance(settings.cache_limit_gib, bool):
        settings.cache_limit_gib = defaults.cache_limit_gib
    settings.cache_limit_gib = max(0, min(settings.cache_limit_gib, 1024))
    return settings


def save_settings(settings: BuildSettings) -> None:
    SETTINGS_PATH.write_text(json.dumps(asdict(settings), indent=2) + "\n", encoding="utf-8")


def virtual_environment_python() -> Path:
    if sys.platform == "win32":
        return VIRTUAL_ENVIRONMENT / "Scripts" / "python.exe"
    return VIRTUAL_ENVIRONMENT / "bin" / "python"


def query_scons_version(python_executable: Path) -> str | None:
    completed = subprocess.run(
        [str(python_executable), "-c", "import SCons; print(SCons.__version__)"],
        cwd=GODOT_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        return None
    return completed.stdout.strip()


def bootstrap_virtual_environment() -> Path:
    python_executable = virtual_environment_python()
    if not python_executable.is_file():
        print(f"Creating Python virtual environment: {VIRTUAL_ENVIRONMENT}", flush=True)
        completed = subprocess.run(
            [sys.executable, "-m", "venv", str(VIRTUAL_ENVIRONMENT)],
            cwd=GODOT_ROOT,
            check=False,
        )
        if completed.returncode != 0 or not python_executable.is_file():
            raise RuntimeError("Python could not create the local virtual environment.")

    if query_scons_version(python_executable) != SCONS_VERSION:
        PIP_CACHE_DIRECTORY.mkdir(parents=True, exist_ok=True)
        environment = os.environ.copy()
        environment["PIP_CACHE_DIR"] = str(PIP_CACHE_DIRECTORY)
        print(f"Installing SCons {SCONS_VERSION} into the local virtual environment.", flush=True)
        completed = subprocess.run(
            [
                str(python_executable),
                "-m",
                "pip",
                "install",
                "--disable-pip-version-check",
                f"scons=={SCONS_VERSION}",
            ],
            cwd=GODOT_ROOT,
            env=environment,
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError("SCons installation failed.")

    return python_executable


def choose(label: str, values: Sequence[str], current: str) -> str:
    print(f"\n{label}:")
    for index, value in enumerate(values, start=1):
        marker = " *" if value == current else ""
        print(f"  {index}. {value}{marker}")
    response = input("Selection (Enter keeps current): ").strip()
    if not response:
        return current
    try:
        selected_index = int(response) - 1
    except ValueError:
        print("Invalid selection; keeping the current value.")
        return current
    if selected_index < 0 or selected_index >= len(values):
        print("Invalid selection; keeping the current value.")
        return current
    return values[selected_index]


def choose_integer(label: str, current: int, minimum: int, maximum: int) -> int:
    response = input(f"{label} [{current}]: ").strip()
    if not response:
        return current
    try:
        value = int(response)
    except ValueError:
        print("Invalid number; keeping the current value.")
        return current
    if value < minimum or value > maximum:
        print(f"Value must be between {minimum} and {maximum}; keeping the current value.")
        return current
    return value


def display_settings(settings: BuildSettings, godot_platform: str) -> None:
    print("\nGodot desktop build settings")
    print(f"  Host platform:  {godot_platform}")
    print(f"  Architecture:   {settings.architecture}")
    print(f"  Target:         {settings.target}")
    print(f"  Mono:           {'included' if settings.mono else 'excluded'}")
    print(f"  HCSR:           {'included' if settings.hcsr else 'excluded'}")
    if godot_platform == "windows":
        print(f"  ANGLE:          {'included' if settings.angle else 'excluded'}")
    print(f"  Developer build:{' yes' if settings.developer_build else ' no'}")
    print(f"  Parallel jobs:  {settings.jobs}")
    cache_limit = "unlimited" if settings.cache_limit_gib == 0 else f"{settings.cache_limit_gib} GiB"
    print(f"  SCons cache:    {cache_limit}")


def configure_interactively(settings: BuildSettings, godot_platform: str) -> BuildSettings | None:
    while True:
        display_settings(settings, godot_platform)
        print("\n  1. Change architecture")
        print("  2. Change target")
        print("  3. Toggle Mono")
        print("  4. Toggle HCSR")
        if godot_platform == "windows":
            print("  5. Toggle ANGLE")
        print("  6. Toggle developer build")
        print("  7. Change parallel jobs")
        print("  8. Change SCons cache limit")
        print("  R. Reset defaults")
        print("  B. Save and build")
        print("  Q. Quit")
        response = input("\nSelection [B]: ").strip().lower() or "b"
        if response == "1":
            settings.architecture = choose("Architecture", ARCHITECTURES, settings.architecture)
        elif response == "2":
            settings.target = choose("Target", TARGETS, settings.target)
        elif response == "3":
            settings.mono = not settings.mono
        elif response == "4":
            settings.hcsr = not settings.hcsr
        elif response == "5":
            if godot_platform != "windows":
                print("ANGLE is available only for Windows builds.")
                continue
            settings.angle = not settings.angle
        elif response == "6":
            settings.developer_build = not settings.developer_build
        elif response == "7":
            settings.jobs = choose_integer("Parallel jobs", settings.jobs, 1, 256)
        elif response == "8":
            settings.cache_limit_gib = choose_integer(
                "Cache limit in GiB (0 is unlimited)", settings.cache_limit_gib, 0, 1024
            )
        elif response == "r":
            settings = default_settings()
        elif response == "b":
            return settings
        elif response == "q":
            return None
        else:
            print("Unknown selection.")


def validate_settings(settings: BuildSettings, godot_platform: str) -> None:
    if settings.hcsr and settings.architecture not in ("x86_64", "arm64"):
        raise RuntimeError("HCSR builds support x86_64 and arm64 architectures only.")
    if godot_platform == "macos" and settings.architecture == "x86_32":
        raise RuntimeError("Godot does not support x86_32 macOS builds.")


def build_command(
    settings: BuildSettings,
    godot_platform: str,
    clean: bool,
    extra_arguments: Sequence[str],
) -> list[str]:
    SCONS_CACHE_DIRECTORY.mkdir(parents=True, exist_ok=True)
    command = [
        sys.executable,
        "-m",
        "SCons",
        f"platform={godot_platform}",
        f"arch={settings.architecture}",
        f"target={settings.target}",
        f"module_mono_enabled={'yes' if settings.mono else 'no'}",
        f"module_html_css_renderer={'hcsr' if settings.hcsr else 'none'}",
        f"dev_build={'yes' if settings.developer_build else 'no'}",
        f"cache_path={SCONS_CACHE_DIRECTORY}",
        f"cache_limit={settings.cache_limit_gib}",
        f"-j{settings.jobs}",
    ]
    if clean:
        command.append("--clean")
    if godot_platform == "macos" and settings.target == "editor":
        command.append("generate_bundle=yes")
    if godot_platform == "windows":
        command.extend((f"angle={'yes' if settings.angle else 'no'}", "d3d12=yes", "vulkan=yes"))
    command.extend(extra_arguments)
    return command


def format_command(command: Sequence[str]) -> str:
    if sys.platform == "win32":
        return subprocess.list2cmdline(command)
    return shlex.join(command)


def force_macos_hcsr_relink(settings: BuildSettings, godot_platform: str) -> None:
    if godot_platform != "macos" or settings.target != "editor" or not settings.hcsr:
        return

    runtime_identifier = "osx-arm64" if settings.architecture == "arm64" else "osx-x64"
    publish_directory = (
        GODOT_ROOT
        / "thirdparty"
        / "hcsr"
        / "src"
        / "Renderer.NativeBridge"
        / "bin"
        / "Release"
        / "net10.0"
        / runtime_identifier
        / "publish"
    )
    hcsr_inputs = (
        publish_directory / "hcsr_renderer_combined.a",
        publish_directory / "hcsr_renderer_initializer.o",
    )
    editor_suffix = ".mono" if settings.mono else ""
    editor_binary = (
        GODOT_ROOT
        / "bin"
        / f"godot.macos.editor.{settings.architecture}{editor_suffix}"
    )
    existing_inputs = [path for path in hcsr_inputs if path.is_file()]
    if not editor_binary.is_file() or not existing_inputs:
        return
    if max(path.stat().st_mtime_ns for path in existing_inputs) <= editor_binary.stat().st_mtime_ns:
        return

    print(
        "HCSR static package is newer than the editor; forcing a fresh link: "
        f"{editor_binary}",
        flush=True,
    )
    editor_binary.unlink()


def build_managed_editor_assemblies(settings: BuildSettings, godot_platform: str) -> None:
    if not settings.mono or settings.target != "editor":
        return

    editor_suffix = ".mono"
    if godot_platform == "windows":
        editor_candidates = (
            GODOT_ROOT
            / "bin"
            / f"godot.windows.editor.{settings.architecture}{editor_suffix}.console.exe",
            GODOT_ROOT / "bin" / f"godot.windows.editor.{settings.architecture}{editor_suffix}.exe",
        )
    elif godot_platform == "macos":
        editor_candidates = (
            GODOT_ROOT / "bin" / "Godot.app" / "Contents" / "MacOS" / "Godot",
            GODOT_ROOT / "bin" / f"godot.macos.editor.{settings.architecture}{editor_suffix}",
        )
    else:
        editor_candidates = (
            GODOT_ROOT / "bin" / f"godot.linuxbsd.editor.{settings.architecture}{editor_suffix}",
        )

    editor_binary = next((path for path in editor_candidates if path.is_file()), None)
    if editor_binary is None:
        raise RuntimeError(
            "Godot Mono editor build did not produce any expected executable: "
            + ", ".join(str(path) for path in editor_candidates)
        )

    glue_directory = GODOT_ROOT / "modules" / "mono" / "glue"
    glue_command = [
        str(editor_binary),
        "--headless",
        "--generate-mono-glue",
        str(glue_directory),
    ]
    print(f"\nMono glue command: {format_command(glue_command)}\n", flush=True)
    completed = subprocess.run(glue_command, cwd=GODOT_ROOT, check=False)
    if completed.returncode != 0:
        raise RuntimeError("Godot Mono glue generation failed.")

    build_script = GODOT_ROOT / "modules" / "mono" / "build_scripts" / "build_assemblies.py"
    command = [
        sys.executable,
        str(build_script),
        "--godot-output-dir",
        str(GODOT_ROOT / "bin"),
        "--godot-platform",
        godot_platform,
    ]
    if settings.developer_build:
        command.append("--dev-debug")

    print(f"\nManaged assembly command: {format_command(command)}\n", flush=True)
    completed = subprocess.run(command, cwd=GODOT_ROOT, check=False)
    if completed.returncode != 0:
        raise RuntimeError("Godot managed assembly build failed.")

    required_assemblies = (
        GODOT_ROOT / "bin" / "GodotSharp" / "Api" / "Debug" / "GodotSharp.dll",
        GODOT_ROOT / "bin" / "GodotSharp" / "Api" / "Debug" / "GodotSharpEditor.dll",
        GODOT_ROOT / "bin" / "GodotSharp" / "Api" / "Release" / "GodotSharp.dll",
        GODOT_ROOT / "bin" / "GodotSharp" / "Api" / "Release" / "GodotSharpEditor.dll",
    )
    missing_assemblies = [str(path) for path in required_assemblies if not path.is_file()]
    if missing_assemblies:
        raise RuntimeError("Godot managed assembly build did not produce: " + ", ".join(missing_assemblies))


def parse_arguments() -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(
        description="Configure and build Godot using a cached local Python/SCons environment.",
        epilog="Arguments after -- are forwarded verbatim to SCons.",
    )
    parser.add_argument(
        "--non-interactive",
        action="store_true",
        help="Build immediately using saved settings (or platform defaults).",
    )
    parser.add_argument("--reset-settings", action="store_true", help="Discard saved selections before configuring.")
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Run SCons in clean mode using the selected configuration.",
    )
    parser.add_argument(
        "--print-command",
        action="store_true",
        help="Print the final SCons command without running it.",
    )
    arguments, extra_arguments = parser.parse_known_args()
    if extra_arguments and extra_arguments[0] == "--":
        extra_arguments = extra_arguments[1:]
    return arguments, extra_arguments


def run() -> int:
    if sys.version_info < (3, 9):
        raise RuntimeError("Python 3.9 or newer is required.")

    arguments, extra_arguments = parse_arguments()
    python_executable = bootstrap_virtual_environment()
    # A virtual environment's Python executable is commonly a symlink to the
    # base interpreter on POSIX hosts. Comparing resolved executable paths
    # therefore cannot tell whether this process is running inside the venv.
    if Path(sys.prefix).resolve() != VIRTUAL_ENVIRONMENT.resolve():
        sys.stdout.flush()
        completed = subprocess.run(
            [str(python_executable), str(Path(__file__).resolve()), *sys.argv[1:]],
            cwd=GODOT_ROOT,
            check=False,
        )
        return completed.returncode

    godot_platform = detect_godot_platform()
    if arguments.reset_settings and SETTINGS_PATH.exists():
        SETTINGS_PATH.unlink()
    settings = load_settings()

    if not arguments.non_interactive and sys.stdin.isatty():
        configured_settings = configure_interactively(settings, godot_platform)
        if configured_settings is None:
            return 0
        settings = configured_settings
    else:
        display_settings(settings, godot_platform)

    validate_settings(settings, godot_platform)
    save_settings(settings)
    command = build_command(settings, godot_platform, arguments.clean, extra_arguments)
    print(f"\nGodot root: {GODOT_ROOT}")
    print(f"Build command: {format_command(command)}\n")
    if arguments.print_command:
        return 0

    force_macos_hcsr_relink(settings, godot_platform)
    sys.stdout.flush()
    completed = subprocess.run(command, cwd=GODOT_ROOT, check=False)
    if completed.returncode != 0 or arguments.clean:
        return completed.returncode

    build_managed_editor_assemblies(settings, godot_platform)
    return 0


def main() -> int:
    try:
        return run()
    except (OSError, RuntimeError) as error:
        print(f"Build setup failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nBuild cancelled.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())

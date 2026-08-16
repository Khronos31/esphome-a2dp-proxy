#!/usr/bin/env python3
"""Synchronize and validate public release version metadata."""

from __future__ import annotations

import argparse
import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION_FILE = ROOT / "VERSION"
COMPONENT_INIT = ROOT / "components" / "a2dp_source" / "__init__.py"
EXAMPLE_FILE = ROOT / "examples" / "esp32-devkitc-ve.yaml"
README_FILE = ROOT / "README.md"

SEMVER = re.compile(
    r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
    r"(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)"
    r"(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?"
    r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$"
)
EXAMPLE_START = "<!-- BEGIN: esp32-devkitc-ve-example -->"
EXAMPLE_END = "<!-- END: esp32-devkitc-ve-example -->"


class VersionError(ValueError):
    """Raised when release metadata is invalid or inconsistent."""


def read_version() -> str:
    return VERSION_FILE.read_text(encoding="utf-8").strip()


def validate_semver(version: str) -> None:
    if not SEMVER.fullmatch(version):
        raise VersionError(f"invalid semantic version: {version!r}")


def read_component_version() -> str:
    tree = ast.parse(COMPONENT_INIT.read_text(encoding="utf-8"), COMPONENT_INIT.name)
    values: list[str] = []
    for node in tree.body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if isinstance(target, ast.Name) and target.id == "__version__":
            value = ast.literal_eval(node.value)
            if isinstance(value, str):
                values.append(value)
    if len(values) != 1:
        raise VersionError("component __init__.py must assign __version__ exactly once")
    return values[0]


def extract_readme_example(readme: str) -> str:
    if readme.count(EXAMPLE_START) != 1 or readme.count(EXAMPLE_END) != 1:
        raise VersionError("README must contain exactly one marked example block")
    marked = readme.split(EXAMPLE_START, 1)[1].split(EXAMPLE_END, 1)[0]
    if marked.count("```yaml") != 1 or marked.count("```") != 2:
        raise VersionError("marked README example must contain one YAML code fence")
    return marked.split("```yaml", 1)[1].split("```", 1)[0].strip()


def replace_exact_once(text: str, old: str, new: str, label: str) -> str:
    if text.count(old) != 1:
        raise VersionError(f"expected exactly one {label}, found {text.count(old)}")
    return text.replace(old, new, 1)


def set_version(version: str) -> None:
    """Write a version to every version-bearing public file."""
    validate_semver(version)
    old_version = read_version()
    validate_semver(old_version)

    component = COMPONENT_INIT.read_text(encoding="utf-8")
    component = replace_exact_once(
        component,
        f'__version__ = "{old_version}"',
        f'__version__ = "{version}"',
        "component version",
    )

    example = EXAMPLE_FILE.read_text(encoding="utf-8")
    example = replace_exact_once(
        example,
        f'    version: "{old_version}"',
        f'    version: "{version}"',
        "project version",
    )
    example = replace_exact_once(
        example,
        f"github://Khronos31/esphome-a2dp-proxy@v{old_version}",
        f"github://Khronos31/esphome-a2dp-proxy@v{version}",
        "external component release ref",
    )

    readme = README_FILE.read_text(encoding="utf-8")
    old_block = extract_readme_example(readme)
    readme = replace_exact_once(readme, old_block, example.strip(), "README example")

    VERSION_FILE.write_text(f"{version}\n", encoding="utf-8")
    COMPONENT_INIT.write_text(component, encoding="utf-8")
    EXAMPLE_FILE.write_text(example, encoding="utf-8")
    README_FILE.write_text(readme, encoding="utf-8")


def check_version(tag: str | None = None) -> str:
    """Fail if VERSION, component, example, README, or a tag disagree."""
    version = read_version()
    validate_semver(version)

    component_version = read_component_version()
    if component_version != version:
        raise VersionError(
            f"component version {component_version!r} does not match VERSION {version!r}"
        )

    example = EXAMPLE_FILE.read_text(encoding="utf-8").strip()
    project_version = f'    version: "{version}"'
    component_ref = f"github://Khronos31/esphome-a2dp-proxy@v{version}"
    if example.count(project_version) != 1:
        raise VersionError("example project.version does not match VERSION")
    if example.count(component_ref) != 1:
        raise VersionError("example external component ref does not match VERSION")

    readme = README_FILE.read_text(encoding="utf-8")
    if extract_readme_example(readme) != example:
        raise VersionError("README YAML does not exactly match the public example")

    expected_tag = f"v{version}"
    if tag is not None and tag != expected_tag:
        raise VersionError(f"release tag {tag!r} must equal {expected_tag!r}")
    return version


def main() -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    set_parser = commands.add_parser("set", help="synchronize release versions")
    set_parser.add_argument("version")
    check_parser = commands.add_parser("check", help="validate release versions")
    check_parser.add_argument("--tag")
    args = parser.parse_args()

    try:
        if args.command == "set":
            set_version(args.version)
            version = check_version()
        else:
            version = check_version(args.tag)
    except (OSError, SyntaxError, ValueError) as err:
        print(f"release metadata error: {err}", file=sys.stderr)
        return 1

    print(f"release metadata is consistent: {version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

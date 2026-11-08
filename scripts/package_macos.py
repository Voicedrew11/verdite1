#!/usr/bin/env python3
"""Bundle the Mac executable and its native libraries for a local Finder launch."""

import argparse
from pathlib import Path
import plistlib
import shutil
import subprocess


def run(*arguments):
    return subprocess.check_output(arguments, text=True).strip()


def dependencies(binary):
    return [line.strip().split(" (compatibility", 1)[0]
            for line in run("otool", "-L", str(binary)).splitlines()[1:]]


def minimum_macos(binary):
    """The lowest macOS the binary runs on, from its LC_BUILD_VERSION."""
    lines = run("otool", "-l", str(binary)).splitlines()
    for at, line in enumerate(lines):
        if line.strip() == "cmd LC_BUILD_VERSION":
            for following in lines[at + 1:at + 6]:
                fields = following.split()
                if fields and fields[0] == "minos":
                    return tuple(int(part) for part in fields[1].split("."))
    raise RuntimeError(f"No minimum macOS in {binary}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("build/macos"))
    parser.add_argument("--output", type=Path, default=Path("dist/Verdite1.app"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    build = (root / args.build).resolve()
    bundle = (root / args.output).resolve()
    if bundle.suffix != ".app" or bundle == root:
        parser.error("output must name an .app bundle")
    binary = build / "verdite1"
    if not binary.is_file():
        parser.error(f"build the executable first: {binary}")
    driver = Path(run("brew", "--prefix", "molten-vk")) / "lib/libMoltenVK.dylib"
    staging = bundle.with_name(bundle.name + ".staging")
    if staging.exists():
        shutil.rmtree(staging)
    contents = staging / "Contents"
    frameworks = contents / "Frameworks"
    resources = contents / "Resources"
    executable = contents / "MacOS/verdite1"
    for folder in (frameworks, resources, executable.parent):
        folder.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binary, executable)
    shutil.copytree(root / "mods", resources / "mods")
    shutil.copy2(root / "LICENSE", resources / "LICENSE")
    licenses = resources / "licenses"
    licenses.mkdir()
    shutil.copy2(driver.parent.parent / "LICENSE", licenses / "MoltenVK.txt")
    sdl = Path(run("brew", "--prefix", "sdl3"))
    shutil.copy2(sdl / "LICENSE.txt", licenses / "SDL3.txt")
    for source, name in (
        (root / "third_party/imgui/LICENSE.txt", "DearImGui.txt"),
        (root / "third_party/lua/LICENSE", "Lua.txt"),
        (root / "third_party/cjson/LICENSE", "cJSON.txt"),
        (root / "third_party/libchdr/LICENSE.txt", "libchdr.txt"),
        (root / "third_party/libchdr/deps/zstd-1.5.7/LICENSE", "zstd.txt"),
        (build / "_deps/cpptrace-src/LICENSE", "cpptrace.txt"),
        (build / "_deps/vk_bootstrap-src/LICENSE.txt", "vk-bootstrap.txt"),
        (build / "_deps/vulkanmemoryallocator-src/LICENSE.txt", "VulkanMemoryAllocator.txt"),
    ):
        shutil.copy2(source, licenses / name)

    # Resolve the dependency graph before rewriting install names. System frameworks
    # stay on the OS; everything else travels with the app.
    copied = {}
    pending = [(binary, executable), (driver, frameworks / driver.name)]
    edits = []
    while pending:
        source, destination = pending.pop()
        source = source.resolve()
        if source in copied:
            continue
        copied[source] = destination
        if destination != executable:
            shutil.copy2(source, destination)
        replacements = []
        for dependency in dependencies(source):
            if dependency.startswith(("/usr/lib/", "/System/Library/")):
                continue
            dep = Path(dependency)
            if not dep.is_absolute() or not dep.exists():
                raise RuntimeError(f"Unresolved dependency in {source}: {dependency}")
            if dep.resolve() == source:
                continue
            target = frameworks / dep.name
            pending.append((dep, target))
            replacements += ["-change", dependency, f"@executable_path/../Frameworks/{dep.name}"]
        edits.append((destination, replacements))
    for destination, replacements in edits:
        if destination != executable:
            replacements += ["-id", f"@rpath/{destination.name}"]
        if replacements:
            run("install_name_tool", *replacements, str(destination))

    # The app runs where every library in it does: Homebrew's are built for the OS they came from.
    minimum = max(minimum_macos(destination) for destination in copied.values())
    version = run("sh", str(root / "scripts/version.sh"))
    with (contents / "Info.plist").open("wb") as output:
        plistlib.dump({
            "CFBundleExecutable": "verdite1",
            "CFBundleIdentifier": "io.github.voicedrew11.verdite1",
            "CFBundleName": "Verdite1",
            "CFBundleDisplayName": "Verdite1",
            "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": version,
            "CFBundleVersion": version,
            "LSMinimumSystemVersion": ".".join(str(part) for part in minimum),
            "NSHighResolutionCapable": True,
            "NSPrincipalClass": "NSApplication",
        }, output)
    for library in frameworks.iterdir():
        run("codesign", "--force", "--sign", "-", str(library))
    run("codesign", "--force", "--sign", "-", str(staging))
    run("codesign", "--verify", "--deep", "--strict", str(staging))
    if bundle.exists():
        shutil.rmtree(bundle)
    staging.rename(bundle)
    print(bundle)


if __name__ == "__main__":
    main()

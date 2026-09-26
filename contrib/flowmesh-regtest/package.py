#!/usr/bin/env python3
"""Package ONLY the guarded regtest GUI. Never launch, deploy, or publish it."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile
import zipfile

SOURCE = Path(__file__).resolve().parents[2]
VERSION = "1.1.5-flowmesh-test.4"
PROFILE = "vps-regtest-20260926-test4"
TARGET = "test_b3_flowmeshclosed-gui"
CA_SHA = "03d252bba9e5723f943415a74038d9367ffe0bfd9e6a683d3a65d43922b23d83"

def require(ok, message):
    if not ok:
        raise RuntimeError(message)

def run(*args):
    return subprocess.check_output([str(a) for a in args], text=True, cwd=SOURCE)

def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()

def save(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")

def cache_values(path):
    result = {}
    for line in path.read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            result[key.split(":", 1)[0]] = value
    return result

def validate_profile(raw, ca):
    profile = json.loads(raw)
    require(set(profile) == {"schema", "ready", "profile_id", "network", "regtest_profile",
        "public_session_approval", "operator", "availability", "b3_peer", "https_endpoints", "ca_pem"},
        "Unsupported profile fields")
    require(profile.get("schema") == 2 and profile.get("ready") is True and
            profile.get("network") == "regtest" and profile.get("profile_id") == PROFILE and
            profile.get("public_session_approval") == PROFILE and
            profile.get("regtest_profile") == "flowmesh-client-v1", "Wrong enabled regtest profile")
    require(profile.get("b3_peer") == "88.216.63.161:18547" and
            profile.get("https_endpoints") == ["https://88.216.63.161:18580/flowmesh/v1"], "Unapproved endpoint")
    require(profile.get("ca_pem", "").encode() == ca and hashlib.sha256(ca).hexdigest() == CA_SHA,
            "Public CA mismatch")
    require(b"PRIVATE KEY" not in raw and len(raw) <= 32768, "Private or oversized profile")
    return profile

def version_tuple(value):
    require(bool(re.fullmatch(r"\d+(?:\.\d+){0,3}", value)), "Invalid minimum OS version")
    return tuple(map(int, value.split("."))) + (0,) * (4 - len(value.split(".")))

def validate_cache(cache, build):
    require(Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() == SOURCE, "Wrong build source")
    require(cache.get("BUILD_FLOWMESH_REGTEST_CLIENT") == "ON", "Build is not guarded regtest")
    require(Path(cache["B3_FLOWMESH_CLOSED_TEST_PROFILE"]).resolve() ==
            SOURCE / "contrib/flowmesh-regtest/profile-vps-20260926.json", "Wrong embedded profile path")
    require(not cache.get("B3_UPDATE_MANIFEST_URL"), "Production update channel is forbidden")
    # Catch a stale configure/version without trusting output filenames.
    config = (build / "src/config/bitcoin-config.h").read_text() if (build / "src/config/bitcoin-config.h").exists() else ""
    generated = list((build / "src").rglob("bitcoin-build-config.h"))
    require(generated, "Generated build identity missing")
    config += generated[0].read_text()
    require(VERSION in config, "Generated source version differs")

def validate_compiled_identity(build, binary, commit):
    header = (build / "src/bitcoin-build-info.h").read_text()
    require(f'#define BUILD_GIT_COMMIT "{commit[:12]}"' in header, "Build is stale or was compiled from a dirty tree")
    required = {VERSION.encode(), commit[:12].encode()}
    tail = b""
    with binary.open("rb") as stream:
        while required and (block := stream.read(1024 * 1024)):
            block = tail + block
            required = {value for value in required if value not in block}
            tail = block[-256:]
    require(not required, "Executable lacks the expected compiled source identity")

def verify_macos(app, architecture, minimum):
    executable = app / "Contents/MacOS" / TARGET
    frameworks = app / "Contents/Frameworks"
    info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
    require(info["CFBundleIdentifier"] == "org.b3coin.flowmesh.regtest.test4", "Wrong bundle identity")
    require(info["CFBundleShortVersionString"] == "1.1.5" and info.get("B3CandidateVersion") == VERSION,
            "Wrong bundle version")
    require(version_tuple(info["LSMinimumSystemVersion"]) == version_tuple(minimum), "Minimum OS metadata differs")
    binaries = []
    for path in sorted(app.rglob("*")):
        if path.is_symlink():
            require(path.resolve().is_relative_to(app), "Escaping bundle link")
            continue
        if not path.is_file() or "Mach-O" not in run("file", "-b", path):
            continue
        require(run("lipo", "-archs", path).strip() == architecture, "Wrong binary architecture")
        load = run("otool", "-l", path)
        minimums = re.findall(r"\bminos ([0-9.]+)", load)
        minimums += re.findall(r"cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version ([0-9.]+)", load)
        require(minimums and all(version_tuple(v) <= version_tuple(minimum) for v in minimums),
                f"Dependency minimum exceeds macOS {minimum}: {path.name}: {minimums}")
        for dependency in run("otool", "-L", path).splitlines()[1:]:
            dep = dependency.strip().split(" (", 1)[0]
            if dep.startswith(("/System/Library/", "/usr/lib/")):
                continue
            if dep.startswith("@executable_path/"):
                target = executable.parent / dep.removeprefix("@executable_path/")
            elif dep.startswith("@loader_path/"):
                target = path.parent / dep.removeprefix("@loader_path/")
            elif dep.startswith("@rpath/"):
                target = frameworks / dep.removeprefix("@rpath/")
            else:
                raise RuntimeError("Unbundled dependency: " + dep)
            require(target.exists() and target.resolve().is_relative_to(app), "Unresolved bundled dependency: " + dep)
        binaries.append(str(path.relative_to(app)))
    require(binaries and executable.is_file(), "No guarded Mach-O executable")
    require((app / "Contents/PlugIns/platforms/libqcocoa.dylib").is_file(), "Missing Cocoa plugin")
    return binaries

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--platform", choices=("win64", "macos-arm64", "macos-x86_64"), required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--depends", type=Path)
    parser.add_argument("--policy-probe-output", type=Path)
    parser.add_argument("--macdeployqt", type=Path)
    parser.add_argument("--notices", type=Path, required=True)
    parser.add_argument("--minimum-macos", default="15.0")
    args = parser.parse_args()
    build = args.build.resolve(strict=True)
    output = args.output.absolute()
    require(not output.exists(), "Output already exists; do not overwrite a frozen package")
    require(not run("git", "status", "--porcelain", "--untracked-files=all").strip(), "Source must be clean and committed")
    commit = run("git", "rev-parse", "HEAD").strip()
    cache = cache_values(build / "CMakeCache.txt")
    validate_cache(cache, build)
    profile_path = SOURCE / "contrib/flowmesh-regtest/profile-vps-20260926.json"
    ca_path = SOURCE / "contrib/flowmesh-regtest/regtest-ca.pem"
    profile = validate_profile(profile_path.read_bytes(), ca_path.read_bytes())
    notices = args.notices.resolve(strict=True)
    notice_manifest = json.loads((notices / "NOTICE-MANIFEST.json").read_text())
    require(notice_manifest.get("collection_complete") is True, "Notice collection incomplete")
    for row in notice_manifest["files"]:
        path = notices / row["path"]
        require(path.resolve().is_relative_to(notices) and not path.is_symlink() and sha(path) == row["sha256"],
                "Unsafe or changed public notice input")
    require(any("LGPL-3.0" in row["path"] for row in notice_manifest["files"]), "Qt license missing")
    output.mkdir(parents=True)
    # Build-local staging is not a repository/runtime input and is preserved.
    stage = Path(tempfile.mkdtemp(prefix="regtest-package-", dir=build))
    payload = stage / "payload"
    payload.mkdir()
    original = build / "bin" / (TARGET + ".exe" if args.platform == "win64" else TARGET + ".app/Contents/MacOS/" + TARGET)
    validate_compiled_identity(build, original, commit)
    if args.platform == "win64":
        require(args.depends is not None, "Windows dependency root required")
        spec = importlib.util.spec_from_file_location("portable", SOURCE / "contrib/windeploy/package_portable.py")
        portable = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(portable)
        # Reuse existing PE-import closure, but never package a normal node tool.
        portable.PROGRAMS = (TARGET + ".exe",)
        closure = portable.collect_payload(build / "bin", args.depends.resolve(),
            "x86_64-w64-mingw32-objdump", "x86_64-w64-mingw32-g++-posix")
        for name, path in closure.items():
            shutil.copy2(path, payload / ("B3FlowMeshRegtest.exe" if name == TARGET + ".exe" else name))
        binary = payload / "B3FlowMeshRegtest.exe"
        if args.policy_probe_output:
            args.policy_probe_output.mkdir(parents=True, exist_ok=False)
            portable.PROGRAMS = ("test_b3_flowmeshclosed-policy.exe",)
            probes = portable.collect_payload(build / "bin", args.depends.resolve(),
                "x86_64-w64-mingw32-objdump", "x86_64-w64-mingw32-g++-posix")
            for name, path in probes.items():
                shutil.copy2(path, args.policy_probe_output / name)
    else:
        require(args.macdeployqt and args.macdeployqt.is_file(), "macdeployqt required")
        app = payload / "B3 FlowMesh REGTEST.app"
        shutil.copytree(build / "bin" / (TARGET + ".app"), app, symlinks=True)
        run(args.macdeployqt, app, "-verbose=1", "-always-overwrite")
        # qtSvg plugins may be discovered without the corresponding framework.
        qt_prefix = Path(cache["Qt6_DIR"]).resolve().parents[2]
        svg = qt_prefix / "lib/QtSvg.framework"
        if svg.is_dir() and not (app / "Contents/Frameworks/QtSvg.framework").exists():
            shutil.copytree(svg, app / "Contents/Frameworks/QtSvg.framework", symlinks=True)
        run("ruby", SOURCE / "contrib/macdeploy/normalize_bundled_libraries.rb", app)
        binary = app / "Contents/MacOS" / TARGET
        load = run("otool", "-l", binary)
        if "path @executable_path/../Frameworks (offset" not in load:
            run("install_name_tool", "-add_rpath", "@executable_path/../Frameworks", binary)
        # Remove absolute build RPATHs only after dependencies were deployed.
        for path in app.rglob("*"):
            if not path.is_symlink() and path.is_file() and "Mach-O" in run("file", "-b", path):
                for rpath in re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset", run("otool", "-l", path)):
                    if rpath.startswith("/"):
                        run("install_name_tool", "-delete_rpath", rpath, path)
        verify_macos(app, args.platform.removeprefix("macos-"), args.minimum_macos)
        run("codesign", "--force", "--deep", "--sign", "-", app)
        run("codesign", "--verify", "--deep", "--strict", app)
    for name, path in {"README.md": SOURCE / "contrib/flowmesh-regtest/README.md", "COPYING": SOURCE / "COPYING",
                       "PUBLIC-PROFILE.json": profile_path, "regtest-ca.pem": ca_path}.items():
        shutil.copy2(path, payload / name)
    (payload / "licenses").mkdir()
    for row in notice_manifest["files"]:
        destination = payload / "licenses" / row["path"]
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(notices / row["path"], destination)
    shutil.copy2(notices / "NOTICE-MANIFEST.json", payload / "licenses/NOTICE-MANIFEST.json")
    with (payload / "SOURCE-REBUILD.txt").open("x") as stream:
        stream.write("B3 corresponding application source and build workflow at exact revision:\n"
            + "https://github.com/B3-Coin/B3-CoinV2/tree/" + commit + "\n"
            + "Workflow: .github/workflows/flowmesh-regtest-build.yml\n"
            + "Qt source and license notices: licenses/NOTICE-MANIFEST.json\n"
            + "Windows exact dependencies and local Qt patches: depends/packages and depends/patches in source.\n"
            + "No restriction on modifying/rebuilding these open-source components for personal use.\n")
    identity = {"format": "b3-regtest-package-v1", "version": VERSION, "source_commit": commit,
        "profile_id": profile["profile_id"], "profile_sha256": sha(profile_path), "ca_sha256": sha(ca_path),
        "platform": args.platform, "minimum_macos": args.minimum_macos if args.platform.startswith("macos-") else None,
        "build_type": cache["CMAKE_BUILD_TYPE"], "guarded_executable": str(binary.relative_to(payload)),
        "qt_notice_version": notice_manifest["qt_version"],
        "executable_sha256": sha(binary), "validator_engine": False, "mainnet": False,
        "full_v2": False, "futures_trading": False, "performance_qualified": False,
        "signing": "unsigned; macOS ad-hoc integrity seal only, not notarized",
        "availability": profile["availability"], "original_accessibility_crash": "OPEN / UNRESOLVED",
        "native_ui_qualification": "not established by compilation or policy tests"}
    save(payload / "BUILD-INFO.json", identity)
    save(output / ("BUILD-INFO." + args.platform + ".json"), identity)
    archive = output / ("b3-flowmesh-regtest-v" + VERSION + "-unsigned-" + args.platform + ".zip")
    if args.platform.startswith("macos-"):
        run("ditto", "-c", "-k", "--noextattr", payload, archive)
        extracted = stage / "extracted"
        run("ditto", "-x", "-k", archive, extracted)
        require(sha(extracted / identity["guarded_executable"]) == identity["executable_sha256"], "Extracted binary mismatch")
        run("codesign", "--verify", "--deep", "--strict", extracted / "B3 FlowMesh REGTEST.app")
    else:
        with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED) as target:
            for path in sorted(payload.rglob("*")):
                require(not path.is_symlink(), "Unexpected Windows package link")
                if path.is_file():
                    target.write(path, path.relative_to(payload))
        with zipfile.ZipFile(archive) as target:
            require(target.testzip() is None, "Corrupt archive")
            require(hashlib.sha256(target.read(identity["guarded_executable"])).hexdigest() == identity["executable_sha256"], "Extracted binary mismatch")
    with (output / ("SHA256SUMS." + args.platform)).open("x") as stream:
        for path in sorted(output.iterdir()):
            if path.suffix in (".zip", ".json"):
                stream.write(sha(path) + "  " + path.name + "\n")
    require(run("git", "rev-parse", "HEAD").strip() == commit and
            not run("git", "status", "--porcelain", "--untracked-files=all").strip(), "Source changed during packaging")
    print(json.dumps(identity, sort_keys=True))

if __name__ == "__main__":
    main()

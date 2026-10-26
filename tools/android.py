# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Android TV host (native/android): builds an APK for 32-bit ARM, and with --device installs it, pushes
the app's bundle and fonts, and starts it.

    python3 tools/android.py --app-dir ../my-app --device <tv-address>:5555

The app is laid out at 1920x1080 whatever its app.json says: Android TV apps draw at 1080p and the
system scales them to the TV. Needs the Android SDK (ANDROID_HOME, default ~/Library/Android/sdk) with an
NDK and build-tools. curl, OpenSSL and FFmpeg are built for the ABI once, from pinned sources."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

from common import ROOT, app_config, bundle, dependency, fetch, run, stb_image
from text_fonts import harfbuzz, sheenbidi
import ffmpeg

ABI = "armeabi-v7a"
API = 30
WIDTH, HEIGHT = 1920, 1080
OPENSSL = ("3.5.2", "https://github.com/openssl/openssl/releases/download/openssl-3.5.2/openssl-3.5.2.tar.gz",
           "c53a47e5e441c930c3928cf7bf6fb00e5d129b630e0aa873b08258656e7345ec")
CURL = ("8.18.0", "https://curl.se/download/curl-8.18.0.tar.xz",
        "40df79166e74aa20149365e11ee4c798a46ad57c34e4f68fd13100e2c9a91946")
DEPS = ROOT / ".deps" / "android"


def sdk():
    return Path(os.environ.get("ANDROID_HOME", Path.home() / "Library/Android/sdk"))


def newest(directory):
    versions = sorted((p for p in directory.iterdir() if p.is_dir() and p.name[0].isdigit()),
                      key=lambda p: [int(x) for x in p.name.split(".") if x.isdigit()])
    if not versions: raise RuntimeError(f"Nothing installed in {directory}")
    return versions[-1]


def network_libraries(ndk):
    """curl over OpenSSL, static, for ABI: the prefix holding their headers and libraries."""
    prefix = DEPS / f"prefix-{ABI}"
    if (prefix / "lib/libcurl.a").exists(): return prefix
    toolchain = ndk / "toolchains/llvm/prebuilt/darwin-x86_64/bin"
    env = {**os.environ, "ANDROID_NDK_ROOT": str(ndk), "PATH": f"{toolchain}:{os.environ['PATH']}"}
    version, url, sha = OPENSSL
    archive = fetch(url, DEPS / Path(url).name, sha)
    source = DEPS / f"openssl-{version}"
    shutil.rmtree(source, ignore_errors=True)
    run(["tar", "xzf", archive, "-C", DEPS])
    run(["./Configure", "android-arm", f"-D__ANDROID_API__={API}", "no-shared", "no-tests", "no-docs", "no-apps",
         "no-module", f"--prefix={prefix}", "--libdir=lib"], env=env, cwd=source, log=DEPS / "openssl-configure.log")
    run(["make", "-j8", "build_libs"], env=env, cwd=source, log=DEPS / "openssl-build.log")
    run(["make", "install_dev"], env=env, cwd=source, log=DEPS / "openssl-install.log")
    version, url, sha = CURL
    archive = fetch(url, DEPS / Path(url).name, sha)
    source = DEPS / f"curl-{version}"
    shutil.rmtree(source, ignore_errors=True)
    run(["tar", "xJf", archive, "-C", DEPS])
    run(["cmake", "-S", source, "-B", source / "build", "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake", f"-DANDROID_ABI={ABI}",
         f"-DANDROID_PLATFORM=android-{API}", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=OFF",
         "-DBUILD_CURL_EXE=OFF", "-DBUILD_TESTING=OFF", "-DBUILD_LIBCURL_DOCS=OFF", "-DBUILD_MISC_DOCS=OFF",
         "-DENABLE_CURL_MANUAL=OFF", "-DCURL_USE_OPENSSL=ON", f"-DCMAKE_FIND_ROOT_PATH={prefix}",
         f"-DOPENSSL_ROOT_DIR={prefix}", f"-DOPENSSL_INCLUDE_DIR={prefix}/include",
         f"-DOPENSSL_CRYPTO_LIBRARY={prefix}/lib/libcrypto.a", f"-DOPENSSL_SSL_LIBRARY={prefix}/lib/libssl.a",
         "-DOPENSSL_USE_STATIC_LIBS=ON", "-DCURL_USE_LIBPSL=OFF", "-DCURL_USE_LIBSSH2=OFF", "-DCURL_BROTLI=OFF",
         "-DCURL_ZSTD=OFF", "-DUSE_NGHTTP2=OFF", "-DUSE_LIBIDN2=OFF", "-DCURL_DISABLE_LDAP=ON",
         f"-DCMAKE_INSTALL_PREFIX={prefix}"], log=DEPS / "curl-configure.log")
    run(["cmake", "--build", source / "build"], log=DEPS / "curl-build.log")
    run(["cmake", "--install", source / "build"], log=DEPS / "curl-install.log")
    return prefix


def launcher_art(app, res):
    """The launcher's art: a 16:9 banner (the app's android/banner.png, else its PS5 background pic0.png,
    else its icon on black) and the square icon, at xhdpi, which a 1080p TV launcher draws them at:
    the banner at 640x360, laid out at 320x180."""
    from PIL import Image, ImageOps
    shutil.rmtree(res, ignore_errors=True)
    (res / "drawable-xhdpi").mkdir(parents=True)
    icon = Image.open(app / "sce_sys/icon0.png").convert("RGBA")
    art = next((path for path in (app / "android/banner.png", app / "sce_sys/pic0.png") if path.exists()), None)
    if art:
        banner = ImageOps.fit(Image.open(art).convert("RGBA"), (640, 360), Image.LANCZOS)
    else:
        banner = Image.new("RGBA", (640, 360), (0, 0, 0, 255))
        small = icon.copy()
        small.thumbnail((360, 360))
        banner.alpha_composite(small, ((640 - small.width) // 2, (360 - small.height) // 2))
    banner.save(res / "drawable-xhdpi/banner.png")
    icon.resize((96, 96), Image.LANCZOS).save(res / "drawable-xhdpi/icon.png")


def package_name(config):
    return "dev.reactonanything." + "".join(c for c in config["name"].lower() if c.isalnum())


def build(app):
    er = dependency("embeddedReact")
    app, config, generated = bundle(app, er)
    layout = generated / f"{WIDTH}x{HEIGHT}"
    if not (layout / "app.bundle.js").exists() or config.get("resolution") != "auto":
        layout.mkdir(exist_ok=True)
        run(["node", ROOT / "tools/bundle.mjs", er, app, layout, str(WIDTH), str(HEIGHT)],
            log=generated.parent / f"bundle-{layout.name}.log")
    ndk = newest(sdk() / "ndk")
    tools = newest(sdk() / "build-tools")
    output = ROOT / ".build" / app.name / "android"
    run(["cmake", "-S", ROOT / "native/android", "-B", output / "native", "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake", f"-DANDROID_ABI={ABI}",
         f"-DANDROID_PLATFORM=android-{API}", "-DCMAKE_BUILD_TYPE=Release", f"-DANDROID_DEPS={network_libraries(ndk)}",
         f"-DFFMPEG_PREFIX={ffmpeg.android(ndk, ABI, API, network_libraries(ndk))}",
         f"-DER_ROOT={er}", f"-DHUI_ROOT={dependency('platform')}",
         f"-DFETCHCONTENT_SOURCE_DIR_QUICKJS={dependency('quickjsSource')}", f"-DAPP_GENERATED={generated}",
         f"-DAPP_LAYOUT={layout}", f"-DAPP_LAYOUT_WIDTH={WIDTH}", f"-DAPP_LAYOUT_HEIGHT={HEIGHT}",
         f"-DSTB_IMAGE_DIR={stb_image()}", f"-DHB_ROOT={harfbuzz()}", f"-DSB_ROOT={sheenbidi()}"],
        log=output / "configure.log")
    run(["cmake", "--build", output / "native", "--target", "main"], log=output / "build.log")

    package = package_name(config)
    manifest = output / "AndroidManifest.xml"
    manifest.write_text((ROOT / "native/android/AndroidManifest.xml").read_text()
                        .replace("@PACKAGE@", package).replace("@LABEL@", config["name"]))
    launcher_art(app, output / "res")
    run([tools / "aapt2", "compile", "-o", output / "res.zip", "--dir", output / "res"], log=output / "aapt2.log")
    unsigned = output / "unsigned.apk"
    run([tools / "aapt2", "link", "-o", unsigned, "--manifest", manifest, "-I",
         newest_platform() / "android.jar", output / "res.zip"], log=output / "aapt2.log")
    with zipfile.ZipFile(unsigned, "a") as apk:
        apk.write(output / "native/libmain.so", f"lib/{ABI}/libmain.so")
    aligned = output / "aligned.apk"
    aligned.unlink(missing_ok=True)
    run([tools / "zipalign", "-p", "-f", "4", unsigned, aligned], log=output / "zipalign.log")
    apk = output / f"{app.name}.apk"
    run([tools / "apksigner", "sign", "--ks", Path.home() / ".android/debug.keystore", "--ks-pass", "pass:android",
         "--out", apk, aligned], log=output / "apksigner.log")
    print(f"APK {apk}")
    return apk, package, layout, generated


def newest_platform():
    return max((p for p in (sdk() / "platforms").iterdir() if (p / "android.jar").exists()),
               key=lambda p: p.name)


def deploy(device, apk, package, layout, generated, options):
    adb = [sdk() / "platform-tools/adb", "-s", device]
    # -d: a build from a checkout with an older version code still replaces what is installed.
    run([*adb, "install", "-r", "-d", apk])
    files = f"/sdcard/Android/data/{package}/files"
    run([*adb, "shell", "mkdir", "-p", f"{files}/fonts"])
    run([*adb, "push", layout / "app.bundle.js", f"{files}/app.bundle.js"])
    if fonts := sorted((generated / "fonts").iterdir()): run([*adb, "push", *fonts, f"{files}/fonts/"])
    env_file = apk.parent / "options.env"
    env_file.write_text(options + "\n" if options else "")
    run([*adb, "push", env_file, f"{files}/options.env"])
    run([*adb, "shell", "am", "force-stop", package])
    run([*adb, "shell", "am", "start", "-n", f"{package}/android.app.NativeActivity"])


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--app-dir", type=Path, default=ROOT / "apps/starter")
    parser.add_argument("--device", help="adb serial to install on and start")
    parser.add_argument("--options", default="", help="options.env lines, e.g. 'ROA_GPU=0'")
    args = parser.parse_args()
    apk, package, layout, generated = build(args.app_dir.expanduser().resolve())
    if args.device: deploy(args.device, apk, package, layout, generated, args.options.replace(";", "\n"))


if __name__ == "__main__":
    sys.exit(main())

# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Nintendo Switch host (native/switch): builds a homebrew NRO with the app's bundle and fonts in its
RomFS, and with --send pushes it to the Homebrew Menu's netloader and prints the app's output.

    python3 tools/switch.py --app-dir ../anyfin --send 192.168.0.142

The app is laid out at 1920x1080 whatever its app.json says; the system scales it to the screen. Needs
devkitPro (DEVKITPRO, default /opt/devkitpro) with switch-dev, switch-mesa, switch-ffmpeg and switch-curl.
The console runs the NRO from the Homebrew Menu opened over a game (hold R), for the full memory."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

from common import ROOT, bundle, dependency, run, stb_image
from text_fonts import harfbuzz, sheenbidi

WIDTH, HEIGHT = 1920, 1080
DEVKITPRO = Path(os.environ.get("DEVKITPRO", "/opt/devkitpro"))


def app_name(config):
    return "".join(c for c in config["name"].lower() if c.isalnum())


def icon(app, path):
    """The Homebrew Menu's 256x256 JPEG: the app's icon centred on black."""
    from PIL import Image
    image = Image.new("RGB", (256, 256), (0, 0, 0))
    source = app / "sce_sys/icon0.png"
    if source.exists():
        art = Image.open(source).convert("RGBA")
        art.thumbnail((256, 256))
        image.paste(art, ((256 - art.width) // 2, (256 - art.height) // 2), art)
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, quality=90)


def build(app):
    er = dependency("embeddedReact")
    app, config, generated = bundle(app, er)
    layout = generated / f"{WIDTH}x{HEIGHT}"
    if not (layout / "app.bundle.js").exists() or config.get("resolution") != "auto":
        layout.mkdir(exist_ok=True)
        run(["node", ROOT / "tools/bundle.mjs", er, app, layout, str(WIDTH), str(HEIGHT)],
            log=generated.parent / f"bundle-{layout.name}.log")
    output = ROOT / ".build" / app.name / "switch"
    romfs = output / "romfs"
    shutil.rmtree(romfs, ignore_errors=True)
    (romfs / "fonts").mkdir(parents=True)
    shutil.copy(layout / "app.bundle.js", romfs / "app.bundle.js")
    for font in sorted((generated / "fonts").iterdir()): shutil.copy(font, romfs / "fonts" / font.name)
    icon(app, output / "icon.jpg")
    nro = output / f"{app.name}.nro"
    env = {**os.environ, "DEVKITPRO": str(DEVKITPRO)}
    run(["cmake", "-S", ROOT / "native/switch", "-B", output / "native", "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={DEVKITPRO}/cmake/Switch.cmake", "-DCMAKE_BUILD_TYPE=Release",
         f"-DER_ROOT={er}", f"-DHUI_ROOT={dependency('platform')}",
         f"-DFETCHCONTENT_SOURCE_DIR_QUICKJS={dependency('quickjsSource')}", f"-DAPP_GENERATED={generated}",
         f"-DAPP_LAYOUT={layout}", f"-DAPP_LAYOUT_WIDTH={WIDTH}", f"-DAPP_LAYOUT_HEIGHT={HEIGHT}",
         f"-DAPP_NAME={app_name(config)}", f"-DAPP_TITLE={config['name']}",
         f"-DAPP_VERSION={config.get('version', '1.0.0')}", f"-DAPP_ICON={output / 'icon.jpg'}",
         f"-DAPP_ROMFS={romfs}", f"-DNRO_OUTPUT={nro}",
         f"-DSTB_IMAGE_DIR={stb_image()}", f"-DHB_ROOT={harfbuzz()}", f"-DSB_ROOT={sheenbidi()}"],
        env=env, log=output / "configure.log")
    run(["cmake", "--build", output / "native", "--target", "main_nro"], env=env, log=output / "build.log")
    print(f"NRO {nro}")
    return nro


def send(nro, address):
    """Pushes the NRO to the netloader (Y in the Homebrew Menu) and stays for its output until it exits."""
    subprocess.run([DEVKITPRO / "tools/bin/nxlink", "-a", address, "-s", nro], check=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--app-dir", type=Path, default=ROOT / "apps/starter")
    parser.add_argument("--send", metavar="ADDRESS", help="the console's address, shown by the netloader")
    args = parser.parse_args()
    nro = build(args.app_dir.expanduser().resolve())
    if args.send: send(nro, args.send)


if __name__ == "__main__":
    sys.exit(main())

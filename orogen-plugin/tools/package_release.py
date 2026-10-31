#!/usr/bin/env python3
"""Собрать исходный ZIP и Linux VST3 ZIP из текущего workspace.

Linux: сначала собрать цель OROGEN_VST3 в build/vst3. Файл сборки Windows
отсутствует: Windows ZIP содержит только исходники и скрипты. Архивы перед
заменой проверяются CRC; бинарник упаковывается со снятыми символами и 0755.
"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
BUNDLE = ROOT / "build/vst3/OROGEN_artefacts/Release/VST3/OROGEN.vst3"
SOURCE_ZIP = ROOT / "OROGEN-vst3-vs2022.zip"
LINUX_ZIP = ROOT / "OROGEN-linux-vst3.zip"
ARTIFACT = ROOT / "orogen-plugin/artifact/OROGEN.vst3"
SOURCE_PREFIX = "OROGEN-vst3-vs2022"


def checked_file(path: Path) -> Path:
    if not path.is_file() or path.is_symlink():
        raise RuntimeError(f"Не найден обычный файл: {path}")
    return path


def tree_entries(path: Path, excluded: set[str] = frozenset()):
    for here, dirs, files in os.walk(path):
        dirs[:] = sorted(d for d in dirs if d not in excluded and not (Path(here) / d).is_symlink())
        for name in sorted(files):
            file = Path(here) / name
            if file.is_symlink():
                raise RuntimeError(f"Символическая ссылка в исходниках: {file}")
            yield file


def add(zipper: zipfile.ZipFile, file: Path, name: str):
    checked_file(file)
    zipper.write(file, name, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def check_zip(path: Path):
    with zipfile.ZipFile(path) as zipper:
        names = zipper.namelist()
        if len(names) != len(set(names)):
            raise RuntimeError(f"Повтор имён в {path}")
        bad = zipper.testzip()
        if bad is not None:
            raise RuntimeError(f"Неверная CRC в {path}: {bad}")
    return len(names)


def build_source(path: Path):
    with zipfile.ZipFile(path, "w") as zipper:
        roots = {
            "LICENSE": ROOT / "orogen-dsp/LICENSE",
            "NOTICE.md": ROOT / "orogen-dsp/NOTICE.md",
            "gui-spec.md": ROOT / "gui-spec.md",
        }
        for name in ("README-BUILD-WINDOWS.md", "build_vst3.bat", "build_vst3.ps1", "clean.bat"):
            roots[name] = ROOT / "orogen-plugin" / name
        for arc, src in sorted(roots.items()):
            add(zipper, src, f"{SOURCE_PREFIX}/{arc}")
        for src in tree_entries(ROOT / "orogen-plugin/licenses"):
            add(zipper, src, f"{SOURCE_PREFIX}/licenses/{src.name}")
        for project in ("orogen-dsp", "orogen-plugin"):
            base = ROOT / project
            for src in tree_entries(base, {"build", "audio", "artifact", "__pycache__"}):
                rel = src.relative_to(base)
                # Скрипты Windows и инструкция уже находятся в корне ZIP.
                if project == "orogen-plugin" and len(rel.parts) == 1 and rel.name in {
                    "README-BUILD-WINDOWS.md", "build_vst3.bat", "build_vst3.ps1", "clean.bat"
                }:
                    continue
                if src.suffix.lower() in {".o", ".so", ".wav", ".pyc"}:
                    continue
                add(zipper, src, f"{SOURCE_PREFIX}/{project}/{rel.as_posix()}")
    return check_zip(path)


def build_linux(path: Path, bundle: Path):
    with zipfile.ZipFile(path, "w") as zipper:
        roots = {"LICENSE": ROOT / "orogen-dsp/LICENSE",
                 "NOTICE.md": ROOT / "orogen-dsp/NOTICE.md",
                 "README-LINUX-VST3.md": ROOT / "orogen-plugin/README-LINUX-VST3.md"}
        for arc, src in sorted(roots.items()):
            add(zipper, src, arc)
        for src in tree_entries(bundle):
            add(zipper, src, f"OROGEN.vst3/{src.relative_to(bundle).as_posix()}")
        licenses = {
            "CHOWDSP-WDF-BSD.txt": ROOT / "orogen-dsp/third_party/chowdsp_wdf/LICENSE",
            "DATTORRO-MIT.txt": ROOT / "orogen-dsp/third_party/dattorro-verb/LICENSE",
        }
        for folder in (ROOT / "orogen-plugin/licenses", ROOT / "orogen-dsp/third_party/monojuice/licenses"):
            for src in tree_entries(folder):
                if src.name in licenses:
                    raise RuntimeError(f"Два файла лицензии с именем {src.name}")
                licenses[src.name] = src
        for name, src in sorted(licenses.items()):
            add(zipper, src, f"licenses/{name}")
    count = check_zip(path)
    with zipfile.ZipFile(path) as zipper:
        info = zipper.getinfo("OROGEN.vst3/Contents/x86_64-linux/OROGEN.so")
        if stat.S_IMODE(info.external_attr >> 16) != 0o755:
            raise RuntimeError("ELF в ZIP должен иметь права 0755")
    return count


def main():
    checked_file(BUNDLE / "Contents/x86_64-linux/OROGEN.so")
    checked_file(BUNDLE / "Contents/Resources/moduleinfo.json")
    with tempfile.TemporaryDirectory(prefix=".orogen-package-", dir=ROOT) as temp:
        tmp = Path(temp)
        staged = tmp / "OROGEN.vst3"
        shutil.copytree(BUNDLE, staged)
        binary = staged / "Contents/x86_64-linux/OROGEN.so"
        subprocess.run(["strip", "--strip-unneeded", str(binary)], check=True)
        binary.chmod(0o755)
        source, linux = tmp / "source.zip", tmp / "linux.zip"
        source_count = build_source(source)
        linux_count = build_linux(linux, staged)
        # Сохранить прежний бандл до успешной установки нового.
        ARTIFACT.parent.mkdir(parents=True, exist_ok=True)
        old = tmp / "previous.vst3"
        if ARTIFACT.exists():
            os.replace(ARTIFACT, old)
        try:
            os.replace(staged, ARTIFACT)
        except Exception:
            if old.exists():
                os.replace(old, ARTIFACT)
            raise
        os.replace(source, SOURCE_ZIP)
        os.replace(linux, LINUX_ZIP)
    print(f"Windows source ZIP: {source_count} файлов, CRC OK")
    print(f"Linux VST3 ZIP: {linux_count} файлов, CRC OK, ELF 0755")
    print(f"ELF: {ARTIFACT / 'Contents/x86_64-linux/OROGEN.so'} ({(ARTIFACT / 'Contents/x86_64-linux/OROGEN.so').stat().st_size} байт)")


if __name__ == "__main__":
    main()

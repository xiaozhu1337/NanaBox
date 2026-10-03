"""Remove individual build intermediates from the artifact staging directory."""

from pathlib import Path
import os


ROOT = Path(__file__).resolve().parents[1] / 'Output/Binaries'
INTERMEDIATE_SUFFIXES = {
    '.exp', '.idb', '.ilk', '.iobj', '.ipdb', '.lastbuildstate',
    '.lib', '.obj', '.res', '.tlog',
}


def main():
    staging = ROOT.resolve(strict=True)
    for directory, _, names in os.walk(staging, followlinks=False):
        for name in names:
            candidate = Path(directory) / name
            if candidate.suffix.lower() not in INTERMEDIATE_SUFFIXES:
                continue
            resolved = candidate.resolve(strict=True)
            if not resolved.is_relative_to(staging) or candidate.is_symlink():
                raise RuntimeError(f'Unsafe artifact path: {candidate}')
            print(f'Removing "{candidate}"')
            candidate.unlink()


if __name__ == '__main__':
    main()

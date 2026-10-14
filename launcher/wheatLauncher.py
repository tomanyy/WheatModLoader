from pathlib import Path
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from launcher.launcherWindow import main

if __name__ == "__main__":
    raise SystemExit(main())

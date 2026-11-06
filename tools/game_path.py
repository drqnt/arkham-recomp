"""Locate the user's own installed copy. Nothing from the game is ever copied into the repo."""
import os

DEFAULT = os.path.expanduser(
    "~/Library/Application Support/Steam/steamapps/common/Batman Arkham Asylum GOTY"
)


def game_root() -> str:
    return os.environ.get("ARKHAM_GAME_DIR", DEFAULT)


def game_binary() -> str:
    root = game_root()
    return os.path.join(root, "Batman Arkham Asylum GOTY.app/Contents/MacOS/Batman Arkham Asylum GOTY")


def game_bink() -> str:
    return os.path.join(game_root(), "Batman Arkham Asylum GOTY.app/Contents/Frameworks/libBinkMacx86.dylib")

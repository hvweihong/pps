import importlib


def load_board_e2e():
    try:
        return importlib.import_module("tools.board_e2e")
    except ModuleNotFoundError as exc:
        if exc.name == "tools.board_e2e":
            raise AssertionError("tools.board_e2e is not implemented") from exc
        raise


def require_symbol(module, name):
    try:
        return getattr(module, name)
    except AttributeError as exc:
        raise AssertionError(f"tools.board_e2e.{name} is not implemented") from exc

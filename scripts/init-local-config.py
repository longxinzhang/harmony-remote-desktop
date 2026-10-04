#!/usr/bin/env python3
"""Create missing signing-free Harmony build profiles without replacing local ones."""
from pathlib import Path
import json


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    for relative in ("host-harmony", "host-harmony/entry"):
        directory = root / relative
        template = directory / "build-profile.template.json5"
        target = directory / "build-profile.json5"
        if target.exists() or target.is_symlink():
            print(f"Preserved existing {target.relative_to(root)}")
            continue
        content = template.read_text(encoding="utf-8")
        profile = json.loads(content)
        app = profile.get("app", {})
        if "signingConfigs" in app or any(
            "signingConfig" in product for product in app.get("products", [])
        ):
            raise ValueError(f"Template must not configure signing: {template.name}")
        # Exclusive creation also protects a profile created after the check.
        try:
            with target.open("x", encoding="utf-8") as output:
                output.write(content)
        except FileExistsError:
            print(f"Preserved concurrently created {target.relative_to(root)}")
        else:
            print(f"Created unsigned {target.relative_to(root)}")
    print("Configure device signing locally in DevEco when needed; never commit signing material.")


if __name__ == "__main__":
    main()

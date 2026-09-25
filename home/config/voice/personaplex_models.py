"""Install pinned, gated PersonaPlex assets outside the Nix store."""

import argparse
import hashlib
import os
from pathlib import Path
import tarfile
import tempfile
import urllib.error
import urllib.parse
import urllib.request

REVISION = "fdaf4090a61cb315c138a1faee287ffd6c716309"
ASSETS = {
    "model.safetensors": (
        16742874000,
        "db1290db583cdaa6cb4de444ed279e0b586ca2a372b41434b07a7461c8c0e2f4",
    ),
    "tokenizer-e351c8d8-checkpoint125.safetensors": (
        384644900,
        "09b782f0629851a271227fb9d36db65c041790365f11bbe5d3d59369cf863f50",
    ),
    "tokenizer_spm_32k_3.model": (
        552778,
        "78d4336533ddc26f9acf7250d7fb83492152196c6ea4212c841df76933f18d2d",
    ),
    "voices.tgz": (
        6095521,
        "8564e9ca7a06ca723b07c3a77c623f0faa5937d04b2647b3a727b06c5ca0b7bb",
    ),
    "dist.tgz": (
        598195,
        "8de47fe2477491fac3dca404185d0430c8d39f9e0daf4c90ada5f03fdf830f45",
    ),
}


class PrivateRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        redirected = super().redirect_request(
            req, fp, code, msg, headers, newurl
        )
        if urllib.parse.urlparse(newurl).scheme != "https":
            raise ValueError("Refusing non-HTTPS model redirect")
        if (
            redirected
            and urllib.parse.urlparse(req.full_url).netloc
            != urllib.parse.urlparse(newurl).netloc
        ):
            redirected.remove_header("Authorization")
        return redirected


def matches(path, size, digest):
    if not path.is_file() or path.stat().st_size != size:
        return False
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest() == digest


def token():
    value = os.environ.get("HF_TOKEN")
    if not value:
        home = Path(
            os.environ.get("HF_HOME", Path.home() / ".cache/huggingface")
        )
        try:
            value = (home / "token").read_text().strip()
        except FileNotFoundError:
            pass
    if not value:
        raise ValueError(
            "Accept the PersonaPlex model terms and "
            "configure Hugging Face access first"
        )
    return value


def install(root, check_only=False):
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    opener = urllib.request.build_opener(PrivateRedirect())
    for name, (size, digest) in ASSETS.items():
        destination = root / name
        if matches(destination, size, digest):
            print(f"Verified {name}", flush=True)
            continue
        if check_only:
            raise ValueError(
                f"Missing or invalid {name}; run personaplex-models"
            )
        print(f"Downloading {name} ({size:,} bytes)", flush=True)
        request = urllib.request.Request(
            "https://huggingface.co/nvidia/personaplex-7b-v1/resolve/"
            f"{REVISION}/{name}",
            headers={"Authorization": "Bearer " + token()},
        )
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(
                dir=root, suffix=".partial", delete=False
            ) as output:
                temporary = Path(output.name)
                with opener.open(request, timeout=60) as response:
                    received = 0
                    while chunk := response.read(4 * 1024 * 1024):
                        received += len(chunk)
                        if received > size:
                            raise ValueError(
                                f"Unexpected download size for {name}"
                            )
                        output.write(chunk)
            if not matches(temporary, size, digest):
                raise ValueError(f"Integrity check failed for {name}")
            temporary.replace(destination)
        except urllib.error.HTTPError as exc:
            raise ValueError(
                f"Model download failed (HTTP {exc.code}); "
                "check gated model access"
            ) from None
        except urllib.error.URLError:
            raise ValueError(
                "Model download failed; check the network and retry"
            ) from None
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    if not check_only:
        for name in ("voices", "dist"):
            with tarfile.open(root / f"{name}.tgz") as archive:
                for member in archive.getmembers():
                    parts = Path(member.name).parts
                    if (
                        not parts
                        or parts[0] != name
                        or ".." in parts
                        or not (member.isfile() or member.isdir())
                    ):
                        raise ValueError(f"Unsafe member in {name} archive")
                archive.extractall(root, filter="data")
    for needed in ("dist/index.html", "voices/NATF2.pt"):
        if not (root / needed).is_file():
            raise ValueError(f"Missing {needed}; run personaplex-models")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    try:
        install(args.data_dir, args.check_only)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"PersonaPlex setup: {exc}\n")


if __name__ == "__main__":
    main()

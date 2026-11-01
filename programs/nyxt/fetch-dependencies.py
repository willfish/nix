"""Fetch the source archives pinned by Nyxt's ocicl.csv without Ocicl."""

import concurrent.futures
import csv
import hashlib
import io
import json
import pathlib
import re
import sys
import tarfile
import urllib.parse
import urllib.request


def download(url, headers=None):
    request = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def verified(data, digest):
    if hashlib.sha256(data).hexdigest() != digest.removeprefix("sha256:"):
        raise ValueError(f"Archive digest mismatch: {digest}")
    return data


def fetch(item):
    reference, relative_path = item
    if reference.startswith("ghcr.io/"):
        repository, digest = reference.removeprefix("ghcr.io/").split("@")
        scope = urllib.parse.quote(f"repository:{repository}:pull")
        token_response = download(f"https://ghcr.io/token?scope={scope}")
        token = json.loads(token_response)["token"]
        headers = {
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.oci.image.manifest.v1+json",
        }
        base = f"https://ghcr.io/v2/{repository}"
        manifest_data = download(f"{base}/manifests/{digest}", headers)
        manifest = json.loads(verified(manifest_data, digest))
        layers = manifest.get("layers") or manifest["blobs"]
        archives = [
            verified(
                download(f"{base}/blobs/{layer['digest']}", headers),
                layer["digest"],
            )
            for layer in layers
            if "tar" in layer["mediaType"]
            or layer.get("annotations", {})
            .get("org.opencontainers.image.title", "")
            .endswith(".tar.gz")
        ]
        if not archives:
            raise ValueError(f"No source archive in {reference}")
        return [(archive, None) for archive in archives]

    match = re.fullmatch(
        r"git\+https://github.com/([^@]+)@([0-9a-f]{40})(?:#.*)?", reference
    )
    if not match:
        raise ValueError(f"Unsupported unpinned source: {reference}")
    repository, revision = match.groups()
    archive = download(
        f"https://api.github.com/repos/{repository}/tarball/{revision}"
    )
    return [(archive, relative_path.split("/")[0])]


def main():
    manifest, destination = sys.argv[1:]
    destination = pathlib.Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    with open(manifest) as stream:
        sources = {
            row[1]: row[2]
            for row in csv.reader(stream, skipinitialspace=True)
        }
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        for archives in executor.map(fetch, sorted(sources.items())):
            for data, directory in archives:
                with tarfile.open(fileobj=io.BytesIO(data)) as archive:
                    target = destination
                    if directory:
                        # GitHub and Ocicl use different checkout prefixes.
                        target /= directory
                        prefix = archive.getmembers()[0].name.split("/")[0]
                        prefix += "/"
                        members = []
                        for member in archive.getmembers():
                            if member.name.startswith(prefix):
                                member.name = member.name[len(prefix):]
                                if member.name:
                                    members.append(member)
                    else:
                        members = archive.getmembers()
                    archive.extractall(target, members=members, filter="data")
    for path in destination.rglob("*"):
        if path.is_file():
            path.chmod(0o755 if path.stat().st_mode & 0o111 else 0o644)


if __name__ == "__main__":
    main()

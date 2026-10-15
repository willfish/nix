# Radio Archive

Terminus holds a personal audio archive and serves it read-only on the tailnet.
The repository owns the scripts. The pool owns the bytes.

## Layout

| Path | Contents |
| --- | --- |
| `/srv/media/radio-archive/audio` | one MP3 per catalogued object, named by track id |
| `/srv/media/radio-archive/music` | the same bytes, organised and tagged for players |
| `/srv/media/radio-archive/unsorted` | pulls whose catalog row carries no title |
| `/srv/media/radio-archive/playlists` | generated M3U stations and `index.m3u` |
| `/srv/media/radio-archive/artwork` | channel artwork |
| `/srv/media/radio-archive/metadata` | catalog dump and `stations.json` |
| `/srv/media/radio-archive/work` | downloader state and failure logs |
| `/etc/radio-archive` | the scripts, deployed from `system/terminus/radio-archive/` |

`audio/` is never rewritten. `music/` is derived from it and can be rebuilt.
Completion is judged by byte length, not by the catalog's own size field.

## Serving

`system/terminus/media-http.nix` serves the tree on port 8085. The firewall stays
closed on the LAN. `tailscale0` is a trusted interface, so the archive is reachable
on the tailnet at `http://terminus:8085`. `terminus.local` does not resolve away
from home.

Playlist entries are `audio/<track id>.mp3`. cliamp refuses a plain M3U larger
than 1 MiB, so `build-stations.py` trims to that size. `playlists/index.m3u`
lists the stations. It does not contain their songs.

cliamp calls a named list of tracks a playlist. Press `L` for local
playlists. Each archive station is one playlist. Opening it expands the
station into songs.

Do not play a row inside a list of station addresses. cliamp treats that
row as a live stream and reconnects forever. Open the station playlist
itself.

Home Manager refreshes those playlists from the live index on switch.
The station names are not stored in this repository.

## Refreshing

```bash
ssh terminus
sudo bash /etc/radio-archive/refresh-library.sh --fetch
```

The run checks pulled objects, copies new files into the tagged tree, writes
tags with `id3v2` from nix, and regenerates playlists. Expect `problems: 0`.
Anything else lands in `work/bad_audio.txt` for a second `--fetch`.

```bash
sudo bash /etc/radio-archive/verify-remote-sizes.sh 8
python3 /etc/radio-archive/fetch-catalog.py --base ~/radio-archive-meta --sizes
```

## Stations

Official stations come from each channel's playlist data: an explicit track list
or a weighted matrix per energy tier. The rotation is seeded, so a rerun writes
the same playlist. Most channels publish only a few tracks this way. Each channel
also gets one playlist per tier; those tier files are not in the index.

Genre stations are the tracks no official playlist references. Energy stations
and Everything include the whole matching pool.

## Known gaps

- One catalogued object answers `400` upstream, so it is absent.
- Catalog size metadata is wrong for a few rows. Trust `work/expected_sizes.tsv`.
- Pulls with no title, artist or album stay under `unsorted/`.
- Playlists embed `http://terminus:8085`. Another name or port needs
  `build-stations.py --stream-base`.

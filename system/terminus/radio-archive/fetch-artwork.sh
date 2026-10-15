#!/usr/bin/env bash
# Download the official radio archive channel artwork into artwork/ (39 public
# JPGs).
set -uo pipefail
BASE=${BASE:-/srv/media/radio-archive}
cd "$BASE" || exit
mkdir -p artwork artwork/sets work

python3 - metadata/channels.json >work/artwork.tsv <<'PY'
import json, re, sys, unicodedata

def slug(text, fallback):
    text = unicodedata.normalize("NFKD", text or "")
    text = re.sub(r"[^\w\s.-]", "", text, flags=re.UNICODE)
    text = re.sub(r"\s+", "-", text.strip()).strip("-.").lower()
    return text or fallback

for chan in json.load(open(sys.argv[1])):
    keys = (
        "image_url",
        "about_image_url",
        "onboarding_image_url",
        "image_url_dark",
    )
    for key in keys:
        url = chan.get(key)
        if url:
            if key == "image_url":
                suffix = ""
            else:
                suffix = "-" + key.replace("_url", "").replace("_", "-")
            print(
                "%02d\t%s%s\t%s"
                % (
                    chan["channel_number"],
                    slug(chan["channel_name"], "chan"),
                    suffix,
                    url,
                )
            )
PY

new=0
while IFS=$'\t' read -r num name url; do
  out="artwork/${num}-${name}.jpg"
  if [[ -s $out ]]; then continue; fi
  if curl -fsSL --retry 3 --retry-delay 2 --max-time 60 -o "$out" "$url"; then
    new=$((new + 1))
  else
    echo "art fail ${num} ${url}" >>work/artwork_failed.txt
    rm -f "$out"
  fi
done <work/artwork.tsv

echo "artwork: ${new} new, $(find artwork -type f | wc -l) total, " \
  "$(wc -l <work/artwork.tsv) referenced"

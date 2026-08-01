# Bachata S4 compatibility frontend

This directory contains only the static frontend. Compatibility source data, screenshots,
and compressed logs live in `JICA98/Bachata-S4-Compatibility`.

The Pages workflow checks out both repositories, validates the append-only report source,
generates a compact homepage index and one full JSON file per CUSA, copies only optimized
screenshots into the Pages artifact, and leaves logs in the data repository.

For local preview from sibling clones:

```bash
rm -rf /tmp/bachata-compat-preview
cp -a compatibility-site /tmp/bachata-compat-preview
python3 ../Bachata-S4-Compatibility/scripts/build_site_data.py \
  --root ../Bachata-S4-Compatibility \
  --output /tmp/bachata-generated
mkdir -p /tmp/bachata-compat-preview/data /tmp/bachata-compat-preview/evidence
cp /tmp/bachata-generated/site-index.json /tmp/bachata-compat-preview/data/
cp /tmp/bachata-generated/releases.json /tmp/bachata-compat-preview/data/
cp -a /tmp/bachata-generated/games /tmp/bachata-compat-preview/data/
(cd ../Bachata-S4-Compatibility && find assets -type f -path '*/screenshots/*' \
  -exec cp --parents '{}' /tmp/bachata-compat-preview/evidence/ \;)
python3 -m http.server 8080 --directory /tmp/bachata-compat-preview
```

#!/usr/bin/env python3
"""Refresh bundled defaults from the same sources used by first-launch updates."""

import datetime
import json
from pathlib import Path
import re
from html.parser import HTMLParser
from urllib.parse import urljoin
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]


class TablePage(HTMLParser):
    header_url = None

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "meta" and attrs.get("name", "").lower() == "bmstable":
            self.header_url = attrs.get("content")


def fetch(url):
    request = Request(url, headers={"User-Agent": "AsoBMaShow-table-snapshot"})
    with urlopen(request, timeout=60) as response:
        return response.read().decode("utf-8-sig")


def main():
    source = (ROOT / "src/DifficultyTableDefaults.h").read_text(encoding="utf-8")
    defaults = re.search(r"kDefaultSources\[\] = \{(.*?)\};", source, re.S)
    urls = re.findall(r'"(https://[^"]+)"', defaults.group(1))
    if not urls:
        raise ValueError("No default difficulty table URLs found")
    tables = []
    for url in urls:
        source_text = fetch(url)
        try:
            header = json.loads(source_text)
            header_url = url
        except json.JSONDecodeError:
            page = TablePage()
            page.feed(source_text)
            if not page.header_url:
                raise ValueError(f"No bmstable metadata at {url}")
            header_url = urljoin(url, page.header_url)
            header = json.loads(fetch(header_url))
        data_url = urljoin(header_url, header["data_url"])
        data = json.loads(fetch(data_url))
        if not header.get("name") or not header.get("symbol") or not isinstance(data, list) or not data:
            raise ValueError(f"Invalid or empty table at {url}")
        tables.append({"source_url": url, "header_url": header_url,
                       "data_url": data_url, "header": header, "data": data})
        print(f'{header["name"]}: {len(data)} charts')
    snapshot = {"captured_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "tables": tables}
    output = ROOT / "assets/difficulty-tables/defaults.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(".tmp")
    temporary.write_text(json.dumps(snapshot, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    temporary.replace(output)
    print(f"Wrote {output} ({output.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()

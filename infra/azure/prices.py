#!/usr/bin/env python3
"""Archives Azure pay-as-you-go Linux VM prices (Azure Retail Prices API, no login needed) for the
cloud experiments' cost analysis: results/prices-azure.json.

  python3 infra/azure/prices.py [--region southcentralus] [SIZE ...]
"""

from __future__ import annotations

import argparse
import datetime
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

SIZES = ["Standard_D2as_v6", "Standard_D16ads_v6", "Standard_D16ds_v6", "Standard_D16s_v6"]
OUT = Path(__file__).resolve().parents[2] / "results" / "prices-azure.json"


def price(region: str, size: str) -> dict:
    query = (f"serviceName eq 'Virtual Machines' and armRegionName eq '{region}' and armSkuName eq '{size}' "
             f"and priceType eq 'Consumption'")
    url = "https://prices.azure.com/api/retail/prices?" + urllib.parse.urlencode({"$filter": query})
    for attempt in range(6):
        try:
            items = json.load(urllib.request.urlopen(url, timeout=60))["Items"]
            break
        except urllib.error.HTTPError as error:
            if error.code != 429 or attempt == 5:
                raise
            time.sleep(10 * (attempt + 1))
    linux = [i for i in items if i["productName"].startswith("Virtual Machines") and "Windows" not in i["productName"]
             and "Spot" not in i["skuName"]
             and "Low Priority" not in i["skuName"] and i["unitOfMeasure"] == "1 Hour"]
    if len(linux) != 1:
        raise SystemExit(f"{size}: expected one Linux pay-as-you-go price, found {len(linux)}")
    item = linux[0]
    return {"usd_per_hour": item["retailPrice"], "currency": item["currencyCode"], "meter": item["meterName"],
            "product": item["productName"], "effective": item["effectiveStartDate"]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--region", default="southcentralus")
    parser.add_argument("sizes", nargs="*", default=SIZES)
    arguments = parser.parse_args()
    data = {"source": "https://prices.azure.com/api/retail/prices (Linux, pay-as-you-go)",
            "retrieved": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
            "region": arguments.region,
            "aks_control_plane": "free tier, no charge",
            "sizes": {size: price(arguments.region, size) for size in arguments.sizes}}
    OUT.write_text(json.dumps(data, indent=1) + "\n")
    for size, item in data["sizes"].items():
        print(f"{size:<22} ${item['usd_per_hour']:.4f}/h")
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

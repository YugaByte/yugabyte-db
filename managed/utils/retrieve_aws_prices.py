#!/usr/bin/env python3
"""
Downloads all relevant AWS price info to managed/src/main/resources/aws_prices. Should be run
monthly or with major AWS pricing changes (e.g. API updates or adding regions).
"""
import atexit
import json
import logging
import os
import shutil
from datetime import datetime, timedelta
from urllib.request import urlopen

BASE_PRICING_URL = "https://pricing.us-east-1.amazonaws.com"
REGION_INDEX_URL = BASE_PRICING_URL + "/offers/v1.0/aws/AmazonEC2/current/region_index.json"
SUPPORTED_TYPES = ["m3.", "c5.", "c4.", "c5d.", "c3.", "i3."]
TARGET_DIRECTORY = os.path.expanduser('')
YW_DIR = os.path.abspath(os.path.dirname(os.path.dirname(os.path.realpath(__file__))))
AWS_PRICE_DIR = os.path.join(YW_DIR, "src/main/resources/aws_pricing")
VERSION_FILE = os.path.join(AWS_PRICE_DIR, "version_metadata.json")
UPDATE_INTERVAL = timedelta(weeks=4)
DATE_FORMAT = "%Y-%m-%d"


def should_save(product_pair):
    product = product_pair[1]
    att = product['attributes']
    isSupported = att.get('preInstalledSw', 'NA') == 'NA' and 'BoxUsage' in att.get('usagetype') \
        and any([att.get('instanceType', "").startswith(prefix) for prefix in SUPPORTED_TYPES])
    return isSupported and (
        (product.get('productFamily') == "Compute Instance"
            and att.get('servicecode') == "AmazonEC2"
            and att.get('operatingSystem') == "Linux"
            and att.get('licenseModel') in ("No License required", "NA")
            and ("SSD" in att.get('storage') or "EBS" in att.get('storage'))
            and att.get('currentGeneration') == "Yes"
            and att.get('tenancy') == "Shared")
        or (product.get('productFamily') == "Storage"
            and att.get("volumeType") in ("Provisioned IOPS", "General Purpose"))
        or (product.get('productFamily') == "System Operation"
            and att.get('group') == "EBS IOPS"))


def create_pricing_file(region_data):
    products = region_data['products']
    kept_products = dict(filter(should_save, products.items()))
    od = region_data['terms']['OnDemand']
    kept_od = {key: od[key] for key in kept_products}

    final_data = {'terms': {'OnDemand': kept_od}, 'products': kept_products}
    return final_data


def main():
    logging.info("Retrieving AWS pricing data...")
    with urlopen(REGION_INDEX_URL) as url:
        region_metadata = json.loads(url.read().decode()).get('regions')

    # Only update pricing data if it is old or if it doesn't exist.
    if os.path.exists(VERSION_FILE):
        old_version_data = {}
        try:
            with open(VERSION_FILE) as f:
                old_version_data = json.load(f)
        except IOError as e:
            logging.info("Failed to read %s and will download new data: %s", VERSION_FILE, e)
        old_date = old_version_data.get("date")
        if old_date and (
                datetime.now() - datetime.strptime(old_date, DATE_FORMAT) < UPDATE_INTERVAL):
            logging.info("Pricing information is up to date - skipping download.")
            return

    logging.info("Removing old pricing data if exists.")
    shutil.rmtree(AWS_PRICE_DIR, ignore_errors=True)

    os.makedirs(AWS_PRICE_DIR)

    with open(VERSION_FILE, 'w+') as f:
        json.dump({"date": datetime.now().strftime(DATE_FORMAT)}, f, indent=4)

    for region in region_metadata:
        region_price_url = BASE_PRICING_URL + region_metadata[region]['currentVersionUrl']
        logging.info("Downloading information for %s from %s", region, region_price_url)
        region_data = {}
        with urlopen(region_price_url) as url:
            region_data = json.loads(url.read().decode())
        final_data = create_pricing_file(region_data)
        target_file = os.path.join(AWS_PRICE_DIR, region)
        with open(target_file, 'w+') as f:
            json.dump(final_data, f, indent=4)
        logging.info("Parsed %s info to %s", region, target_file)

    logging.info("Finished retrieving AWS pricing data.")


if __name__ == '__main__':
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s: %(message)s")
    main()

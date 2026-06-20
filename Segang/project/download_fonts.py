import os
import urllib.request

fonts = [
    "SUITE-Light.woff2",
    "SUITE-Regular.woff2",
    "SUITE-Medium.woff2",
    "SUITE-SemiBold.woff2",
    "SUITE-Bold.woff2",
    "SUITE-ExtraBold.woff2",
    "SUITE-Heavy.woff2"
]

base_url = "https://cdn.jsdelivr.net/gh/projectnoonnu/noonfonts_2304-2@1.0/"
output_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static", "fonts")

os.makedirs(output_dir, exist_ok=True)

for font in fonts:
    url = base_url + font
    dest = os.path.join(output_dir, font)
    print(f"Downloading {url} to {dest}...")
    try:
        urllib.request.urlretrieve(url, dest)
        print(f"Downloaded {font} successfully.")
    except Exception as e:
        print(f"Failed to download {font}: {e}")

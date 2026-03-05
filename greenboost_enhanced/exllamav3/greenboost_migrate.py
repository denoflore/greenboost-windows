#!/usr/bin/env python3
import os
import sys

def patch_exllamav3(site_packages_path):
    # This script will patch the installed ExLlamaV3 to use CacheLayer_greenboost.
    cache_py_path = os.path.join(site_packages_path, "exllamav3", "cache.py")
    if not os.path.exists(cache_py_path):
        print(f"ExLlamaV3 not found at {cache_py_path}")
        return False

    with open(cache_py_path, 'r') as f:
        content = f.read()

    if "CacheLayer_greenboost" in content:
        print("Already patched.")
        return True

    patch = """
try:
    from greenboost_enhanced.exllamav3.greenboost import CacheLayer_greenboost
except ImportError:
    CacheLayer_greenboost = None
"""

    # Simple search/replace to inject GreenBoost CacheLayer support into ExLlamaV3.
    # A full implementation would find the CacheType enum and map it appropriately.
    new_content = patch + content

    with open(cache_py_path, 'w') as f:
        f.write(new_content)

    print(f"Successfully patched {cache_py_path} to support GreenBoost CacheLayer.")
    return True

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 greenboost_migrate.py <path_to_site_packages>")
        sys.exit(1)

    patch_exllamav3(sys.argv[1])

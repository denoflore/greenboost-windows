import torch
import sys
import os

from cache.greenboost import CacheLayer_greenboost

def greenboost_migrate_model(model_name: str, use_greenboost: bool = True):
    """
    Migrates ExLlamaV3 to use GreenBoost caching.
    By default, ExLlamaV3 initializes normal PyTorch tensors. We replace the
    CacheLayer instantiation to utilize CacheLayer_greenboost.
    """
    if not use_greenboost:
        print("GreenBoost migration disabled.")
        return

    try:
        gb_fd = os.open("/dev/greenboost", os.O_RDWR)
        print(f"[ExLlamaV3 GreenBoost] Opened /dev/greenboost with fd {gb_fd}")
    except Exception as e:
        print(f"[ExLlamaV3 GreenBoost] Error opening /dev/greenboost: {e}. Migration aborted.")
        return

    # Typical ExLlamaV3 init overrides:
    # config = ExLlamaV3Config(...)
    # model = ExLlamaV3(config)
    # cache = ExLlamaV3Cache(model, lazy=True)
    # For each layer, it initializes a CacheLayer.
    # We monkey-patch the ExLlamaV3Cache __init__ to use GreenBoost.

    try:
        import exllamav3.cache.base as exl_cache
        original_init = exl_cache.ExLlamaV3CacheBase._allocate_layer

        def new_allocate_layer(self, layer_idx):
            print(f"[ExLlamaV3 GreenBoost] Intercepting allocate_layer for layer {layer_idx}")
            hidden_size = self.model.config.hidden_size
            max_seq_len = self.max_seq_len

            return CacheLayer_greenboost(layer_idx, hidden_size, max_seq_len, gb_fd=gb_fd)

        exl_cache.ExLlamaV3CacheBase._allocate_layer = new_allocate_layer
        print("[ExLlamaV3 GreenBoost] Successfully patched ExLlamaV3CacheBase._allocate_layer")

    except ImportError:
        print("[ExLlamaV3 GreenBoost] Could not import exllamav3.cache.base. Ensure exllamav3 is installed.")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        greenboost_migrate_model(sys.argv[1])
    else:
        print("Usage: python3 greenboost_migrate.py <model_name>")

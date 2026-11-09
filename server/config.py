"""Private server key loading shared by the service and administrative tools."""
import os
from pathlib import Path


def parent_url():
    filename = os.environ.get('PARENT_URL_FILE')
    return Path(filename).read_text().strip() if filename else os.environ.get('PARENT_URL', '')


def data_key():
    filename = os.environ.get('MEMORY_DATA_KEY_FILE')
    value = Path(filename).read_text().strip() if filename else os.environ['MEMORY_DATA_KEY']
    key = bytes.fromhex(value)
    if len(key) != 32:
        raise RuntimeError('data key must be 32 bytes')
    return key

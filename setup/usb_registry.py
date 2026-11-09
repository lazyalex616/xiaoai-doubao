import re
HEADER = re.compile(r"^.*\+-o\s+.*?@([0-9A-Fa-f]{6,10})\s+<class IOUSBHostDevice\b")
VENDOR = re.compile(r'"idVendor"\s*=\s*(\d+)')
PRODUCT = re.compile(r'"idProduct"\s*=\s*(\d+)')
WORLD_VID_PID = (0x1B8E, 0xC003)
NORMAL_VID_PID = (0x2717, 0xD002)


def parse_ioreg(text: str) -> list[dict]:
    devices = []
    current = None
    for line in text.splitlines():
        header = HEADER.match(line)
        if header:
            if current:
                devices.append(current)
            current = {"location": header.group(1).lower(), "vendor": None,
                       "product": None}
            continue
        if current is None:
            continue
        for key, pattern in (("vendor", VENDOR), ("product", PRODUCT)):
            match = pattern.search(line)
            if match:
                current[key] = int(match.group(1))
    if current:
        devices.append(current)
    return [d for d in devices if d["vendor"] is not None and d["product"] is not None]


def classified(devices: list[dict]) -> dict:
    normal = [d for d in devices if (d["vendor"], d["product"]) == NORMAL_VID_PID]
    world = [d for d in devices if (d["vendor"], d["product"]) == WORLD_VID_PID]
    other_aml = [d for d in devices if d["vendor"] == 0x1B8E and d not in world]
    return {"normal": normal, "worldcup": world, "other_amlogic": other_aml}

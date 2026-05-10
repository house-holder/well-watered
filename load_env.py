import os

with open(".env") as f:
    for line in f:
        line = line.strip()
        if line and not line.startswith("#"):
            key, value = line.split("=", 1)
            os.environ[key] = value

env.Append(CPPDEFINES=[
    ("WIFI_SSID", '\\"%s\\"' % os.environ["WELLWATEREDNTWK"]),
    ("WIFI_PASSWORD", '\\"%s\\"' % os.environ["WELLWATEREDKEY"]),
])

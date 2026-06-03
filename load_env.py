import os
Import("env")

env_vars = {}
try:
    with open(".env") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                key, value = line.split("=", 1)
                env_vars[key.strip()] = value.strip()
except FileNotFoundError:
    print("load_env: no .env found, using production defaults")

defines = [
    ("WNET", '\\"%s\\"' % env_vars["WNW"]),
    ("WKEY", '\\"%s\\"' % env_vars["WKY"]),
]

dev = env_vars.get("DEV_ENV", "").lower()
if dev in ("1", "true", "yes"):
    defines.append("DEV_ENV")
    print("load_env: DEV_ENV set -> DHCP build")
else:
    print("load_env: production build -> static IP")

env.Append(CPPDEFINES=defines)

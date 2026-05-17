import os
Import("env")

with open(".env") as f:
    for line in f:
        line = line.strip()
        if line and not line.startswith("#"):
            key, value = line.split("=", 1)
            os.environ[key] = value

env.Append(CPPDEFINES=[
    ("WNET", '\\"%s\\"' % os.environ["WNW"]),
    ("WKEY", '\\"%s\\"' % os.environ["WKY"]),
])

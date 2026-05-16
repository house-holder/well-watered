import os
Import("env")

with open(".env") as f:
    for line in f:
        line = line.strip()
        if line and not line.startswith("#"):
            key, value = line.split("=", 1)
            os.environ[key] = value

env.Append(CPPDEFINES=[
    ("HOUSENET", '\\"%s\\"' % os.environ["HNW"]),
    ("HOUSEKEY", '\\"%s\\"' % os.environ["HKY"]),
    ("CGINET", '\\"%s\\"' % os.environ["CNW"]),
    ("CGIKEY", '\\"%s\\"' % os.environ["CKY"]),
    ("PNET", '\\"%s\\"' % os.environ["PNW"]),
    ("PKEY", '\\"%s\\"' % os.environ["PKY"]),
])

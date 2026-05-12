import os
Import("env")

with open(".env") as f:
    for line in f:
        line = line.strip()
        if line and not line.startswith("#"):
            key, value = line.split("=", 1)
            os.environ[key] = value

env.Append(CPPDEFINES=[
    ("HOUSENET", '\\"%s\\"' % os.environ["HOUSENTW"]),
    ("HOUSEKEY", '\\"%s\\"' % os.environ["HOUSEKEY"]),
    # ("CGINET", '\\"%s\\"' % os.environ["CGINTW"]),
    # ("CGIKEY", '\\"%s\\"' % os.environ["CGIKEY"]),
])

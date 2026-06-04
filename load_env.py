import os
Import("env")

env_vars = {}
with open(".env") as f:
    for line in f:
        line = line.strip()
        if line and not line.startswith("#"):
            key, value = line.split("=", 1)
            env_vars[key.strip()] = value.strip()

env.Append(CPPDEFINES=[
    ("HOME_NET", '\\"%s\\"' % env_vars["HNW"]),
    ("HOME_KEY", '\\"%s\\"' % env_vars["HKY"]),
    ("CGI_NET", '\\"%s\\"' % env_vars["CNW"]),
    ("CGI_KEY", '\\"%s\\"' % env_vars["CKY"]),
])

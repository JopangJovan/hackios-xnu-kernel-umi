from pathlib import Path
import os, subprocess, sys

stage=Path(sys.argv[1])
recipe=(stage/"xnu-cxx.args").read_text().splitlines()
wd=recipe.pop(0)
obj=stage/"UmiDwc3Xdci.o"
args=["xnu-clang++"]
while recipe:
    v=recipe.pop(0)
    if v=="-o":
        recipe.pop(0)
        args.extend(["-o",str(obj)])
    elif v in ("-MF","-MT","-MJ"):
        recipe.pop(0)
    elif v not in ("-MD","-MP"):
        args.append(str(stage/"UmiDwc3Xdci.cpp") if v.endswith("/IOKitBSDInit.cpp") else v)
subprocess.run(args, cwd=wd, check=True)

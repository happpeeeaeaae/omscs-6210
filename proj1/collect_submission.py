from pathlib import Path
import shutil
import sys
import re

from os import listdir
from os.path import isfile, join


def checkSubmission(requiredFiles, required=True):
    allOk = True
    missingFiles = []
    for r in requiredFiles:
        if not Path(r).is_file():
            allOk = False
            missingFiles.append(r)
            if required:
                print(f"ERROR:required file {r} not found! ")
            else:
                print(f"WARNING: file {r} not found! ")
    return allOk, missingFiles


def _copy_all_files(src_dir: Path, dst_dir: Path, label: str):
    if not src_dir.exists():
        raise FileNotFoundError(f"ERROR:{label} directory {src_dir} does not exist")
    if not src_dir.is_dir():
        raise NotADirectoryError(f"ERROR:{label} path {src_dir} is not a directory")

    files = [str(src_dir / f) for f in listdir(src_dir) if isfile(join(src_dir, f))]
    print(files)

    for f in files:
        print("copying " + f)
        shutil.copy2(f, dst_dir / Path(f).name)

def sanitize_name(name: str) -> str:
    name = name.strip().lower()
    name = name.replace(" ", "_")
    name = re.sub(r'[^A-Za-z0-9_]', '', name)
    return name

def createZippedFile(dirName):
    d = Path(dirName)
    cpu_dst = d / dirName / "cpu"
    mem_dst = d / dirName / "memory"

    # start clean
    if d.exists():
        shutil.rmtree(d, ignore_errors=True)

    cpu_dst.mkdir(parents=True, exist_ok=True)
    mem_dst.mkdir(parents=True, exist_ok=True)
    print("created directory", dirName)

    # copy cpu files
    print("copying cpu/src to cpu")
    _copy_all_files(Path("cpu/src"), cpu_dst, "cpu/src")

    # copy memory files
    print("copying memory/src to memory")
    _copy_all_files(Path("memory/src"), mem_dst, "memory/src")

    # create zip
    print("creating zip file")
    shutil.make_archive(dirName, "zip", root_dir=d, base_dir=dirName)

    # remove the intermediate folder
    print("removing intermediate folder")
    shutil.rmtree(d, ignore_errors=False)

    print("Zip file has been created successfully!")


if __name__ == "__main__":
    try:
        print("checking for required files..")
        requiredFiles = [
            "cpu/src/vcpu_scheduler.c",
            "cpu/src/Makefile",
            "cpu/src/Readme.md",
            "memory/src/memory_coordinator.c",
            "memory/src/Makefile",
            "memory/src/Readme.md",
        ]
        allOk, _ = checkSubmission(requiredFiles)

        logFiles = [
            "cpu/src/vcpu_scheduler1.log",
            "cpu/src/vcpu_scheduler2.log",
            "cpu/src/vcpu_scheduler3.log",
            "memory/src/memory_coordinator1.log",
            "memory/src/memory_coordinator2.log",
            "memory/src/memory_coordinator3.log",
        ]
        logAllOk, _ = checkSubmission(logFiles, required=False)
        if not logAllOk:
            print("Warning. Some log files are missing")
        else:
            print("All log files present")

        if not allOk:
            print("Aborting. Please make sure all required files are present")
            sys.exit(1)
        else:
            print("All required files present")
            firstname = sanitize_name(input("Enter first name: "))
            lastname = sanitize_name(input("Enter last name: "))
            dirName = f"{firstname}_{lastname}_p1"
            createZippedFile(dirName)

    except Exception as e:
        print(f"ERROR:{e}")
        sys.exit(1)
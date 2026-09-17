# Cap parallel compile jobs. TFLM's cc1plus OOMs on this PC if every core runs.
Import("env")

from SCons.Script import GetOption, SetOption

MAX_JOBS = 2
jobs = GetOption("num_jobs")
if jobs is None or jobs > MAX_JOBS:
    SetOption("num_jobs", MAX_JOBS)
    print("limit_jobs: num_jobs %s -> %d" % (jobs, MAX_JOBS))

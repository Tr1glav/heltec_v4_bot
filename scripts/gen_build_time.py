import time
Import("env")

ts = int(time.time())
print("[gen_build_time] BUILD_UNIX_TIME = %d" % ts)
env.Append(CPPDEFINES=[("BUILD_UNIX_TIME", ts)])
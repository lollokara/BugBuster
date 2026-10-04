"""Run a simulated 1-cell LiPo for 60 s and print its 1 s samples.

Creates a 500 mAh run at 100 % SOC, starts it, waits a minute, prints the
samples recorded so far and stops the run (it stays on flash).
"""
import bugbuster
import daq

rid = daq.run.new("example", "lipo", 1, 500, soc=100)
daq.run.start(rid)
bugbuster.sleep(60000)
for t_s, v, i, soc, flags in daq.samples(rid, 0, 60):
    print("%4d s  %.3f V  %.2f mA  %.2f %%  flags=%d" % (t_s, v, i * 1e3, soc, flags))
daq.run.stop()
print(daq.run.status())

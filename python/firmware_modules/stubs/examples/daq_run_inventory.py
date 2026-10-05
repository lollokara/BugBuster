"""List the battery-simulator runs stored on the DAQ HAT (read-only).

Prints each run and the state of the loaded one. Starts and changes nothing.
"""
import daq

if not daq.present():
    print("no DAQ HAT connected")
else:
    for run in daq.run.list():
        print("#%d %s: %s x%d, %d mAh%s" % (
            run["run_id"], run["name"], run["chem"], run["cells"], run["capacity_mah"],
            "  (loaded)" if run["active"] else ""))
    st = daq.run.status()
    print("state: %s, SOC %.1f %%" % (st["state"], st["soc"]))

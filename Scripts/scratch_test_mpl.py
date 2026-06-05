import threading
import time
import matplotlib
import matplotlib.pyplot as plt
import matplotlib.animation as animation

data = []

def bg_thread():
    for i in range(50):
        print("Bg thread tick", i)
        data.append(i)
        time.sleep(0.5)

t = threading.Thread(target=bg_thread, daemon=True)
t.start()

fig, ax = plt.subplots()
ax.text(0.5, 0.5, "Test", ha="center")

def update(frame):
    print("Update called, len data:", len(data))

anim = animation.FuncAnimation(fig, update, interval=500, cache_frame_data=False)
print("Starting plt.show()")
plt.show()
print("Done plt.show()")

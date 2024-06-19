import csv
import matplotlib.pyplot as plt

class impl:
    x = []
    y = []

impls = { }

with open("fifo-data-latest.csv") as file:
    lines = csv.reader(file)
    for row in lines:
        name = row[0]
        if name not in impls:
            impls[name] = impl()
        impls[name].x.append(int(row[1]))
        impls[name].y.append(int(row[2]))

print(impls)

for k, v in impls.items():
    plt.plot(v.x, v.y, label = k)

plt.xscale("log", base = 2)
plt.xlabel("Processors")
plt.ylabel("Iterations")
plt.title("FIFO-Queue Comparison")
plt.grid()
plt.legend()
plt.show()

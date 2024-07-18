import csv
import matplotlib.pyplot as plt
import sys

class impl:
    def __init__(self):
        self.x = []
        self.y = []

impls = { }

file = sys.argv[1] if len(sys.argv) == 2 else input("Please enter the .csv data file: ")

with open(file) as file:
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

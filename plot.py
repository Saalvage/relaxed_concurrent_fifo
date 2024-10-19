import csv
import matplotlib.pyplot as plt
import sys
import statistics

def first(x):
    return next(iter(x))

class impl:
    def __init__(self):
        self.values = { }

impls = { }

file = sys.argv[1] if len(sys.argv) == 2 else input("Please enter the .csv data file: ")

with open(file) as file:
    lines = csv.reader(file)
    for row in lines:
        if len(row) < 3:
            continue
        name = row[0]
        if name not in impls:
            impls[name] = impl()
        x = int(row[1])
        y = float(row[2])
        if (x not in impls[name].values):
            impls[name].values[x] = [y]
        else:
            impls[name].values[x].append(y)

for k, v in impls.items():
    values = v.values.values()
    avgs = list(map(statistics.mean, values))
    std = list(map(statistics.stdev, values)) if len(first(values)) > 1 else ([0] * len(values))
    xs, ys, std = zip(*sorted(zip(v.values.keys(), avgs, std)))
    plt.errorbar(xs, ys, yerr=std, label=k, fmt="-o", capsize=3, ecolor="black")
    plt.xlabel("Processors")
    plt.title("FIFO-Queue Comparison")

plt.xscale("log", base = 2)
plt.ylabel("Iterations")
plt.grid()
plt.legend()
plt.show()

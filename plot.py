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
        name = row[0]
        if name not in impls:
            impls[name] = impl()
        x = int(row[1])
        y = int(row[2])
        if (x not in impls[name].values):
            impls[name].values[x] = [y]
        else:
            impls[name].values[x].append(y)

if len(list(impls.values())[0].values) > 1:
    for k, v in impls.items():
        values = v.values.values()
        avgs = list(map(statistics.mean, values))
        std = list(map(statistics.stdev, values)) if len(first(values)) > 1 else 0
        plt.errorbar(v.values.keys(), avgs, yerr=std, label=k, fmt="-o", capsize=3, ecolor="black")
        plt.xlabel("Processors")
        plt.title("FIFO-Queue Comparison")
else:
    values = list(impls.values())
    avgs = list(map(lambda l: statistics.mean(first(l.values.values())), values))
    std = list(map(lambda l: statistics.stdev(first(l.values.values())), values))
    plt.errorbar(list(map(int, impls.keys())), avgs, yerr=std, label=list(values[0].values.keys())[0], fmt="-o", capsize=3, ecolor="black")
    plt.xlabel("Blocks per window per thread")
    plt.title("Relaxed-FIFO Window Size Comparison")

plt.xscale("log", base = 2)
plt.ylabel("Iterations")
plt.grid()
plt.legend()
plt.show()

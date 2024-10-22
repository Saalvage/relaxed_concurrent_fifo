import csv
import matplotlib.pyplot as plt
import sys
import statistics

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
        y = float(row[2])
        if (x not in impls[name].values):
            impls[name].values[x] = [y]
        else:
            impls[name].values[x].append(y)

for k, v in impls.items():
    for x, y in sorted(v.values.items()):
        print(k + " " + str(x) + " " + str(statistics.mean(y)) + " " + str(statistics.stdev(y)))
    print()

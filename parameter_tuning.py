import csv
import matplotlib.pyplot as plt
import sys
import numpy as np

class ParameterSet:
    pass

file_performance = sys.argv[1] if len(sys.argv) > 1 else input("Please enter the performance .csv data file: ")
file_quality = sys.argv[2] if len(sys.argv) > 2 else input("Please enter the quality .csv data file: ")

values = { }
block_multipliers = []
block_sizes = []

with open(file_performance) as file:
    lines = csv.reader(file)
    for row in lines:
        new_val = ParameterSet()
        new_val.performance = row[3]
        values[(row[0], row[1])] = new_val
        block_multipliers.append(row[0])
        block_sizes.append(row[1])

with open(file_quality) as file:
    lines = csv.reader(file)
    for row in lines:
        values[(row[0], row[1])].quality = row[3]

x = np.array([float(x.quality) for x in values.values()])
y = np.array([float(x.performance) for x in values.values()])

# TODO: I have no idea what I'm doing with these weights. We have less data points with higher values so we make them have more weight?
fit = np.polyfit(np.log(x), y, 1, w=np.arange(len(x)))
print(fit)

for k, v in values.items():
    print(v.quality + " " + v.performance + " " + k[0] + " " + k[1])

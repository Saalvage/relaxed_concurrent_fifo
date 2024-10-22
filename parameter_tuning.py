import csv
import sys
import statistics
import numpy as np

max_int = sys.maxsize
while True:
    try:
        csv.field_size_limit(max_int)
        break
    except OverflowError:
        max_int = int(max_int/10)

class ParameterSet:
    def __init__(self):
        self.performance = []
        self.quality = []

file_performance = sys.argv[1] if len(sys.argv) > 1 else input("Please enter the performance .csv data file: ")
file_quality = sys.argv[2] if len(sys.argv) > 2 else input("Please enter the quality .csv data file: ")

values = { }

def get_key(row):
    if row[0].isdigit():
        if row[1].isdigit():
            return ((row[0], row[1]), 2)
        return ((row[0],), 1)
    return ((), 0)

with open(file_performance) as file:
    lines = csv.reader(file)
    for row in lines:
        (key, data_index) = get_key(row)
        inner_values = values.setdefault(row[data_index], { })
        inner_values.setdefault(key, ParameterSet()).performance.append(float(row[data_index + 2]))

with open(file_quality) as file:
    lines = csv.reader(file)
    for row in lines:
        (key, data_index) = get_key(row)
        values[row[data_index]][key].quality.append(float(row[data_index + 2]))

for k, parameters in values.items():
    xstd = list(map(lambda x: statistics.stdev(x.quality), parameters.values()))
    x = list(map(lambda x: statistics.mean(x.quality), parameters.values()))
    ystd = np.array(list(map(lambda x: statistics.stdev(x.performance), parameters.values())))
    y = np.array(list(map(lambda x: statistics.mean(x.performance), parameters.values())))

    print(k)

    # TODO: I have no idea what I'm doing with these weights. We have less data points with higher values so we make them have more weight?
    if len(x) > 1:
        fit = np.polyfit(np.log(x), y, 1, w=np.arange(len(x)))
        print(fit)

    for parameter, x, xstd, y, ystd in zip(parameters.keys(), x, xstd, y, ystd):
        print(str(x) + " " + str(y) + " " + str(xstd) + " " + str(ystd) + " " + " ".join(parameter))

    print()
